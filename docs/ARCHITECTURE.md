# Architecture

How the native port is put together: what runs unmodified, what is
replaced, and the handful of hard rules everything else hangs off.

## Module map

```
decomp repo (read-only, sibling checkout -- see DECOMP_DEPENDENCY.md)
  src/game/*.c  src/REL/{bootDll,selmenuDll,fileseldll,mdseldll}/*.c
        |                                   |
        |  compiled as-is                   |  + patches/decomp/ queue
        v                                   v
   +---------------------------------------------------------+
   |                    game code (unmodified control flow)    |
   +---------------------------------------------------------+
        |  dolphin SDK headers (unchanged) + shim/include/ compat prelude
        v
   +----------------+  +----------------+  +--------------------+
   | platform/os    |  | platform/gx    |  | platform/audio     |
   | arena, HuPrc   |  | aurora_bridge  |  | msm bridge, ADPCM, |
   | scheduler, REL |  | (VI/GX/PAD),   |  | SDL audio out      |
   | bridge, CARD,  |  | framescope     |  +--------------------+
   | save marshal   |  +----------------+  +--------------------+
   +----------------+  | platform/dvd   |  | platform/hsf       |
   | platform/host  |  | FST + files/   |  | native HSF         |
   | os seam:       |  +----------------+  | deserializer       |
   | win32/android  |                      +--------------------+
   +----------------+
        |
        v
   Aurora (GX/VI/PAD/CARD over SDL3 + Dawn/WebGPU) + host OS
```

- **Game code** — the decomp's `game/` tree plus the recovered overlay
  sources (bootDll, selmenuDll, fileseldll, mdseldll), compiled directly
  into the executable. Control flow is unmodified; the small set of
  genuine port fixes lives in `patches/decomp/` (unified diffs applied
  into a build-time staging copy by `tools/apply_patches.py` — the decomp
  checkout itself is never written). Minigame DLLs have no recovered
  source; `platform/os/dll_bridge.c` binds a black-screen stub for them
  (and for any unrecognized overlay id).
- **platform/os** — the low-4GB arena and OS heap family, the native
  HuPrc process scheduler (`process_native.c`), the synthetic-REL loader
  bridge (`dll_bridge.c`), CARD wiring (`card_native.c`), the save-box
  endian marshal (`save_endian.c`), allocation diagnostics
  (`malloc_direct.c`).
- **platform/host** — the OS seam (`host.h`): time, sleep, arena
  reservation, paths, coroutines, mutexes/threads, RSS, crash reporting.
  One backend file per platform (`host_win32.c`, `host_android.c`) plus
  the shared arena-backed coroutine backend (`coro_arena.c` + vendored
  `minicoro.h`). Everything else in `platform/` is OS-agnostic C.
- **platform/gx** — `aurora_bridge.c`, the adapter between the game's
  GameCube idioms and Aurora: VI frame pacing, GXSetArray sizing,
  GXBegin/GXEnd tolerance, keyboard/touch→PAD, the input-script engine,
  the 60Hz tick throttle, window/aspect policy, and debug harnesses
  (draw bisect, framescope).
- **platform/android** and **platforms/android** — the on-device shell:
  a bootstrap that maps `libmp6game.so` below 4GB, the SDL3 activity and
  gradle project, and the touch overlay. (Windows builds compile none of
  it.)
- **Aurora** — consumed from `external_refs/repos/aurora` (read-only)
  with this port's patch series in `platform/gx/aurora-patches/`
  (mingw fixes, stable texture ids, content-fingerprint texture
  versioning, copy-tex bind invalidation, CARD fixes, the Android
  no-JNI asset fallback). Two build trees exist: plain (`build/`) and
  RmlUi-enabled (`build-rmlui/`, used by the launcher). See
  BUILDING.md.
- **Launcher** — the pre-boot settings menu (Windows windowed builds):
  `platform/gx/ui/` is an RmlUi component framework adapted from
  partyboard (see `PARTYBOARD_PROVENANCE.md` for the legal/provenance
  ledger; ripped files carry a two-line header) driven by
  `launcher_core.cpp` (our code: config model, mode decision, settings
  application). Config lives in a portable `mp6_config.json` next to the
  exe. The launcher never runs for automation invocations — see
  TESTING.md ("the automation contract").

## The memory model (low 4GB everywhere)

The game casts pointers to `u32` in many places — heap tags, AMEM
pointers, and crucially CODE addresses (`jmp_buf.lr`,
`OSModuleHeader.prolog/epilog`) — and `game/memory.c` sanity-checks the
high bit of block pointers (`0x80000000` set means "looks like MEM1").
Rather than chase every cast, the port makes every `u32<->pointer`
round-trip valid by construction:

1. **The image is low.** The exe links at `--image-base=0x10000000` with
   ASLR off (`--no-dynamicbase`); on Android the shell reserves a low
   region and `android_dlopen_ext`s the .so into it. Both mains assert
   the whole image sits below 4GB at boot and fail closed.
2. **The game arena is low.** A 256MB reservation at the first free
   candidate base, `0x80000000` preferred (so the allocator's high-bit
   check holds), via `mp6_host_arena_reserve` (VirtualAlloc / mmap with
   `MAP_FIXED_NOREPLACE`).
3. **Coroutine stacks are low.** Every HuPrc process runs on a stackful
   coroutine (minicoro) whose stack comes from a second low reservation
   (`coro_arena.c`, 128 slots × 1MB stacks). OS-allocated stacks (win32
   fibers, plain threads) are NOT guaranteed below 4GB, and process
   locals do escape into `u32` fields — which is why the fiber backend
   is only a compile-time fallback (`-DMP6_CORO_FIBERS`) and why **no
   game code may run on a foreign stack**.

