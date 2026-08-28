# On-device GPU numeric probe — the #135 E3 measurement (feeds #127)

**Question** ([gpu-bit-exact.md](gpu-bit-exact.md) §10.3, the decider for option B): on real
device hardware, does the fp32 GPU scan integral sit inside the engine's oracle tolerance
(`max_abs ≤ 1e-4`, `rms ≤ 1e-5`), and is it same-device deterministic?

**Answer: YES — worst case `max_abs = 2.15e-06` (46× inside the bar), `rms ≈ 7.1e-08`
(141× inside), byte-identical outputs across every repeated dispatch.** And not just
against the shader's own math: with the engine's CAT02 round-trip matrix composed in
(`sweep_mc` below), the GPU is within **≈2.3e-06 of the engine's full linear chain** —
the fp32 error is three orders of magnitude below the tolerance either way.

*Method: `tools/gpu_probe/` — a standalone arm64 NDK executable (no app change, engine
sources byte-untouched) that runs the **unmodified** `spk::gpu::scan_spectral` host +
vendored SPIR-V (`gpu/scan_spectral.comp`) and compares against an f64 CPU reference
compiled without fast-math that mirrors the shader 1:1 (same folded fp32 tables, same op
order — verified down to the SPIR-V disassembly), so the diff isolates **precision**, not
algorithm. Tables extracted through the engine's own loaders/constants
(`profiles/profile.cpp`, `model/color_output`, `model/spectral`) and folded per the
`gpu/vulkan_compute.h` contract; NaN bands zeroed (the engine's `w = NaN → 0` semantics —
20 of 81 Portra bands). An independent adversarial review (3 lenses: table-fold,
reference-fidelity, methodology) confirmed the fold is equivalent to
`runtime/stages/scanning.cpp`'s direct path to ≤1.02e-7 in f64, surfaced the one material
gap — the shader omits the engine's `kRGB_to_RGB_CCTF` (Mc) matrix, whose off-diagonals
matter ~1e-4 near black — and that gap is closed below by the `_mc` cases (Mc·M composed
into the push-constant matrix, an exact linear composition needing no shader change).
Captured 2026-08-27 on the device below, repo commit `bb6c9db`, profile
`kodak_portra_400`, scan route.*

## Device

Samsung SM-S948W (Galaxy S26 Ultra), Android 16, SoC SM8850 — **Adreno (TM) 840**,
Vulkan **1.4.295**, driver **512.842.19** (raw `0x8034a013`), driverID 8
(`VK_DRIVER_ID_QUALCOMM_PROPRIETARY`), build 87ff20b216 / compiler E031.50.19.18
(2026-03-26). Subgroup size 64. Timestamps supported (period 52.08 ns).

## Tier 1 — fp32 GPU vs f64 CPU reference

| case | npix | max_abs | rms | det ×5 | notes |
|---|---:|---:|---:|---|---|
| golden (`scan_portra` density plane) | 4,096 | **4.74e-07** | **5.98e-08** | IDENTICAL | worst px cmy=(0.125, 1.057, 0.661), comp G |
| golden_mc (matrix = Mc·M, engine chain) | 4,096 | **4.70e-07** | **5.95e-08** | IDENTICAL | composing the CAT02 round-trip costs nothing in precision |
| sweep (64³ lattice, −0.1..nanmax(density_curves) per ch) | 262,144 | **2.15e-06** | **7.07e-08** | IDENTICAL | worst px at the negative-density edge, cmy=(−0.1, 0.957, 0.005), comp G |
| sweep_mc (same lattice, matrix = Mc·M) | 262,144 | **2.11e-06** | **7.07e-08** | IDENTICAL | same worst pixel |
| NaN density | 3 | — | — | — | GPU emits **(0,0,0)** for NaN inputs (Adreno's `clamp(NaN,0,1)` → 0); engine semantics for NaN density = black — behaviourally aligned **on this driver**, but GLSL leaves `clamp(NaN)` undefined, so any future GPU-export path needs an explicit NaN guard, not driver luck |

Tolerance bar: `max_abs ≤ 1e-4`, `rms ≤ 1e-5`. The worst case is **46×** inside on
`max_abs`, **141×** on `rms`. Errors concentrate in dark output values where the sRGB
CCTF slope is steep — exactly where fp32 `pow(10,-D)` + the synthesized
`exp2(y·log2(x))` ULP bounds land. The sweep's lower bound covers the engine's
negative-density scan domain (`-grain_density_min`).

**GPU-vs-engine chain**: the review measured the f64 mirror (with Mc composed) against
the engine's actual `scanning.cpp` semantics at ≤1.02e-7 (the residual is the disclosed
`log10/pow10` 1e-10 floor, ~1.6e-9, plus fp32 table quantization — the shader's declared
contract). Chained with `sweep_mc`, the GPU result is **≤ ≈2.3e-06 from the CPU engine's
default output path** (BW/glare corrections off, as in the goldens).

