# Compact mesh instancing experiment

Not enabled in production. The shipping renderer remains patch 0053.

`renderer.patch` is an incremental experiment against the port's patched Aurora
source (through 0053), not against upstream Aurora or the game decomp. It is kept
outside `compat/aurora/base` deliberately. Do not move it there without Android
frame-time/thermal comparisons and additional scene coverage.

The experiment preserves ordered adjacent draws with identical pipeline, texture
bindings, alpha, vertex format, vertex/index counts, index topology and remaining
uniform bytes. A 160-byte storage record supplies each instance's vertex/array
addresses and position/normal matrices. Fragment shading is unchanged. Groups
are limited to eight; line/point expansion and matrix palettes fall back.

The CPU snapshots are important: comparing the mapped GPU upload buffers caused
a severe regression in the first prototype. That version was rejected, not
shipped. The retained candidate compares normal CPU memory and only writes the
GPU upload buffer. A pending shader leaves ordinary draws intact.

Run the standalone Windows correctness checks with:

```powershell
rtk proxy python tests/experiments/compact_instancing/check.py
```

The packing/eligibility test compiles the exact experimental helper with a mock
renderer. The snapshot test compiles the current production `UniformWriter`
against a small append-without-initialization buffer fixture. It poisons the
simulated GPU upload bytes and checks that comparisons still use CPU snapshots.
These are correctness checks, not GPU or FPS benchmarks.

Local full-build/capture/device evidence and reproduction commands are recorded
in `docs/ENGINE_PIPELINE_AUDIT.md`. Raw binaries and capture files stay under
`build/compact-instance-20260912` and `build/board-qa-runs`; no APK was uploaded.
