# Aurora, port-modified source

This directory is Aurora (https://github.com/encounter/aurora, MIT licence,
see `LICENSE`) at upstream commit `1dde08f` ("Move depth correction to
projection matrix"), with every Mario Party 6 port change already applied.
Edit it directly; there is no patch series, apply step or patch fingerprint.

## How it was produced

On 2026-10-21 the former patch queue was applied once and the result
committed here:

- `compat/aurora/base/` 0002-0089 (85 patches shared by every platform),
- `compat/aurora/windows/` 0001-0010 (Windows and Android: immediate
  uncapped present, live AA, background QA, retained replay, render scale,
  skip present, XeGTAO depth storage, release GPU diagnostics),
- `compat/aurora/wasm/0001-emscripten-webgpu.patch` (web build). Its changes
  sit behind `EMSCRIPTEN` / `__EMSCRIPTEN__` guards. One change it made for
  every platform, the `array<vec4u, 3>` uniform layout that Safari requires,
  is now web-only (`PackedArrayStart` in `lib/gx/shader.cpp`), so native
  shader text and every pipeline-cache key derived from it are unchanged.

The native and web halves were three-way merged against the shared base;
the only overlap was two functions appended at the end of
`lib/webgpu/gpu.cpp`, and both were kept.

## What is still a patch

Aurora's CMake fetches third-party sources. Four small adaptations to those
fetched trees stay in `compat/aurora/deps/` (abseil on MinGW, two RmlUi
fixes, and Dear ImGui's WebGPU backend for the web build). They are applied
to the fetched dependency directories, not to this tree.

## Updating Aurora

Diff upstream between `1dde08f` and the new commit, apply that diff here,
resolve conflicts against the port changes, then rebuild every platform.
