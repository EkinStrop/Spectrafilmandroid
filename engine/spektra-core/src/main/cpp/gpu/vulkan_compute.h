/*
 * Spektrafilm for Android — GPU (Vulkan compute) fast-path. GPLv3.
 * Film modeling powered by spektrafilm.
 *
 * OPT-IN, OFF BY DEFAULT (CMake `SPK_ENABLE_VULKAN`, default OFF). This is the
 * foundation of the GPU offload described in docs/PERF_ROADMAP.md (#1, the real
 * Lightroom-class lever). It runs a per-element engine op on the GPU via a Vulkan
 * compute shader. Under the adopted proxy-approximate / export-exact policy it is a
 * PREVIEW-only acceleration: GPU float math is not bit-identical to the CPU/oracle
 * path, so the export + parity-gated path never call this.
 *
 * Build: when SPK_ENABLE_VULKAN is defined the engine links libvulkan; otherwise this
 * whole module is compiled out and the library is byte-identical to today. The CPU
 * fallback (spk::gpu::available() == false) is always correct.
 */
#ifndef SPK_GPU_VULKAN_COMPUTE_H
#define SPK_GPU_VULKAN_COMPUTE_H

#include <cstddef>
#include <cstdint>

namespace spk::gpu {

// True when SPK_ENABLE_VULKAN is compiled in AND a usable Vulkan device + compute
// queue were found at runtime. When false, callers must use the CPU path.
bool available();

// Apply the sRGB CCTF encode + clip to [0,1] to `data` (length `n` interleaved float
// RGB components) on the GPU, in place. Returns false if the GPU path is unavailable
// or any Vulkan call failed (caller then falls back to the CPU path). Never throws.
bool cctf_encode_srgb(float* data, size_t n);

// GPU 81-band spectral SCAN integral (the bottleneck-class kernel, preview-only):
// density_cmy[npix*3] -> output RGB[npix*3], via per-pixel spectral transmittance
// (10^-D over 81 bands) -> XYZ -> output RGB + sRGB CCTF. Spectral tables:
//   dye     : NB*3 per-channel dye densities D_c(lambda)  (band-major c,m,y)
//   icmf    : NB*3 illuminant-premultiplied CMFs          (band-major X,Y,Z)
//   xyz2rgb : 9 floats, row-major 3x3 XYZ->output-RGB matrix (pass Mc.M composed
//             to mirror the engine's full linear chain — see the PR #145 probe)
// Returns false if the GPU path is unavailable or any Vulkan call failed (caller
// falls back to the CPU scan). NOT bit-exact vs the f64 oracle -> preview only;
// the export + parity-gated path never call this (the #149 law revision opens
// oracle-verified GPU export as future M4 work). MEASURED ON DEVICE (PR #145,
// docs/research/gpu-device-probe.md): worst-case max_abs 2.15e-06 / rms 7.07e-08
// vs the f64 chain — 46x/141x inside the oracle tolerance — and byte-identical
// across repeated dispatches (Adreno 840, driver 512.842.19). The host guards
// non-finite densities at upload (NaN/Inf -> 1e4f -> black), so shader NaN
// behaviour never decides pixels.
bool scan_spectral(const float* cmy, float* rgb, uint32_t npix,
                   const float* dye, const float* icmf, const float* xyz2rgb);

// LINEAR variant (GPU M1, #146): the same 81-band integral, but the output is
// UNCLIPPED linear output-space RGB — no CAT02 fold, no CCTF, no clamp — so the
// CPU plane ops (unsharp / lens blur / gamut compression) and the standard
// encode tail run on it unchanged. `xyz2rgb` is the frame's plain XYZ->RGB
// matrix (Mc stays in the CPU encode). Same tables, same fallback contract,
// same preview-only law as scan_spectral.
bool scan_spectral_linear(const float* cmy, float* rgb, uint32_t npix,
                          const float* dye, const float* icmf, const float* xyz2rgb);

}  // namespace spk::gpu

#endif  // SPK_GPU_VULKAN_COMPUTE_H
