# Spektrafilm Android — Session Handoff

## Current state (2026-08-26, fork-engine adoption worktree — #125)

- **Fork engine adoption LANDED (local commit series; see issues #117/#118/#125).** The
  VirtuaTOA/spektrafilm-android engine overlay — verified green in #118 (36/36 gates +
  statistical grain checks + 12 MP 1-vs-8-worker memcmp) — is adopted per the #125 resolution:
  grain-stage parallelization (fixed 8192-px blocks, per-block SplitMix64 seeding, dynamic
  atomic-counter scheduling), the spectral 3D-LUT memo + shared interpolator
  (`kernels/lut3d_cache.{h,cpp}`), the debug `-O2` CMake guard, the AE-off `spk_bake_cube_lut`
  + sRGB-shaped lattice, the `spk_meter_exposure_ev` / `meterExposureEv` / `exposureGain`
  metering API, and `tools/parity/run_engine_parity.sh` (full-suite local replay with a
  ci.yml drift guard). Our `kernels/parallel.{h,cpp}` + `tests/test_parallel.cpp` were kept at
  our HEAD (the `959e786` non-vacuous thread-invariance gate; the fork never touched them).
- **Hardening on top of the overlay:** LUT-memo key segments are now length-prefixed (4-byte LE
  byte-count header; two distinct input sequences can no longer concatenate to one byte
  string), and the stale eviction comment was fixed. New gates: `test_parallel` scenario 5
  (192×160 = 30,720 px → 4 grain blocks, 1-vs-8 workers memcmp-identical — the 64×64 fixture
  runs grain in a single block and could not exercise the scheduler) and `test_bake_lut`'s
  shaper property case (corner byte-equality, shaped≠linear, shaped entries vs a 65³ linear
  reference within a measured 6e-2 interpolation-error bound).
- **Host parity suite = 36 gates** (was 35): `test_lut_cache_e2e` added to ci.yml's
  engine-parity job — the single-line workflow edit the owner authorized on #125.
  `run_engine_parity.sh` drift guard counts 36 == 36. Full local run: `ALL OK`, zero FAIL.
- **GRAIN REPRODUCIBILITY NOTE (accepted behavior change):** the grain field differs from
  releases built before the adoption — per-block seeding replaced the old whole-image serial
  RNG stream. It stays deterministic (same input+params+seed ⇒ same bytes), thread-invariant,
  and inside the oracle's statistical band; grain was never oracle-bit-exact (stochastic stage).
  Parity goldens are grain-off and unaffected.
- **App wiring:** GPU preview bakes with `SpektraEngine.SHAPER_SRGB` and the GLES shader
  decodes through the exact inverse + multiplies the metered AE gain (`uExposureGain`);
  `.cube`/CLF export bakes stay `SHAPER_NONE`. Android SDK absent in the work container, so
  `:app:testDebugUnitTest`/`:app:lint` were NOT run there — run them before merging.

## Previous state (2026-07-02, branch `claude/exciting-hamilton-hya62`)

