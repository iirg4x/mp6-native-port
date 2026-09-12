# Matrix concatenation experiment — September 9

Status: disabled by default. `MP6_EXPERIMENT_VECTOR_CONCAT` is an opt-in
correctness-tested experiment, not a claimed Android optimization.

The sampled board executes approximately 3,869 matrix concatenations per frame.
An explicit four-column vector implementation reduced isolated Windows kernel
time by roughly 13–20%, but did not produce a consistent full-game improvement.
The scalar implementation remains the normal build path.

Six alternating Release runs per set, fixed RNG seed, 1920×1080, FXAA,
4,000 measured board frames, no diagnostic overlays or capture readback:

| Set | Baseline frame ms | Candidate frame ms | Frame-time reduction |
|---|---:|---:|---:|
| AO off | 0.700041 | 0.713087 | -1.86% |
| AO off repeat | 0.725242 | 0.717206 | 1.11% |
| Strong AO | 1.092674 | 1.075132 | 1.61% |

These are Windows measurements, not tablet FPS. Results are in
`build/engine-benchmark/matrix-concat-*.json`.

Correctness checks passed 1,200,000 x86 matrix layouts and 14,400 layouts
executed from NDK-built ARM64 kernels using Unicorn 2.1.4. Cases include exact
aliases, partial overlaps, finite extremes and signed zero. Finite results
compare bitwise; NaNs compare classification. ARM instruction emulation is
not Android runtime testing and cannot measure Mali performance.

Tests: `tests/test_matrix_concat.py` and
`tests/integration/test_matrix_arm.py`. The latter's optional dependency is
installed locally under `build/arm-math-test-deps`.
