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
 */
#ifndef SPK_TOOLS_GPU_PROBE_REF_SCAN_F64_H
#define SPK_TOOLS_GPU_PROBE_REF_SCAN_F64_H

#include <cstdint>

// Float64 CPU reference for gpu/scan_spectral.comp, mirroring the shader 1:1
// (same folded fp32 tables, same op order, pow(10,-D) band loop -> XYZ -> 3x3
// -> sRGB CCTF -> clamp) so a GPU-vs-reference diff isolates PRECISION, not
// algorithm. Compiled WITHOUT -ffast-math (see build_push_run.sh).
// cmy: npix*3 interleaved; rgb_out: npix*3 (double). Tables as in
// gpu/vulkan_compute.h: dye NB*3 band-major, icmf NB*3 band-major, m 9 row-major.
void ref_scan_f64(const float* cmy, double* rgb_out, uint32_t npix,
                  const float* dye, const float* icmf, const float* m);

#endif  // SPK_TOOLS_GPU_PROBE_REF_SCAN_F64_H
