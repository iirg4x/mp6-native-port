# Windows uncapped rendering verification — 2026-09-05

The existing game window ran Towering Treetop's welcome scene at roughly
62–64 FPS. The optimized build sustained a 1,392 FPS mean across twelve
consecutive five-second samples (1,291–1,429 FPS), with every corresponding
game-logic sample at **60.000 ticks/s**. This is rendered/presented frame
throughput, not faster gameplay. Results are scene/hardware dependent.

The comparison used the real GP6E01 disc assets at 1280×720 with the same
widescreen, AA, shadow, and enhancement settings. Only automatically saved
window coordinates differ in the configuration files. The original memory
card file is byte-identical to the backup.

## Causes and changes

- Windows game/platform code and Aurora were unoptimized Debug builds.
  The isolated release profile uses O2, retains debug symbols, preserves
  wrapping integer semantics, and disables strict aliasing rather than
  enabling unsafe fast-math.
- Frame interpolation allowed only eight extra frames per simulation tick
  and inserted artificial spacing. VSync-off now uses available throughput;
  the existing VSync-on policy remains.
- A transient full render queue previously ended all replay attempts for
  that tick. Uncapped presentation now yields and retries against the same
  absolute deadline without erasing the measured replay-cost estimate.
- The Port-local optimized backend prefers supported Immediate presentation
  when VSync is off, falling back to Mailbox/FIFO as needed.
- The Port's gamemes patch uses the host CRT's `fabs`, avoiding duplicate
  inline definitions exposed by optimization.

No decomp or shared Aurora source/build checkout was modified. See
[Building](BUILDING.md#windows) for the reproducible release commands.

## Verification

- 55 setup/build/release regression tests passed.
- `fi_timing_selftest`: uncapped and paced policies plus deadline admission passed.
- Debug and optimized headless 600-tick runs both exited successfully:
  `[ua1_logdiff] PASS: 287 normalized game-flow lines are IDENTICAL`.
- Final windowed build: 170 translation units, zero compile/link failures.
- F5 capture and F8 restore succeeded on the final optimized executable;
  board rendering and 60 Hz game logic continued after restore.
- Read-only five-minute sampling of the live final game used the standing
  leakgate's memory sampler and slope calculation, excluding a 120-second
  warmup that included board loading and save/restore. RSS slope was
  +244.65 KB/min (500 KB/min threshold; resolution floor 199.99 KB/min):
  PASS. Private commit slope was +41.79 KB/min.

Final tested executable SHA-256:
`a1e34d59953abd02c071a87efaa5c2cc369eaa0bdf45ad44b4c8d8d0d5331b86`.
Local detailed evidence is retained in `build/release/performance-summary.json`,
`performance.log`, and `live-memory-check.json`; disc assets and binary
artifacts are not committed. The original executable/PDB/state are preserved
in `build/perf-baseline/`. Build-specific save states are never migrated by
bypassing their compatibility checks.
