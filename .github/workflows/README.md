# CI workflows

## `ci.yml`

Runs on every push, PR, and manual dispatch. Six jobs:

| Job | What it gates |
|-----|---------------|
| **engine-native** | The engine C++ + JNI bridge compile and link into `libspektra.so` on a host g++ toolchain (`-Wall -Wextra`; JDK provides `jni.h`); checks the exported `spk_*` symbols exist. |
| **engine-parity** | The stage-parity gate: **38 `build_run` tests** against bundled assets and committed goldens (e2e goldens pinned to oracle `c1d0e44`), incl. thread-invariance (`SPK_NUM_THREADS` 1 vs 8). `tools/parity/run_engine_parity.sh` mirrors it locally and fails loudly if its table drifts from this job's `build_run` count. |
| **parity** | The standalone `.spkvec` comparator (`tools/parity`) builds via CMake and its `spkvec_selftest` ctest passes. |
| **python-lint** | The parity harness scripts (`tools/parity/gen_goldens.py`, `spkvec.py`) byte-compile. |
| **android** | JDK 21. Runs `:app:testDebugUnitTest`, then `:app:lint` (a hard gate — `abortOnError = true`, baseline at `app/lint-baseline.xml`), assembles the debug APK with the NDK-built `.so` for all 3 ABIs, and runs the **16 KB-page gate** (`zipalign -c -P 16 4` on the APK + `readelf -lW` requiring `0x4000` `LOAD` alignment on every 64-bit `.so`). Uploads the debug APK as `Spektrafilm-debug-apk`. |
| **android-emulator** | Manual `workflow_dispatch` only, `continue-on-error` (advisory — does not gate). Installs the `android` job's APK on an API 34 x86_64 AVD and asserts `MainActivity` launches with no `FATAL EXCEPTION` / `UnsatisfiedLinkError`. |

## `release.yml`

Fires on a `v*` tag push (or manual dispatch with an existing tag name). Builds a
**production-signed** release APK from the keystore secrets (`SIGNING_KEYSTORE` + alias/password
secrets → `keystore.properties`), verifies the signature with `apksigner`, and publishes the APK
plus a `.sha256` sidecar as GitHub Release assets. Note: builds on **JDK 17**, which differs from
CI's JDK 21; same NDK r27 / CMake 3.22.1 / build-tools 35.0.0 pins as `ci.yml`.

## `r8-smoke.yml` (added 2026-08-26)

Manual `workflow_dispatch` only. Builds the **R8-minified** release APK (debug-signed fallback —
no keystore secrets involved), runs the same 16 KB-page checks as `ci.yml`, and uploads the APK
as `Spektrafilm-r8-smoke-apk`. Purpose: the CI `android` job builds debug (minify off), so a wrong
R8 keep-rule surfaces only at runtime — download this artifact and smoke-test it on a device
**before** tagging a release. See `docs/RELEASE_CHECKLIST.md`.
