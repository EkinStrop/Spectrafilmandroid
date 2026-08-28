/*
 * Spektrafilm for Android — GPU device probe: printing f64 CPU reference. GPLv3.
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
 * against. Every line mirrors tools/gpu_probe/printing.comp — the fp32 tables
 * are only upcast, the 81-band loop and accumulation order, the midgray/
 * preflash/round-trip expression order and the interp bracket rule are
 * identical, in IEEE double. GLSL's NaN-undefined max is mirrored the fmax way
 * (picks the other operand on NaN).
 * --------------------------------------------------------------------------------
 */
#include "ref_printing_f64.h"

#include <cmath>

namespace {
constexpr int kNB = 81;  // == the shader's NB

inline double maxg(double a, double b) { return a > b ? a : b; }  // GLSL max, fmax-on-NaN

// fast_interp mirror over a channel-column (n,3) fp32 axis/curve, in double.
double interp_col(double x, const float* axis, const float* curve, int n, int c) {
    const double x0 = static_cast<double>(axis[c]);
    const double xN = static_cast<double>(axis[(n - 1) * 3 + c]);
    if (x <= x0) return static_cast<double>(curve[c]);
    if (x >= xN) return static_cast<double>(curve[(n - 1) * 3 + c]);
    int low = n - 2;
    for (int k = 1; k < n; ++k) {
        if (x < static_cast<double>(axis[k * 3 + c])) { low = k - 1; break; }
    }
    const double xl = static_cast<double>(axis[low * 3 + c]);
    const double xh = static_cast<double>(axis[(low + 1) * 3 + c]);
    const double dx = xh - xl;
    const double t = (dx != 0.0) ? (x - xl) / dx : 0.0;
    const double y0 = static_cast<double>(curve[low * 3 + c]);
    const double y1 = static_cast<double>(curve[(low + 1) * 3 + c]);
    return y0 + t * (y1 - y0);
}

}  // namespace

void ref_printing_f64(const float* cmy, double* dens_out, uint32_t npix,
                      const float* dye, const float* isens,
                      const float* paper_axis, const float* paper_curve, int n,
                      float midgray, float mult, const float pf[3]) {
    const double mg = static_cast<double>(midgray);
    const double mu = static_cast<double>(mult);
    const double p0 = static_cast<double>(pf[0]);
    const double p1 = static_cast<double>(pf[1]);
    const double p2 = static_cast<double>(pf[2]);

    for (uint32_t p = 0; p < npix; ++p) {
        const double c0 = static_cast<double>(cmy[p * 3 + 0]);
        const double c1 = static_cast<double>(cmy[p * 3 + 1]);
        const double c2 = static_cast<double>(cmy[p * 3 + 2]);

        double R0 = 0.0, R1 = 0.0, R2 = 0.0;
        for (int l = 0; l < kNB; ++l) {
            const double D = c0 * static_cast<double>(dye[l * 3 + 0]) +
                             c1 * static_cast<double>(dye[l * 3 + 1]) +
                             c2 * static_cast<double>(dye[l * 3 + 2]);
            const double T = std::pow(10.0, -D);
            R0 += T * static_cast<double>(isens[l * 3 + 0]);
            R1 += T * static_cast<double>(isens[l * 3 + 1]);
            R2 += T * static_cast<double>(isens[l * 3 + 2]);
        }
        R0 = R0 * mg + p0;
        R1 = R1 * mg + p1;
        R2 = R2 * mg + p2;

        double lr0 = std::log10(maxg(R0, 0.0) + 1e-10);
        double lr1 = std::log10(maxg(R1, 0.0) + 1e-10);
        double lr2 = std::log10(maxg(R2, 0.0) + 1e-10);
        const double r0 = std::pow(10.0, lr0) * mu;
        const double r1 = std::pow(10.0, lr1) * mu;
        const double r2 = std::pow(10.0, lr2) * mu;
        lr0 = std::log10(maxg(r0, 0.0) + 1e-10);
        lr1 = std::log10(maxg(r1, 0.0) + 1e-10);
        lr2 = std::log10(maxg(r2, 0.0) + 1e-10);

        dens_out[p * 3 + 0] = interp_col(lr0, paper_axis, paper_curve, n, 0);
        dens_out[p * 3 + 1] = interp_col(lr1, paper_axis, paper_curve, n, 1);
        dens_out[p * 3 + 2] = interp_col(lr2, paper_axis, paper_curve, n, 2);
    }
}
