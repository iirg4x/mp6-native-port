# Debugging toolkit

Everything here is opt-in (env-gated or a standalone tool) and costs
nothing when unset. All of it works on automation runs (see TESTING.md
for the automation contract and input scripting).

## Crash reporting (always on)

Unhandled faults print a symbolized backtrace (dbghelp on Windows,
dladdr + best-effort backtrace on Android) before the process dies. The
Windows build relies on the fixed image base + the PDB next to the exe;
addresses in any log can also be resolved after the fact with
`mp6_symbolize_addr`'s recipe. `[MP6-CRASH]` lines are the marker.

## Frame/GX inspection

- **framescope** — `MP6_FRAMESCOPE=N` captures the N-th frame's complete
  GX configuration trace (TEV inputs/ops/order, kColors, swap tables,
  texture/TLUT loads, blend/z modes, EFB copies), one line per call,
  tagged with the per-frame draw index; written to
  `build/framescope.txt` (`MP6_FRAMESCOPE_OUT=path` overrides).
  `MP6_FRAMESCOPE_TEXDUMP=1` additionally dumps each texture/TLUT bound
  during the armed frame as raw `.bin`s for offline decoding.
- **gc_tex_decode** — `python tools/gc_tex_decode.py` turns dumped
  GameCube texture data (tiled formats, CI+TLUT) into PNGs. Remember:
  TLUT bytes on disk/in dumps are big-endian and must be decoded as
  such — swapping them host-side inverts alpha.
- **draw census** — `MP6_DIAG_DRAWCOUNT=1` prints the previous frame's
  total draw count once per second (find a sane bisect upper bound).
- **draw bisect** — `MP6_SKIP_DRAWS="lo-hi"` hides per-frame draw
  indices lo..hi (zero-area scissor around the real calls, so FIFO
  bookkeeping is untouched). Binary-search which draw paints a given
  pixel; `[MP6-DRAWBISECT]` lines name the call sites.
- **arrayprobe** — `MP6_ARRAYPROBE=1` logs every GXSetArray bind
  (registry vs learned-size vs zero path), learned-size grows, and
  display-list recording brackets; flags any bind injected INTO a DL
  (`<-- INJECTED-INTO-DL`), which poisons every replay of that list.
- **DL/face validation** — `MP6_DL_DUMP` is dual-purpose: any value
  arms the HSF face-index sweep at parse time (`platform/hsf/`,
  out-of-range vertex/index detection); `MP6_DL_DUMP=<object-name>`
  additionally dumps that one named object's parsed faces, vertex/st
  arrays, and material vertex-descriptor flags to
  `build/dldump_<name>.txt` at DL-build time. Comparing the two catches
  "bad source bytes" (both dumps agree, already wrong at parse) vs a
  "heap stomp between parse and build" (they disagree).
- **object/model draw census** — `MP6_DRAW_CENSUS=<tick>` (two ticks
  from there) dumps Hu3DExec's per-model routing decisions (layer,
  camera, attr bits, skip reasons); `MP6_DRAW_CENSUS_OBJ=<tick>` is the
  per-object companion, walking each Hu3DDraw tree to show which
  objects were visited, skipped, deferred to the XLU sort, or drawn.
  Useful when a model never reaches a single GX call and framescope has
  nothing to show.
- **decode guard** — `MP6_DECODE_GUARD=1` logs every `HuDecodeData`
  call (type, destination range, expected vs. actual bytes written, a
  leftover-size underflow check, and an output checksum) to catch a
  decompression overrun stomping a neighboring heap block, or
  non-deterministic decode output between runs.
- **object-manager census** — `MP6_OM_CENSUS=1` makes `omMain`'s
  per-overlay object walk auditable: every add/delete, every stat-bit
  transition with its symbolized caller, per-object walk verdicts
  logged on change, and a heartbeat every 600 passes.

## Memory

- **RSS watchdog** (always on) — the process samples its own RSS every
  ~600 ticks and aborts loudly past `MP6_RSS_CAP_MB` (default 4096).
- **alloc census** — `MP6_ALLOC_CENSUS_START_TICK=<tick>` arms, from
  that tick on: a per-heap used-bytes/block-count summary every 60
  ticks for all five HuMem heaps, plus a symbolized per-call trace of
  every HuMemDirect{Malloc,MallocNum,Free,FreeNum} (budget-capped), and
  a tagged-block walk before/after every bulk free. The tool for
  "which heap grows, and who allocates into it".
- **coro pool stats** — `MP6_CORO_DEBUG=1` prints the coroutine pool
  layout at boot and peak slot concurrency at exit.
- **leak gate** — `tools/leakgate.py`, the acceptance harness (see
  TESTING.md for thresholds and the lockfile protocol).

