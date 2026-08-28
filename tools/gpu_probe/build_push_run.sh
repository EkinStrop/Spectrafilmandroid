#!/usr/bin/env bash
#
# Spektrafilm for Android — GPU device probe: build + push + run. GPLv3.
# Film modeling powered by spektrafilm.
#
# Cross-compiles the standalone probe (tools/gpu_probe/) for arm64 with the NDK,
# pushes it to /data/local/tmp on the connected device, runs Tier 0/1/2 (and the
# Tier 3 shader variants when glslc is available), and saves every capture under
# tools/gpu_probe/captures/. Engine sources are compiled AS-IS from engine/ —
# nothing under engine/ is modified. The f64 reference mandate: no -ffast-math
# anywhere in this build (enforced with -fno-fast-math).
#
# Usage: bash tools/gpu_probe/build_push_run.sh   (from anywhere; repo-root aware)
# Env:   ANDROID_NDK (default: ~/AppData/Local/Android/Sdk/ndk/27.0.12077973)
#        ADB_SERIAL  (default: the only connected device)
set -euo pipefail
cd "$(dirname "$0")/../.."
# Git-Bash/MSYS rewrites device paths like /data/local/tmp into Windows paths
# before adb sees them — disable that conversion for everything below.
export MSYS2_ARG_CONV_EXCL="*"
export MSYS_NO_PATHCONV=1

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) HOST=windows-x86_64 ;;
  Darwin)               HOST=darwin-x86_64 ;;
  *)                    HOST=linux-x86_64 ;;
esac
NDK="${ANDROID_NDK:-$HOME/AppData/Local/Android/Sdk/ndk/27.0.12077973}"
CXX="$NDK/toolchains/llvm/prebuilt/$HOST/bin/clang++"
GLSLC="$NDK/shader-tools/$HOST/glslc"
[[ -x "$CXX" || -x "$CXX.exe" ]] || { echo "NDK clang++ not found: $CXX (set ANDROID_NDK)"; exit 1; }

adbw() { if [[ -n "${ADB_SERIAL:-}" ]]; then adb -s "$ADB_SERIAL" "$@"; else adb "$@"; fi }

CPP=engine/spektra-core/src/main/cpp
OUT=tools/gpu_probe/build
CAP=tools/gpu_probe/captures
mkdir -p "$OUT" "$CAP"

PROBE_SRC="tools/gpu_probe/probe_main.cpp tools/gpu_probe/ref_scan_f64.cpp"
ENGINE_SRC="$CPP/profiles/profile.cpp $CPP/model/spectral.cpp $CPP/model/color_output.cpp"
# android30: the probe queries Vulkan 1.1 entry points (vkGetPhysicalDevice*2),
# absent from the API-24 libvulkan stub. Probe-only; the app's minSdk is untouched.
FLAGS="--target=aarch64-linux-android30 -std=c++17 -O2 -fno-fast-math \
  -DSPK_ENABLE_VULKAN -Itools/parity -Itools/gpu_probe -static-libstdc++"

echo "== build: base probe (vendored SPIR-V, unmodified host) =="
"$CXX" $FLAGS -I"$CPP" $PROBE_SRC $ENGINE_SRC "$CPP/gpu/vulkan_compute.cpp" \
  -lvulkan -o "$OUT/gpu_probe"

# ── Tier 3 shader variants (optional; skipped cleanly without glslc) ────────
BINS="gpu_probe"
if [[ -x "$GLSLC" || -x "$GLSLC.exe" ]]; then
  for V in precise mediump; do
    VDIR="$OUT/tier3_$V/gpu"
    mkdir -p "$VDIR"
    case "$V" in
      precise)  # NoContraction on the band-loop accumulation chain
        sed 's/^    float X = 0.0, Y = 0.0, Z = 0.0;/    precise float X = 0.0, Y = 0.0, Z = 0.0;/' \
          "$CPP/gpu/scan_spectral.comp" > "$VDIR/scan_spectral_$V.comp" ;;
      mediump)  # RelaxedPrecision everywhere (driver MAY evaluate as fp16)
        sed '1a precision mediump float;' \
          "$CPP/gpu/scan_spectral.comp" > "$VDIR/scan_spectral_$V.comp" ;;
    esac
    if "$GLSLC" -fshader-stage=compute --target-env=vulkan1.1 -mfmt=c \
         -o "$VDIR/scan_spectral_spv.inc" "$VDIR/scan_spectral_$V.comp"; then
      cat > "$VDIR/scan_spectral_spv.h" <<'EOF'
// Tier-3 probe variant SPIR-V (generated; shadows gpu/scan_spectral_spv.h).
#ifndef SPK_GPU_SCAN_SPECTRAL_SPV_H
#define SPK_GPU_SCAN_SPECTRAL_SPV_H
#include <cstdint>
static const uint32_t kScanSpectralSpv[] =
#include "scan_spectral_spv.inc"
;
#endif
EOF
      echo "== build: tier3 variant $V =="
      "$CXX" $FLAGS -I"$OUT/tier3_$V" -I"$CPP" $PROBE_SRC $ENGINE_SRC \
        "$CPP/gpu/vulkan_compute.cpp" -lvulkan -o "$OUT/gpu_probe_$V"
      BINS="$BINS gpu_probe_$V"
    else
      echo "tier3 $V: glslc failed — skipping variant"
    fi
  done
else
  echo "glslc not found — skipping Tier 3 variants"
fi

# ── push + run ──────────────────────────────────────────────────────────────
DEV=/data/local/tmp/spk_gpu_probe
PROFILE=engine/spektra-core/src/main/assets/spektra/profiles/kodak_portra_400.json
GOLDEN=tools/parity/goldens/scan_portra/film_density_cmy.spkvec
adbw shell mkdir -p "$DEV"
for B in $BINS; do adbw push "$OUT/$B" "$DEV/" >/dev/null; done
adbw push "$PROFILE" "$GOLDEN" "$DEV/" >/dev/null
adbw shell chmod +x "$DEV"/gpu_probe*

echo "== Tier 0: caps =="
adbw shell "$DEV/gpu_probe caps" | tee "$CAP/caps.txt"
adbw shell cmd gpu vkjson > "$CAP/vkjson.txt" 2>&1 || echo "(cmd gpu vkjson unavailable)"

echo "== Tier 1: fp32 GPU vs f64 reference =="
adbw shell "$DEV/gpu_probe run $DEV/kodak_portra_400.json $DEV/film_density_cmy.spkvec" \
  | tee "$CAP/tier1.txt"

echo "== Tier 2: perf =="
adbw shell "$DEV/gpu_probe perf $DEV/kodak_portra_400.json $DEV/film_density_cmy.spkvec" \
  | tee "$CAP/tier2.txt"

for B in $BINS; do
  [[ "$B" == gpu_probe ]] && continue
  echo "== Tier 3: ${B#gpu_probe_} =="
  adbw shell "$DEV/$B run $DEV/kodak_portra_400.json $DEV/film_density_cmy.spkvec" \
    | tee "$CAP/tier3_${B#gpu_probe_}.txt"
done

echo "== done; captures in $CAP =="
