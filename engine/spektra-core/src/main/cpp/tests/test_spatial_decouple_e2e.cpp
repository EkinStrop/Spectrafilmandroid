/*
 * Spektrafilm for Android — END-TO-END host parity test for the SPATIAL-EFFECT
 * DECOUPLING: the camera lens blur (camera.lens_blur_um) must run with the
 * halation/scatter kernels independently disabled.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * Historically the engine gated EVERY spatial effect (DIR-coupler diffusion,
 * camera diffusion filter, camera lens blur, halation/scatter, scanner
 * blur/unsharp) on the single halation_active toggle. The oracle instead gates
 * each effect on its OWN params (a zero value is inert) and only the
 * deactivate_spatial_effects debug switch masters them — by ZEROING the
 * per-effect fields (params_builder.py c1d0e44 L73-86). This test pins the
 * decoupled behavior against the scan_portra_lensblur_nohalation golden:
 * lens blur ON (1650 µm) + DIR diffusion + scanner unsharp at their nonzero
 * schema defaults, while the halation/scatter triples are OFF
 * (halation_active=0) — the exact composition the old master gate could not
 * produce.
 *
 * It checks film_log_raw / film_density_cmy / final_rgb against the golden
 * bit-exact (max_abs <= 1e-4, rms <= 1e-5), confirms lens-blur ON-vs-OFF
 * changes the output with halation still off (the decoupling proof), confirms
 * the output DIFFERS from the with-halation scan_portra_lensblur golden (so
 * halation is genuinely off, not silently on), and asserts SPK_NUM_THREADS
 * 1-vs-8 byte-identical output (thread invariance of the spatial branch).
 *
 * Build (host) — full source set, run from the cpp root:
 *   g++ -std=c++17 -O2 -pthread -I <cpp_root> -I <tools/parity> \
 *     tests/test_spatial_decouple_e2e.cpp spektra.cpp \
 *     model/*.cpp kernels/*.cpp io/*.cpp profiles/*.cpp \
 *     runtime/*.cpp runtime/stages/*.cpp \
 *     -o /tmp/test_spatial_decouple_e2e
 * Run (argv[4] optionally overrides the goldens ROOT):
 *   /tmp/test_spatial_decouple_e2e <asset_dir> \
 *     <scan_portra_lensblur_nohalation_golden_dir> <input.f64> [goldens_root]
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "spkvec_io.h"
#include "spektra.h"

namespace {

const char* kAssetDir = "/home/user/spektrafilm/src/spektrafilm/data";
const char* kGoldenDir =
    "/home/user/Spectrafilmandroid/tools/parity/goldens/"
    "scan_portra_lensblur_nohalation";
const char* kLensblurGoldenDir =
    "/home/user/Spectrafilmandroid/tools/parity/goldens/scan_portra_lensblur";
const char* kInputF64 =
    "/home/user/Spectrafilmandroid/engine/spektra-core/src/main/cpp/tests/"
    "scan_portra_input_rgb.f64";

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

double max_abs_diff(const float* a, const float* b, size_t n) {
    double m = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double d = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        if (d > m) m = d;
    }
    return m;
}

// Feature params mirroring gen_goldens.py's scan_portra_lensblur_nohalation case:
// scan route, lens blur 1650 µm ON, halation OFF, grain/glare OFF. The DIR
// diffusion trio and scanner unsharp stay at their NONZERO spk_default_params
// values — the golden was generated with deactivate_spatial_effects=False, so
// those effects are ACTIVE alongside the lens blur.
void set_case_params(spk_params* p) {
    p->film_profile = "kodak_portra_400";
    p->print_profile = "kodak_portra_endura";
    spk_default_params(p);
    p->exposure_compensation_ev = 0.0f;
    p->auto_exposure = 0;
    p->density_curve_gamma = 1.0f;
    p->grain_active = 0;
    p->halation_active = 0;  // halation OFF — the decoupling under test
    p->lens_blur_um = 1650.0f;
    p->dir_couplers_active = 1;
    p->glare_active = 0;
    p->scan_film = 1;
    p->output_color_space = SPK_CS_SRGB;
    p->output_cctf_encoding = 1;
    p->rgb_to_raw_method = SPK_RGB2RAW_HANATOS2025;
    p->preview_max_size = 640;
}

}  // namespace

int main(int argc, char** argv) {
    std::string asset_dir = argc > 1 ? argv[1] : kAssetDir;
    std::string golden_dir = argc > 2 ? argv[2] : kGoldenDir;
    std::string input_path = argc > 3 ? argv[3] : kInputF64;
    std::string lensblur_dir = kLensblurGoldenDir;
    if (argc > 4) {
        std::string root = argv[4];
        golden_dir = root + "/scan_portra_lensblur_nohalation";
        lensblur_dir = root + "/scan_portra_lensblur";
    }

    // --- Load goldens + input image ---
    spkvec::Array gold_rgb = spkvec::read(golden_dir + "/final_rgb.spkvec");
    spkvec::Array gold_logr = spkvec::read(golden_dir + "/film_log_raw.spkvec");
    spkvec::Array gold_fcmy =
        spkvec::read(golden_dir + "/film_density_cmy.spkvec");
    spkvec::Array gold_hal_rgb =
        spkvec::read(lensblur_dir + "/final_rgb.spkvec");
    const int height = static_cast<int>(gold_rgb.shape[0]);
    const int width = static_cast<int>(gold_rgb.shape[1]);
    const int npix = width * height;
    std::printf("Image: %dx%dx3 (%d pixels)\n", width, height, npix);

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
    spk_image in_img{rgb32.data(), width, height, SPK_CS_PROPHOTO};

    spk_engine* eng = nullptr;
    spk_status st = spk_engine_create(asset_dir.c_str(), &eng);
    if (st != SPK_OK) {
        std::fprintf(stderr, "engine create failed: %s\n", spk_status_str(st));
        return 2;
    }

    spk_params p{};
    set_case_params(&p);

    bool pass_logr = false, pass_fcmy = false, pass_rgb = false;

    spk_image tap_logr{};
    st = spk_simulate_tap(eng, &in_img, &p, "film_log_raw", &tap_logr);
    if (st != SPK_OK) {
        std::fprintf(stderr, "tap(film_log_raw) failed: %s\n", spk_status_str(st));
    } else {
        pass_logr = check("decouple film_log_raw", tap_logr.data, gold_logr.data);
        spk_image_free(&tap_logr);
    }
    spk_image tap_fcmy{};
    st = spk_simulate_tap(eng, &in_img, &p, "film_density_cmy", &tap_fcmy);
    if (st != SPK_OK) {
        std::fprintf(stderr, "tap(film_density_cmy) failed: %s\n", spk_status_str(st));
    } else {
        pass_fcmy = check("decouple film_density_cmy", tap_fcmy.data, gold_fcmy.data);
        spk_image_free(&tap_fcmy);
    }

    spk_image out{};
    st = spk_simulate(eng, &in_img, &p, &out);
    if (st != SPK_OK) {
        std::fprintf(stderr, "spk_simulate failed: %s\n", spk_status_str(st));
        spk_engine_destroy(eng);
        return 2;
    }
    pass_rgb = check("decouple final_rgb", out.data, gold_rgb.data);

    // --- Decoupling proof 1: lens blur ON-vs-OFF changes the output while
    //     halation stays OFF. Under the old master gate (halation_active) the
    //     lens blur was forced to 0 here, so this delta was exactly 0. ---
    bool blur_active = false;
    {
        spk_params p_off = p;
        p_off.lens_blur_um = 0.0f;
        spk_image out_off{};
        st = spk_simulate(eng, &in_img, &p_off, &out_off);
        if (st != SPK_OK) {
            std::fprintf(stderr, "spk_simulate(blur off) failed: %s\n",
                         spk_status_str(st));
        } else {
            double delta = max_abs_diff(out.data, out_off.data,
                                        static_cast<size_t>(npix) * 3);
            blur_active = delta > 1e-4;
            std::printf("[lens blur on vs off, halation off] max_abs = %.6e "
                        "(must be > 1e-4) -> %s\n",
                        delta, blur_active ? "BLUR ACTIVE" : "BLUR INERT?!");
            spk_image_free(&out_off);
        }
    }

    // --- Decoupling proof 2: the output matches the NO-halation golden at
    //     least 10x tighter than the with-halation scan_portra_lensblur golden.
    //     At 64x64 the halation kernels are sub-pixel (the two goldens differ by
    //     only ~1e-5), so an absolute threshold cannot discriminate; the RATIO
    //     proves halation is genuinely off, not silently riding along. ---
    double match_delta = max_abs_diff(out.data, gold_rgb.data.data(),
                                      static_cast<size_t>(npix) * 3);
    double hal_delta = max_abs_diff(out.data, gold_hal_rgb.data.data(),
                                    static_cast<size_t>(npix) * 3);
    bool halation_off =
        hal_delta > 1e-6 && hal_delta > 10.0 * (match_delta > 0 ? match_delta : 1e-9);
    std::printf("[vs with-halation lensblur golden] max_abs = %.6e vs own-golden "
                "max_abs = %.6e (must be >1e-6 and >10x own) -> %s\n",
                hal_delta, match_delta,
                halation_off ? "HALATION OFF" : "HALATION LEAKED?!");
    spk_image_free(&out);
    spk_engine_destroy(eng);

    // --- Thread invariance: byte-identical output at 1 vs 8 workers (fresh
    //     engine per run so no engine-level cache can mask a divergence). ---
    bool pass_threads = false;
    {
        std::vector<float> imgs[2];
        const char* counts[2] = {"1", "8"};
        bool ok = true;
        for (int i = 0; i < 2 && ok; ++i) {
            setenv("SPK_NUM_THREADS", counts[i], 1);
            spk_engine* e2 = nullptr;
            if (spk_engine_create(asset_dir.c_str(), &e2) != SPK_OK) { ok = false; break; }
            spk_image o2{};
            if (spk_simulate(e2, &in_img, &p, &o2) != SPK_OK || !o2.data) {
                ok = false;
            } else {
                imgs[i].assign(o2.data, o2.data + static_cast<size_t>(npix) * 3);
                spk_image_free(&o2);
            }
            spk_engine_destroy(e2);
        }
        unsetenv("SPK_NUM_THREADS");
        if (ok) {
            pass_threads = imgs[0].size() == imgs[1].size() &&
                           std::memcmp(imgs[0].data(), imgs[1].data(),
                                       imgs[0].size() * sizeof(float)) == 0;
        }
        std::printf("[threads 1 vs 8] %s\n",
                    pass_threads ? "byte-identical -> PASS"
                                 : "DIVERGED (or setup error) -> FAIL");
    }

    bool all = pass_logr && pass_fcmy && pass_rgb && blur_active &&
               halation_off && pass_threads;
    std::printf("%s\n", all ? "ALL PASS" : "FAIL");
    return all ? 0 : 1;
}
