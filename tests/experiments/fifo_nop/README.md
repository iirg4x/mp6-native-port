# Bounded FIFO padding skip (not adopted)

This candidate skips consecutive no-op opcode bytes eight at a time instead
of returning through the full command dispatcher for each byte. It is called
only after a NOP has been decoded. The existing opcode mask accepts bytes
0x00 through 0x07; the word mask preserves that behavior, any alignment and
the first real command. Short tails remain byte-checked. Command payloads
are not scanned or changed.

The fixed W01 census counted a median 6,172 NOP bytes out of 19,558 command
dispatches per warmed frame. This is workload evidence, not Android FPS.
The separate array census found no same-address repeated uploads within a
frame, so no geometry-address cache was added.

Native tests compare the actual helper to a byte oracle over 2,060,196 cases,
including all next-opcode values, unaligned starts, nonzero starting offsets,
short/exact ends and protected-page boundaries. They pass on Windows and the
physical S22+. Six fixed-seed PC frames match production exactly with AO off/on.
The Android renderer builds against the verified Release profile. A physical
S22+ baseline/candidate/baseline comparison did not establish a repeatable FPS
gain. Original median rates were 199.872 / 196.348 / 179.120 presents/s. All
admitted thermal samples were status 0, with the same startup limits and settings,
but baseline variation was substantial. A post-hoc stricter board-age cutoff
also leaves no reliable advantage. This patch remains outside production.

Evidence: `build/fifo-nop-20260912/verification.json` and
`build/array-census-20260912/report.json`. Local reproduction helpers are
`build/recovery/build_fifo_nop{,_android}.py`, `test_fifo_nop.py`,
`run_fifo_nop.ps1` and `verify_fifo_nop.py`. The patch replays exactly in memory
against both port-local renderer sources. No decomp files are changed.

Phone results and exact settings/input hashes are in
`build/s22-fifo-nop-20260912/comparison.json`; the original frozen windows are
retained separately. This was Fast Forward, FXAA, AO off and shadow quality 4,
not normal-speed interpolation, sustained performance or Mali evidence. The
original APK and player-file hashes were verified after restoration, the owned
profiling root was removed, and USB stay-awake remained enabled at the user's
request. See that session's `restoration.json`. Nothing was sent to Google or
uploaded for distribution.
