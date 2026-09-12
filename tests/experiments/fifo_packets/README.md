# Whole-register FIFO writes — experimental, not adopted

The GX frontend currently writes each BP/CP/scalar-XF packet through two or
three scalar appends. These append operations are already inline. This
experiment reduces repeated capacity checks and cursor updates; it does not
claim to remove out-of-line calls, GPU draws, or redundant GX register states.

`packet_write.inc` reserves one complete 5/6/9-byte command when space permits.
It keeps the original scalar fallback when the stream needs growth or a display
list has insufficient room. That fallback matters: existing display-list
overflow may admit the opcode but omit later fields. Dropping the whole command
instead would be a behavior change. No packing structs or unaligned typed
stores are used, and all values retain the original big-endian byte order.

Run from the port root:

```
rtk proxy python tests/experiments/fifo_packets/verify.py
```

The verifier exports actual FIFO header code and the three actual GX macros
from both source profiles, inserts the candidate helper only into private test
copies, then compiles both implementations in one executable. It checks 433,312
packet writes: 32 alignments, short/empty/full display lists, partial command
overflow, buffer growth, mixed commands, canaries and independently specified
endian bytes. It also cross-compiles the standalone test for Android.

`--phone` additionally runs that standalone executable on the explicitly bound
S22+ (SM-S906E/RZCTB00SF3W), only while MP6 is closed. It creates and removes one
owned temporary directory and verifies the installed APK is unchanged. It does
not install an APK or change player data/settings.

Outputs are under `build/fifo-packets-20260912`, including a candidate patch,
exact input/binary hashes, compiler commands and raw timings. The paired
microbenchmark alternates test order. Its command counts approximate the board
census; command order is synthetic. These kernel timings are **not Android game
FPS evidence**.

The native oracle passed on Windows and physical S22+. In 12 alternating-order
pairs, the S22+ writer kernel took a median 9.462 ns/packet for baseline versus
7.526 ns/packet for the candidate (median paired time reduction 20.46%). The
candidate was faster in all 12 pairs. Windows' paired reduction was 3.66%.
These results are limited to the writer kernel, exclude most renderer work,
and do not justify extrapolating that percentage to game FPS. Exact pairs and
limitations are in `kernel-comparison.json`; thermal reports remain in
`native-tests.json`. The temporary Android executable was removed afterward.

Private Windows and Android Release renderers now rebuild all 16 GX source
files that include the changed macros, using the production `-O2 -DNDEBUG`
flags. `build_renderer.py windows|android` verifies production input hashes
before relinking; neither production binaries nor staged APK libraries change.
The caller audit found no function calls or increments whose evaluation order
could change. Values and fields are computed before command emission.

`verify_renderer.py` verifies build provenance and six settings-matched W01
frames each with AO off and Normal AO: all 12 pairs are pixel-identical to the
fixed-seed PC reference. This covers recorded/replayed board rendering, not
every display list or game scene. Proof is in `renderer-verification.json`.

The integrated candidate subsequently ran on the physical S22+ at normal
60 Hz simulation with unlocked rendering, FXAA and AO off. After separate
warm-up/cooling, one 20-second baseline/candidate comparison admitted four
complete rate reports each: median 190.766 versus 185.684 rendered presents/s.
Both started at AP/SKIN 35.8 C, used the same admission limits, and sampled
thermal status 0 throughout. Clocks were not locked and this is not a repeated
or sustained comparison. It does not establish either a reliable regression
estimate or an FPS gain; the candidate remains unadopted. Board screenshots
were reviewed, but are not tick-aligned pixel-equivalence proof on Android.

`build/s22-fifo-packets-20260912/comparison.json` binds frozen logs, settings,
resume markers, thermal windows and binaries. The original APK and player
files were verified unchanged after restoration, the isolated device files were
removed, and USB stay-awake remains 2. A fresh restore prompt was answered
**Don't send**. Nothing was submitted to Google, distributed or pushed.

The packet helper does not affect `GX_WRITE_SOME_REG2/3/4`, bulk
matrix uploads, raw display lists, or other custom command payloads. Do not
rerun the native timing test and silently reuse integrated provenance: the
renderer proof binds the exact native-test report hash.