## Audio

- **WAV capture** — `MP6_AUDIO_WAV_DUMP=<path.wav>` writes the final
  mix from the audio callback for offline inspection.
- **selftests** — `MP6_AUDIO_SELFTEST_SE / _STREAMS / _GROUPS / _FADE`
  run one-shot bring-up checks of the respective mixer paths.
- **stress hooks** — `MP6_AUDIO_LEAKTEST_SE / _STREAM / _GRPSWAP` spin
  background threads hammering play/stop/group-swap paths for leak
  soaks.

## Input / session driving

- **input script** — `--input-script "wait:N;press:X;stick:D"` (see
  TESTING.md for full syntax); `[TEST]` lines confirm parsing.
- **scheduled START presses** — `MP6_AUTO_START_TICKS=a,b,c`.
- **screenshots** — `tools/winshot.ps1 [-Process name] [-Out path.png]`
  (defaults: `mp6native`, `build\winshot.png`) captures the most-recently-
  started matching window via `PrintWindow`; see TESTING.md's lockfile
  protocol for why this is only safe with a single windowed run active.
- **stub loader test** — `MP6_TEST_LOAD_DLL=m601Dll` drives a full
  synthetic DVDOpen→OSLink→prolog round trip through the minigame-stub
  path (`MP6_TEST_LOAD_DLL_HOLD=1` keeps it loaded).

## Environment variable inventory

Boot/game behavior:

| var | values | effect |
|---|---|---|
| `MP6_LAUNCHER` | `0`/`1` | force automation mode / force the launcher menu (see TESTING.md) |
| `MP6_AUTO_START_TICKS` | tick list | inject PAD START on those ticks |
| `MP6_TICK_HZ` | float, `0` | tick throttle rate (default 60; 0 = free-run/vsync-paced) |
| `MP6_HOST_BASE` | path | Android: base dir for disc/saves/pref (set by the shell) |
| `MP6_HSF_STUB` | `1` | inert HSF loader stub (isolate deserializer vs consumer bugs) |
| `MP6_FREE_ASPECT` | `1` | disable the 4:3 resize constraint + letterboxed present |
| `MP6_RSS_CAP_MB` | MB | RSS watchdog abort cap (default 4096) |

Diagnostics (all default-off):

| var | values | effect |
|---|---|---|
| `MP6_FRAMESCOPE` | frame N | capture frame N's GX call trace |
| `MP6_FRAMESCOPE_OUT` | path | framescope output override |
| `MP6_FRAMESCOPE_TEXDUMP` | `1` | dump textures/TLUTs bound in the armed frame |
| `MP6_DIAG_DRAWCOUNT` | `1` | per-frame draw-count line, 1/s |
| `MP6_SKIP_DRAWS` | `lo-hi` | hide that per-frame draw-index range |
| `MP6_ARRAYPROBE` | `1` | GXSetArray bind/grow/DL-recording probe |
| `MP6_DL_DUMP` | any / object name | HSF face-index validation sweep; a name additionally dumps that object's DL-build data |
| `MP6_DRAW_CENSUS` | tick | per-model Hu3DExec routing/skip census |
| `MP6_DRAW_CENSUS_OBJ` | tick | per-object Hu3DDraw tree-walk census |
| `MP6_DECODE_GUARD` | `1` | HuDecodeData overrun/checksum guard |
| `MP6_OM_CENSUS` | `1` | object-manager (omMain) walk census |
| `MP6_ALLOC_CENSUS_START_TICK` | tick | all-heap census + symbolized alloc trace |
| `MP6_CORO_DEBUG` | `1` | coroutine pool boot/exit stats |
| `MP6_TICK_RATE_LOG` | `1` | measured tick rate + scheduler lateness, 1/5s |
| `MP6_AUDIO_WAV_DUMP` | path | capture the audio mix to WAV |
| `MP6_AUDIO_SELFTEST_SE/_STREAMS/_GROUPS/_FADE` | `1` | one-shot audio selftests |
| `MP6_AUDIO_LEAKTEST_SE/_STREAM/_GRPSWAP` | `1` | audio stress threads for leak soaks |

Test-only hooks:

| var | values | effect |
|---|---|---|
| `MP6_TEST_LOAD_DLL` | name | force a minigame-stub load round trip |
| `MP6_TEST_LOAD_DLL_HOLD` | `1` | keep the test-loaded stub resident |
| `MP6_LAUNCH_HIGH_TEST` | `1` | Android: plain dlopen at a high address — the image-low boot assert must FATAL (negative test) |

(On Android, `MP6_NAME=VALUE` pairs passed as activity args are exported
into the environment before boot, so all of the above work via
`adb shell am start ... --es args "MP6_...=..."`.)
