/*
 * Spektrafilm for Android — end-to-end host parity test for spk_simulate.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * Constructs an spk_params for the scan_portra case + an spk_image from the
 * deterministic scan_portra_input_rgb.f64 fixture, runs the WHOLE scan_film
 * pipeline through one spk_simulate() call, and compares the output to
 * final_rgb.spkvec — printing max_abs/rms and PASS/FAIL against the manifest
 * tolerances (max_abs <= 1e-4, rms <= 1e-5).
 *
 * It then exercises the PRINT (enlarger) route on the same fixture for TWO
 * (film, paper) pairs through the generalized native digest (neutral dichroic
 * CC resolved from neutral_print_filters.json + midgray exposure factor computed
 * natively — no baked per-pair constants):
 *   - print_portra: kodak_portra_400 -> kodak_portra_endura
 *   - print_ektar:  kodak_ektar_100  -> kodak_supra_endura
 * Each compares print_density_cmy (spk_simulate_tap) and final_rgb (spk_simulate)
 * to the committed goldens, proving the generalized print path is bit-exact for
 * an additional pair, not just the original portra case.
 *
 * Build (host) — full source set, run from the cpp root:
 *   g++ -std=c++17 -O2 -I <cpp_root> -I <tools/parity> \
 *     tests/test_simulate_e2e.cpp spektra.cpp \
 *     model/*.cpp kernels/*.cpp io/*.cpp profiles/*.cpp \
 *     runtime/params.cpp runtime/print_digest.cpp runtime/stages/*.cpp \
 *     -o /tmp/test_simulate_e2e
 * Run (golden dirs default to the repo-root /home/user/Spectrafilmandroid path;
 * argv[4] optionally overrides the goldens ROOT for a git worktree):
 *   /tmp/test_simulate_e2e <asset_dir> <scan_portra_golden_dir> <input.f64> \
 *     [goldens_root]
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "spkvec_io.h"
#include "spektra.h"

// Host-only accessors for the print-route film_density_cmy cache counters. Defined
// in spektra.cpp on the HOST build (#ifndef __ANDROID__); the host parity build
// compiles spektra.cpp directly into this binary, so they are available without any
// extra -D flag. Forward-declared here so the test can assert the cache engaged
// WITHOUT adding anything to spektra.h / the public ABI.
extern uint64_t spk_test_film_cache_hits(spk_engine* eng);
extern uint64_t spk_test_film_cache_misses(spk_engine* eng);
extern uint64_t spk_test_scan_film_cache_hits(spk_engine* eng);
extern uint64_t spk_test_scan_film_cache_misses(spk_engine* eng);
extern uint64_t spk_test_print_density_cache_hits(spk_engine* eng);
extern uint64_t spk_test_print_density_cache_misses(spk_engine* eng);

namespace {

const char* kAssetDir   = "/home/user/spektrafilm/src/spektrafilm/data";
const char* kGoldenDir  =
    "/home/user/Spectrafilmandroid/tools/parity/goldens/scan_portra";
const char* kPrintGoldenDir =
    "/home/user/Spectrafilmandroid/tools/parity/goldens/print_portra";
// Second (film, paper) pair exercising the GENERALIZED print path (native
// neutral-CC + midgray digest, no baked portra/ektar special-case): the
// kodak_ektar_100 negative printed on kodak_supra_endura paper. Driven by the
// SAME deterministic input fixture (scan_portra_input_rgb.f64 == make_test_image(64))
// the print_ektar golden was generated from.
const char* kPrintEktarGoldenDir =
    "/home/user/Spectrafilmandroid/tools/parity/goldens/print_ektar";
const char* kInputF64   =
    "/home/user/Spectrafilmandroid/engine/spektra-core/src/main/cpp/tests/"
    "scan_portra_input_rgb.f64";

// Compare a flat float buffer against a golden, print + return PASS/FAIL.
bool check(const char* label, const float* got, const std::vector<float>& gold) {
    double max_abs = 0.0, sse = 0.0;
    size_t argmax = 0;
    for (size_t i = 0; i < gold.size(); ++i) {
        double d = std::fabs(static_cast<double>(got[i]) -
                             static_cast<double>(gold[i]));
        if (d > max_abs) { max_abs = d; argmax = i; }
        sse += d * d;
    }
    double rms = std::sqrt(sse / static_cast<double>(gold.size()));
    const double tol_max_abs = 1e-4, tol_rms = 1e-5;
    bool pass = (max_abs <= tol_max_abs) && (rms <= tol_rms);
    std::printf("[%s] max_abs=%.6e (tol %.0e) rms=%.6e (tol %.0e) "
                "worst idx=%zu got=%.8f gold=%.8f -> %s\n",
                label, max_abs, tol_max_abs, rms, tol_rms, argmax,
                got[argmax], gold[argmax], pass ? "PASS" : "FAIL");
    return pass;
}

}  // namespace

int main(int argc, char** argv) {
    std::string asset_dir  = argc > 1 ? argv[1] : kAssetDir;
    std::string golden_dir = argc > 2 ? argv[2] : kGoldenDir;
    std::string input_path = argc > 3 ? argv[3] : kInputF64;
    // Optional argv[4] = goldens ROOT (the dir containing print_portra/,
    // print_ektar/, ...). When given, the print-route golden dirs are taken from
    // it; otherwise the repo-root defaults are used (CI / installed repo). This
    // lets the test run from a git worktree before the goldens land in the repo.
    std::string print_portra_dir = kPrintGoldenDir;
    std::string print_ektar_dir  = kPrintEktarGoldenDir;
    if (argc > 4) {
        std::string root = argv[4];
        print_portra_dir = root + "/print_portra";
        print_ektar_dir  = root + "/print_ektar";
    }

    spk_engine* eng = nullptr;
    spk_status st = spk_engine_create(asset_dir.c_str(), &eng);
    if (st != SPK_OK) {
        std::fprintf(stderr, "engine create failed: %s\n", spk_status_str(st));
        return 2;
    }

    spkvec::Array gold = spkvec::read(golden_dir + "/final_rgb.spkvec");
    const int height = static_cast<int>(gold.shape[0]);
    const int width  = static_cast<int>(gold.shape[1]);
    const int npix   = width * height;
    std::printf("Image: %dx%dx3 (%d pixels)\n", width, height, npix);

    // Load the deterministic float64 input fixture, promote to float32 for the
    // C API's linear-RGB spk_image buffer.
    std::vector<double> rgb64(static_cast<size_t>(npix) * 3);
    {
        std::ifstream in(input_path, std::ios::binary);
        if (!in) { std::fprintf(stderr, "cannot open %s\n", input_path.c_str()); return 2; }
        in.read(reinterpret_cast<char*>(rgb64.data()),
                static_cast<std::streamsize>(rgb64.size() * sizeof(double)));
        if (in.gcount() != static_cast<std::streamsize>(rgb64.size() * sizeof(double))) {
            std::fprintf(stderr, "input size mismatch\n");
            return 2;
        }
    }
    std::vector<float> rgb32(rgb64.begin(), rgb64.end());

    spk_image in_img{rgb32.data(), width, height, /*color_space=*/SPK_CS_PROPHOTO};

    spk_params p{};
    p.film_profile = "kodak_portra_400";
    p.print_profile = "kodak_portra_endura";
    spk_default_params(&p);  // physical defaults; parity case overrides below.
    p.exposure_compensation_ev = 0.0f;
    p.auto_exposure = 0;
    p.density_curve_gamma = 1.0f;
    p.grain_active = 0;       // deterministic goldens: stochastic + spatial off.
    p.halation_active = 0;
    // Spatial effects are per-effect gated (zero = inert); express the
    // oracle's deactivate_spatial_effects by zeroing the nonzero defaults.
    p.dir_diffusion_size_um = 0.0f;
    p.scanner_unsharp[0] = 0.0f;
    p.scanner_unsharp[1] = 0.0f;
    p.dir_couplers_active = 1;
    p.glare_active = 0;
    p.scan_film = 1;
    p.output_color_space = SPK_CS_SRGB;
    p.output_cctf_encoding = 1;
    p.rgb_to_raw_method = SPK_RGB2RAW_HANATOS2025;
    p.preview_max_size = 640;

    spk_image out{};
    st = spk_simulate(eng, &in_img, &p, &out);
    if (st != SPK_OK) {
        std::fprintf(stderr, "spk_simulate failed: %s\n", spk_status_str(st));
        spk_engine_destroy(eng);
        return 2;
    }

    const size_t n = static_cast<size_t>(npix) * 3;
    if (n != gold.data.size()) {
        std::fprintf(stderr, "size mismatch: got %zu, golden %zu\n", n, gold.data.size());
        spk_image_free(&out);
        spk_engine_destroy(eng);
        return 2;
    }

    double max_abs = 0.0, sse = 0.0;
    size_t argmax = 0;
    for (size_t i = 0; i < n; ++i) {
        double d = std::fabs(static_cast<double>(out.data[i]) -
                             static_cast<double>(gold.data[i]));
        if (d > max_abs) { max_abs = d; argmax = i; }
        sse += d * d;
    }
    double rms = std::sqrt(sse / static_cast<double>(n));

    const double tol_max_abs = 1e-4, tol_rms = 1e-5;
    bool pass = (max_abs <= tol_max_abs) && (rms <= tol_rms);
    std::printf("[scan_portra final_rgb] max_abs = %.6e (tol %.0e)\n",
                max_abs, tol_max_abs);
    std::printf("[scan_portra final_rgb] rms     = %.6e (tol %.0e)\n",
                rms, tol_rms);
    std::printf("worst idx=%zu: got=%.8f golden=%.8f -> %s\n", argmax,
                out.data[argmax], gold.data[argmax], pass ? "PASS" : "FAIL");
    spk_image_free(&out);

    // --- Print (enlarger) route: same input fixture, scan_film off. ----------
    std::string print_dir = print_portra_dir;
    spkvec::Array gold_print_cmy = spkvec::read(print_dir + "/print_density_cmy.spkvec");
    spkvec::Array gold_print_rgb = spkvec::read(print_dir + "/final_rgb.spkvec");

    p.scan_film = 0;  // negative -> print -> scan route.

    bool pass_print_cmy = false, pass_print_rgb = false;

    spk_image tap_cmy{};
    st = spk_simulate_tap(eng, &in_img, &p, "print_density_cmy", &tap_cmy);
    if (st != SPK_OK) {
        std::fprintf(stderr, "spk_simulate_tap(print_density_cmy) failed: %s\n",
                     spk_status_str(st));
    } else {
        pass_print_cmy = check("print_portra print_density_cmy", tap_cmy.data,
                               gold_print_cmy.data);
        spk_image_free(&tap_cmy);
    }

    spk_image print_out{};
    st = spk_simulate(eng, &in_img, &p, &print_out);
    if (st != SPK_OK) {
        std::fprintf(stderr, "spk_simulate(print) failed: %s\n", spk_status_str(st));
    } else {
        pass_print_rgb = check("print_portra final_rgb", print_out.data,
                               gold_print_rgb.data);
        spk_image_free(&print_out);
    }

    // --- Generalized print path: a SECOND (film, paper) pair --------------
    // kodak_ektar_100 -> kodak_supra_endura through the SAME C API. The neutral
    // dichroic CC + midgray exposure factor are computed natively for this pair
    // (no baked constants), proving the generalized print route matches the
    // oracle bit-exact under the same 1e-4/1e-5 tolerances.
    std::string ektar_dir = print_ektar_dir;
    spkvec::Array gold_ektar_cmy = spkvec::read(ektar_dir + "/print_density_cmy.spkvec");
    spkvec::Array gold_ektar_rgb = spkvec::read(ektar_dir + "/final_rgb.spkvec");

    spk_params pe{};
    pe.film_profile = "kodak_ektar_100";
    pe.print_profile = "kodak_supra_endura";
    spk_default_params(&pe);  // preserves film_profile/print_profile set above.
    pe.exposure_compensation_ev = 0.0f;
    pe.auto_exposure = 0;
    pe.density_curve_gamma = 1.0f;
    pe.grain_active = 0;
    pe.halation_active = 0;
    // Spatial effects are per-effect gated (zero = inert); express the
    // oracle's deactivate_spatial_effects by zeroing the nonzero defaults.
    pe.dir_diffusion_size_um = 0.0f;
    pe.scanner_unsharp[0] = 0.0f;
    pe.scanner_unsharp[1] = 0.0f;
    pe.dir_couplers_active = 1;
    pe.glare_active = 0;
    pe.scan_film = 0;  // negative -> print -> scan route.
    pe.output_color_space = SPK_CS_SRGB;
    pe.output_cctf_encoding = 1;
    pe.rgb_to_raw_method = SPK_RGB2RAW_HANATOS2025;
    pe.preview_max_size = 640;

    bool pass_ektar_cmy = false, pass_ektar_rgb = false;

    spk_image ektar_cmy{};
    st = spk_simulate_tap(eng, &in_img, &pe, "print_density_cmy", &ektar_cmy);
    if (st != SPK_OK) {
        std::fprintf(stderr, "spk_simulate_tap(print_ektar cmy) failed: %s\n",
                     spk_status_str(st));
    } else {
        pass_ektar_cmy = check("print_ektar print_density_cmy", ektar_cmy.data,
                               gold_ektar_cmy.data);
        spk_image_free(&ektar_cmy);
    }

    spk_image ektar_out{};
    st = spk_simulate(eng, &in_img, &pe, &ektar_out);
    if (st != SPK_OK) {
        std::fprintf(stderr, "spk_simulate(print_ektar) failed: %s\n",
                     spk_status_str(st));
    } else {
        pass_ektar_rgb = check("print_ektar final_rgb", ektar_out.data,
                               gold_ektar_rgb.data);
        spk_image_free(&ektar_out);
    }

    // --- Cache-hit correctness (engine profile/tc_lut PERF caches) -----------
    // A scan_portra render on the SAME, now warm-cached engine (kodak_portra_400
    // profile + tc_lut were loaded during the scan/print blocks above) must be
    // BYTE-IDENTICAL to a render on a FRESH engine (cold parse + build). This
    // guards the id-keyed caches against any cross-param staleness — exact
    // equality, not tolerance, since a cached entry is the same data as a fresh
    // parse/build (a memo, not an approximation).
    p.scan_film = 1;  // restore the scan_portra route (the print block set it 0).
    bool pass_cache = false;
    spk_image warm{}, cold{};
    spk_engine* eng_cold = nullptr;
    spk_status stw = spk_simulate(eng, &in_img, &p, &warm);
    spk_status stc = spk_engine_create(asset_dir.c_str(), &eng_cold);
    if (stc == SPK_OK) stc = spk_simulate(eng_cold, &in_img, &p, &cold);
    if (stw == SPK_OK && stc == SPK_OK && warm.data && cold.data) {
        pass_cache = (std::memcmp(warm.data, cold.data, n * sizeof(float)) == 0);
        std::printf("[cache warm==cold scan_portra] -> %s\n",
                    pass_cache ? "PASS (byte-identical)" : "FAIL");
    } else {
        std::fprintf(stderr, "cache-hit check setup failed (warm=%s cold=%s)\n",
                     spk_status_str(stw), spk_status_str(stc));
    }
    if (warm.data) spk_image_free(&warm);
    if (cold.data) spk_image_free(&cold);
    if (eng_cold) spk_engine_destroy(eng_cold);

    // --- Print-route film_density_cmy memo: HIT/MISS byte-identity ------------
    // The print route (run_print) memoizes the developed film_density_cmy in a
    // single content+param-hashed slot on the engine and skips expose+develop when
    // ONLY downstream (printing/scanning/tone-curve) params change. These scenarios
    // prove (1) a downstream-only edit on a WARM engine is BYTE-IDENTICAL to the
    // same edit on a FRESH (cold) engine AND that the cache HIT (filming reused),
    // and (2) a FILMING-side edit MISSES the cache but is still byte-identical.
    //
    // Base print-route params P0 (kodak_portra_400 -> kodak_portra_endura,
    // scan_film=0), warmed once on `eng` so the slot holds its film_density_cmy.
    auto make_p0 = []() {
        spk_params q{};
        q.film_profile = "kodak_portra_400";
        q.print_profile = "kodak_portra_endura";
        spk_default_params(&q);
        q.exposure_compensation_ev = 0.0f;
        q.auto_exposure = 0;
        q.density_curve_gamma = 1.0f;
        q.grain_active = 0;       // print route: spatial + stochastic OFF -> cache on
        q.halation_active = 0;
        // Spatial effects are per-effect gated (zero = inert); express the
        // oracle's deactivate_spatial_effects by zeroing the nonzero defaults.
        q.dir_diffusion_size_um = 0.0f;
        q.scanner_unsharp[0] = 0.0f;
        q.scanner_unsharp[1] = 0.0f;
        q.dir_couplers_active = 1;
        q.glare_active = 0;
        q.scan_film = 0;          // negative -> print -> scan route
        q.output_color_space = SPK_CS_SRGB;
        q.output_cctf_encoding = 1;
        q.rgb_to_raw_method = SPK_RGB2RAW_HANATOS2025;
        q.preview_max_size = 640;
        return q;
    };

    // Run `pp` on warm `eng`, compare byte-for-byte to a FRESH cold engine running
    // the SAME pp; returns true on exact equality (n is the scan_portra pixel count,
    // identical geometry for this fixture). Each call leaves `eng` warm.
    auto print_byte_identical = [&](const char* label, const spk_params* pp) -> bool {
        spk_image w{}, c{};
        spk_engine* ec = nullptr;
        spk_status sw = spk_simulate(eng, &in_img, pp, &w);
        spk_status sc = spk_engine_create(asset_dir.c_str(), &ec);
        if (sc == SPK_OK) sc = spk_simulate(ec, &in_img, pp, &c);
        bool ok = false;
        if (sw == SPK_OK && sc == SPK_OK && w.data && c.data) {
            ok = (std::memcmp(w.data, c.data, n * sizeof(float)) == 0);
            std::printf("[%s warm==cold] -> %s\n", label,
                        ok ? "PASS (byte-identical)" : "FAIL");
        } else {
            std::fprintf(stderr, "[%s] setup failed (warm=%s cold=%s)\n", label,
                         spk_status_str(sw), spk_status_str(sc));
        }
        if (w.data) spk_image_free(&w);
        if (c.data) spk_image_free(&c);
        if (ec) spk_engine_destroy(ec);
        return ok;
    };

    bool pass_film_cache = true;
    {
        // Warm the slot with P0 (this is a MISS that populates the cache).
        spk_params p0 = make_p0();
        spk_image warm0{};
        if (spk_simulate(eng, &in_img, &p0, &warm0) == SPK_OK && warm0.data) {
            spk_image_free(&warm0);
        }

        // Scenario A: downstream-only edits must HIT (filming reused) AND be
        // byte-identical to a fresh cold engine. Each edit changes nothing that
        // feeds filming, so the slot (still holding P0's film_density_cmy) must hit.
        struct DownEdit { const char* label; void (*apply)(spk_params*); };
        DownEdit downs[] = {
            {"film_cache A: y_filter_shift", [](spk_params* q){ q->y_filter_shift = 0.05f; }},
            {"film_cache A: output_color_space", [](spk_params* q){ q->output_color_space = SPK_CS_ADOBE_RGB; }},
            {"film_cache A: tone_curve S", [](spk_params* q){
                q->tone_curve_active = 1;
                q->tone_curve_master_n = 3;
                q->tone_curve_master_x[0] = 0.0f; q->tone_curve_master_y[0] = 0.0f;
                q->tone_curve_master_x[1] = 0.5f; q->tone_curve_master_y[1] = 0.45f;
                q->tone_curve_master_x[2] = 1.0f; q->tone_curve_master_y[2] = 1.0f;
            }},
        };
        for (const auto& d : downs) {
            // NOTE: re-warm the slot with P0 before each downstream edit so the
            // "hit" assertion is about THIS edit (the previous edit may itself have
            // populated the slot).
            spk_params pw = make_p0();
            spk_image rew{};
            if (spk_simulate(eng, &in_img, &pw, &rew) == SPK_OK && rew.data) spk_image_free(&rew);
            uint64_t hits0 = spk_test_film_cache_hits(eng);

            spk_params pe2 = make_p0();
            d.apply(&pe2);
            bool bi = print_byte_identical(d.label, &pe2);
            uint64_t hits1 = spk_test_film_cache_hits(eng);
            // These are genuine downstream-only edits: nothing feeding filming
            // changes, so the slot (still holding P0's film_density_cmy) must HIT.
            bool hit_ok = (hits1 > hits0);
            std::printf("[%s cache-hit] hits %llu->%llu -> %s\n", d.label,
                        (unsigned long long)hits0, (unsigned long long)hits1,
                        hit_ok ? "PASS" : "FAIL");
            pass_film_cache = pass_film_cache && bi && hit_ok;
        }

        // Scenario A (spatial memoizes): the memo key folds every deterministic
        // spatial shape param (Option A), so a spatial-ON render is cacheable —
        // the first run MISSES (stored), an identical repeat HITS, and both are
        // byte-identical to a fresh cold engine.
        {
            const char* label = "film_cache A: halation+unsharp spatial memoizes";
            spk_params pe2 = make_p0();
            pe2.halation_active = 1;            // spatial branch ON — now cacheable
            pe2.scanner_unsharp[0] = 1.0f; pe2.scanner_unsharp[1] = 0.5f;

            uint64_t miss0 = spk_test_film_cache_misses(eng);
            bool bi1 = print_byte_identical(label, &pe2);   // first run: MISS+store
            uint64_t miss1 = spk_test_film_cache_misses(eng);
            uint64_t hits0 = spk_test_film_cache_hits(eng);
            bool bi2 = print_byte_identical(label, &pe2);   // repeat: HIT
            uint64_t hits1 = spk_test_film_cache_hits(eng);
            bool memo_ok = (miss1 > miss0) && (hits1 > hits0);
            std::printf("[%s] misses %llu->%llu then hits %llu->%llu -> %s\n", label,
                        (unsigned long long)miss0, (unsigned long long)miss1,
                        (unsigned long long)hits0, (unsigned long long)hits1,
                        memo_ok ? "PASS" : "FAIL");
            pass_film_cache = pass_film_cache && bi1 && bi2 && memo_ok;
        }

        // Scenario A2 (grain bypass): grain is stochastic state — the memo must
        // NOT be consulted at all (neither counter moves), and the seeded grain
        // must still be byte-identical warm-vs-cold.
        {
            const char* label = "film_cache A2: grain cache-bypass";
            spk_params pw = make_p0();
            spk_image rew{};
            if (spk_simulate(eng, &in_img, &pw, &rew) == SPK_OK && rew.data) spk_image_free(&rew);
            uint64_t hits0 = spk_test_film_cache_hits(eng);
            uint64_t miss0 = spk_test_film_cache_misses(eng);

            spk_params pe2 = make_p0();
            pe2.grain_active = 1;               // stochastic -> bypass
            bool bi = print_byte_identical(label, &pe2);
            uint64_t hits1 = spk_test_film_cache_hits(eng);
            uint64_t miss1 = spk_test_film_cache_misses(eng);
            bool bypass_ok = (hits1 == hits0) && (miss1 == miss0);
            std::printf("[%s] hits %llu->%llu misses %llu->%llu -> %s\n", label,
                        (unsigned long long)hits0, (unsigned long long)hits1,
                        (unsigned long long)miss0, (unsigned long long)miss1,
                        bypass_ok ? "PASS" : "FAIL");
            pass_film_cache = pass_film_cache && bi && bypass_ok;
        }

        // Scenario D (KEY COMPLETENESS, mandated by the Option-A design): with the
        // spatial branch warm, every deterministic spatial shape param must MISS
        // when tweaked (it feeds a spatial kernel, so it MUST be in the key) and
        // stay byte-identical to a fresh cold engine. A stale-key aliasing bug
        // here would silently serve the previous shape's negative.
        {
            auto make_spatial = [&]() {
                spk_params q = make_p0();
                q.halation_active = 1;
                return q;
            };
            struct SpatialEdit { const char* label; void (*apply)(spk_params*); };
            SpatialEdit edits[] = {
                {"film_cache D: halation_scatter_amount", [](spk_params* q){ q->halation_scatter_amount = 0.9f; }},
                {"film_cache D: halation_halation_amount", [](spk_params* q){ q->halation_halation_amount = 1.7f; }},
                {"film_cache D: dir_diffusion_size_um", [](spk_params* q){ q->dir_diffusion_size_um = 20.0f; }},
                {"film_cache D: camera_diffusion(+strength)", [](spk_params* q){
                    q->camera_diffusion_active = 1;
                    q->camera_diffusion_strength = 0.8f;
                }},
                {"film_cache D: lens_blur_um", [](spk_params* q){ q->lens_blur_um = 800.0f; }},
                {"film_cache D: film_format_mm (pixel size)", [](spk_params* q){ q->film_format_mm = 60.0f; }},
            };
            for (const auto& s : edits) {
                // Warm the slot with the spatial-ON base; the tweak must then MISS.
                spk_params pw = make_spatial();
                spk_image rew{};
                if (spk_simulate(eng, &in_img, &pw, &rew) == SPK_OK && rew.data) spk_image_free(&rew);
                uint64_t miss0 = spk_test_film_cache_misses(eng);

                spk_params pe2 = make_spatial();
                s.apply(&pe2);
                bool bi = print_byte_identical(s.label, &pe2);
                uint64_t miss1 = spk_test_film_cache_misses(eng);
                bool miss_ok = (miss1 > miss0);
                std::printf("[%s cache-miss] misses %llu->%llu -> %s\n", s.label,
                            (unsigned long long)miss0, (unsigned long long)miss1,
                            miss_ok ? "PASS" : "FAIL");
                pass_film_cache = pass_film_cache && bi && miss_ok;
            }
        }

        // Scenario B: FILMING-side edits must MISS (filming recomputed) but still be
        // byte-identical to a fresh cold engine.
        struct FilmEdit { const char* label; void (*apply)(spk_params*); };
        FilmEdit films[] = {
            {"film_cache B: exposure_compensation_ev", [](spk_params* q){ q->exposure_compensation_ev = 0.7f; }},
            {"film_cache B: density_curve_gamma", [](spk_params* q){ q->density_curve_gamma = 0.9f; }},
            {"film_cache B: dir_amount", [](spk_params* q){ q->dir_amount = 0.5f; }},
            {"film_cache B: film_profile", [](spk_params* q){
                q->film_profile = "kodak_ektar_100";
                q->print_profile = "kodak_supra_endura";
            }},
        };
        for (const auto& f : films) {
            // Re-warm with P0 so the slot holds P0's filming output; the edit then
            // changes a filming input and must MISS.
            spk_params pw = make_p0();
            spk_image rew{};
            if (spk_simulate(eng, &in_img, &pw, &rew) == SPK_OK && rew.data) spk_image_free(&rew);
            uint64_t miss0 = spk_test_film_cache_misses(eng);

            spk_params pe2 = make_p0();
            f.apply(&pe2);
            bool bi = print_byte_identical(f.label, &pe2);
            uint64_t miss1 = spk_test_film_cache_misses(eng);
            bool miss_ok = (miss1 > miss0);
            std::printf("[%s cache-miss] misses %llu->%llu -> %s\n", f.label,
                        (unsigned long long)miss0, (unsigned long long)miss1,
                        miss_ok ? "PASS" : "FAIL");
            pass_film_cache = pass_film_cache && bi && miss_ok;
        }

        // Scenario E (SCAN-ROUTE memo): run_scan_film now has its own film-density
        // slot (film_memo[kMemoScan]). Assert: cold MISS -> identical repeat HIT;
        // a downstream-only edit (output space) HITs; a filming edit MISSes; grain
        // bypasses. Every case byte-identical to a fresh cold engine.
        {
            auto make_scan = [&]() {
                spk_params q = make_p0();
                q.scan_film = 1;
                // Unique filming value: the golden sections earlier in this test
                // already ran the plain scan composition on this warm engine, so
                // the scan slot may hold it — a distinct EV makes the first run
                // of THIS scenario a genuine MISS.
                q.exposure_compensation_ev = 0.31f;
                return q;
            };
            spk_params ps = make_scan();

            uint64_t smiss0 = spk_test_scan_film_cache_misses(eng);
            bool bi1 = print_byte_identical("film_cache E: scan first (miss)", &ps);
            uint64_t smiss1 = spk_test_scan_film_cache_misses(eng);
            uint64_t shit0 = spk_test_scan_film_cache_hits(eng);
            bool bi2 = print_byte_identical("film_cache E: scan repeat (hit)", &ps);
            uint64_t shit1 = spk_test_scan_film_cache_hits(eng);
            bool warm_ok = (smiss1 > smiss0) && (shit1 > shit0);
            std::printf("[film_cache E: scan miss-then-hit] misses %llu->%llu hits "
                        "%llu->%llu -> %s\n",
                        (unsigned long long)smiss0, (unsigned long long)smiss1,
                        (unsigned long long)shit0, (unsigned long long)shit1,
                        warm_ok ? "PASS" : "FAIL");
            pass_film_cache = pass_film_cache && bi1 && bi2 && warm_ok;

            // Downstream-only edit -> HIT (filming reused).
            spk_params pd = make_scan();
            pd.output_color_space = SPK_CS_ADOBE_RGB;
            uint64_t dhit0 = spk_test_scan_film_cache_hits(eng);
            bool bid = print_byte_identical("film_cache E: scan output-only edit", &pd);
            uint64_t dhit1 = spk_test_scan_film_cache_hits(eng);
            bool dhit_ok = (dhit1 > dhit0);
            std::printf("[film_cache E: scan output-only cache-hit] hits %llu->%llu -> %s\n",
                        (unsigned long long)dhit0, (unsigned long long)dhit1,
                        dhit_ok ? "PASS" : "FAIL");
            pass_film_cache = pass_film_cache && bid && dhit_ok;

            // Filming edit -> MISS.
            spk_params pf = make_scan();
            pf.exposure_compensation_ev = 0.7f;
            uint64_t fmiss0 = spk_test_scan_film_cache_misses(eng);
            bool bif = print_byte_identical("film_cache E: scan filming edit", &pf);
            uint64_t fmiss1 = spk_test_scan_film_cache_misses(eng);
            bool fmiss_ok = (fmiss1 > fmiss0);
            std::printf("[film_cache E: scan filming cache-miss] misses %llu->%llu -> %s\n",
                        (unsigned long long)fmiss0, (unsigned long long)fmiss1,
                        fmiss_ok ? "PASS" : "FAIL");
            pass_film_cache = pass_film_cache && bif && fmiss_ok;

            // Grain -> bypass (neither scan counter moves).
            spk_params pg = make_scan();
            pg.grain_active = 1;
            uint64_t ghit0 = spk_test_scan_film_cache_hits(eng);
            uint64_t gmiss0 = spk_test_scan_film_cache_misses(eng);
            bool big = print_byte_identical("film_cache E: scan grain bypass", &pg);
            uint64_t ghit1 = spk_test_scan_film_cache_hits(eng);
            uint64_t gmiss1 = spk_test_scan_film_cache_misses(eng);
            bool gbypass_ok = (ghit1 == ghit0) && (gmiss1 == gmiss0);
            std::printf("[film_cache E: scan grain cache-bypass] hits %llu->%llu misses "
                        "%llu->%llu -> %s\n",
                        (unsigned long long)ghit0, (unsigned long long)ghit1,
                        (unsigned long long)gmiss0, (unsigned long long)gmiss1,
                        gbypass_ok ? "PASS" : "FAIL");
            pass_film_cache = pass_film_cache && big && gbypass_ok;
        }

        // Scenario F (PRINT-DENSITY memo): print_expose+print_develop memoize on
        // the film_density_cmy CONTENT + printing inputs. Assert: output-only
        // edit HITs (scan() alone reruns); print-side edit MISSes; and — the
        // content-hash property — grain-on repeats HIT even though the FILM memo
        // bypasses (seeded grain produces identical film bytes).
        {
            auto pd_hits = [&]() { return spk_test_print_density_cache_hits(eng); };
            auto pd_miss = [&]() { return spk_test_print_density_cache_misses(eng); };

            // Warm the print-density slot with a distinct print base.
            spk_params pp = make_p0();
            pp.print_exposure = 1.11f;  // unique: this scenario owns the slot
            {
                spk_image w{};
                if (spk_simulate(eng, &in_img, &pp, &w) == SPK_OK && w.data) spk_image_free(&w);
            }

            // Output-only edit -> print-density HIT (only scan() reruns).
            spk_params po = pp;
            po.output_color_space = SPK_CS_ADOBE_RGB;
            uint64_t h0 = pd_hits();
            bool bio = print_byte_identical("print_density F: output-only edit", &po);
            bool ohit_ok = (pd_hits() > h0);
            std::printf("[print_density F: output-only cache-hit] hits %llu->%llu -> %s\n",
                        (unsigned long long)h0, (unsigned long long)pd_hits(),
                        ohit_ok ? "PASS" : "FAIL");
            pass_film_cache = pass_film_cache && bio && ohit_ok;

            // Print-side edit -> print-density MISS (film memo still HITs).
            spk_params py = pp;
            py.y_filter_shift = 0.07f;
            uint64_t m0 = pd_miss();
            bool biy = print_byte_identical("print_density F: y_filter_shift edit", &py);
            bool ymiss_ok = (pd_miss() > m0);
            std::printf("[print_density F: print-side cache-miss] misses %llu->%llu -> %s\n",
                        (unsigned long long)m0, (unsigned long long)pd_miss(),
                        ymiss_ok ? "PASS" : "FAIL");
            pass_film_cache = pass_film_cache && biy && ymiss_ok;

            // CONTENT-HASH property: with grain ON the FILM memo bypasses, but the
            // seeded grain reproduces identical film bytes, so an identical repeat
            // must HIT the print-density memo.
            spk_params pg = pp;
            pg.grain_active = 1;
            {
                spk_image w{};
                if (spk_simulate(eng, &in_img, &pg, &w) == SPK_OK && w.data) spk_image_free(&w);
            }
            uint64_t g0 = pd_hits();
            bool big2 = print_byte_identical("print_density F: grain repeat", &pg);
            bool ghit_ok = (pd_hits() > g0);
            std::printf("[print_density F: grain repeat content-hash hit] hits %llu->%llu -> %s\n",
                        (unsigned long long)g0, (unsigned long long)pd_hits(),
                        ghit_ok ? "PASS" : "FAIL");
            pass_film_cache = pass_film_cache && big2 && ghit_ok;
        }

        // Scenario G (ONE-SHOT MEMO OPT-OUT, EXPORT_FASTPATH item 2): a render
        // with disable_buffer_memos=1 must (1) produce byte-identical pixels to
        // the same params with the memos on (the memos are transparent, so the
        // flag cannot touch output), (2) move NO memo counter on either route
        // (no lookup, no store — the export path pays zero key hashing), and
        // (3) leave the warm slots intact rather than evicting them.
        {
            // -- Print route: warm both print-route slots with a unique base. --
            spk_params pb = make_p0();
            pb.exposure_compensation_ev = 0.53f;  // unique: owns this warm-up
            spk_image ref{};
            bool ok_ref =
                (spk_simulate(eng, &in_img, &pb, &ref) == SPK_OK && ref.data);

            uint64_t h0 = spk_test_film_cache_hits(eng);
            uint64_t m0 = spk_test_film_cache_misses(eng);
            uint64_t sh0 = spk_test_scan_film_cache_hits(eng);
            uint64_t sm0 = spk_test_scan_film_cache_misses(eng);
            uint64_t ph0 = spk_test_print_density_cache_hits(eng);
            uint64_t pm0 = spk_test_print_density_cache_misses(eng);

            spk_params poff = pb;
            poff.disable_buffer_memos = 1;
            spk_image off{};
            bool ok_off =
                (spk_simulate(eng, &in_img, &poff, &off) == SPK_OK && off.data);
            bool same = ok_ref && ok_off &&
                        std::memcmp(ref.data, off.data, n * sizeof(float)) == 0;
            bool frozen = h0 == spk_test_film_cache_hits(eng) &&
                          m0 == spk_test_film_cache_misses(eng) &&
                          sh0 == spk_test_scan_film_cache_hits(eng) &&
                          sm0 == spk_test_scan_film_cache_misses(eng) &&
                          ph0 == spk_test_print_density_cache_hits(eng) &&
                          pm0 == spk_test_print_density_cache_misses(eng);
            if (ref.data) spk_image_free(&ref);
            if (off.data) spk_image_free(&off);

            // The warm slots must have survived the opted-out render: repeating
            // the memo-on params must HIT (film + print-density), not re-store.
            uint64_t h1 = spk_test_film_cache_hits(eng);
            uint64_t p1 = spk_test_print_density_cache_hits(eng);
            spk_image rep{};
            bool ok_rep =
                (spk_simulate(eng, &in_img, &pb, &rep) == SPK_OK && rep.data);
            if (rep.data) spk_image_free(&rep);
            bool warm_kept = ok_rep && spk_test_film_cache_hits(eng) > h1 &&
                             spk_test_print_density_cache_hits(eng) > p1;

            // -- Scan route: the same counter-freeze property. --
            spk_params ps = make_p0();
            ps.scan_film = 1;
            ps.exposure_compensation_ev = 0.53f;
            ps.disable_buffer_memos = 1;
            uint64_t sh1 = spk_test_scan_film_cache_hits(eng);
            uint64_t sm1 = spk_test_scan_film_cache_misses(eng);
            spk_image so{};
            bool ok_scan =
                (spk_simulate(eng, &in_img, &ps, &so) == SPK_OK && so.data);
            if (so.data) spk_image_free(&so);
            bool scan_frozen = ok_scan &&
                               sh1 == spk_test_scan_film_cache_hits(eng) &&
                               sm1 == spk_test_scan_film_cache_misses(eng);

            bool g_ok = same && frozen && warm_kept && scan_frozen;
            std::printf("[film_cache G: disable_buffer_memos opt-out] pixels %s, "
                        "counters %s, warm slots %s, scan route %s -> %s\n",
                        same ? "identical" : "DIFFER",
                        frozen ? "frozen" : "MOVED",
                        warm_kept ? "kept" : "LOST",
                        scan_frozen ? "frozen" : "MOVED",
                        g_ok ? "PASS" : "FAIL");
            pass_film_cache = pass_film_cache && g_ok;
        }
    }

    spk_engine_destroy(eng);
    bool all = pass && pass_print_cmy && pass_print_rgb &&
               pass_ektar_cmy && pass_ektar_rgb && pass_cache && pass_film_cache;
    std::printf("%s\n", all ? "ALL PASS" : "FAIL");
    return all ? 0 : 1;
}
