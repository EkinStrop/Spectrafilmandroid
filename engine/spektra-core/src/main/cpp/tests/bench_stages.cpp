/*
 * Spektrafilm for Android — LOCAL host micro-benchmark for the simulate routes.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * PURE LOCAL TOOL. This is NOT a parity gate and is deliberately NOT wired into
 * .github/workflows/ci.yml. It measures wall-clock cost of the scan_film and
 * print routes and observes the print-route film_density_cmy memo (the extern
 * host counters spk_test_film_cache_hits/misses defined in spektra.cpp ~:216).
 *
 * Params mirror the DETERMINISTIC parity config (grain / halation / glare /
 * auto_exposure OFF, dir_couplers ON) — the same config make_p0 uses in
 * test_simulate_e2e.cpp. That config is required for the timings to be stable and
 * for the print-route film memo to engage at all: spektra.cpp gates the memo with
 * `use_film_cache = !tap_bypass && !(halation_active || grain_active)` (:1120), so
 * with the shipped defaults (grain+halation ON) the memo is BYPASSED and scenario
 * 5's "film memo HIT" is unobservable. The scan route (run_scan_film) never
 * consults the memo, so its counters stay 0/0 by design.
 *
 * Scenarios (each: 1 warmup + N>=3 reps, median ms via steady_clock):
 *   1 cold scan_film default (full pipeline)      — fresh engine/rep, time simulate
 *   2 warm scan repeat identical params           — no scan-route memo today (~cold)
 *   3 warm scan output-only edit (output cs)       — no scan-route memo today (~cold)
 *   4 cold print default (full pipeline)          — fresh engine/rep, 1 film MISS
 *   5 warm print y_filter_shift edit               — downstream-only -> film memo HIT
 *   6 warm print output-only edit (output cs)      — downstream-only -> film memo HIT
 *
 * Counter columns: for COLD rows, the fresh engine's absolute counters after one
 * cold simulate (scan 0/0; print 0 hit / 1 miss). For WARM rows, the DELTA over
 * the measured block (1 warmup + N reps) — scan stays 0/0, print climbs on hits.
 * Honours SPK_NUM_THREADS (read by the engine's parallel_for) and reports the
 * active worker count. Optional env BENCH_REPS (>=3, default 3).
 *
 * Build (host) — full source set, run from the cpp root:
 *   g++ -std=c++17 -O2 -pthread -I. -I../../../../../tools/parity \
 *     tests/bench_stages.cpp spektra.cpp kernels/*.cpp io/*.cpp model/*.cpp \
 *     profiles/*.cpp runtime/*.cpp runtime/stages/*.cpp -o /tmp/bench_stages
 * Run:
 *   SPK_NUM_THREADS=8 /tmp/bench_stages [asset_dir]   # asset_dir default ../assets/spektra
 */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "spektra.h"
#include "kernels/parallel.h"  // spk::parallel_num_threads() — exact engine worker count

// Host-only film_density_cmy cache counters (spektra.cpp, #ifndef __ANDROID__).
// Forward-declared exactly as test_simulate_e2e.cpp does — no header / ABI change.
extern uint64_t spk_test_film_cache_hits(spk_engine* eng);
extern uint64_t spk_test_film_cache_misses(spk_engine* eng);

namespace {

using clock_t_ = std::chrono::steady_clock;

// Deterministic parity-style params (mirrors test_simulate_e2e make_p0), routed by
// scan_film (1 = scan negative directly, 0 = negative -> print -> scan).
spk_params base_params(int scan_film) {
    spk_params p{};
    p.film_profile  = "kodak_portra_400";
    p.print_profile = "kodak_portra_endura";
    spk_default_params(&p);  // physical defaults; deterministic overrides below.
    p.exposure_compensation_ev = 0.0f;
    p.auto_exposure = 0;
    p.density_curve_gamma = 1.0f;
    p.grain_active = 0;        // spatial + stochastic OFF: stable timing + memo active
    p.halation_active = 0;
    // Spatial effects are per-effect gated (zero = inert); express the
    // oracle's deactivate_spatial_effects by zeroing the nonzero defaults.
    p.dir_diffusion_size_um = 0.0f;
    p.scanner_unsharp[0] = 0.0f;
    p.scanner_unsharp[1] = 0.0f;
    p.dir_couplers_active = 1;
    p.glare_active = 0;
    p.scan_film = scan_film;
    p.output_color_space = SPK_CS_SRGB;
    p.output_cctf_encoding = 1;
    p.rgb_to_raw_method = SPK_RGB2RAW_HANATOS2025;
    p.preview_max_size = 640;
    return p;
}

// Deterministic synthetic gradient (linear ProPhoto), values in ~[0.02, 0.92]:
// R ramps with x, G ramps with y, B on the anti-diagonal. No external file dep.
std::vector<float> make_gradient(int w, int h) {
    std::vector<float> img(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t i = (static_cast<size_t>(y) * w + x) * 3;
            float u = (w > 1) ? static_cast<float>(x) / (w - 1) : 0.0f;
            float v = (h > 1) ? static_cast<float>(y) / (h - 1) : 0.0f;
            img[i + 0] = 0.02f + 0.90f * u;
            img[i + 1] = 0.02f + 0.90f * v;
            img[i + 2] = 0.02f + 0.90f * (1.0f - 0.5f * (u + v));
        }
    }
    return img;
}

