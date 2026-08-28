/*
 * Spektrafilm for Android — GPU device probe: f64 CPU reference. GPLv3.
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
 * This translation unit MUST be compiled without -ffast-math (the build script
 * enforces it): it is the precision yardstick the fp32 GPU result is measured
 * against. Every line mirrors gpu/scan_spectral.comp — the fp32 tables are only
 * upcast, the band-loop order, the accumulation order, the 3x3 product, the sRGB
 * CCTF and the final clamp are identical, in IEEE double.
 * --------------------------------------------------------------------------------
 */
#include "ref_scan_f64.h"

#include <cmath>

namespace {
constexpr int kNB = 81;  // == the shader's NB

inline double cctf_srgb(double v) {
    // Same branch + constants as the shader's cctf().
    return (v <= 0.0031308) ? (12.92 * v) : (1.055 * std::pow(v, 1.0 / 2.4) - 0.055);
}

inline double clamp01(double v) {
    // GLSL clamp(x,0,1): min(max(x,0),1). NaN behaviour is undefined in GLSL;
    // here NaN propagates (fmin/fmax semantics would eat it — use comparisons
    // that keep NaN, mirroring "undefined" honestly as NaN in the reference).
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;  // NaN falls through
}
}  // namespace

void ref_scan_f64(const float* cmy, double* rgb_out, uint32_t npix,
                  const float* dye, const float* icmf, const float* m) {
    for (uint32_t p = 0; p < npix; ++p) {
        const double c = static_cast<double>(cmy[p * 3 + 0]);
        const double mm = static_cast<double>(cmy[p * 3 + 1]);
        const double y = static_cast<double>(cmy[p * 3 + 2]);
        double X = 0.0, Y = 0.0, Z = 0.0;
        for (int b = 0; b < kNB; ++b) {
            const double D = c * static_cast<double>(dye[b * 3 + 0]) +
                             mm * static_cast<double>(dye[b * 3 + 1]) +
                             y * static_cast<double>(dye[b * 3 + 2]);
            const double T = std::pow(10.0, -D);
            X += T * static_cast<double>(icmf[b * 3 + 0]);
            Y += T * static_cast<double>(icmf[b * 3 + 1]);
            Z += T * static_cast<double>(icmf[b * 3 + 2]);
        }
        const double r = static_cast<double>(m[0]) * X + static_cast<double>(m[1]) * Y +
                         static_cast<double>(m[2]) * Z;
        const double g = static_cast<double>(m[3]) * X + static_cast<double>(m[4]) * Y +
                         static_cast<double>(m[5]) * Z;
        const double b2 = static_cast<double>(m[6]) * X + static_cast<double>(m[7]) * Y +
                          static_cast<double>(m[8]) * Z;
        rgb_out[p * 3 + 0] = clamp01(cctf_srgb(r));
        rgb_out[p * 3 + 1] = clamp01(cctf_srgb(g));
        rgb_out[p * 3 + 2] = clamp01(cctf_srgb(b2));
    }
}