Corollary on Android: **never enter JNI from a coroutine stack.** ART's
JNI-entry check compares `sp` against the registered thread stack and
throws a spurious `StackOverflowError` for arena stacks. Anything that
can reach Java (vibrator/rumble, asset manager) must be deferred to
main-loop context — `PADControlMotor` is queued and applied from
`VIWaitForRetrace`; aurora patch 0011 keeps CARD file probing off JNI.

## The HuPrc scheduler

`game/process.c` is the one game file that cannot be compiled as-is: its
dispatch loop re-enters a single `gcsetjmp` buffer with new values —
legal for raw PPC register-poking, undefined behavior for host
`setjmp/longjmp` (and it fails silently on this toolchain).
`platform/os/process_native.c` reimplements the scheduler loop natively
(verified line-by-line against the original for observable behavior:
canary byte, priority ordering, exec states, sleep/pause semantics) and
gives each process a coroutine. Two coroutine-specific rules the
original never needed: a terminated process's table slot is freed
eagerly (heap-address reuse would otherwise resurrect a dead coroutine),
and a KILLED process is torn down without ever being resumed (a resume
cannot be redirected to HuPrcEnd the way overwriting a saved `lr`
could).

## Data endianness discipline

The GameCube is big-endian; no build target is. The split that keeps
this manageable:

- **GPU-consumed data stays raw big-endian.** Texture blobs, tiled
  formats, TLUTs, display lists go to Aurora byte-for-byte as they came
  off the disc — Aurora's GX implementation expects GameCube-native
  data. **TLUTs especially must never be byte-swapped host-side**: the
  palette decoder consumes raw BE `u16` entries, and a host swap inverts
  alpha (opaque↔transparent).
- **CPU-parsed fields are swapped at the parse site**, explicitly and
  visibly, via `shim/include/be.h` (`mp6_be16/32`, byte-at-a-time,
  alignment-agnostic). The DVD layer returns raw disc bytes; each format
  parser (data.c container headers, sprite/ANM fields, message decode,
  FST walking) owns its own swaps via the patch queue.
- **HSF models get a real deserializer** (`platform/hsf/`): the original
  loader patched pointers inside the loaded file in place, which cannot
  survive 64-bit pointers. The deserializer reads the BE 32-bit-offset
  file and builds a fresh, natively-laid-out `HSF_*` graph; consumers
  (`hsfdraw.c`, `hsfman.c`) run unmodified. Every sub-allocation is
  tagged with the model pointer so the game's own tagged bulk-free
  reclaims it on model kill.
- **Saves are marshalled field-wise** — see below.

## Save system

Saves are Dolphin-interchangeable GCI files under `saves/USA/Card A/`
(aurora's GCI-folder CARD backend; slot A wired as `GP6E`/`01`). The
on-card image must stay byte-true to the console format, so
`platform/os/save_endian.c` marshals the persisted structs
(`GW_COMMON`/`GW_SYSTEM`/`GW_PLAYER`) field-by-field at the six
struct↔buffer boundaries in the patched `saveload.c`:

- scalars are composed/decomposed big-endian;
- **bitfields need bit-level remapping, not byte swaps** — MWCC packs
  bitfields MSB-first, clang/gcc-LE LSB-first, so each field's
  shift/mask transcribes MWCC's allocation order;
- `GW_PLAYER`'s native interior layout genuinely diverges from the
  on-card one (MWCC tucks `s8 handicap` into a bitfield container's
  tail byte), so its offsets come from an explicit on-card table, and
  `_Static_assert`s pin every offset this depends on so an ABI change
  fails the build instead of corrupting cards;
- checksum compare sites compose the stored `u16` big-endian.

## Audio

Game code never calls MusyX directly — everything funnels through the
narrow `msm*` layer, so that is where the port cuts:
`platform/audio/msm_bridge.c` implements the msm surface over its own
mixer (streams + sound effects, group/volume/fade semantics), decoding
DSP-ADPCM (`dspadpcm.c`) from the disc's own sound data, and feeds an
SDL3 audio callback (`audio_out_sdl.c`). The mixer allocates at
init/load time, not per callback; the callback thread and game thread
share state behind host-seam mutexes. `MP6_AUDIO_WAV_DUMP` captures the
mix for verification (see DEBUGGING.md).

## Timing

The engine is fixed-tick: logic advances exactly one tick per
`VIWaitForRetrace`, no delta-time anywhere. Real hardware's VI provided
the 60Hz cadence; a vsync'd present provides the display's rate instead,
so on a high-refresh monitor the game would run fast. The bridge paces
ticks to 60Hz with an absolute-deadline scheduler (sleep to ~2ms out,
then spin), re-anchoring after long stalls instead of bursting.
`MP6_TICK_HZ` overrides (0 disables — the pure-vsync legacy path).

## Boot shapes

One codebase, three process shapes (see BUILDING.md for how to build
each):

- **Windows windowed** (the deliverable): Aurora owns the window;
  `main` runs the launcher (interactive runs only), then `GameMain()`.
- **Windows headless**: no Aurora/SDL linked at all; VI is a tick
  counter; used by the log-diff and leak gates.
- **Android**: `libmp6game.so` exports the entries; a tiny shell maps it
  low and the SDL activity drives `mp6_android_main` (windowed) or
  `mp6_headless_main` (adb smoke runs).
