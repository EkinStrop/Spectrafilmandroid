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
 */
#ifndef SPK_TOOLS_GPU_PROBE_REF_PRINTING_F64_H
#define SPK_TOOLS_GPU_PROBE_REF_PRINTING_F64_H

#include <cstdint>

// f64 mirror of printing.comp (1:1 op order; fp32 tables/inputs only upcast).
// Compiled without fast-math (build script enforces -fno-fast-math).
void ref_printing_f64(const float* cmy, double* dens_out, uint32_t npix,
                      const float* dye, const float* isens,
                      const float* paper_axis, const float* paper_curve, int n,
                      float midgray, float mult, const float pf[3]);

#endif  // SPK_TOOLS_GPU_PROBE_REF_PRINTING_F64_H
