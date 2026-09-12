# Renderer experiments — September 9

Neither candidate below is enabled or included in production binaries.
Port-local source was restored against its audited checkpoint and both
Windows and Android renderer archives were rebuilt and verified.

## Identical transform writes

Bit-exact dirty-state suppression passed 380,000 component-change checks.
Six alternating fixed-seed Release board runs showed unchanged submitted
draws (298.794/frame), merged draws (192.880/frame), and uniform uploads
(257,080 bytes/frame). Median frame time was 0.740430 ms before and
0.740542 ms after: no benefit. The candidate was reverted.

## Aligned indexed F32 vertex fetch

Specialized word-aligned indexed loads preserved the bounds guard and left
direct/odd-stride attributes on the original fallback. GPU tests covered
2,055 offsets, both byte orders and one through four components, comparing
both implementations to an independent bit oracle. The test specialized
storage-pointer parameters to a global buffer because Naga lacks the
pointer-parameter support used by Dawn. Production builds compiled with Dawn.

Six alternating runs per set:

| Set | Baseline ms | Candidate ms | Frame-time reduction |
|---|---:|---:|---:|
| AO off | 0.738886 | 0.734800 | 0.55% |
| Strong AO | 1.111745 | 1.129479 | -1.60% |
| Strong AO repeat | 1.118056 | 1.158793 | -3.64% |

No useful full-game gain was established; the candidate was reverted.
Rejected patches and tests are recoverable under
`build/checkpoints/transform-dedup-20260909` and
`build/checkpoints/aligned-fetch-20260909`. Timing JSON is under
`build/engine-benchmark`. Production binaries were not replaced by either.

## Completion-notified admission waits

A condition-variable generation signal replaced fixed sleeps while awaiting
frame/upload slots, with notifications after map callbacks and worker frame
release. Concurrency tests passed, but the full-game result rejected it:

| Set | Baseline ms | Candidate ms | Frame-time reduction |
|---|---:|---:|---:|
| AO off | 0.726596 | 0.718505 | 1.11% |
| Strong AO | 1.088771 | 1.219141 | -11.97% |

Six alternating runs per set; unchanged draw and upload counts. Restored
the original source, archived the candidate under
`build/checkpoints/notified-wait-20260909`, and rebuilt both renderer archives.
No Android performance claim. The first candidate startup exhausted the old
20,000-tick allowance before the real-time intro completed; it is an invalid
measurement. The valid sets used `--startup-ticks 200000` for both variants,
retaining the same 4,000-frame board measurement window.

## GPU-pass benchmark support (usage)

`build_engine_benchmark.py --gpu-profile` now creates a separate
`current-gpu` or `baseline-gpu` executable. Run with `--gpu-timings`
through board QA, or use the benchmark runner's `--gpu-profile` option.
It records the final 120 completed GPU frames separately from the 4,000-frame
CPU benchmark window. The parser rejects unavailable/incomplete windows.
Query instrumentation is enabled only in these diagnostic runs.

Initial 1920×1080 FXAA fixed-seed desktop captures:

| Pass | AO off ms | Strong AO ms |
|---|---:|---:|
| GPU span | 0.075290 | 0.532122 |
| EFB | 0.049195 | 0.093355 |
| GTAO | — | 0.157687 |
| AO blur X | — | 0.052412 |
| AO blur Y | — | 0.042061 |
| Depth snapshot | — | 0.029167 |
| Present + touch UI | 0.024917 | 0.024823 |
| Offscreen | — | 0.021205 |
| AO decal depth | — | 0.007006 |
| Between passes | 0.001178 | 0.104405 |

Both windows had 120 samples and zero dropped frames. Sources:
`engine-pass-profile-ao0/game.log` and `engine-pass-profile-ao2/game.log`
under `build/board-qa-runs`. These are one window each, not sustained
performance or tablet FPS. AO-off GPU work is much shorter than desktop
whole-frame time, so isolated GPU changes cannot explain all frame overhead.
No new APK was packaged or uploaded.
