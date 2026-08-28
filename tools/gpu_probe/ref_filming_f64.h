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
 */
#ifndef SPK_TOOLS_GPU_PROBE_REF_FILMING_F64_H
#define SPK_TOOLS_GPU_PROBE_REF_FILMING_F64_H

#include <cstdint>

// f64 mirror of filming.comp (1:1 op order; fp32 tables/inputs only upcast).
// Compiled without fast-math (build script enforces -fno-fast-math).
void ref_filming_f64(const float* rgb, double* dens_out, uint32_t npix,
                     const float* tc_lut, int L,
                     const float* dev_axis, const float* dev_curve,
                     const float* dir_axis, const float* dir_curve, int n,
                     float exp_mult, float shift, const float* m9);

#endif  // SPK_TOOLS_GPU_PROBE_REF_FILMING_F64_H
