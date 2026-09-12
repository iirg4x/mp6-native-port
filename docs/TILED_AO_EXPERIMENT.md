# Tiled AO experiment — not enabled in production

The shared-memory compute denoiser remains available to the GPU test harness,
but the native renderer uses the previous two-pass filter. No APK was uploaded
for this experiment.

The candidate preserves sample counts, filter weights, resolution and receiver
rejection. Its explicit intermediate binary16 rounding can differ from the
render attachment rounding; comparisons permit at most 1/1024 visibility error.
All 18 visual-quality tests and the raw/prepared-depth compatibility comparison
passed on the RTX 4090 Vulkan backend. This does not validate Mali execution.

The six alternating Strong AO, 1920x1080 FXAA board runs in
`build/engine-benchmark/tiled-ao-steady-ao2.json` measured median frame time of
1.093676 ms for the baseline and 1.220463 ms for the candidate: an 11.59% regression.
Most of the difference appears in the game timing bucket; its underlying cause
has not been isolated. Faster isolated denoiser measurements therefore do not
justify enabling this path. Android FPS has not been measured.

The pre-experiment production renderer was restored from the exact local
checkpoint without reverting the earlier renderer optimizations. The experiment
shader and GPU tests remain for further investigation.
