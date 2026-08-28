/*
 * Spektrafilm for Android — on-device GPU numeric probe (#135 E3 -> #127). GPLv3.
 * Copyright (C) 2026 Spektrafilm Android contributors.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See <https://www.gnu.org/licenses/>.
 *
 * Film modeling powered by spektrafilm.
 *
 * --------------------------------------------------------------------------------
 * Standalone arm64 executable (adb push to /data/local/tmp; NOT an app change).
 * Measures the fp32 GPU scan integral (gpu/scan_spectral.comp via the unmodified
 * spk::gpu::scan_spectral host) against an f64 CPU reference that mirrors the
 * shader 1:1, on real hardware. GPU stays PREVIEW-ONLY regardless of the result;
 * this binary wires nothing into the app — it produces the number the #127 /
 * option-B decision consumes.
 *
 * Subcommands:
 *   gpu_probe caps                              — device + float-controls facts
 *   gpu_probe run  <profile.json> <golden.spkvec> — Tier 1: error vs f64 + det x5
 *   gpu_probe perf <profile.json> <golden.spkvec> — Tier 2: warm-call wall times
 *
 * Tables are extracted through the ENGINE'S OWN loaders/constants (profiles/
 * profile.cpp, model/color_output, model/spectral), folded exactly as
 * gpu/vulkan_compute.h documents:
 *   dye[b][k]  = channel_density[b][k]                       (fp32, verbatim)
 *   icmf[b][k] = 10^-base_density[b] * illum[b] * cmf[b][k] / normalization
 * Bands where channel_density or base_density is NaN contribute w=NaN->0 in the
 * CPU engine for EVERY pixel (runtime/stages/scanning.cpp), so both table rows
 * are zeroed — the same folding feeds GPU and reference alike.
 * --------------------------------------------------------------------------------
 */
#include <vulkan/vulkan.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gpu/vulkan_compute.h"
#include "model/color_output.h"
#include "model/spectral.h"
#include "profiles/profile.h"
#include "spkvec_io.h"

#include "ref_scan_f64.h"

namespace {

constexpr int kNB = 81;

// ── Table folding (engine data -> the gpu/vulkan_compute.h table contract) ──

struct Tables {
    std::vector<float> dye;   // NB*3 band-major (c,m,y)
    std::vector<float> icmf;  // NB*3 band-major (X,Y,Z), illum+base+norm folded
    float m[9];               // row-major XYZ->sRGB (engine kXYZ_to_RGB[SPK_CS_SRGB])
    // Composed matrix Mc.M (kRGB_to_RGB_CCTF . kXYZ_to_RGB, double product): the
    // engine's default output path applies the CAT02 round-trip Mc BEFORE the
    // CCTF (scanning.cpp encode_pixel); folding it into the XYZ->RGB matrix is an
    // exact linear composition, so a dispatch with m_engine mirrors the engine's
    // full linear chain with no shader change.
    float m_engine[9];
    int nulled_bands = 0;     // bands zeroed for NaN channel/base density
    double cmax[3];           // per-channel nanmax of density_curves (sweep domain)
};

Tables build_tables(const spk::Profile& film) {
    if (film.n_samples != kNB) {
        std::fprintf(stderr, "profile n_samples=%d != %d\n", film.n_samples, kNB);
        std::exit(2);
    }
    Tables t;
    t.dye.assign(kNB * 3, 0.0f);
    t.icmf.assign(kNB * 3, 0.0f);
    const double inv_norm = 1.0 / spk::kNormD50;
    for (int l = 0; l < kNB; ++l) {
        const float* cd = film.channel_density.data() + static_cast<size_t>(l) * 3;
        const float base = film.base_density[static_cast<size_t>(l)];
        const bool nul = std::isnan(base) || std::isnan(cd[0]) || std::isnan(cd[1]) ||
                         std::isnan(cd[2]);
        if (nul) {  // engine: w = NaN -> 0 for every pixel on this band
            ++t.nulled_bands;
            continue;  // both rows stay 0
        }
        t.dye[l * 3 + 0] = cd[0];
        t.dye[l * 3 + 1] = cd[1];
        t.dye[l * 3 + 2] = cd[2];
        const double w = std::pow(10.0, -static_cast<double>(base)) *
                         static_cast<double>(spk::kIlluminantD50[l]) * inv_norm;
        for (int k = 0; k < 3; ++k)
            t.icmf[l * 3 + k] =
                static_cast<float>(w * static_cast<double>(spk::kCieCmf1931[l][k]));
    }
    for (int k = 0; k < 9; ++k)
        t.m[k] = static_cast<float>(spk::kXYZ_to_RGB[SPK_CS_SRGB][k]);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            double acc = 0.0;
            for (int k = 0; k < 3; ++k)
                acc += spk::kRGB_to_RGB_CCTF[SPK_CS_SRGB][r * 3 + k] *
                       spk::kXYZ_to_RGB[SPK_CS_SRGB][k * 3 + c];
            t.m_engine[r * 3 + c] = static_cast<float>(acc);
        }
    // Sweep domain: per-channel nanmax of density_curves (the engine's own LUT
    // domain upper bound, scanning.cpp).
    for (int c = 0; c < 3; ++c) t.cmax[c] = 0.0;
    for (int n = 0; n < film.n_density_pts; ++n) {
        const float* dc = film.density_curves.data() + static_cast<size_t>(n) * 3;
        for (int c = 0; c < 3; ++c) {
            const double v = static_cast<double>(dc[c]);
            if (!std::isnan(v) && v > t.cmax[c]) t.cmax[c] = v;
        }
    }
    return t;
}

