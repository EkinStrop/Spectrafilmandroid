# GPU device probe (#135 E3 → #127)

Standalone arm64 probe that measures the fp32 Vulkan scan integral
(`engine/.../gpu/scan_spectral.comp` via the **unmodified** `spk::gpu::scan_spectral`
host) against an f64 CPU reference mirroring the shader 1:1, on real device
hardware. Produces the "does fp32 sit inside the oracle tolerance
(`max_abs ≤ 1e-4`, `rms ≤ 1e-5`)" number that `docs/research/gpu-bit-exact.md`
§10.3 calls the decider for #127 / option B. **This wires nothing into the app;
the GPU-preview-only law is untouched either way.**

Run (phone on USB, NDK r27 installed):

```bash
bash tools/gpu_probe/build_push_run.sh
```

- Tier 0 `caps`: device identity, `shaderFloat64/16`, the full float-controls block.
- Tier 1 `run`: golden density plane + 64³ CMY sweep + NaN case; `max_abs`/`rms`
  vs f64, determinism ×5 (byte-compare).
- Tier 2 `perf`: warm-call wall time at 0.3 MP / 12 MP (includes the host's
  per-call buffer+pipeline rebuild — offload cost, not pure kernel time).
- Tier 3 (auto, needs glslc): `precise` (NoContraction) and `mediump`
  (RelaxedPrecision) shader variants, same Tier 1 run.

Tables are extracted through the engine's own loaders/constants and folded per
the `gpu/vulkan_compute.h` contract (base density + illuminant + normalization
into `icmf`; NaN bands zeroed — the engine's w=NaN→0 semantics). Results land in
`tools/gpu_probe/captures/` (untracked); the committed writeup is
`docs/research/gpu-device-probe.md`.

Film modeling powered by spektrafilm (GPLv3).
