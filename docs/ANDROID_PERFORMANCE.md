# Android and shared-port performance

This is the current decisions-and-results index for Android and shared-port
performance. The dated run-by-run lab notebook is retained in
[`ANDROID_PERFORMANCE_lab-notebook-20261008.md`](archive/ANDROID_PERFORMANCE_lab-notebook-20261008.md).

## Current status

- Private Android preview 40022 is the latest published artifact. It contains
  the accepted shared source through canonical patch 0081; Android keeps its
  legacy AO default, and XeGTAO remains opt-in experimental on Android.
  The release record is
  [`android-performance-0422-20261007`](https://github.com/iirg4x/mp6-android-builds/releases/tag/android-performance-0422-20261007).
- The APK is `mp6-v40022-ce23137-dirty-aee058fa-arm64-v8a.apk`,
  51,754,445 bytes, SHA-256
  `b918cecc451de286161ad32af47643fd08f0be562eaa3d0f0f380f2518f32f9d`.
  Its 392 native units were clean-built at `-O2 -fno-strict-aliasing`; the
  focused regressions, Gradle dependency verification, release checks,
  provenance, signing metadata and 16 KB alignment passed.
- This exact APK was not installed or device-tested. No tablet connection or
  installation occurred for this release. The whole-game 2× goal remains
  unmet.

## Decisions and retained results

- Keep Android's legacy/default-off policy until matched physical-device
  quality, complete-cost performance and sustained-presentation evidence
  satisfy the acceptance plan. Desktop shader, synthetic, unpaced and
  instrumented measurements are not Android FPS evidence.
- The immutable pipeline-default template (0082) preserved values, draw/upload
  counts and visuals in its oracle. Two reversed-order PC comparisons showed
  12.58% and 12.86% lower unpaced CPU loop means, but this is not displayed
  FPS or Android evidence, and 0082 is not in preview 40022.
- The dated 2026-10-07 experiment batch recorded non-adoption of its private
  mapped-copy suppression, config-hash-only reuse, storage-content memo,
  duplicate vertex-format elimination and render-bundle reuse candidates.
  Those receipts document lifecycle/order risks, unstable or sub-1% observations,
  insufficient whole-workload attribution, or instrumentation that perturbed
  cadence; this historical result does not blanket any later candidate.
- Historical tablet/component profiles remain useful for diagnosis only. They
  use bounded windows, synthetic streams or partial GPU rows and do not close
  the current whole-frame, sustained-60-Hz or thermal gates.

## Open gates and limitations

Android execution is currently deferred. Before changing the Android default,
resume with an authenticated APK and device/GPU/driver record, identical scene
and settings, whole-frame AO-on/AO-off GPU spans, active CPU deltas and actual
compositor present timestamps. Preserve the raw runs and review motion as well
as still captures. Do not extrapolate desktop or one-device evidence to other
GPU families.

No result in this summary establishes a 2× whole-game gain, a sustained Android
FPS result, or a production XeGTAO Android policy. The detailed budgets and
measurement protocol are maintained in
[`XEGTAO_IMPLEMENTATION_PLAN.md`](XEGTAO_IMPLEMENTATION_PLAN.md).

## Evidence navigation

- Local-only build/release receipt directories (not web links):
  `build/android-performance-0422-publish/` and `build/recovery/`.
- Ambient-occlusion contracts and current renderer context:
  [`AMBIENT_OCCLUSION.md`](AMBIENT_OCCLUSION.md).
- Test and measurement conventions: [`TESTING.md`](TESTING.md).
- Full dated narrative, including rejected experiments and historical device
  state: [`archive/ANDROID_PERFORMANCE_lab-notebook-20261008.md`](archive/ANDROID_PERFORMANCE_lab-notebook-20261008.md).
  The archive retains the historical narrative; repository-relative links inside
  it are rebased for docs/archive/, while build/ receipts remain local-only text.

The full goal is not complete while Android physical-device evidence remains
unmeasured.
