# On-device baseline capture (#119)

Interactive wizard that walks the device owner through capturing the
[#119](https://github.com/thetechgeekko/Spektrafilm-android/issues/119) performance
baseline on the phone: 12 MP export wall times (grain × route × format), 640 px
preview slider-settle latency, peak RSS, and a simpleperf capture of a grain-ON
export. It reads the app's `Spektra` logcat breadcrumbs (the export start/duration
lines and the `<profileable android:shell>` flag it needs shipped with the #119
prep commit), and writes the finished table to `docs/baseline-s26u.md`.

```bash
# From the repo root, phone plugged in with USB debugging on:
bash tools/baseline/baseline_wizard.sh
```

- Re-runnable: captured values persist in `tools/baseline/.baseline.env`
  (untracked), so a stopped run resumes where it left off and already-captured
  export combos are skipped.
- Raw captures (simpleperf `perf.data`) land in `tools/baseline/captures/`
  (untracked).
- Flamegraph, afterwards (needs an NDK checkout for the simpleperf scripts and
  the *unstripped* libs from the same build):

```bash
python "$NDK/simpleperf/report_html.py" \
  -i tools/baseline/captures/spk-baseline.perf.data \
  --binary_filter libspektra.so \
  -o tools/baseline/captures/spk-baseline-report.html
# unstripped .so: app/build/intermediates/merged_native_libs/release/out/lib/arm64-v8a/
```

When done: commit `docs/baseline-s26u.md` and paste its tables into #119 —
that unblocks #126 (numeric targets) and #127 (GPU preview route).

*Film modeling powered by spektrafilm (GPLv3).*