**Determinism**: 5 identical dispatches byte-compare equal on every case, rerun buffers
poisoned beforehand (Vulkan Invariance Rule 7 verified, not assumed); the five 12 MP
perf runs hash identically too.

## Tier 0 — float-controls facts (answers two #135 open questions)

- `shaderFloat64` = **false** — confirms the fleet expectation on Adreno 8xx stock
  Samsung driver 512.842.19; the 512.863.x fp64=true reports in the #135 research do
  not apply to this branch.
- `shaderFloat16` = true, `storageBuffer16BitAccess` = true
  (`uniformAndStorageBuffer16BitAccess` = false).
- fp32: `shaderRoundingModeRTEFloat32` = **true**, `signedZeroInfNanPreserveFloat32` =
  **true**, `denormFlushToZeroFloat32` = true (denormPreserve false) — the driver
  *advertises* the float_controls facilities the E2/option-C route would need.
- fp16: RTE true, NaN/Inf preserve true, denormPreserve true.
- fp64: all controls false (consistent with no fp64 at all).
- Independence: denorm and rounding-mode both `..._INDEPENDENCE_ALL`.

## Tier 2 — perf sanity (preview-decision context only)

| size | warm-call median | throughput |
|---|---:|---:|
| 0.3 MP (640×480) | 48.3 ms (24.8 ms in an earlier, cooler run) | 6–12 MPix/s |
| 12 MP (4000×3000) | 158.3 ms (101.5 ms earlier run) | 76–118 MPix/s |

Warm-call wall time of the **current host as-is**, which re-creates buffers + pipeline
and round-trips host-visible memory every call — honest *offload* cost, not kernel time
(the 0.3 MP call is almost entirely fixed overhead: ~48 ms vs ~158 ms for 39× the
pixels). Run-to-run spread across sessions is thermal/clock state; within a session the
5-run spread was ≤20%. A persistent-pipeline host would cut the small sizes hard. No
export implications — GPU stays preview-only.

## Tier 3 — precision brackets (same Tier 1 run, recompiled shader)

| variant | sweep max_abs | verdict |
|---|---:|---|
| `precise` (NoContraction on the band accumulators; 12 decorations verified in the SPIR-V) | 2.15e-06 | **outputs byte-identical to the default compile** — the driver's default codegen for this kernel already matches the NoContraction result |
| `mediump` (RelaxedPrecision everywhere — 99 decorations; driver evaluates fp16) | 1.24e-02 | **~124× OUTSIDE tolerance** — fp16 arithmetic (vkdt's floor) fails the oracle regime, as predicted; fine for a proxy preview, unusable for oracle-verified work |

## What this means

1. **The E3 bar is met on this device, against the engine's own chain**: fp32 GPU scan
   is oracle-tolerance-accurate with ~50× margin and same-device deterministic. Per
   [gpu-bit-exact.md](gpu-bit-exact.md) §10.3 / option B, **GPU export
   ("oracle-verified on your device") is now a legitimate owner decision** — the
   standing law (GPU preview-only) is untouched until the owner makes it; this probe
   wires nothing into the app.
2. For the **#127 preview route**, fp32 Vulkan compute is numerically over-qualified —
   even fp16 (1e-2-class error) is visually plausible for a 640 px proxy, and the fp32
   kernel moves ~100 MPix/s through a deliberately naive per-call host.
3. If the scan kernel ever feeds anything engine-facing, push **Mc·M** as the matrix
   (exact composition, no shader change) — the raw `kXYZ_to_RGB` alone differs from the
   engine's default output path by up to ~1.5e-4 near black (the Mc off-diagonals ×
   the 12.92 CCTF slope), which *would* breach the tolerance.
4. Caveats bounding the claim: one device, one driver (512.842.19), one kernel (the
   scan integral — printing/filming integrals are the same op class but unmeasured),
   BW/glare corrections and the spatial branch not in scope, and GPU NaN handling is
   driver behaviour, not spec — guard it explicitly before any export-path use.

*Probe: `tools/gpu_probe/` (`build_push_run.sh` reproduces everything; raw captures in
`tools/gpu_probe/captures/`, untracked). Research for
[#127](https://github.com/thetechgeekko/Spektrafilm-android/issues/127) /
[#135](https://github.com/thetechgeekko/Spektrafilm-android/issues/135), part of map
[#117](https://github.com/thetechgeekko/Spektrafilm-android/issues/117). Film modeling
powered by spektrafilm (GPLv3).*
