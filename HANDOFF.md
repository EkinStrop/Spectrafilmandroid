# Spektrafilm Android — Session Handoff

## ▶ NEXT SESSION — START HERE (written 2026-07-02, post-#109-merge)

**PR #109 is MERGED to `main` (`e0c3736`) and its branch was deleted.** The local
`claude/exciting-hamilton-hya62` was restarted from `origin/main` (same name, fresh history —
per protocol the next PR is a NEW PR; never stack on merged history). This handoff commit is
the branch's first commit.

**The user chose the next task: P2 #6 — perceptual output-gamut compression algos**
(`cam16ucs` / `oklch` / `oklrab` / `jzazbz`, default-OFF, effort XL). Plan of record:
- Hook: the existing `output_gamut_compress` path — `model/gamut_compression.{h,cpp}`
  (`OutputGamutCompress` has RESERVED enum slots; `kLegacyClip` default, `kAcesRgc` shipped),
  `scanning.cpp` gated call site, JNI `enum_ordinal_int` marshal,
  `SpektraParams.IoParams.OutputGamutCompress`, dropdown under Simulation→Output. Adding an
  algo = new enum value + new compress function + extend the same dropdown; recipes
  round-trip by ordinal (old recipes → default, unchanged look).
- Oracle: `spektrafilm/model/gamut_compression.py` (`compress_rgb` + colour-science
  color-appearance conversions). The XL meat is porting CAM16-UCS / OKLCH / OKLrAB / JzAzBz
  in float64 bit-matching colour-science. Gate the PRIMITIVES at function level (the pattern
  #3/#5 used — one golden per algo, gen script under `tools/parity/`, oracle loaded via the
  matplotlib-shim like `gen_gamut_in_golden.py`; gamut goldens were generated at oracle HEAD
  `27bd085`, e2e param-wiring goldens stay pinned at `c1d0e44` — check `git -C
  /home/user/spektrafilm log` and pin whatever SHA you generate at, in the gen script).
- Default path must stay byte-identical: new algos are opt-in; the 33-gate suite must stay
  green untouched; add the new gate(s) to ci.yml + CLAUDE.md/skill counts.
- Verify per protocol: full suite (argv from ci.yml), `SPK_NUM_THREADS` 1≡8, NDK build,
  commit+push each increment. Oracle env: see Context notes below (PYTHONPATH + /tmp/spkstubs).

Everything below is the completed prior pass, kept for reference.

---

## State (2026-07-02, branch `claude/exciting-hamilton-hya62`) — PM "exact + fast" pass COMPLETE (PR #109, MERGED)

The PM pass (*"we need spektrafilm exact result with ultra fast speed"*) is **fully landed and
pushed**: Wave 1 (F1–F7 Kotlin fixes), Wave 2a (E1 spatial decouple, E2 print-route
spatial+grain), Wave 2b (S1 scan-route film memo, S2 print-density memo), Wave 2c (S4 loop
parallelization), Wave 3 (S3 Kotlin grade cache), Wave 4 (docs truth-sync). Verification at
HEAD: host parity suite **33/33 green** (argv replayed from ci.yml, fail=0),
`SPK_NUM_THREADS` 1≡8 byte-identical on all four `test_parallel` routes, NDK r27
`externalNativeBuildDebug` green (3 ABIs) after each engine commit.

### Commits (oldest→newest; F/E/S1/S3 from the salvage session, S2/S4/docs from this one)
- `894cdfc` F1p2+F2+F7 (serialize-on-main IO, DIR-gamma + Glare disclosures), `a4d0649` F3
  (closed-engine guards), `b7b52fe` F4 (GPU-LUT GL re-arm), `921f9a9` F5 (RawCoilDecoder
  free), `b2c4c53` F6 (RotationTest)
- `992e044` **E1** per-effect spatial gates (oracle semantics) + `test_spatial_decouple_e2e`
- `5f51e40` **E2** print-route filming spatial branch + grain + `test_print_spatial_e2e`
- `3722dca` **S1** scan-route film-density memo, Option-A spatial key, per-param
  key-completeness tests (test_simulate_e2e scenarios D/E)
- `e1b0a2c` **S3** Kotlin retained-result grade cache (grade-only edits: zero native work)
- `a4b39f7` **S2** print-density memo — print_expose+print_develop keyed on the
  film_density_cmy CONTENT ⊕ all printing inputs ⊕ the tc_lut-shaping film params
  (spectral blur, hanatos window/surface, camera UV/IR, input_gamut_compress — the midgray
  factor reads the tc_lut directly, NOT through the film bytes, so they must not alias).
  Scenario F gates output-only HIT / print-side MISS / grain-repeat content-hash HIT, all
  byte-identical-to-cold. Works with grain ON (film memo bypasses, content hash matches).
- `16f6372` **S4** — DIR-coupler develop loops (pointwise + spatial variant),
  interpolate_exposure_to_density chunking (covers print_develop + morph), expose bw/log10
  tails → the deterministic `parallel_for`. grain + recursive blur filters stay serial.
  scan() was already parallel at both hot loops.

### Measured perf (THIS container: 4 cores, SPK_NUM_THREADS=8, 512×512 deterministic, median)
| scenario | ms |
|---|---|
| cold scan (full pipeline) | **211** (243 pre-S4, −13%) |
| warm scan repeat / output-only edit | 144–159 (scan-route film memo) |
| cold print (full pipeline) | ~400 |
| warm print y-shift (steady) / output-only edit | **153–162** (film + print-density memo → scan() alone) |

Tap decomposition (cold, fresh engine, taps bypass memos): print→film_density 191 ms vs
scan→film_density 39 ms (the delta ≈ the one-time print digest, cached warm); the print
stages cost only **~5 ms** at 512² (print_expose already parallel) — so S2's absolute win
scales with export resolution / enlarger-diffusion / enlarger-LUT paths and grain-ON edits;
scan() ≈ 150 ms is the dominant warm cost (already parallel — further gains need algorithmic
work, e.g. the opt-in scanner LUT, not threading). ⚠ Prior handoff bench numbers came from a
different (likely 2-core) container — never mix boxes when quoting deltas.

