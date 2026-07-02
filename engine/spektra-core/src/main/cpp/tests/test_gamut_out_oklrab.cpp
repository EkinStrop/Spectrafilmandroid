/*
 * Spektrafilm for Android — host parity test for the Oklrab output gamut compression
 * (model/gamut_compression.cpp::compress_rgb_oklrab_chroma).
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Port of spektrafilm (GPLv3) by Andrea Volpato — film modeling powered by spektrafilm.
 *
 * The oracle's Oklrab chroma reduction (utils/gamut_compression.py::
 * compress_rgb_oklrab_chroma, dispatched by compress_rgb with algorithm=="oklrab") IS the
 * spec for the native port. It is the OkLch reduction with the per-pixel C_max lookup
 * indexed by Ottosson's rebased lightness Lr instead of raw OkLab L (and the C_max table
 * built over an Lr grid). This checks the C++ port against tests/gamut_oklrab_cases.bin
 * — 48 in/out-of-gamut linear-RGB pixels across ALL SIX engine output color spaces and
 * four knee triples, each output captured directly from the oracle
 * (tools/parity/gen_gamut_oklrab_golden.py, lightness_compression pinned to (0.7,1.0,2.2)).
 * The port is the same double-precision pipeline (per-space RGB<->XYZ, OkLab cube-root,
 * the 64x720 C_max(Lr,h) bisection table, the shared reinhard_knee, the Lr remap), so with
 * bit-identical constants + bisection the match is essentially exact (well inside parity
 * tolerance). This gates ONLY the opt-in kOklrab path; the engine default (kLegacyClip)
 * applies no compression and stays byte-identical.
 *
 * Binary format (little-endian): int32 num_cases; per case int32 space_index, f64
 * threshold, f64 limit, f64 power, int32 npix, f64 rgb_in[npix*3], f64 expected[npix*3].
 * (lightness_compression is constant (0.7,1.0,2.2) for every case — not stored.)
 *
 * Build (host): full source set per tests/README, e.g.
 *   g++ -std=c++17 -O2 -pthread -I. -I <tools/parity> -DSPK_TEST_DIR=... \
 *     tests/test_gamut_out_oklrab.cpp spektra.cpp kernels/*.cpp io/*.cpp \
 *     model/*.cpp profiles/*.cpp runtime/*.cpp runtime/stages/*.cpp -o /tmp/test_gamut_out_oklrab
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "model/gamut_compression.h"

#ifndef SPK_TEST_DIR
#define SPK_TEST_DIR "."
#endif

namespace {

template <typename T>
bool read_n(std::ifstream& in, T* dst, size_t count) {
    in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(count * sizeof(T)));
    return static_cast<size_t>(in.gcount()) == count * sizeof(T);
}

int g_fail = 0;
void chk(bool c, const char* m) {
    std::printf("  [%s] %s\n", c ? "ok" : "FAIL", m);
    if (!c) g_fail = 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path =
        argc > 1 ? argv[1] : (std::string(SPK_TEST_DIR) + "/gamut_oklrab_cases.bin");
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 2; }

    int32_t num_cases = 0;
    if (!read_n(in, &num_cases, 1) || num_cases <= 0) {
        std::fprintf(stderr, "bad case count\n"); return 2;
    }

    double max_abs = 0.0;
    int worst_case = -1;
    size_t total_pixels = 0;
    for (int c = 0; c < num_cases; ++c) {
        int32_t space = 0;
        double t = 0.0, l = 0.0, p = 0.0;
        int32_t npix = 0;
        if (!read_n(in, &space, 1) || !read_n(in, &t, 1) || !read_n(in, &l, 1) ||
            !read_n(in, &p, 1) || !read_n(in, &npix, 1) || npix < 1 ||
            space < 0 || space > 5) {
            std::fprintf(stderr, "truncated/bad header at case %d\n", c); return 2;
        }
        std::vector<double> rgb(static_cast<size_t>(npix) * 3);
        std::vector<double> expected(static_cast<size_t>(npix) * 3);
        if (!read_n(in, rgb.data(), rgb.size()) ||
            !read_n(in, expected.data(), expected.size())) {
            std::fprintf(stderr, "truncated payload at case %d\n", c); return 2;
        }
        // Build the table once (inside the wrapper) and compress in place, exactly as
        // the scanning hook does, then compare against the oracle output.
        spk::compress_rgb_oklrab_chroma(rgb.data(), npix, space, t, l, p);
        for (size_t i = 0; i < rgb.size(); ++i) {
            double d = std::fabs(rgb[i] - expected[i]);
            if (d > max_abs) { max_abs = d; worst_case = c; }
        }
        total_pixels += static_cast<size_t>(npix);
    }

    // Same double-precision pipeline + bisection as the oracle -> essentially exact.
    // Parity budget is max_abs <= 1e-4; a bit-identical build lands ~1e-12 or better.
    const double tol = 1e-9;
    chk(max_abs <= tol, "compress_rgb_oklrab_chroma matches the oracle golden");
    std::printf("    %d cases, %zu pixels -> max_abs=%.3e (tol %.0e) worst case=%d\n",
                num_cases, total_pixels, max_abs, tol, worst_case);

    // Independent hand-captured probes (utils/gamut_compression.py at knee (0,1,6),
    // lightness_compression (0.7,1.0,2.2)) across every space, incl. black (anchored to
    // 0) and OOG pixels that reconstruct slightly outside the cube (no clip here — the
    // scanning stage clips afterwards). space: 0 sRGB, 1 Adobe, 2 ProPhoto, 3 Rec2020,
    // 4 ACES2065-1, 5 linear-sRGB (== 0). Saturated hues differ from the oklch probes —
    // the Lr-indexed C_max lookup shifts the boundary versus the raw-L index.
    {
        struct { int space; double in[3]; double want[3]; } probes[] = {
            {0, {0.5, 0.5, 0.5},
                {0.49413690624150763, 0.49415674201235837, 0.49413931146031415}},
            {0, {1.2, -0.1, 0.3},
                {0.91164885658270678, 0.0072221151329287915, 0.28894699678692776}},
            {0, {1.0, 0.0, 0.0},
                {0.89983107609368385, 0.033880438411178201, 0.019946307611340454}},
            {0, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},
            {1, {0.0, 1.1, -0.2},
                {0.19604220104151379, 0.89684650188490711, 0.0036343509715366594}},
            {2, {0.3, 0.6, 0.2},
                {0.29818522986790619, 0.59448262540439267, 0.19993170424821297}},
            {2, {1.5, 1.5, 1.5},
                {0.87699988124989126, 0.86776184355388308, 0.81926483631452673}},
            {3, {1.3, 0.0, 1.2},
                {0.99310825246244139, 0.19112821545540104, 0.92273499698479811}},
            {4, {0.4, 0.4, 0.4},
                {0.39972976485304645, 0.39972872005128762, 0.39972255084815345}},
            {5, {2.0, 0.1, -0.5},
                {0.67576227604858208, 0.52126172730834608, -1.0553961265624013e-06}},
        };
        double probe_max = 0.0;
        for (auto& pr : probes) {
            double buf[3] = {pr.in[0], pr.in[1], pr.in[2]};
            spk::compress_rgb_oklrab_chroma(buf, 1, pr.space, 0.0, 1.0, 6.0);
            for (int ch = 0; ch < 3; ++ch)
                probe_max = std::fmax(probe_max, std::fabs(buf[ch] - pr.want[ch]));
        }
        chk(probe_max <= 1e-9, "compress_rgb_oklrab_chroma matches the oracle probes");
        std::printf("    probe max_abs=%.3e\n", probe_max);
    }

    std::printf("%s\n", g_fail == 0 ? "ALL PASS" : "FAIL");
    return g_fail;
}