- **"Exact + fast" pass MERGED (PR #109 + #110).** The PM directive (*"spektrafilm-exact result at
  ultra-fast speed"*) is fully landed: F1–F7 Kotlin robustness fixes, **E1** per-effect spatial
  decouple, **E2** print-route spatial + grain enable, **S1** scan-route film-density memo,
  **S2** print-density memo, **S3** Kotlin retained-result grade cache, **S4** deterministic loop
  parallelization. Every default engine path stays byte-identical; the two intentional look
  changes (RAW-input colorspace correction, print-route now carries film character) are onto the
  oracle. See the changelog entry below for commits + the perf table.
- **Oklch perceptual output-gamut compression MERGED (PR #111) — P2 #6 slice 1.** Opt-in /
  default-OFF, byte-identical off. C++ `OutputGamutCompress::kOklch=3` / facade `OKLCH` / UI
  "Oklch (perceptual, keep hue)": perceptual-hue-preserving chroma compression at constant
  Oklch(L, h) — Reinhard knee on `C / C_max`, `C_max` regenerated in-engine by a 64×720 bisection,
  float64 matrices from colour-science. Bit-exact to the oracle (gate `max_abs 1.077e-14`), gated
  by **`test_gamut_out_oklch`** (golden generated at oracle `27bd085`).
- **Oklrab perceptual output-gamut compression LANDED on this branch (P2 #6 slice 2) — NOT yet
  merged.** Opt-in / default-OFF, byte-identical off. C++ `OutputGamutCompress::kOklrab=4` / facade
  `OKLRAB` / UI "Oklrab (perceptual, even lightness)": the oklch chroma reduction with the per-pixel
  `C_max` lookup indexed by Ottosson 2023's rebased lightness `Lr = f(L)` instead of raw `L` (and the
  `C_max(Lr,h)` table built over an Lr grid, OkLab `L` recovered per row by the inverse remap before
  `Oklab→XYZ`); reconstruction still preserves `L`. Bit-exact to the oracle (gate `max_abs 1.055e-14`
  / probes `1.221e-15`), gated by **`test_gamut_out_oklrab`** (golden at oracle `27bd085`). Commits
  `9cb0a0b` golden / `94d2274` engine+ci / `e1b75d8` app on `claude/exciting-hamilton-hya62`.
- **Host parity suite was 35 gates then** (now 36 — see Current state above), all green (argv
  authoritative in `.github/workflows/ci.yml`);
  `SPK_NUM_THREADS` 1≡8 byte-identical (oklrab compress is serial+stateless); NDK r27 3-ABI build
  path unchanged. App **0.9.0 / versionCode 11** (bumped for the 0.9.0 release, issue #129).
- **This branch now carries the unmerged oklrab commits (slice 2) on top of `origin/main` + the
  `1174fd8` docs commit.** Open a NEW draft PR for them; the remote branch auto-deletes on merge and
  recreates with a plain push. Never stack new work on already-merged history.

## Next — P2 #6 slice 3: `jzazbz` (then slice 4 `cam16ucs`)

Slice 2 `oklrab` is DONE (see the state block above). Clone the same pattern; the templates are now
`tools/parity/gen_gamut_oklrab_golden.py`, `model/gamut_compression.{h,cpp}` (oklch + oklrab
sections), and `tests/test_gamut_out_oklrab.cpp` + its ci.yml argv. **`jzazbz` is harder than
oklrab** — it is NOT a simple L-remap: it needs a JzAzBz forward/inverse (PQ encoding + matrices,
absolute-luminance scaled by `_JZAZBZ_Y_W_CDM2 = 100` cd/m²) and its OWN C_max table geometry
(`L_grid=linspace(0.002, 0.18, 64)`, `chroma_initial_upper=0.3`) plus a per-space Jz-white
normalizer for the lightness knee (`_jzazbz_white_Jz`). See oracle `compress_rgb_jzazbz_chroma` +
the `"jzazbz"` branch of `_get_output_c_max_table` in `utils/gamut_compression.py`.
- **Golden:** new `gen_gamut_oklrab_golden.py`-clone → `gen_gamut_jzazbz_golden.py`; call
  `gc.compress_rgb_jzazbz_chroma`; generate at oracle `27bd085` (already checked out) and pin the SHA.
- **C++:** port JzAzBz forward/inverse into `model/gamut_compression.cpp` (capture the colour-science
  matrices/constants as bit-exact hex, as oklch did for OkLab); enum slot `kJzazbz=5` is reserved.
- **Gate:** `test_gamut_out_jzazbz` + its `ci.yml` `build_run … tests/gamut_jzazbz_cases.bin` line
  (bumps the suite to 37 — `lut_cache_e2e` already made it 36; also sync
  `tools/parity/run_engine_parity.sh`'s table or its drift guard fails). Add `gamut_out_jzazbz`
  to the enumerated lists in CLAUDE.md + the skills.
- **Facade/UI:** add `JZAZBZ` to `enum class OutputGamutCompress` (+ the exhaustive `when` in
  MainActivity — Kotlin will error if you forget) and the Output-gamut dropdown.
- Then **`cam16ucs`** (`kCam16ucs=6`, the heaviest — full CIECAM16 forward/inverse). Default upstream.

Per increment: default path byte-identical, opt-in/default-OFF, feature-on within tol
(`max_abs ≤ 1e-4`, `rms ≤ 1e-5`), `SPK_NUM_THREADS` 1≡8, NDK r27 3-ABI build, commit+push on green.
**Ship ONE algo per PR** — subagents died on token limits when given more, so keep each unit small.

### Also open (unchanged)
- **Strategy-B rebaseline cluster** (`PRIORITY_ROADMAP` #20-27; incl. CAT02→CAT16 + xy-clip removal)
  — one coordinated baseline bump; trigger NOT fired (upstream WB-norm `e301791`/`526e200` still
  churning on `reflectance-upsampling-methods`, checked 2026-07-01). Keep the `c1d0e44` pin.
- **Device-gated queue** (user tests on his SM-S948W/Android 16): R8 0.8.0 release smoke; GPU-LUT
  re-arm feel; the E2 print-route look change (film character now in prints — intentional, eyeball
  it); AUDIT §A param-wiring UX decisions.
- **MALLETT2019** — disclosed as a GatedBlock; implement-vs-remove decision still open (`#18`).

---

## Evergreen operating notes (read once per session)

- **Container-reset recovery** (drilled 5+ times): the env re-clones to a stale commit mid-session.
  Recover via `git fetch origin main` (and the branch) → `git remote prune origin` → verify pushed
  work is on origin → `git reset --hard <ref>`. Untracked new files SURVIVE `reset --hard`; tracked
  edits do NOT. Rule: `git add && git commit -c commit.gpgsign=false && git push` the instant a unit
  builds green. `/tmp` and pip envs do not persist.
- **Proxy-desync recovery:** the local git proxy can return a stale snapshot and refuse
  `git fetch origin <branch>` by name — `git fetch origin <full-sha>` or `refs/pull/<N>/head` still
  works → `git reset --hard FETCH_HEAD`. Once a PR is merged the work is safe on real GitHub.
- **PR/branch lifecycle:** the remote branch auto-deletes on merge — recreate with a plain
  `git push` (`--force-with-lease` fails 'stale info'; `git fetch --prune` first). After a merge,
  restart from origin/main and open a NEW PR (never stack on merged history). The user may merge
  mid-session and webhooks don't deliver merges — re-check PR state before pushing. Merging is
  policy-gated (explicit user go-ahead); tag-push releases allowed when asked.
- **Oracle setup:** local clone at `/home/user/spektrafilm`; env = system python3.11 with
  `PYTHONPATH=/home/user/spektrafilm/src:/tmp/spkstubs` (stubs mock heavy IO deps). **e2e /
  param-wiring goldens pinned at `c1d0e44`** (upstream drift began at `a9bccd6` — never regenerate
  from tip); **gamut primitive goldens generated at `27bd085`**. Checkout the pin SHA before
  generating, restore the branch after; new gen scripts must pin the SHA they generate at.
- **Parity gate: 34 host tests**; per-test argv is authoritative in `.github/workflows/ci.yml`
  (copy, never guess) — any doc citing 15/26/31/33 gates is stale. Every engine change: default
  path byte-identical, feature-on within tol, `SPK_NUM_THREADS` 1≡8, NDK r27 3-ABI build green. All
  new engine features ship opt-in / default-OFF.
- **Land engine fixes ONE AT A TIME**, one small item per subagent — parallel agents collide on the
  shared engine files and the PR, and larger tasks blew the subagents' token limits mid-run.
- **`-fno-finite-math-only` is required** (scanning relies on NaN propagation through
  `density_to_light`); **GPU is preview-only, NEVER export** (vendor-varying float, float64 expose
  integrals, implementation-defined NaN handling).
- **Build distributable debug APKs with plain `./gradlew :app:assembleDebug`** — NEVER
  `-Pandroid.injected.build.abi` (stamps `android:testOnly`, blocks tap-install, moves output to
  `intermediates/`). **R8/minified release is NOT exercised by CI** — smoke-test on-device before
  tagging (last validated 2026-06-04 on SM-S948W/Android 16).
- **User directives on record:** do NOT modify `.github/workflows/` ('everything works there'); do
  NOT convert `.lut`→`.bin` (measured net-negative); **GPLv3 attribution "Film modeling powered by
  spektrafilm" must stay**; never put the model identifier in committed artifacts.
- **Toolchain** at `/opt/android-sdk` (NDK 27.0.12077973, CMake 3.22.1, build-tools 35.0.0) may not
  persist across containers — reinstall via `sdkmanager` if gone; `local.properties` is gitignored.
- **Kotlin/UI-only changes never touch the parity suite.** Post-engine grades and masks composite
  once, in-place on `res.data` via `simResultToBitmapGraded` right after simulate — never inside
  `simResultToBitmap` (the export site feeds `res` to both the bitmap and the 16-bit writers, so
  consumer-side mutation double-applies).
- **Engine param honesty:** presets/UI must set only engine-honored fields (halation via
  `halationAmount`/`scatterAmount`/`boostEv` — `halationStrength`/`halationFirstSigmaUm` are baked
  per-profile and ignored); params threaded only inside conditional blocks (e.g. `if(spatial)`) get
  silently dropped on the default path — thread unconditionally and fold into the relevant cache keys.
- **Perf medians are container-specific** — never compare benchmark numbers across boxes (the older
  2-core numbers are not comparable with the current 4-core ones).
- **CI flake:** the android job intermittently fails setup-android with 'Error on ZipFile unknown
  archive' (corrupt SDK download) — not a code failure; re-run the job.
- **Orphaned commit:** §6g ProfileValidator was committed as `660d33a` and pushed but never merged
  (slipped #102, force-dropped from #103) — re-land it if profile import is prioritized.
- **The user is Akshay Sharma**, the app's author (pixls.us megathread), testing on a Galaxy S26
  Ultra (SM-S948W, Android 16, arm64) — device-gated items queue until he tests. His laptop env
  (adb device testing): working copy `C:\Filmcam123\Spectrafilmandroid` (`C:\Spectrafilm` is
  docs-only — a trap); oracle = Python 3.13 venv `C:\Filmcam123\spkenv` + `spkstubs`; arm64 test
  binaries at `C:\Filmcam123\spk_arm64`; `JAVA_HOME` = Android Studio jbr JDK 21.
- `docs/PRIORITY_ROADMAP_2026-06-24.md` defines the P0–P3 item numbering (#1–#27) used throughout
  (P2 #6 = perceptual gamut algos, #18 = MALLETT2019, #20-27 = Strategy-B rebaseline cluster).

---

## Session history (compressed; full prose in this file's git history)

- **2026-08-26 — fork engine adoption (#117/#118/#125).** Adopted the verified
  VirtuaTOA fork overlay: grain parallelization (fixed 8192-px blocks + SplitMix64 per-block
  seeds, dynamic scheduling; #118-verified 12 MP 114.8→35.2 s / 3.1 MP 45.1→11.7 s at 1→8
  workers on a 4-core container, 12 MP memcmp-identical), spectral 3D-LUT memo
  (`kernels/lut3d_cache`), debug `-O2` guard, AE-off + sRGB-shaped `spk_bake_cube_lut`,
  `spk_meter_exposure_ev` API, `run_engine_parity.sh`. Kept OUR `kernels/parallel` +
  `test_parallel` (959e786 gate). Hardened memo keys (length-prefixed segments). New gates:
  multi-block grain scenario in `test_parallel`, shaper property case in `test_bake_lut`;
  `test_lut_cache_e2e` added to ci.yml (owner-authorized single line) — suite 35→36, ALL OK.
  App: GPU preview bakes SHAPER_SRGB + shader-side sRGB encode + metered `uExposureGain`;
  exports stay SHAPER_NONE. Accepted change: grain field differs vs pre-adoption releases
  (deterministic + thread-invariant still).
- **2026-07-02 — P2 #6 slice 2: `oklrab` output-gamut compression (new draft PR, unmerged).** Cloned
  the merged `oklch` pattern: `compress_rgb_oklrab_chroma` = the oklch chroma reduction with the
  `C_max` lookup indexed by Ottosson 2023's rebased lightness `Lr = f(L)` (constants k1=0.206,
  k2=0.03, k3=(1+k1)/(1+k2)); the `C_max(Lr,h)` bisection table is built over an Lr grid with each
  row's OkLab `L` recovered by the inverse remap before `Oklab→XYZ`, and reconstruction preserves the
  original (lightness-compressed) `L`. Reuses oklch's OkLab/RGB↔XYZ hex constants, `cmax_lookup`,
  `reinhard_knee`; table built locally per call (thread-invariant, warm==cold). Golden
  `gen_gamut_oklrab_golden.py` @ oracle `27bd085` (24 cases / 1152 px); gate `test_gamut_out_oklrab`
  `max_abs 1.055e-14` / probes `1.221e-15`. Suite 34→35, defaults byte-identical, oklch/aces/
  output_spaces/simulate_e2e/test_parallel unchanged. Facade `OKLRAB`=4 + Output-gamut dropdown
  ("Oklrab (perceptual, even lightness)"); `:app:compileDebugKotlin` green. Commits `9cb0a0b` /
  `94d2274` / `e1b75d8`.
- **2026-07-02 — "exact + fast" pass (PR #109 + #110).** F1–F7 Kotlin fixes; **E1** per-effect
  spatial gates (`test_spatial_decouple_e2e`, golden `scan_portra_lensblur_nohalation`); **E2**
  print-route filming spatial + grain (`test_print_spatial_e2e`, golden `print_portra_spatial`);
  **S1** scan-route film-density memo + per-param key-completeness tests; **S2** print-density memo
  (keyed on film_density_cmy CONTENT ⊕ printing inputs ⊕ the tc_lut-shaping film params); **S3**
  Kotlin retained-result grade cache (grade-only edits = zero native work); **S4** DIR-develop +
  exposure-interp + expose-tail loops → deterministic `parallel_for`. Both gamut e9e70f8 goldens
  ACCEPTED. Perf (4-core, 8 threads, 512²): cold scan **211 ms** (−13% from S4); warm scan / output-
  only edit 144–159; cold print ~400; warm print y-shift / output-only 153–162 (film + print memo →
  `scan()` alone).
- **2026-06-24 — P2 #5/#7 gamut + #8/#9 + P3 quick-wins (PR #109).** Output ACES-RGC v1.3
  (`test_gamut_out_aces`) + input radial-to-locus xy tc_lut bake (`test_gamut_in_xy`), both
  default-OFF, goldens @ `27bd085`; gamut flags → JNI → facade → two Simulation→Output dropdowns;
  `input_gamut_compress` folded into the tc_lut + film-memo keys only when active. Preset/diagnostics
  IO off-main; undo restoring-flag window fix; P3 quick-wins #10-16. SCOPE finding: CAT02→CAT16 +
  xy-clip removal are UNCONDITIONAL default-path changes (Strategy-B), NOT the opt-in locus bake.
- **2026-06-09 — WB wave + v0.8.0 release prep (PR #103).** Gray-point eyedropper, "Balance to film
  stock" (virtual-85, Bradford-adapt of the profile `reference_illuminant` CCT), auto-exposure
  default ON (matches upstream). versionCode 9→10, versionName 0.7.0→0.8.0. Scanner white/black
  correction gated to the strict-no-op case in UI.
- **2026-06-08 — masking + color/tone foundation (PRs #90–#103).** The keystone arc, all device-
  confirmed: §2 P0 color management (display tag + wide-gamut + ICC embed), §3.1 Contrast, §3.2
  Sat/Vibrance (Oklab post-engine grade), §3.3 couplers relabel, §2 P1 ACES gamut slider (post-
  engine v1), the full masking system (radial/linear/luminance/color-range masks → per-mask Tier-A
  Temp/Tint/Exp/Sat/Hue/Contrast/Whites/Blacks → draw-on-preview overlay → Class-S spatial ops,
  13 of LR's ~14 local ops), CLF/`.cube` LUT export (17/33/65), Lightroom-style export sheet
  (JPEG/UltraHDR/PNG16/TIFF16/TIFF32F/scene-linear), onboarding + slide-mode, the
  spectrafilm-solutions skill + `docs/USER_DRIVEN_SOLUTIONS.md`, and the full Lightroom RE
  (`docs/lightroom-re/`). Zero engine C++ changes in this arc. Design rule established: post-engine
  grades composite once in-place on `res.data`, mask ordinals pinned to crs:MaskBlendMode for XMP
  interop, gray-neutrality from Oklab/Rec-709 rows summing to 1.
- **2026-06-05 — editor + preview-speed wave (PRs #82–#88).** Point tone-curve editor (#88, faithful
  Fritsch–Carlson monotone-cubic Kotlin port); Lightroom UX + draft/final render worker + zoom ROI
  (#85/#86); highlight-boost ported (#82, `diffusion.cpp::apply_highlight_boost`, golden
  `scan_portra_boost` @ `c1d0e44`, `test_highlight_boost_e2e`); half→float LUT-load speedup (#83).
  GPU fit-preview promoted then reverted (broke the editor on SM-S948W). brutalist-re skill added.
- **2026-06-05 — param-wiring audit + print EV-comp (PRs #77–#80).** Downscale AA-prefilter parity
  fix (#77, real ~0.18–0.4 bug); print EV-compensation midgray fix (#80, `runtime/print_digest.cpp`,
  goldens `print_portra_evcomp{,_nonorm}`); R8 on-device validation recorded (#79). Opened the
  5-finding audit ledger (all since closed: boost→#82, MALLETT disclosed, spatial→E1, print→E2,
  dead sliders disclosed).
- **2026-06-04 — oracle pin + inert params + positive-film coupler (PRs #67–#76).** Oracle PINNED at
  **`c1d0e44`** (#67; drift = `a9bccd6`, changed filming raw-scaling). Wired all inert marshalled
  params: spectral blur (#68), hanatos window/surface (#69, surface = per-cell degree-4 2D poly),
  camera UV/IR (#72), enlarger preflash (#73, print-route only, NOT in the film-memo key), scanner
  white/black (#74, new `runtime/color_reference`). Positive-film DIR coupler fix (#75: per-stock
  provia/velvia gamma overrides; ~0.32 divergence on scan_film with couplers ON).
- **2026-06-03 — audit + lifecycle + zoom (PR #60).** Removed stale committed `dist/` APKs + closed
  the ICC license gap; process-scoped `EngineHolder` singleton (immutable engine never closed mid-
  life); profile+tc_lut memo keyed on immutable profile id (byte-exact); Lightroom ROI zoom. GPU
  standing verdict recorded: preview-only accelerator, never export.
- **2026-06-02 — v0.7.0 released.** `release.yml` published the signed APK + `.sha256`; apksigner
  verify passed. Workflow: feature branch → PR → merge (policy-gated); tag-push releases on request.
- **v0.7.0 session — engine completion (PR #59, Windows laptop + Galaxy S25 over adb).** AAssetManager
  direct-load (`SpektraEngine.fromAssets`, `#ifdef __ANDROID__`, skips the ~17 MB first-run extract)
  and `use_enlarger_lut` wired (opt-in PCHIP LUT mirroring the scanner LUT, `test_enlarger_lut_e2e`)
  — last reserved engine LUT flag gone. On-device parity runner: NDK clang `--target=aarch64-linux-
  android24`, push test + `libc++_shared.so` + assets + goldens, run under adb (`max_abs 5.96e-08`).
- **RAW export OOM (PR #56, device-confirmed v0.6.3).** Full-res RAW input + engine output moved off
  the ART managed heap via `malloc` + `NewDirectByteBuffer`; `LinearImage`/`SimResult` made
  `AutoCloseable`. Root cause: `ByteBuffer.allocateDirect` is a non-movable `byte[]` on the ~256 MB
  managed heap, not native memory — two ~140 MB full-res buffers cannot coexist there.
- **Since v0.4.0 (merged).** LR-RE feature wave (#35–#42 preset amount / copy-paste / resets /
  tone-curve stage), MotionCam `.mcraw` parser (#37/#38), perf scaffolding all opt-in/off (#46–#52:
  Vulkan compute + SPIR-V scan port, fp16 NEON, oneTBB, LiteRT stub), big-file RAW fixes
  (#43/#44/#56), Neutral (Adobe-like) preset (#55). GPU speedup remains UNPROVEN/hardware-blocked.

## Doc map (what to read for what)

`CLAUDE.md` build/parity/arch · `docs/AUDIT.md` open items + severity · `CHANGELOG.md` release notes ·
`docs/PRIORITY_ROADMAP_2026-06-24.md` the #1–#27 priority numbering ·
`docs/UPSTREAM_SYNC_2026-06-24.md` Strategy-A/B port plan · `docs/IMPROVEMENT_BACKLOG.md` LR-RE'd
feature list · `docs/PERF_ROADMAP.md` perf plan+policy · `docs/USER_DRIVEN_SOLUTIONS.md` +
`.claude/skills/spectrafilm-solutions/` the user-need catalog · `docs/RESEARCH_*` / `docs/lightroom-re/`
RE studies · `docs/PRESETS.md` / `docs/FILM_STOCKS.md` content · `docs/maps/` source-project maps.