double median(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

// Time `fn` (a single measured operation) reps times after one untimed warmup;
// return the median milliseconds.
template <typename F>
double warm_median_ms(int reps, F&& fn) {
    fn();  // warmup (untimed)
    std::vector<double> ts;
    ts.reserve(static_cast<size_t>(reps));
    for (int r = 0; r < reps; ++r) {
        auto t0 = clock_t_::now();
        fn();
        auto t1 = clock_t_::now();
        ts.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return median(ts);
}

// Cold timing: FRESH engine per rep, time ONLY the first (cold) simulate on it.
// Returns median ms; leaves the last rep's engine alive in *keep so the caller can
// read its counters (cold cache effect of one run) and then destroy it.
double cold_median_ms(const char* asset_dir, const spk_image* in,
                      const spk_params* p, int reps, spk_engine** keep) {
    *keep = nullptr;
    // warmup on a throwaway fresh engine
    {
        spk_engine* e = nullptr;
        if (spk_engine_create(asset_dir, &e) == SPK_OK) {
            spk_image o{};
            spk_simulate(e, in, p, &o);
            if (o.data) spk_image_free(&o);
            spk_engine_destroy(e);
        }
    }
    std::vector<double> ts;
    ts.reserve(static_cast<size_t>(reps));
    for (int r = 0; r < reps; ++r) {
        spk_engine* e = nullptr;
        if (spk_engine_create(asset_dir, &e) != SPK_OK) continue;
        spk_image o{};
        auto t0 = clock_t_::now();
        spk_simulate(e, in, p, &o);
        auto t1 = clock_t_::now();
        ts.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        if (o.data) spk_image_free(&o);
        if (r == reps - 1) {
            *keep = e;  // caller reads counters, then destroys
        } else {
            spk_engine_destroy(e);
        }
    }
    return median(ts);
}

struct Row {
    std::string label;
    double ms;
    long long hits;
    long long misses;
};

void print_table(const std::vector<Row>& rows) {
    std::printf("\n%-42s | %8s | %6s | %6s\n", "scenario", "ms", "hits", "misses");
    std::printf("-------------------------------------------+----------+--------+-------\n");
    for (const auto& r : rows) {
        std::printf("%-42s | %8.2f | %6lld | %6lld\n", r.label.c_str(), r.ms,
                    r.hits, r.misses);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* asset_dir = argc > 1 ? argv[1] : "../assets/spektra";

    int reps = 3;
    if (const char* env = std::getenv("BENCH_REPS")) {
        int n = std::atoi(env);
        if (n >= 3) reps = n;
    }

    const char* thr_env = std::getenv("SPK_NUM_THREADS");
    const int threads = spk::parallel_num_threads();

    const int W = 512, H = 512;
    std::vector<float> rgb = make_gradient(W, H);
    spk_image in{rgb.data(), W, H, static_cast<int>(SPK_CS_PROPHOTO)};

    // Validate the asset dir up front (fail loud rather than mis-timing).
    {
        spk_engine* probe = nullptr;
        if (spk_engine_create(asset_dir, &probe) != SPK_OK || !probe) {
            std::fprintf(stderr, "engine create failed (asset dir '%s')\n", asset_dir);
            return 2;
        }
        spk_engine_destroy(probe);
    }

    std::printf("bench_stages — Spektrafilm engine route micro-benchmark (LOCAL)\n");
    std::printf("image:   %dx%dx3 synthetic gradient (linear ProPhoto)\n", W, H);
    std::printf("threads: %d (SPK_NUM_THREADS=%s)\n", threads,
                thr_env ? thr_env : "unset");
    std::printf("reps:    %d timed + 1 warmup per scenario (median ms)\n", reps);
    std::printf("params:  parity-deterministic (grain/halation/glare/AE off);"
                " memo active on print route only\n");

    std::vector<Row> rows;

    // -- 1: cold scan (full pipeline) ----------------------------------------
    std::fprintf(stderr, "[1/6] cold scan...\n");
    {
        spk_params ps = base_params(1);
        spk_engine* keep = nullptr;
        double ms = cold_median_ms(asset_dir, &in, &ps, reps, &keep);
        long long h = keep ? (long long)spk_test_film_cache_hits(keep) : 0;
        long long m = keep ? (long long)spk_test_film_cache_misses(keep) : 0;
        if (keep) spk_engine_destroy(keep);
        rows.push_back({"1 cold scan default (full pipeline)", ms, h, m});
    }

    // -- 2 & 3: warm scan on a persistent engine -----------------------------
    std::fprintf(stderr, "[2/6] warm scan repeat...\n");
    {
        spk_engine* eng = nullptr;
        spk_engine_create(asset_dir, &eng);
        spk_params ps = base_params(1);
        // Warm tc_lut once so scenarios 2/3 measure the warm-engine cost.
        {
            spk_image o{};
            spk_simulate(eng, &in, &ps, &o);
            if (o.data) spk_image_free(&o);
        }

        // Scenario 2: repeat identical params.
        uint64_t h0 = spk_test_film_cache_hits(eng), m0 = spk_test_film_cache_misses(eng);
        double ms2 = warm_median_ms(reps, [&]() {
            spk_image o{};
            spk_simulate(eng, &in, &ps, &o);
            if (o.data) spk_image_free(&o);
        });
        long long h2 = (long long)(spk_test_film_cache_hits(eng) - h0);
        long long m2 = (long long)(spk_test_film_cache_misses(eng) - m0);
        rows.push_back({"2 warm scan repeat identical params", ms2, h2, m2});

        // Scenario 3: output-only edit (change output color space).
        std::fprintf(stderr, "[3/6] warm scan output-only...\n");
        spk_params p3 = base_params(1);
        p3.output_color_space = SPK_CS_ADOBE_RGB;
        uint64_t h1 = spk_test_film_cache_hits(eng), m1 = spk_test_film_cache_misses(eng);
        double ms3 = warm_median_ms(reps, [&]() {
            spk_image o{};
            spk_simulate(eng, &in, &p3, &o);
            if (o.data) spk_image_free(&o);
        });
        long long h3 = (long long)(spk_test_film_cache_hits(eng) - h1);
        long long m3 = (long long)(spk_test_film_cache_misses(eng) - m1);
        rows.push_back({"3 warm scan output-only edit (out cs)", ms3, h3, m3});

        spk_engine_destroy(eng);
    }

    // -- 4: cold print (full pipeline) ---------------------------------------
    std::fprintf(stderr, "[4/6] cold print...\n");
    {
        spk_params pp = base_params(0);
        spk_engine* keep = nullptr;
        double ms = cold_median_ms(asset_dir, &in, &pp, reps, &keep);
        long long h = keep ? (long long)spk_test_film_cache_hits(keep) : 0;
        long long m = keep ? (long long)spk_test_film_cache_misses(keep) : 0;
        if (keep) spk_engine_destroy(keep);
        rows.push_back({"4 cold print default (full pipeline)", ms, h, m});
    }

    // -- 5 & 6: warm print on a persistent engine (film memo) ----------------
    std::fprintf(stderr, "[5/6] warm print y_filter_shift...\n");
    {
        spk_engine* eng = nullptr;
        spk_engine_create(asset_dir, &eng);
        spk_params pbase = base_params(0);
        // Warm the film-cache slot with the base filming inputs (this is a MISS).
        {
            spk_image o{};
            spk_simulate(eng, &in, &pbase, &o);
            if (o.data) spk_image_free(&o);
        }

        // Scenario 5: enlarger y_filter_shift edit — downstream-only, film memo HIT.
        spk_params p5 = pbase;
        p5.y_filter_shift = 0.05f;
        uint64_t h0 = spk_test_film_cache_hits(eng), m0 = spk_test_film_cache_misses(eng);
        double ms5 = warm_median_ms(reps, [&]() {
            spk_image o{};
            spk_simulate(eng, &in, &p5, &o);
            if (o.data) spk_image_free(&o);
        });
        long long h5 = (long long)(spk_test_film_cache_hits(eng) - h0);
        long long m5 = (long long)(spk_test_film_cache_misses(eng) - m0);
        rows.push_back({"5 warm print y_filter_shift edit", ms5, h5, m5});

        // Scenario 6: output-only edit — downstream-only, film memo HIT (post-S2:
        // a further scan-only memo would skip print too). Cache still holds pbase's
        // filming (scenario 5 only HIT, never replaced the slot).
        std::fprintf(stderr, "[6/6] warm print output-only...\n");
        spk_params p6 = pbase;
        p6.output_color_space = SPK_CS_ADOBE_RGB;
        uint64_t h1 = spk_test_film_cache_hits(eng), m1 = spk_test_film_cache_misses(eng);
        double ms6 = warm_median_ms(reps, [&]() {
            spk_image o{};
            spk_simulate(eng, &in, &p6, &o);
            if (o.data) spk_image_free(&o);
        });
        long long h6 = (long long)(spk_test_film_cache_hits(eng) - h1);
        long long m6 = (long long)(spk_test_film_cache_misses(eng) - m1);
        rows.push_back({"6 warm print output-only edit (out cs)", ms6, h6, m6});

        spk_engine_destroy(eng);
    }

    print_table(rows);
    return 0;
}