### What remains (priorities unchanged)
- **P2 #6** perceptual output-gamut algos (cam16ucs/oklch/oklrab/jzazbz; XL; one oracle
  golden per algo; reserved enum slots exist).
- **Strategy-B rebaseline cluster** (#20-27, now incl. CAT02→CAT16 + the xy-clip removal) —
  one coordinated baseline bump; trigger NOT fired (upstream WB-norm still churning,
  checked 2026-07-01).
- **Device-gated**: R8 0.8.0 release smoke; GPU-LUT re-arm feel (F4); **the E2 default
  print-route look change** (halation/grain now carry into prints — INTENTIONAL per user
  directive, but eyeball it on-device); AUDIT §A param-wiring UX decisions.
- **PR #109 body** still predates this pass — the GitHub MCP connector was unauthenticated
  in this session, so update the PR body (S2/S4 + bench table above) from a session with
  GitHub access, or by hand.
- MALLETT2019: disclosed (GatedBlock); implement-vs-remove decision still open.

### Context notes
- The two e9e70f8 goldens (`scan_portra_lensblur_nohalation`, `print_portra_spatial`) are
  now ACCEPTED — the handoff's "regenerate + sha256 or accept on first engine-test green"
  condition was met by the E1/E2 gates passing (and the full suite re-verified at HEAD twice
  this session).
- Oracle runnable in-env: system python3.11 (numpy/scipy/colour/numba/skimage installed, pip
  no-op), stubs at /tmp/spkstubs, `PYTHONPATH=/home/user/spektrafilm/src:/tmp/spkstubs`;
  generate goldens ONLY at `c1d0e44` (`git -C /home/user/spektrafilm checkout c1d0e44`);
  restore the branch after. Oracle repo left on `claude/exciting-hamilton-hya62` (27bd085).
- Suite replay helper (compile-once archive + all 33 ci.yml argv) used this session:
  rebuild it from ci.yml if needed — argv there is authoritative.
- bench_stages.cpp header now documents the post-S1/S2 memo semantics (scenario 5's median
  is STEADY-STATE: its reps repeat identical params, so only rep 1 pays the pd MISS).

---

## Evergreen operating notes (read once per session)

- Container-reset recovery drill (drilled 5+ times): the env re-clones to a stale commit mid-session; recover via `git fetch origin main` (and the branch) → `git remote prune origin` → verify pushed work is on origin → `git reset --hard <ref>`. Untracked new files SURVIVE reset --hard; tracked edits do NOT. Rule: `git add && git commit -c commit.gpgsign=false && git push` the instant a unit builds green. /tmp and pip envs do not persist.
- Proxy-desync recovery: the local git proxy can come back at a stale snapshot and refuse `git fetch origin <branch>` by name — `git fetch origin <full-sha>` or `git fetch origin refs/pull/<N>/head` still works → `git reset --hard FETCH_HEAD`. Once a PR is merged the work is safe on real GitHub regardless of local state.
- PR/branch lifecycle: the remote branch auto-deletes on merge — recreate with a plain `git push` (`--force-with-lease` fails 'stale info'; `git fetch --prune` first). After a merge, restart the branch from origin/main and open a NEW PR (never stack on merged history). The user may merge mid-session and webhooks don't deliver merges — re-check PR state before pushing. Merging PRs is policy-gated (explicit user go-ahead); tag-push releases allowed when asked.
- Oracle setup: local clone at /home/user/spektrafilm; e2e/param-wiring goldens pinned at c1d0e44 (upstream drift began at commit a9bccd6 — never regenerate from tip); gamut primitive goldens generated at 27bd085; env = system python3.11 with PYTHONPATH=/home/user/spektrafilm/src:/tmp/spkstubs (stubs mock heavy IO deps); checkout the pin SHA before generating goldens and restore the branch after; new gen scripts must pin the SHA they generate at.
- Parity gate: 33 host tests; the per-test argv is authoritative in .github/workflows/ci.yml (copy, never guess) — any doc citing 23/25/26/31 gates is stale. Every engine change: default path byte-identical, feature-on within tol (max_abs≤1e-4, rms≤1e-5), SPK_NUM_THREADS 1≡8 byte-identical, NDK r27 3-ABI build green. All new engine features ship opt-in/default-OFF.
- -fno-finite-math-only is required (scanning relies on NaN propagation through density_to_light); GPU is preview-only, NEVER export (vendor-varying float, float64 expose integrals, implementation-defined NaN handling).
- Build distributable debug APKs with plain `./gradlew :app:assembleDebug` — NEVER `-Pandroid.injected.build.abi` (stamps android:testOnly, blocking tap-install, and moves output to intermediates/).
- R8/minified release is NOT exercised by CI (android job builds debug) — smoke-test a release build on-device before tagging; last validated 2026-06-04 on SM-S948W/Android 16.
- User directives on record: do NOT modify .github/workflows/ files ('everything works there'); do NOT convert .lut→.bin (measured net-negative); GPLv3 attribution 'Film modeling powered by spektrafilm' must stay; never put the model identifier in committed artifacts.
- Toolchain at /opt/android-sdk (NDK 27.0.12077973, CMake 3.22.1, build-tools 35.0.0) may not persist across containers — reinstall via sdkmanager if gone; local.properties is gitignored.
- Orphaned commit: §6g ProfileValidator was committed as 660d33a and pushed but never merged to main (slipped the #102 merge, force-dropped from #103) — re-land it if profile import is prioritized.
- Kotlin/UI-only changes never touch the parity suite. Post-engine grades and masks composite once, in-place on res.data via simResultToBitmapGraded right after simulate — never inside simResultToBitmap (the export site feeds res to both the bitmap and the 16-bit writers, so consumer-side mutation double-applies).
- Perf medians are container-specific: never compare benchmark numbers across boxes (the older 2-core-container numbers are not comparable with the current 4-core ones).
- The user is Akshay Sharma, the app's author (pixls.us megathread), testing on a Galaxy S26 Ultra (SM-S948W, Android 16, arm64) — device-gated items (R8 smoke, GPU-LUT feel, E2 print-look sign-off, mask/gesture feel) queue until he tests on it.
- Engine param honesty: presets/UI must set only engine-honored fields — halation via halationAmount/scatterAmount/boostEv (halationStrength/halationFirstSigmaUm are baked per-profile from use/antihalation and ignored); params threaded only inside conditional blocks (e.g. if(spatial)) get silently dropped on the default path — thread unconditionally and fold into the relevant cache keys.
- CI flake signature: the android job intermittently fails during setup-android with 'Error on ZipFile unknown archive' (corrupt SDK download) — not a code failure; re-run failed jobs.
- `docs/PRIORITY_ROADMAP_2026-06-24.md` defines the P0–P3 item numbering (#1–#27) used throughout this file (P2 #6 = perceptual gamut algos, #18 = MALLETT2019 decision, #20-27 = the Strategy-B rebaseline cluster).
- When parallel sessions/agents touch the engine, land engine fixes ONE AT A TIME — they collide on shared engine files and the PR.
- User's laptop env (device testing over adb): working copy `C:\Filmcam123\Spectrafilmandroid` (`C:\Spectrafilm` is docs-only — a trap); oracle = Python 3.13 venv `C:\Filmcam123\spkenv` + `C:\Filmcam123\spkstubs\sitecustomize.py`; arm64 test binaries at `C:\Filmcam123\spk_arm64`; JAVA_HOME = Android Studio jbr JDK 21.

---

## Session history (compressed 2026-07-02; full text in git history of this file)

### 2026-06-24 — P2 #7 gamut activation + #8/#9 + P3 quick-wins (PR #109) — P2 #7 gamut compression activated end-to-end (b658e6d engine/JNI/facade, 113326c UI dropdowns); P2 #8 preset/diagnostics IO off main thread (008bf2b); P2 #9 undo settleDecision() extraction + tests (e3c42f3); P3 quick-wins #10-13,15 (8d6f1f1). All default-OFF; suite was 31/31 then (now-stale count).
- The Reinhard knee stays at the oracle default (0,1,6) — deliberately NOT user-exposed in v1; could be surfaced later
- input_gamut_compress is folded into the engine_tc_lut cache key ONLY when active, plus compute_film_cache_key (print-route memo)
- Quick-win #14 (GPU-LUT re-arm) was deferred as device-gated and #16 (RawCoilDecoder freeOffHeap) as dead/unreachable until a Coil host exists — both later done in the F-wave
- settleDecision() lives in EditHistory.kt: an edit within the ~500ms restore window pushes the restored baseline so the undo step isn't lost
- Crop constrainToAspect now pivots on the opposite corner (CropConstrainTest); Rotation got byte-count long-widen + OOM guard

### 2026-06-24 — P2 #5 input gamut compression (radial-to-locus tc_lut bake, OPT-IN) — P2 #5 shipped engine-internal and dormant (kOff default): InputGamutCompress enum + spectral_locus_xy + compress_xy_radial in model/gamut_compression, remap_tc_lut_for_compression bake hooked into build_filming_tc_lut; gated by test_gamut_in_xy (goldens generated at oracle HEAD 27bd085; bit-exact 1e-16/1e-13, adversarially re-checked).
- SCOPE finding (verified vs oracle git history): the roadmap's 'CAT02→CAT16 + xy-clip removal + locus bake' bundles THREE independent oracle commits — CAT02→CAT16 is eac6b85 and the runtime xy-clip removal is 30a32a8, both UNCONDITIONAL default-path math in _rgb_to_tc_b belonging to the deferred Strategy-B rebaseline; only the locus bake is opt-in-shaped. Our rgb_to_tc_b stays CAT02 + xy-clip, so the ON path gates PRIMITIVES, not an e2e ON render
- spectral_locus_xy = the closed 66-vertex CIE 1931 2° locus polygon, 380..700nm @ 5nm, baked as a constant captured from colour-science
- remap_tc_lut bilinear edge-clamp resampler bit-matches scipy.ndimage.map_coordinates(order=1, mode='nearest'); asymmetric 7×13 vs 13×7 LUT cases validate the axis order
- Degenerate 1×N LUTs (oracle 0/0→NaN) are out of scope: the engine tc_lut is always 192×192 and C++ guards H>1/W>1
- gen_gamut_in_golden.py reproduces its golden byte-identically (deterministic) and uses a parity-true spektrafilm.utils.spectral_upsampling shim (real _tri2quad/_quad2tri); compress preserves the oracle's at-white dist<1e-9 passthrough and 0*inf==NaN-on-miss

### 2026-06-09 — White-balance wave + release prep v0.8.0 (PR #103) — WB wave on PR #103, all device-confirmed by user ('this works perfectly'): gray-point eyedropper (CreativeWhiteBalance.solveNeutral coordinate-descent), auto-exposure default ON (matches upstream CameraParams.auto_exposure=True), 'Balance to film stock' virtual-85. Release prep: versionCode 9→10, versionName 0.7.0→0.8.0 — v0.8.0 spans 173 commits since v0.7.0.
- FilmStockBalance reads info.reference_illuminant from the profile asset → CCT; adaptD50ToCct Bradford-adapts the D50 working white so tungsten stocks (Vision3 200T/500T, ref ≈2856K) render a daylight scene neutral; default OFF; isMeaningful gates daylight (D55) stocks to a true no-op; keyed into decode caches via filmBalance and round-trips in the recipe
- Scanner white/black correction engine gating (spektra.cpp): active only when bw_on && film.is_positive() on the scan-film route, or a negative paper on the print route — the UI grays it out in the one strict-no-op case (Slide mode on a negative stock); it pins the scan's white/black to target LEVELS (subtle at 0.98/0.01)

### 2026-06-08 — masking Class-S local adjustments (PR #103) — PR #102 MERGED to main (cc917fd); user device-tested: 'apk works and 32bit export works'. PR #103 draft (662ea4b) added the Class-S spatial local ops — Clarity/Texture/Sharpness/Highlights/Shadows via MaskSpatial (separable 3-pass box blur ≈Gaussian, O(n), edge-clamped, luma-only, color preserved by RGB ratio) — reaching 13 of LR's ~14 local ops (Dehaze/DCP deferred).
- ORPHANED COMMIT: §6g ProfileValidator was committed as 660d33a and pushed but slipped the #102 merge, then force-dropped when #103 reset to clean main — NOT in main; recoverable from origin's dangling object; re-land if/when profile import (§6g) is prioritized
- MaskSpatial radii/gains are [RECON] tunables, never device-verified; radii scale with the long edge so draft≡export
- Masking gaps ranked (from the LR RE): brush (cr_mask_paint); AI Select Subject/Sky (cr_mask_image — needs a bundled TFLite + guided filter); Dehaze (DCP); multi-sample color range (LR ≤5); per-component range nesting; on-preview alpha viz; full crs/XMP sidecar export

### 2026-06-08 — UX polish wave: §6h onboarding (PR #101 MERGED) + §6e slide-mode + §6a/b export sheet (PR #102 MERGED) — PR #101 (§6h onboarding: ParamHelp '?' sheets, Basic/Advanced disclosure, 'use its defaults' snackbar) merged at fb8fa0d. PR #102 added §6e slide-mode (111125f), the Lightroom-style export sheet (a8a81a0/7e038d8: format+quality, sizes as post-render downscale, color space, filename, GPS), and §6b TIFF32F + scene-linear TIFF (8ede3db/dcdd352/0b00faa in :lib:tiffwriter, not the parity engine).
- Proxy fetch-by-SHA trick: after a container reset the proxy refuses `git fetch origin <branch>` by name, but `git fetch origin <full-sha>` (or refs/pull/<n>/head) works → `git reset --hard <sha>`
- The user merged PR #101 mid-session, auto-deleting the branch; the next push recreated it with the §6e commits orphaned (no PR) → PR #102 opened for them. Lesson: re-check PR merged-state before assuming branch/PR still open; webhooks don't deliver merges
- §6a finding (don't re-investigate): the LUT INPUT domain is hardcoded to linear ProPhoto in native (spektra.cpp:621 kProPhotoRGB; .cube header L1683) — a true input-CS picker is engine-gated Tier-3, NOT UI-only; output space already flows via params.io.outputColorSpace
- SCENE_LINEAR_TIFF is intentionally UNTAGGED (a display-gamma ICC would mis-describe linear data) and skips the engine; TIFF32F writes the engine SimResult VERBATIM (no quantise/clamp), SampleFormat=3, host-tested incl. out-of-[0,1] round-trip
- AVIF export (§6c) = a new :lib:avifwriter .so — biggest integration risk is 16KB alignment
- Slide-film detection: StockEntry.isReversal() via groupId == StockCatalog.GROUP_COLOR_REVERSAL, grounded against the real catalog.json
- Deferred by design: output sharpening/watermark/border out of scope (LR defaults them off)

### 2026-06-08 — ALL MERGED to main (#90–#99); next = onboarding (§6h) — PRs #90–#99 all merged (trunk dc7bf54), zero engine C++ changes. Arc: §2/§3 color+tone foundation → masking keystone → ColorGrade de-dup → CLF LUT export + size picker (PR #99, 32a19df: ClfWriter + 17³/33³/65³ picker + .cube/.clf toggle).
- ClfWriter emits Academy/ASC CLF v3 ProcessList+LUT3D (32f trilinear) for DaVinci Resolve 17+/OCIO 2.3+; CubeLut.rgb is blue-fastest which IS CLF's 3D-LUT Array order (samples write through unchanged); floats forced to '.' decimal via Locale.US; XML-escaped title
- CLF-import fidelity was never validated in a real Resolve/OCIO host — structure/ordering unit-tested only; real-host validation is the user's step
- A 3D LUT bake is pointwise only: grain/halation/diffusion/glare are omitted (same as .cube); engine bakeCubeLut(params,size) supports any size and threads the output space

### 2026-06-08 — draw-on-the-preview mask geometry overlay — PR #98 MERGED — PR #98 (ef213df) merged: masks drawable on the photo — pure JVM-tested MaskGesture core (pick → handle, applyDrag → clamped normalized geometry) + MaskGeometryOverlay full-screen modal mirroring CropOverlay. Masking feature-complete for slider+gesture v1.
- Branch-recreate lesson: after a merge, --force-with-lease fails with 'stale info' — `git fetch --prune` first, then a plain `git push` recreates the auto-deleted branch
- Overlay alignment is correct by construction: the image fills a Box with the image aspect ratio so normalized 0..1 maps straight to pixels (no zoom/pan); it edits the mask's first component and preserves the rest

### 2026-06-08 — per-mask Tier-A toolset COMPLETE — PR #97 MERGED — PR #97 merged (6 commits ddf6f17→cfd3b21): gradient-mask UI, per-mask Hue (Oklab.rotateHueLinear), ColorGrade de-dup onto OutputCctf/Oklab (−53 lines, exactness guarded by tests), Whites/Blacks endpoint remap, and Temp/Tint via LocalWhiteBalance. Per-mask Tier-A set complete: Temp/Tint/Exposure/Saturation/Hue/Contrast/Whites/Blacks.
- LocalWhiteBalance builds an output-space Bradford CAT 3×3 (Lindbloom RGB↔XYZ per space + CIE daylight locus), REUSING CreativeWhiteBalance.bradfordCat/mul3 (made internal); temp=0,tint=0 → exact identity. The user explicitly asked for the accurate CAT, not cheap channel gains
- Multiple increments were stacked on one open PR because the user kept saying 'continue' — merge-commit style folds them to main together

### 2026-06-08 — COLOR RANGE mask — PR #96 MERGED — PR #96 (f5df3c8) merged: color-range mask ('tame the reds, not the skin') — ColorRange targetR/G/B + tolerance/feather/invert gating coverage in MaskCompositor alongside the luminance gate; MaskJson round-trip (old recipes → null → strict no-op); slider-driven v1 (eyedropper came later).
- Color distance is measured in the Rec-709 (Cr,Cb) chroma plane of the ENCODED output — luma-independent and gray-neutral for EVERY output space (Rec-709 weights sum to 1 → a neutral pixel has zero chroma regardless of primaries), so no per-space matrices or decode/CS coupling; hue=angle + chroma=radius separates saturated red from muted skin

### 2026-06-08 — the MASKING KEYSTONE landed (+ the whole color/tone foundation) — PR #94 OPEN, #90–#93 MERGED — Marathon: PRs #90–#93 merged (§2 P0 color management, §3.1 Contrast, §3.2 Sat/Vibrance, §3.3 couplers relabel, §2 P1 gamut slider, masking foundation, docs/MASKING_SPEC.md); PR #94 (7f6dbbc/6541a6e/9ce22a1) wired masking end-to-end: data model → MaskRaster → MaskCompositor → simResultToBitmapGraded wiring → MaskJson persistence → MaskPanel UI.
- FIVE container resets in this one session; nothing lost only because every increment was pushed on green; the stop-hook caught a near-miss (couplers relabel built+verified but almost not committed)
- Mask BlendMode ordinals are PINNED to crs:MaskBlendMode 0/1/2 (ADD/SUBTRACT/INTERSECT), per-component invert = crs:MaskInverted, value = crs:MaskValue — Lightroom XMP interop by construction; geometry normalized 0..1
- Compositor formula: decode CCTF → Tier-A → re-encode → (1−α)·in + α·out on res.data, run AFTER the global ColorGrade; empty adjustment = strict no-op

### 2026-06-08 — §2 P1 ACES gamut compression (v1) — PR #93 (MERGED) — PR #93 (5272834): Kotlin post-engine 'Gamut compression' slider — ACES 1.3 Reference Gamut Compression shaper folded into ColorGrade's single CCTF round-trip (each stage independently gated, 0=off byte-identical); GamutCompressTest (8).
- GamutCompress.kt pins the ACES 1.3 RGC constants THR=(0.815,0.803,0.880), LIM=(1.147,1.264,1.312), PWR=1.2
- Honest caveat: it runs on the engine's already-clipped output so it SOFTENS the cyan edge rather than curing it — the true pre-clip cure is the Tier-3 engine change (§2 P3), deferred
- Untracked new files survive `git reset --hard` (GamutCompress.kt did); tracked edits do not

### 2026-06-08 — §3.3 couplers relabel → §3 COMPLETE — PR #92 (MERGED) — PR #92: CouplersSection relabelled to plain language ('Film color character (couplers)', Color mix R→G/B, Color bleed radius/tail) with a redirect to Saturation/Vibrance; param bindings/ranges/defaults unchanged.
- SEVERE reset variant: the repo re-cloned fresh AND the local git proxy came back at a stale snapshot (main ref old, branch ref gone, objects missing). Recovery: merged-PR work is safe on real GitHub — the proxy still exposed refs/pull/91/head → `git fetch origin refs/pull/91/head && git reset --hard FETCH_HEAD` restored the full tree

### 2026-06-08 — §3.2 Saturation/Vibrance (Oklab post-engine grade) — PR #91 MERGED — PR #91: ColorGrade.kt Oklab chroma grade (Saturation uniform (1+sat); Vibrance low-chroma-weighted (1+vib·exp(−C/0.12))) applied in place to res.data once right after simulate via new simResultToBitmapGraded wrapping all 5 render sites — preview and every export stay WYSIWYG.
- Gray-neutrality proof used everywhere since: Ottosson's linear-RGB→LMS rows each sum to 1, so (v,v,v)→C=0 for ANY primaries — grays never tint, hence NO per-space color matrices, only each space's 1-D transfer (mirrors color_output.cpp::output_cctf_encode), gated by savingCctfEncoding (cctfEncoded=false skips the CCTF round-trip)
- Because the export site computes the bitmap BEFORE saveSimResultAsTiff(res), mutating res.data in place is what lets TIFF/PNG16/JPEG inherit the grade with no double-apply

### 2026-06-08 — §3.1 Contrast slider (drives the master tone curve) — PR #90 (MERGED) — Commit 3bad23d: Contrast slider [−100,100] → power S-curve pivoted at display 18% gray (0.46, mid-gray fixed) with pivot slope g=2^(contrast/100), driving the parity-gated master tone curve; composes UNDER a hand-drawn curve (out = userCurve(contrast(in))); contrast=0 emits no points = strict no-op.
- Design rule recorded here first: apply post-engine grades once, in-place on res.data right after simulate — NOT inside simResultToBitmap, because the export site feeds res to BOTH simResultToBitmap and saveSimResultAsTiff, so a consumer-side mutation would double-apply

### 2026-06-08 — §2 P0 color management (display tag + wide-color + ICC embed) — PR #90 DRAFT — §2 P0: ColorManagement.kt — per-output-space display tagging, COLOR_MODE_WIDE_COLOR_GAMUT window request, ICC embed on TIFF/PNG16 exports; fixed wide-gamut output being shown/saved as sRGB (the broken display path all gamut judgments were made on). Transfers verified 1:1 against color_output.cpp::output_cctf_encode.
- Bitmap.setColorSpace is API 29 — unusable at minSdk 24 — so tagging goes through createBitmap(...,colorSpace) on API 26+ (plain sRGB fallback on 24–25/ACES/device-reject)
- ACES2065_1 is the ONLY untagged space (AP0 range exceeds [0,1]; no faithful 8-bit tag) — an invariant asserted in ColorManagementTest
- 8-bit JPEG/PNG/UltraHDR inherit the ICC for free: Bitmap.compress embeds a TAGGED bitmap's profile on API 26+
- The needed ICC assets were already bundled at assets/spektra/icc (saucecontrol + ellelstone) — no new assets

### 2026-06-08 — user-driven solutions + skill, full Lightroom RE, Wave-0 fixes + Wave-1 creative WB (PR #90 DRAFT) — PR #90 foundation: spectrafilm-solutions skill + docs/USER_DRIVEN_SOLUTIONS.md; Wave-0 fixes (97aa489: the `isRawFileName(name) || true` DNG-detection bug → SourceDetect.isNonRawImage MIME routing; PRESET_VERSION migrate() seam); full Lightroom RE (90e33af/a487482 → docs/lightroom-re/ + docs/RESEARCH_LIGHTROOM_IMPLEMENTATION.md); Wave-1 CreativeWhiteBalance (bfc226a: pre-engine Bradford CAT on the linear ProPhoto input, all sources, keyed into decode caches).
- The user is Akshay Sharma, the app author — the 'Akshay_Sharma building an Android Lightroom-style spektrafilm UI on a Galaxy S26' in the pixls.us megathread
- LR RE reproduction: APK is the user's Drive file — `curl -sSL "https://drive.usercontent.google.com/download?id=178wy480GhIszxWT-HSKpdnIDMaGKfv9D&export=download&confirm=t" -o /tmp/lr.apk`; it's an APKMirror bundle (unzip base.apk + split_config.arm64_v8a.apk; libLrAndroid.so in the arm64 split); decompile base.apk with the brutalist-re script (pairip only wraps the launcher; native algorithms are NOT decompilable — the reference fuses RE surface with public sources)
- Key RE learnings: LR's working space = linear ProPhoto D50 = our engine's exact input; LR's render arch (edit-list + pyramid + cached prefix + tiled exact export) = our parity policy; Highlights/Shadows are local edge-aware ops, not curves; masking = crs:MaskGroupBasedCorrections mirrored 1:1 for free XMP interop; Color Grading = region-weighted HSL; Dehaze = the DCP patent algorithm
- RE catalogs: 1,038 ICB* JNI methods, 862 typed signatures, 16,841 cr_* symbols, the TIParamsHolder schema — all under docs/lightroom-re/

### 2026-06-05 — point tone-curve editor UI (PR #88 MERGED) — PR #88: Lightroom-style point tone-curve editor (Category.TONE_CURVE, ToneCurve.kt: ToneCurveMath + Canvas editor) over the live histogram — Master/R/G/B tabs, tap-add/drag/double-tap-remove, 16 pts/channel engine cap; auto-arms on first edit, 'Reset all' disarms → tone_curve_active=0 → bit-exact off. The engine stage was already wired+parity-gated; only UI was missing.
- WYSIWYG is achieved by a faithful Kotlin port of the engine's Fritsch–Carlson monotone-cubic bake (build_tone_curve_1d: secants → averaged tangents → radius-3 monotonicity projection → Hermite, flat extrapolation past ends, clamp [0,1])
- The engine applies the tone curve in the scanning stage (scanning.cpp:366) for BOTH run_scan_film and run_print, which spk_simulate_preview also runs — so it's live in preview, and toParams() always includes toneCurve (NOT gated by skipGrainHalation)

### 2026-06-05 — Lightroom UX wave + draft/final render & zoom port + preview-speed (PR #85 + #86 MERGED) — PR #85+#86 merged: numeric slider entry, Simulation sub-tabs, draft/final render worker, zoom controls, panel→bottom-overlay (99d31af, kills the preview-resize churn), single-flight decode (2c1416d), MALLETT2019 GatedBlock (a9e46d2), draft→final zoom ROI (b7d6282), enlarger LUT forced in preview + ungated live draft (8c647c7), grain+halation skipped at fit resolution only (fb0ca59/c21c535 — user-confirmed 'grain at 100%' policy; zoom ROI/magnifier/export do NOT skip).
- Three stale-doc claims VERIFIED WRONG against code: (1) a print-route film_density_cmy cache ALREADY existed (spektra.cpp:181-204 compute_film_cache_key → run_print); (2) the audit's 'dead DIR-coupler gamma sliders' claim was wrong — couplers.cpp:73-82 consumes gamma_samelayer_rgb/gamma_interlayer_* and toParams marshals exactly those sliders (left LIVE); (3) the filming-expose 81-band integral was ALREADY LUT-cached (build_filming_tc_lut, filming.cpp:336) — no 'filming-expose LUT' to add
- Build tap-installable debug APKs with plain `./gradlew :app:assembleDebug` — NEVER `-Pandroid.injected.build.abi=…` (stamps android:testOnly, blocking tap-install; only `adb install -t` works)
- Pre-fix device baseline (SM-S948W): preview render 375x500 took 1297ms — the number the preview-speed work was judged against

### 2026-06-05 (PR #85 DRAFT) — GPU reverted, battery debounce, brutalist-re skill — GPU fit preview reverted to default OFF (6494f21) after breaking the editor on SM-S948W; settle debounce raised 350→500ms preview / 280→500ms zoom ROI (72cc807); stable debug.keystore committed + pinned debug signingConfig (58ed0be) so builds share one signature; brutalist-re skill added.
- GPU breakage root cause (3-agent swarm): preview Box weight(1f) + AnimatedVisibility panel in the SAME Column → GLSurfaceView reallocates every frame (BLASTBufferQueue 'rejecting buffer', 87 dropped frames); LutRenderer uses RENDERMODE_WHEN_DIRTY with redraw only in the AndroidView update lambda → can stay black without a guaranteed redraw; the reported 'export lockout bug' was a FALSE POSITIVE (previewBusy stuck under jank; exporting resets fine via ExportMask onDismiss)
- Battery finding: one pinch fired 5 overlapping full RAW 2048px decodes (cancelled coroutines don't stop LibRaw's thread) — the motivation for single-flight decode
- brutalist-re skill provenance: adapted from SimoneAvogadro/android-reverse-engineering-skill (Apache-2.0); unrelated to the film app, parked in this repo because it's the persistent one (user asked to persist it)

### 2026-06-05 (PR #85 DRAFT) — zoom/OOM fix, GPU fit preview, preset rebuild, grain/halation verification — Zoom OOM fixed (7ea73df: dedicated single-entry zoomSourceCache + largeHeap + crop-scale filmFormatMmOverride); preset mapper fix + full rebuild to 28 redesigned presets (39264ee/f59792f + BuiltInPresetsAssetTest); GPU fit preview promoted (6831939, later reverted); grain/halation audited — MATCH the oracle, no engine change made.
- Crop-scale physics: the engine derives pixel_size_um = film_format_mm·1000/max(w,h) from the crop's OWN pixel count, so a sub-frame crop is treated as a whole 35mm frame → grain/halation too weak when zoomed; toParams(filmFormatMmOverride) scales the effective film format by the crop fraction
- Presets must set only engine-HONORED fields: halation is tuned via halationAmount/scatterAmount/boostEv — halationStrength/halationFirstSigmaUm are BAKED per-profile from use/antihalation tags and IGNORED; DIR uses amount/inhibition/diffusion (gamma matrices baked); diffusion filterFamily is fixed to black_pro_mist by the C API
- Baked halation strengths (strong→(0.015,0.005,0), weak→(0.08,0.02,0), no→(0.30,0.10,0.015)) EXACTLY mirror the oracle's _apply_halation_preset (params.cpp:164-188); all 28 stocks have valid use/antihalation tags
- 'Grain too weak' diagnosis: 640px preview averaging µm-scale effects (grain std ∝ 1/√particles-per-px; ~15k/px at 640 vs ~170/px full-res) — an engine 'fix' would break parity

### 2026-06-05 — highlight-boost WIRED + LUT-load speedup; arm64 APK to device — PR #82 (7a87cad): highlight boost ported (numba_boost_hightlights.py → diffusion.cpp::apply_highlight_boost, called in filming::expose after EV-comp, before diffusion/lens-blur/halation, midgray=0.184; golden scan_portra_boost at c1d0e44; test_highlight_boost_e2e). PR #83 (cfa4e16): rd_f16le branchless half→float — bit-identical over all 65536 half patterns, LUT load 36ms→17ms.
- USER DIRECTIVES: (1) do NOT change any .github/workflows/ files — 'everything works there'; (2) do NOT convert .lut→.bin (measured net-negative; the .lut isn't a runtime file; the runtime .npy load is one-time/cached)
- Boost plumbing gotcha: params threaded only inside the if(spatial) block were silently dropped on the default spatial-OFF path — thread UNCONDITIONALLY in BOTH run_scan_film and run_print and fold into compute_film_cache_key
- All 28 profile JSON + 184 supporting assets are sha256-verified byte-identical to upstream c1d0e44; the only unshipped upstream file is the build-only coeffs .lut (runtime loads pre-baked .npy) — correct to omit
- Oracle repro validated: pip math-stack (numpy/scipy/numba/colour/skimage) + tools/parity/setup_env.sh (add colour-science); the 'do NOT pip install' note means the spektrafilm package itself, not the math stack
- With -Pandroid.injected.build.abi, the APK lands in app/build/intermediates/apk/debug/ NOT outputs/ (historic; plain assembleDebug is the rule anyway)

### 2026-06-05 — full param-wiring audit + print EV-comp fix; R8 validated on-device — Branch claude/intelligent-johnson-DEOqK: PR #77 downscale AA-prefilter parity fix (real bug, ~0.18–0.4 divergence — C++ skipped skimage's anti_aliasing gaussian), #78 Spektra logging breadcrumbs, #79 recorded R8 validation, #80 print EV-comp fix. The 5-finding param-wiring audit ledger opened here (all findings since closed: #1 boost → PR #82, #2 MALLETT disclosed, #3 spatial decouple → E1, #4 print route → E2, #5 dead sliders disclosed).
- #80 detail: native hardcoded EV=0 in the print midgray balance; the fix ported the oracle's 4-case midgray branch (printing.py:104-118 + filming.py:125-134, compensated gray 0.184·2^EV) into runtime/print_digest.cpp; goldens print_portra_evcomp{,_nonorm}
- R8 on-device validation detail (2026-06-04, SM-S948W/Android 16): minified release did RAW import → render → 12MP PNG+TIFF export with libsftiff.so loading 'nativeloader … ok' — name-based JNI keep-rules resolve under R8; 'AdrenoVK shaderType 0/6' logcat lines are benign Compose-popup driver noise (engine Vulkan path is an OFF/stub, never called)
- Full audit detail lives in docs/AUDIT.md §A 'Full param-wiring audit'

### 2026-06-04 — oracle pin + finish all inert engine params + positive-film coupler fix — PRs #67–#76 merged. Keystone: oracle PINNED at c1d0e44 (#67) — upstream drift commit identified as a9bccd6 ('tap inject/collect system', changed filming raw-scaling; regenerating from tip diverged film_log_raw ~4.44). Wired all inert marshalled params: spectral blur (#68), hanatos window/surface (#69), camera UV/IR (#72), enlarger preflash (#73), scanner white/black (#74, new runtime/color_reference). Positive-film DIR coupler fix (#75).
- hanatos SURFACE = per-LUT-cell degree-4 2D polynomial (2**surface), NOT an erf4 — ENGINE_WIRING_PLAN.md was wrong about it; specs were verified by reading the oracle, not trusting plan docs
- Positive-coupler root cause (#75): the generic positive coupler-gamma default (0.12,0.08,0.06) omitted the oracle's per-stock overrides from params_builder._apply_film_specifics — provia (0.156,0.104,0.078), velvia (0.108,0.072,0.054); ~0.32 divergence on the scan_film route with couplers ON, negatives always fine; stock-gated fix
- Preflash is correctly NOT folded into the film-density cache key (print-expose is never cached; print route only)
- Enlarger lens blur is honestly gated: lens_blur_um has exactly ONE oracle consumer, filming.py:66 (the CAMERA blur, already ported) — do not wire
- CI flake signature: the android assemble job intermittently fails with 'Error on ZipFile unknown archive' during setup-android (corrupt SDK download) — NOT a code failure; re-run failed jobs once the run's other jobs finish

### 2026-06-03 — audit + cleanup + lifecycle fixes + render-speed/zoom (PR #60) — PR #60 (6 commits): removed committed dist/ APKs (stale, 16KB-misaligned) + closed the ICC license gap (NOTICE.md + CC-BY-SA-3.0/MIT texts); three lifecycle fixes (process-scoped EngineHolder singleton so the immutable engine is never closed mid-life, rememberSaveable session, DecodedSourceCache close-on-evict, idempotent close); profile+tc_lut memo keyed on immutable profile id (54d4d3d, byte-exact, warm-vs-fresh memcmp test); Lightroom-style ROI zoom (af09449).
- GPU verdict (investigated, standing policy): GPU can be a preview-only accelerator, NEVER export — GPU float varies by vendor, the expose integrals are float64, and -fno-finite-math-only NaN handling is implementation-defined on GPU; gpu/vulkan_compute.cpp + SPIR-V is dead scaffolding covering the SCAN stage, not the measured hotspot (the filming/print expose integrals have no GPU kernel)
- Latent int32 npix*3 overflow exists above ~715 MP but is unreachable via the app's 16384px export cap

### 2026-06-02 — v0.7.0 released — v0.7.0 (versionCode 9) tagged + RELEASED — release.yml built and published the signed Spektrafilm-v0.7.0.apk (21.5MB) + .sha256; apksigner verify passed. Workflow: feature branch → PR → merge; merging/self-merging is policy-gated (needs explicit user go-ahead); tag-push releases allowed once the user asks.
- Standing rule stated here: never put the model identifier in committed artifacts

### v0.7.0 session — engine completion, device-verified — PR #59 (from a Windows laptop + Galaxy S25 Ultra over adb): AAssetManager direct-load (spk_engine_create_asset_manager / SpektraEngine.fromAssets, #ifdef __ANDROID__-guarded, skips the ~17MB first-run extraction) and use_enlarger_lut wired (opt-in PCHIP LUT mirroring the scanner LUT, gated by test_enlarger_lut_e2e) — last reserved engine LUT flag gone.
- On-device parity-runner technique (reusable when no host toolchain): build the parity tests with NDK clang --target=aarch64-linux-android24 (-landroid REQUIRED for AAsset), push test + libc++_shared.so + assets/spektra + goldens + tests/*.f64 to /data/local/tmp/spk, run via adb shell with LD_LIBRARY_PATH=. — results identical to host (max_abs 5.96e-08); MSYS mangles device paths → export MSYS_NO_PATHCONV=1
- Enlarger-LUT accel bands are LOOSER than the scanner LUT (print-expose integral less smooth): res 17 ≈1.1e-4, res 64 ≈1.9e-6 vs direct
- Origin of the spkstubs pattern: a sitecustomize.py stubbing the heavy IO deps (exiv2/rawpy/OpenImageIO/lensfunpy/pyfftw) lets the oracle run with only the math stack — the same idea as today's /tmp/spkstubs

### RAW export OOM (merged, device-confirmed) — PR #56: RAW export OOM fixed and device-confirmed (v0.6.3, SM-S948W) — full-res RAW input + engine output moved off the Java heap via malloc + NewDirectByteBuffer; LinearImage/SimResult made AutoCloseable; export-scale buffers closed by caller, preview buffers stay managed.
- Root-cause knowledge: on Android, ByteBuffer.allocateDirect is a NON-MOVABLE byte[] on the ART managed heap (256MB growth limit), NOT native memory — two ~140MB full-res buffers cannot coexist there; the LR RE showed zero allocateDirect in its native libs (full-res pixels live in scalable_malloc/mmap native memory, only compressed JPEG crosses to Java)
- Test asset: the user's raw_test.bin (~24.9MB, 4080×3060 Bayer DNG, from Google Drive) — /tmp copies don't persist; re-request from the user if needed
- Detail doc: docs/RESEARCH_BIG_FILES.md

### What landed since v0.4.0 (merged to main) — v0.4.0→v0.7.0 ledger: LR-RE feature wave (#35 preset amount, #36 copy/paste, #39/#40 resets, #41/#42 tone-curve stage), MotionCam .mcraw parser (#37/#38), perf scaffolding all opt-in/default-off (#46-#52: Vulkan compute + SPIR-V scan port, fp16 NEON, oneTBB, LiteRT stub), big-file RAW fixes (#43/#44/#56), Neutral (Adobe-like) preset (#55).
- GPU speedup is UNPROVEN and hardware-blocked: the sandbox has no KVM/GPU and the x86 emulator is the wrong ISA — it mis-traps the engine's -O2 vectorized copy

### RE assets / where the analysis lives — Doc pointers: docs/RESEARCH_LIGHTROOM_STACK.md (LR = C++ 'CR' engine + oneTBB + Vulkan/OpenCL/Metal + LiteRT + fp16 under Kotlin UI), RESEARCH_LIGHTROOM_RENDER.md (Adobe Color DCP + ProcessVersion vs ours), RESEARCH_BIG_FILES.md, IMPROVEMENT_BACKLOG.md, AUDIT.md; the 2026-06-02-era LR decompile at /tmp/lrx was ephemeral (evidence source: aapt2 + libLrAndroid.so strings/nm).

### Known engine/app gaps (authoritative list: docs/AUDIT.md, updated 2026-06-02) — 2026-06-02 gap snapshot: AAssetManager + enlarger LUT closed; still open then — native RAW tiling/streaming for pathologically large DNGs (a single buffer > native headroom is unbounded), no instrumented androidTest coverage, glare-on-print default-off (stochastic → not bit-exact).
- The fd-failure file fallback in the RAW decode path still uses allocateDirect (managed heap) — a possible off-heap follow-up never done
- docs/DEVICE_TEST_REPORT.md is the historic v0.4.0 device pass; later re-validations are recorded in docs/AUDIT.md

### Doc map (what to read for what) — CLAUDE.md build/parity/arch · docs/AUDIT.md open items · docs/PRIORITY_ROADMAP_2026-06-24.md the #1–#27 priority numbering · docs/IMPROVEMENT_BACKLOG.md LR-RE'd feature list · docs/PERF_ROADMAP.md perf plan+policy · docs/RESEARCH_* RE studies · docs/PRESETS.md/FILM_STOCKS.md content · docs/maps/ source-project maps.