// ── Error stats ─────────────────────────────────────────────────────────────

struct Stats {
    double max_abs = 0.0, rms = 0.0;
    size_t worst_i = 0;  // flat component index
    int nan_gpu = 0, nan_ref = 0;
};

Stats compare(const float* gpu, const double* ref, size_t ncomp) {
    Stats s;
    double sum2 = 0.0;
    for (size_t i = 0; i < ncomp; ++i) {
        const double g = static_cast<double>(gpu[i]);
        const double r = ref[i];
        if (std::isnan(g)) ++s.nan_gpu;
        if (std::isnan(r)) ++s.nan_ref;
        if (std::isnan(g) || std::isnan(r)) continue;
        const double d = std::fabs(g - r);
        sum2 += d * d;
        if (d > s.max_abs) { s.max_abs = d; s.worst_i = i; }
    }
    s.rms = std::sqrt(sum2 / static_cast<double>(ncomp));
    return s;
}

uint64_t fnv1a(const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

double now_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// One Tier-1 case: dispatch, compare vs f64 reference, determinism x5.
// `m` selects the matrix (t.m = shader contract, t.m_engine = Mc.M composed).
int run_case(const char* name, const std::vector<float>& cmy, uint32_t npix,
             const Tables& t, const float* m, bool print_worst) {
    std::vector<float> gpu(static_cast<size_t>(npix) * 3, -1.0f);
    if (!spk::gpu::scan_spectral(cmy.data(), gpu.data(), npix, t.dye.data(),
                                 t.icmf.data(), m)) {
        std::printf("CASE %-8s GPU_FAIL (scan_spectral returned false)\n", name);
        return 1;
    }
    std::vector<double> ref(static_cast<size_t>(npix) * 3);
    ref_scan_f64(cmy.data(), ref.data(), npix, t.dye.data(), t.icmf.data(), m);
    const Stats s = compare(gpu.data(), ref.data(), gpu.size());

    // Determinism: 4 more identical dispatches, byte-compare against the first.
    bool det = true;
    std::vector<float> again(gpu.size());
    for (int rr = 0; rr < 4 && det; ++rr) {
        std::fill(again.begin(), again.end(), -2.0f);
        if (!spk::gpu::scan_spectral(cmy.data(), again.data(), npix, t.dye.data(),
                                     t.icmf.data(), m)) {
            std::printf("CASE %-8s GPU_FAIL on determinism rerun %d\n", name, rr + 2);
            return 1;
        }
        det = std::memcmp(gpu.data(), again.data(), gpu.size() * sizeof(float)) == 0;
    }

    std::printf("CASE %-8s npix=%-8u max_abs=%.9e rms=%.9e det_x5=%s nan_gpu=%d nan_ref=%d\n",
                name, npix, s.max_abs, s.rms, det ? "IDENTICAL" : "DIFFERS",
                s.nan_gpu, s.nan_ref);
    if (print_worst) {
        const size_t px = s.worst_i / 3, ch = s.worst_i % 3;
        std::printf("  worst: px=%zu comp=%zu cmy=(%.9g, %.9g, %.9g) ref=%.17g gpu=%.9g\n",
                    px, ch, cmy[px * 3], cmy[px * 3 + 1], cmy[px * 3 + 2],
                    ref[s.worst_i], gpu[s.worst_i]);
    }
    return 0;
}

// ── caps ────────────────────────────────────────────────────────────────────

const char* b2s(VkBool32 v) { return v ? "true" : "false"; }

int do_caps() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "spektra-gpu-probe";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        std::printf("CAPS no Vulkan instance (loader/ICD missing?)\n");
        return 1;
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    if (n == 0) { std::printf("CAPS no physical devices\n"); return 1; }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    VkPhysicalDevice pd = devs[0];  // same selection as the engine host (devs[0])

    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(pd, &p);
    std::printf("CAPS deviceName=%s\n", p.deviceName);
    std::printf("CAPS apiVersion=%u.%u.%u driverVersionRaw=0x%08x (%u.%u.%u)\n",
                VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion,
                VK_API_VERSION_MAJOR(p.driverVersion), VK_API_VERSION_MINOR(p.driverVersion),
                VK_API_VERSION_PATCH(p.driverVersion));
    std::printf("CAPS timestampComputeAndGraphics=%s timestampPeriodNs=%g\n",
                b2s(p.limits.timestampComputeAndGraphics), p.limits.timestampPeriod);

    // Core features (fp64) + fp16/16-bit-storage via features2 (instance is 1.1).
    VkPhysicalDeviceShaderFloat16Int8Features f16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    VkPhysicalDevice16BitStorageFeatures s16{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    s16.pNext = &f16;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &s16;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    std::printf("CAPS shaderFloat64=%s shaderFloat16=%s shaderInt8=%s\n",
                b2s(f2.features.shaderFloat64), b2s(f16.shaderFloat16), b2s(f16.shaderInt8));
    std::printf("CAPS storageBuffer16BitAccess=%s uniformAndStorageBuffer16BitAccess=%s\n",
                b2s(s16.storageBuffer16BitAccess), b2s(s16.uniformAndStorageBuffer16BitAccess));

    // Subgroup (1.1 core) + driver id + float controls (1.2 core properties;
    // also exposed by VK_KHR_shader_float_controls on 1.1 drivers).
    VkPhysicalDeviceFloatControlsProperties fc{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDeviceDriverProperties drv{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties sg{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    fc.pNext = &drv;
    drv.pNext = &sg;
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &fc;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    std::printf("CAPS driverID=%d driverName=%s driverInfo=%s\n",
                static_cast<int>(drv.driverID), drv.driverName, drv.driverInfo);
    std::printf("CAPS subgroupSize=%u\n", sg.subgroupSize);
    std::printf("CAPS floatControls: denormBehaviorIndependence=%d roundingModeIndependence=%d\n",
                static_cast<int>(fc.denormBehaviorIndependence),
                static_cast<int>(fc.roundingModeIndependence));
    std::printf("CAPS fp32: signedZeroInfNanPreserve=%s denormPreserve=%s denormFlushToZero=%s roundingModeRTE=%s roundingModeRTZ=%s\n",
                b2s(fc.shaderSignedZeroInfNanPreserveFloat32),
                b2s(fc.shaderDenormPreserveFloat32),
                b2s(fc.shaderDenormFlushToZeroFloat32),
                b2s(fc.shaderRoundingModeRTEFloat32),
                b2s(fc.shaderRoundingModeRTZFloat32));
    std::printf("CAPS fp16: signedZeroInfNanPreserve=%s denormPreserve=%s denormFlushToZero=%s roundingModeRTE=%s roundingModeRTZ=%s\n",
                b2s(fc.shaderSignedZeroInfNanPreserveFloat16),
                b2s(fc.shaderDenormPreserveFloat16),
                b2s(fc.shaderDenormFlushToZeroFloat16),
                b2s(fc.shaderRoundingModeRTEFloat16),
                b2s(fc.shaderRoundingModeRTZFloat16));
    std::printf("CAPS fp64: signedZeroInfNanPreserve=%s denormPreserve=%s denormFlushToZero=%s roundingModeRTE=%s roundingModeRTZ=%s\n",
                b2s(fc.shaderSignedZeroInfNanPreserveFloat64),
                b2s(fc.shaderDenormPreserveFloat64),
                b2s(fc.shaderDenormFlushToZeroFloat64),
                b2s(fc.shaderRoundingModeRTEFloat64),
                b2s(fc.shaderRoundingModeRTZFloat64));
    vkDestroyInstance(inst, nullptr);
    return 0;
}

// ── run (Tier 1) ────────────────────────────────────────────────────────────

int do_run(const char* profile_path, const char* golden_path) {
    if (!spk::gpu::available()) {
        std::printf("RUN GPU unavailable (spk::gpu::available()==false)\n");
        return 1;
    }
    spk::Profile film = spk::load_profile_file(profile_path);
    Tables t = build_tables(film);
    std::printf("TABLES profile=%s nulled_bands=%d cmax=(%.6g, %.6g, %.6g)\n",
                film.stock.c_str(), t.nulled_bands, t.cmax[0], t.cmax[1], t.cmax[2]);

    int rc = 0;

    // Case 1: the golden density plane (real film densities, 64x64).
    spkvec::Array golden = spkvec::read(golden_path);
    const uint32_t gpix = static_cast<uint32_t>(golden.count() / 3);
    rc |= run_case("golden", golden.data, gpix, t, t.m, /*print_worst=*/true);
    rc |= run_case("golden_mc", golden.data, gpix, t, t.m_engine, /*print_worst=*/true);

    // Case 2: synthetic CMY lattice sweeping kLo..cmax per channel (worst-case
    // band sums at the top corner; kLo = -0.1 covers the engine's scan-route
    // negative-density domain, whose LUT lower bound is -grain_density_min).
    // 64^3 = 262,144 pixels.
    {
        const int G = 64;
        const double kLo = -0.1;
        std::vector<float> sweep(static_cast<size_t>(G) * G * G * 3);
        size_t i = 0;
        for (int a = 0; a < G; ++a)
            for (int b = 0; b < G; ++b)
                for (int c = 0; c < G; ++c) {
                    sweep[i * 3 + 0] = static_cast<float>(kLo + (t.cmax[0] - kLo) * a / (G - 1));
                    sweep[i * 3 + 1] = static_cast<float>(kLo + (t.cmax[1] - kLo) * b / (G - 1));
                    sweep[i * 3 + 2] = static_cast<float>(kLo + (t.cmax[2] - kLo) * c / (G - 1));
                    ++i;
                }
        const uint32_t n = static_cast<uint32_t>(G) * G * G;
        rc |= run_case("sweep", sweep, n, t, t.m, /*print_worst=*/true);
        rc |= run_case("sweep_mc", sweep, n, t, t.m_engine, /*print_worst=*/true);
    }

    // Case 3: NaN density (engine semantics: every band w=NaN->0 => black; the
    // shader has NO NaN guard — record what the GPU actually emits).
    {
        const float qnan = std::nanf("");
        std::vector<float> nanc = {qnan, qnan, qnan,  qnan, 0.5f, 0.5f,
                                   0.5f, 0.5f, 0.5f};
        std::vector<float> gpu(nanc.size(), -1.0f);
        if (!spk::gpu::scan_spectral(nanc.data(), gpu.data(), 3, t.dye.data(),
                                     t.icmf.data(), t.m)) {
            std::printf("CASE nan      GPU_FAIL\n");
            rc |= 1;
        } else {
            std::vector<double> ref(nanc.size());
            ref_scan_f64(nanc.data(), ref.data(), 3, t.dye.data(), t.icmf.data(), t.m);
            std::printf("CASE nan      (per-pixel raw values; engine semantics for NaN density = black)\n");
            const char* label[3] = {"all-NaN ", "one-NaN ", "control "};
            for (int px = 0; px < 3; ++px)
                std::printf("  %s gpu=(%.9g, %.9g, %.9g) ref_f64=(%.9g, %.9g, %.9g)\n",
                            label[px], gpu[px * 3], gpu[px * 3 + 1], gpu[px * 3 + 2],
                            ref[px * 3], ref[px * 3 + 1], ref[px * 3 + 2]);
        }
    }
    return rc;
}

// ── perf (Tier 2) ───────────────────────────────────────────────────────────

int do_perf(const char* profile_path, const char* golden_path) {
    if (!spk::gpu::available()) {
        std::printf("PERF GPU unavailable\n");
        return 1;
    }
    spk::Profile film = spk::load_profile_file(profile_path);
    Tables t = build_tables(film);
    spkvec::Array golden = spkvec::read(golden_path);
    const size_t gcomp = golden.count();

    struct Sz { const char* name; uint32_t w, h; };
    const Sz sizes[2] = {{"0.3MP", 640, 480}, {"12MP", 4000, 3000}};
    for (const Sz& sz : sizes) {
        const uint32_t npix = sz.w * sz.h;
        std::vector<float> in(static_cast<size_t>(npix) * 3);
        for (size_t i = 0; i < in.size(); ++i) in[i] = golden.data[i % gcomp];
        std::vector<float> out(in.size());
        // Warmup (device/pipeline init + first dispatch) — excluded.
        if (!spk::gpu::scan_spectral(in.data(), out.data(), npix, t.dye.data(),
                                     t.icmf.data(), t.m)) {
            std::printf("PERF %-5s %ux%u GPU_FAIL\n", sz.name, sz.w, sz.h);
            return 1;
        }
        double ms[5];
        uint64_t hash[5];
        for (int r = 0; r < 5; ++r) {
            const double t0 = now_ms();
            if (!spk::gpu::scan_spectral(in.data(), out.data(), npix, t.dye.data(),
                                         t.icmf.data(), t.m)) {
                std::printf("PERF %-5s %ux%u GPU_FAIL on run %d\n", sz.name, sz.w, sz.h, r);
                return 1;
            }
            ms[r] = now_ms() - t0;
            hash[r] = fnv1a(out.data(), out.size() * sizeof(float));
        }
        // median of 5
        double srt[5]; std::memcpy(srt, ms, sizeof(ms));
        for (int i = 0; i < 5; ++i)
            for (int j = i + 1; j < 5; ++j)
                if (srt[j] < srt[i]) { double tmp = srt[i]; srt[i] = srt[j]; srt[j] = tmp; }
        bool det = true;
        for (int r = 1; r < 5; ++r) det = det && hash[r] == hash[0];
        std::printf("PERF %-5s %ux%u warm_ms=[%.1f, %.1f, %.1f, %.1f, %.1f] median=%.1f MPix/s=%.1f det_hash_x5=%s\n",
                    sz.name, sz.w, sz.h, ms[0], ms[1], ms[2], ms[3], ms[4], srt[2],
                    npix / 1e6 / (srt[2] / 1e3), det ? "IDENTICAL" : "DIFFERS");
        std::printf("  note: each call re-creates buffers+pipeline and round-trips host-visible memory (the host's per-call design) — this is offload cost, not pure kernel time\n");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "caps") == 0) return do_caps();
    if (argc >= 4 && std::strcmp(argv[1], "run") == 0) return do_run(argv[2], argv[3]);
    if (argc >= 4 && std::strcmp(argv[1], "perf") == 0) return do_perf(argv[2], argv[3]);
    std::fprintf(stderr,
                 "usage: gpu_probe caps\n"
                 "       gpu_probe run  <profile.json> <film_density_cmy.spkvec>\n"
                 "       gpu_probe perf <profile.json> <film_density_cmy.spkvec>\n");
    return 2;
}
