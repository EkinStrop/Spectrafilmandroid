/*
 * Spektrafilm for Android — GPU device probe: filming f64 CPU reference. GPLv3.
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
 * against. Every line mirrors tools/gpu_probe/filming.comp — the fp32 tables and
 * fp32 input are only upcast; the matrix constants are the shader's fp32-rounded
 * constants; band/loop/accumulation order, the Mitchell cubic, the interp
 * bracket rule and the final expression order are identical, in IEEE double.
 * (log10/pow are libm's exact forms where the shader synthesizes log2/exp2 —
 * that difference is fp32 implementation precision, which is what is measured.)
 * GLSL's NaN-undefined max/clamp are mirrored the fmax way: max picks the other
 * operand on NaN, clamp lets NaN fall through; the shader's bounded cubic base
 * index is mirrored with an explicit isnan pin (output NaN either way).
 * --------------------------------------------------------------------------------
 */
#include "ref_filming_f64.h"

#include <cmath>

namespace {

// The shader's fp32-rounded ProPhoto->XYZ(D55, CAT02) constants (engine
// kProPhotoToXyzD55 cast to float by the shader compiler).
const float kM_f32[9] = {
    0.7815775876144749f,   0.12427353211547089f,  0.05084064074531416f,
    0.28106991658512925f,  0.7111246050020191f,   0.0078043503519031375f,
    0.0008785229438793953f, 0.0012166783269637077f, 0.9190442562432091f};

inline double maxg(double a, double b) { return a > b ? a : b; }  // GLSL max, fmax-on-NaN
inline double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;  // NaN falls through
}

double mitchell(double t) {
    const double B = 1.0 / 3.0;
    const double C = 1.0 / 3.0;
    double x = std::fabs(t);
    if (x < 1.0)
        return (1.0 / 6.0) * ((12.0 - 9.0 * B - 6.0 * C) * x * x * x +
                              (-18.0 + 12.0 * B + 6.0 * C) * x * x + (6.0 - 2.0 * B));
    if (x < 2.0)
        return (1.0 / 6.0) * ((-B - 6.0 * C) * x * x * x + (6.0 * B + 30.0 * C) * x * x +
                              (-12.0 * B - 48.0 * C) * x + (8.0 * B + 24.0 * C));
    return 0.0;
}

int safe_index(int idx, int L) {
    if (idx < 0) return -idx;
    if (idx >= L) return 2 * (L - 1) - idx;
    return idx;
}

double cubic_bf(double coord, int L, int* base) {
    if (coord <= 0.0) coord = 0.0;
    else if (coord >= static_cast<double>(L - 1)) coord = static_cast<double>(L - 1);
    if (coord >= static_cast<double>(L - 1)) { *base = L - 2; return 1.0; }
    // Shader: base = clamp(int(floor(coord)), 0, L-2); NaN pinned explicitly here
    // (int cast of NaN is undefined in C++) — frac is NaN either way.
    int b = std::isnan(coord) ? 0 : static_cast<int>(std::floor(coord));
    if (b < 0) b = 0;
    if (b > L - 2) b = L - 2;
    *base = b;
    return coord - static_cast<double>(b);
}

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

void ref_filming_f64(const float* rgb, double* dens_out, uint32_t npix,
                     const float* tc_lut, int L,
                     const float* dev_axis, const float* dev_curve,
                     const float* dir_axis, const float* dir_curve, int n,
                     float exp_mult, float shift, const float* m9) {
    const double em = static_cast<double>(exp_mult);
    const double sh = static_cast<double>(shift);
    double M[9];
    for (int i = 0; i < 9; ++i) M[i] = static_cast<double>(m9[i]);

    for (uint32_t p = 0; p < npix; ++p) {
        const double r = static_cast<double>(rgb[p * 3 + 0]);
        const double g = static_cast<double>(rgb[p * 3 + 1]);
        const double bch = static_cast<double>(rgb[p * 3 + 2]);

        const double X = static_cast<double>(kM_f32[0]) * r + static_cast<double>(kM_f32[1]) * g +
                         static_cast<double>(kM_f32[2]) * bch;
        const double Y = static_cast<double>(kM_f32[3]) * r + static_cast<double>(kM_f32[4]) * g +
                         static_cast<double>(kM_f32[5]) * bch;
        const double Z = static_cast<double>(kM_f32[6]) * r + static_cast<double>(kM_f32[7]) * g +
                         static_cast<double>(kM_f32[8]) * bch;
        const double bsum = X + Y + Z;
        const double denom = maxg(bsum, 1e-10);
        const double cx = clamp01(X / denom);
        const double cy = clamp01(Y / denom);
        const double qy = clamp01(cy / maxg(1.0 - cx, 1e-10));
        const double qx = clamp01((1.0 - cx) * (1.0 - cx));
        const double bmul = std::isnan(bsum) ? 0.0 : bsum;

        const double scale = static_cast<double>(L - 1);
        int xb, yb;
        const double xf = cubic_bf(qx * scale, L, &xb);
        const double yf = cubic_bf(qy * scale, L, &yb);
        const double wx[4] = {mitchell(xf + 1.0), mitchell(xf), mitchell(xf - 1.0),
                              mitchell(xf - 2.0)};
        const double wy[4] = {mitchell(yf + 1.0), mitchell(yf), mitchell(yf - 1.0),
                              mitchell(yf - 2.0)};
        double acc0 = 0.0, acc1 = 0.0, acc2 = 0.0, wsum = 0.0;
        for (int i = 0; i < 4; ++i) {
            const int xi = safe_index(xb - 1 + i, L);
            for (int j = 0; j < 4; ++j) {
                const int yj = safe_index(yb - 1 + j, L);
                const double w = wx[i] * wy[j];
                wsum += w;
                const int cell = (xi * L + yj) * 3;
                acc0 += w * static_cast<double>(tc_lut[cell + 0]);
                acc1 += w * static_cast<double>(tc_lut[cell + 1]);
                acc2 += w * static_cast<double>(tc_lut[cell + 2]);
            }
        }
        if (wsum != 0.0) { acc0 /= wsum; acc1 /= wsum; acc2 /= wsum; }

        const double lr0 = std::log10(maxg(acc0 * bmul * em, 0.0) + 1e-10);
        const double lr1 = std::log10(maxg(acc1 * bmul * em, 0.0) + 1e-10);
        const double lr2 = std::log10(maxg(acc2 * bmul * em, 0.0) + 1e-10);

        const double d0 = interp_col(lr0, dev_axis, dev_curve, n, 0);
        const double d1 = interp_col(lr1, dev_axis, dev_curve, n, 1);
        const double d2 = interp_col(lr2, dev_axis, dev_curve, n, 2);

        const double s0 = d0 + sh * d0 * d0;
        const double s1 = d1 + sh * d1 * d1;
        const double s2 = d2 + sh * d2 * d2;
        dens_out[p * 3 + 0] =
            interp_col(lr0 - (s0 * M[0] + s1 * M[3] + s2 * M[6]), dir_axis, dir_curve, n, 0);
        dens_out[p * 3 + 1] =
            interp_col(lr1 - (s0 * M[1] + s1 * M[4] + s2 * M[7]), dir_axis, dir_curve, n, 1);
        dens_out[p * 3 + 2] =
            interp_col(lr2 - (s0 * M[2] + s1 * M[5] + s2 * M[8]), dir_axis, dir_curve, n, 2);
    }
}
