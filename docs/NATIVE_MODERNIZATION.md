# Native modernization

Where the build still reproduces GameCube behaviour instead of doing the
native modern equivalent, ranked by what it costs players (smoothness,
loading, battery) against the effort to fix. Shader compilation and frame
pacing are not listed: the shader-cache and Unlocked FPS work own them.

Measurements are from `MP6_SAMPLE_PROFILE` (Windows release, game thread,
one sample per millisecond) on a boot-to-Towering-Treetop run unless noted.

| # | Item | Evidence | Impact | Effort | State |
|---|------|----------|--------|--------|-------|
| 1 | Heap allocator walks every block (GC first-fit) | 773 of 836 ms in the board-start tick were `HuMemMemoryAlloc2` | Loading hitch at every scene and board start, worse on phones | Small | **Done**: free-block index, same addresses. PC board-start tick 608 → 26 ms (same executable, `MP6_HEAP_INDEX=0` vs on); S22+ board start lost ~70 ticks (~1.2 s) to the walk, now ~0-5 |
| 2 | Textures decoded to RGBA8 on first use (CMPR expands 8x) | `convert_texture` runs on the game thread when a texture is first bound; CMPR becomes RGBA8 | Modest: up to the board start, model textures were 3.0 MB RGB565, 1.8 MB RGB5A3, 0.95 MB RGBA8 and only 0.65 MB CMPR (5.2 MB once expanded) | Medium, Aurora | Deferred: BC1 cannot reproduce GX's 3/8-5/8 CMPR blend or its transparent midpoint texel, so colours would shift by up to ~4%; phones would need a lossy ETC2/ASTC encode on top |
| 3 | Data loads are synchronous reads plus decompression on the game thread | `fopen`/`fread` per DVD read, zlib/LZ on the game thread at overlay start | Scene-change stalls, mostly on phones | Medium | **Done**: archives decompress once on worker threads as soon as the game has them (disc reads, ARAM placement), and HuDecodeData copies the result (`src/os/decode_ahead.c`). Board start decompression: PC 10.6 → 3.2 ms, S22+ 25-28 → 6-12 ms. It remembers which files scenes use (`mp6_decode_ahead.idx`) and decodes only those once known: 69 → 26 MB held to the board. The web has no threads, so there it runs cooperatively in the idle time before each tick deadline, only work predicted to fit: board reload decompression in headless Chrome 17.7 → 3.6 ms |
| 4 | Music decodes a whole track on the game thread at play time | `msmStreamPlay` decodes all ADPCM up front | Short hitch when a track starts (small on PC, larger on phones) | Small | **Done**: streamed decode ahead of the mixer; the S22+ 21-29 ms music-start hitch is gone |
| 5 | Renderer re-parses the game's GX command stream every frame | `aurora::gx::fifo::process` per draw list | CPU per frame, mostly on phones | Large | Not now; the Unlocked FPS work already skips rendering for intermediate ticks |
| 6 | EFB copies and depth readbacks for shadows and effects | Aurora copy and snapshot passes | GPU time on phones | Medium, Aurora | Owned by the AO and Unlocked FPS work |
| 8 | Model conversion did avoidable work per model | `getenv` per material/buffer/track, tracks × objects `strcmp` in LoadMotion and JointModel_Motion, three flushed log lines per model | Board start model work | Small | **Done**: names hashed, switches read once, census lines behind `MP6_HSF_VERBOSE`. Board start model work PC 13.7 → ~5 ms, S22+ 10-13 → 6.3 ms |
| 7 | Disc layer opened, size-checked and closed a host file for every game read | `fopen`/`fseek`/`ftell`/`fclose` per DVDOpen, including one per sound effect started | S22+ 36 ms of opens and closes per boot to the board, PC 11.6 ms | Small | **Done**: one host handle per disc file, kept open, positional reads (`mp6_host_file_*`). S22+ 36 → 2.4 ms, PC 11.6 → 1.1 ms |
| 9 | Sound effects read and decoded their sample from the disc when they start | `msmSePlay` read the ADPCM through DVDOpen/DVDRead, then decoded it | Stall when a sound starts; 3-7 ms per first play on the web | Small | **Done**: groups load their sample pool into memory as the real engine's ARAM does, and the DSP-ADPCM decoder is 2.7x faster (same samples). S22+ sound starts 44.6 → 25.8 ms per run; a long sample's first play still decodes on the game thread (up to 3.9 ms on the S22+) |
| 10 | Web disc reads awaited one Blob slice or Range fetch at a time | Board start 64 reads, 7.2 MB, each a round trip; music read a miss every few ticks | Web board-start hitch, 4-8 ms music stalls during play | Medium | **Done**: concurrent read-ahead, cached reads without suspending, and per-overlay file lists fetched ahead on later launches (`mp6_disc_prefetch.txt`). iPhone gate board-start tick 114-129 → 58-73 ms, music stalls gone |
| 11 | Tick throttle spun the last 2 ms of every tick | `VIWaitForRetrace` and `mp6_host_monotonic_ns` were 12% of game-thread samples on PC | CPU and power | Small | **Done** on Windows: high-resolution waitable timer, spin only its measured lateness (3.3%). Android keeps the spin by default (`MP6_PRECISE_SLEEP=1` to A/B): sleeping lets the S22+ governor drop clocks |
| 12 | Quick save and load ran one deflate stream over the 290 MB image | F5 232-266 ms, F8 181-224 ms on PC | Freeze on every quick save or load | Small | **Done**: 1 MB blocks on every core, zero blocks skipped. F5 59-82 ms, F8 37-45 ms |
| 14 | Every GX draw built the ubershader's copy of its pipeline config and re-hashed the config | `memset`/`memcpy` in `uber_pipeline_config` plus XXH3 in `find_pipeline_impl` were ~24% of PC game-thread time with Unlocked FPS | CPU per draw, every frame | Small | **Done**: built only when the ubershader may draw; consecutive draws with an unchanged config reuse the ready pipeline (`MP6_DRAW_PIPELINE_VERIFY=1`). Draw recording PC 0.95 -> 0.51 ms/tick, S22+ 2.14 -> 1.85 ms/tick |
| 15 | Unlocked FPS rescanned every CPU-deformed vertex buffer on every extra frame | `fi_def_blend` 9.8% and `fi_def_restore` copies on PC | CPU per presented frame | Small | **Done**: which buffers move is decided once per tick; replays blend and restore only those. PC samples in that code 3,702 -> 487 |
| 16 | Converted textures were never released | GX has no texture-free call; the static cache grew 13 -> 54 MB over four board -> minigame loops on PC | GPU memory, the iPhone's minigame peak | Small | **Done**: conversions are dropped when the game frees their source memory (heap release reported to Aurora); a 30 s idle sweep (`MP6_TEXTURE_IDLE_SECONDS`) covers unreported sources. Board -> minigame loops hold 9-21 MB |
| 17 | Startup opened every GPU | Dawn's D3D12 discovery made a device per DXGI adapter: 0.72-0.89 s of a ~1 s Aurora start on a PC listing eight | Time to the first frame | Small | **Done**: the high-performance adapter is requested by LUID; Aurora start 0.54-0.63 s |
| 18 | Static vertex arrays re-uploaded every frame | ~750 KB/frame on a board, 85% unchanged | Upload bandwidth | Medium | **Measured, not adopted**: resident arrays verified exact but hashing costs what the copy did; no gain on PC or S22+ |
| 19 | The board re-converted textures it had just converted | The game loads the same texture data again into fresh heap blocks after every event; the address-keyed cache missed (~11 conversions/s on a Quick Party board, 85% identical bytes; the 3 s idle sweep added more) | CPU conversion and GPU upload, iPhone GPU memory churn | Small | **Done**: a miss looks the texture up by a hash of its bytes and palette before converting; freed conversions stay findable for 10 s (`MP6_TEXTURE_KEEP_SECONDS`). Board conversions 231 -> 69 over the same run, board start 198 -> 138 |
| 20 | Every draw hashes its 2.7 KB pipeline config when the state was set again | 73% of config rebuilds on a PC board reproduce one of the last four configs; the hash is ~5% of game-thread time with Unlocked FPS | CPU per draw | Small | **Measured, not adopted**: comparing against recent configs costs what the hash does (memcmp worse, word compare equal). Shrinking the config (4-byte enums) would cut both, but changes every pipeline cache key |
| 21 | Render worker encodes one bind-group set and often a pipeline switch per draw | PC board: 57% of draws change textures, 42% switch pipelines, every draw sets the uniform offset; Dawn and the driver are ~75% of the render worker, which caps desktop FPS | Desktop Unlocked FPS ceiling | Large | Open: needs uniforms indexed per draw and texture arrays instead of per-draw bind groups |
| 13 | Memory card writes were in place | Aurora's card backend opened, seeked and wrote the .gci; creating a file emptied it first | A crash or Android kill mid-save left a torn or empty save the game refused | Small | **Done**: each write replaces the file atomically (temp file, flush, rename; directory fsync on POSIX), and a new file stays untouched until its first write. S22+ kill between flush and rename: next boot loads the card and saves normally |
| 20 | Android audio buffer | SDL's AAudio defaults: 20 ms callbacks, two queued in SDL, 40 ms AAudio buffer | About 70 ms from mix to speaker on the S22+ | Small | **Done**: 10 ms callbacks and the Game stream role (`MP6_AUDIO_SAMPLE_FRAMES` overrides). About 34 ms, no xruns on Treetop |

How to find the next item: run a scene with
`MP6_SAMPLE_PROFILE=<firstTick>-<lastTick>` and read the `[MP6-SAMPLE] hitch`
lines it prints at exit. Each sample is about one millisecond of game-thread
time. Time inside the C runtime or the OS is charged to the
game function that called it (`[ucrtbase.dll] <- caller`).

On Android the same variable samples the game thread with a 1 ms timer
(simpleperf cannot open perf events on the S22+). When the window ends it
prints raw `[MP6-SAMPLE] self|game` lines to logcat; name them with

    python tools/android_sample_report.py logcat.txt

against the unstripped `build/android/aurora/libmp6game.so` of the same
build. `self` is where the game thread was; `game` charges time in libc,
Dawn or the driver to the first game-library caller.
