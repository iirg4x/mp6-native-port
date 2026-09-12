# Ready-pipeline memo — experimental, not adopted

The recording path's `find_gx_pipeline` calls `find_pipeline_impl` with blocking
priority. Every cache hit still acquires the shared compiler/cache mutex and
searches the authoritative map. The old last-hash return is non-blocking only
and cannot be enabled blindly: it neither supplies `ShaderInfo` nor proves a
pending pipeline is ready.

This candidate retains 64 ready GX layout results in an 8 KiB, recording-thread
owned, direct-mapped memo. It uses the hash already computed by the ordinary
lookup, not another hash of the configuration. It does not hold map pointers,
GPU handles, uniform values, textures or geometry. Slot collisions miss safely.

Only a successful blocking lookup with a real ready pipeline can publish a
memo result. Shutdown wakes without compilation completion cannot publish.
Alignment changes and earlier `firstFrameUsed` requests use the authoritative
path; earlier-frame persistence is not skipped. Renderer initialization and
shutdown clear the memo. The worker does not access it: background compilation
does not request the recording-only `ShaderInfo` output.

The existing authoritative map never erases ready entries before shutdown.
Re-audit this lifetime rule if eviction/replacement is introduced. This design
does not make arbitrary concurrent callers of `find_gx_pipeline` safe.

Commands, from the port root:

```text
rtk proxy python tests/experiments/ready_pipeline_memo/verify.py
rtk proxy python tests/experiments/ready_pipeline_memo/verify_lookup.py
rtk proxy python tests/experiments/ready_pipeline_memo/build_renderer.py windows
rtk proxy python tests/experiments/ready_pipeline_memo/build_renderer.py android
rtk proxy powershell -NoProfile -File tests/experiments/ready_pipeline_memo/board_window.ps1 -Ao 0
rtk proxy powershell -NoProfile -File tests/experiments/ready_pipeline_memo/board_window.ps1 -Ao 1
rtk proxy python tests/experiments/ready_pipeline_memo/verify_renderer.py
```

The native test passes 433,422 checks over 200,000 requests: collisions, first-frame
rewinds, pending entries, alignment changes and cache restarts. It is not a
thread-race or GPU-lifecycle proof. Both standalone correctness executables
subsequently passed on the physical S22+; `android-native.json` records their
binary/proof hashes and cleanup. The renderer builders modify one source file only in private source copies
and use stock production 0053 archives, not the FIFO packet-writer candidate.
All 12 settings-matched PC W01 frame pairs are pixel-identical (six AO off,
six Normal AO). `renderer-verification.json` records the capture/build hashes;
these captures do not exercise every cache/device lifecycle or prove FPS gains.

No Android FPS gain or adoption is claimed. Before adoption, exercise actual
cache lifecycle/persistence and measure Android benefit without a quality change.

`verify_lookup.py` additionally exports the original and candidate's actual
`publish_shader_info` and `find_pipeline_impl` bodies into a deterministic
compiler/worker/persistence fixture. It passes 1,042,177 checks with identical
layout, first-use persistence and compilation results across ready repeats,
collisions, alignment changes, frame rewinds, asynchronous compilation and
shutdown wakes. The simulated repeat stream takes 10,000 mutex acquisitions
before versus 32 after. That is a synthetic operation count, not a runtime hit
rate, contention/race proof or FPS result. The actual private GPU builds and PC
captures are verified separately. `run_native_phone.py` runs both correctness
oracles without installing an APK, and requires the owned S22 game to be closed.

## Same-process Android diagnostic

`build_diagnostic.py` builds a separate optimized native library from the verified
memo source/archive. Its only additional changes are the lookup instrumentation,
frame-packet diagnostic capture and actual-present observation. It never edits
the production sources, archives or staged APK native library.

The private APK alternates baseline/memo/memo/baseline in 600-recorded-frame
phases. Both paths have the same clock/counter overhead. Each frame's mode and
lookup counters travel by value into its render-worker end callback. The callback
checks whether `after_present` actually advanced; queued frames are not presents.
The first 60 frames of each phase settle the queue/cache. The remaining 540 must
be consecutive successful presents, with 539 positive intervals no longer than
250 ms. Failed presents, gaps, pause/resume and partial phases invalidate the
window. `diagnostic_selftest.cpp` checks those accounting rules.

`measure.py` uses the existing isolated-data, thermal-guarded device runner. It
freezes pre/post logs, APK/native hashes and the exact thermal window; excludes
loading, warm-up, partial phases and nonzero thermal status from comparisons.
`report_diagnostic.py` admits only complete ABBA groups within one cool window.
Per-call timings measure actual renderer lookups, not synthetic key sequences.
Neither the timing overhead nor mobile clock variation is hidden; these are not
unmodified Release FPS measurements. Warm-up measurements are retained but
explicitly ineligible. Physical S22 runs are under
`build/s22-ready-memo-20260912`; the private build is under
`build/ready-pipeline-diagnostic-20260912`.

Completed S22 session: settled-board lookup reuse is about 94.2%, with a small
instrumented warm-up saving of about 0.03-0.04 ms/frame. The cool attempt reached
thermal status 1; the shorter retry was refused before resume. Thus the strict
report has no eligible complete ABBA group and no proven FPS gain. The memo is
not adopted. A separate eight-second CPU recording (5,950 samples) is resolved
against the diagnostic's exact ELF build ID by `report_cpu.py`. Its clock-vDSO
bucket mostly lacks caller stacks and must not be blamed on production code.
`capture_cpu.py` verifies the app/native identity and removes its own temporary
profiler; these sampled frames are never FPS evidence.

The original APK is restored, player settings/card bytes are unchanged and the
isolated device root is removed (`restoration.json`). Do not reuse this removed
root for another launch; start a new owned session. USB stay-awake remains enabled
as requested. No APK was sent to Google or uploaded for distribution.
