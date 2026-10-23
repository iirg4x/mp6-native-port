# Browser / PWA build

A WebAssembly build of the windowed game that runs in a browser with
WebGPU and installs as a home-screen app (PWA), including on iPhone
(iOS 26 Safari). You supply your own Mario Party 6 (USA) disc image: the
page copies it into the browser's private storage once, and nothing is
uploaded. The installed app works offline.

## Build

Needs the Emscripten SDK (tested with 6.0.12) at `../toolchain/emsdk`
(or `EMSDK`), plus the same decomp/disc inputs as the Windows build.

```
python tools/build_wasm.py --configure-aurora   # first time: Aurora for wasm
python tools/build_wasm.py                       # game -> build/wasm/web/
python tools/build_wasm.py --headless            # node build, no graphics
node build/wasm/headless/mp6native_headless.js 600
```

`--configure-aurora` configures the vendored `third_party/aurora/` source
(the same one PC and Android use) and builds it with emcmake into
`build/wasm/aurora-build`, taking dependency sources from the existing
Windows RmlUi tree (no downloads apart from the Emscripten SDL3 and
emdawnwebgpu ports).

## Run

Serve `build/wasm/web/` with any static web server over HTTPS (WebGPU and
the offline service worker need a secure origin; `http://localhost` also
counts). For example, on the same machine:

```
python -m http.server 8642 --directory build/wasm/web
```

then open `http://localhost:8642/`. To play on a phone, put the folder on
any HTTPS host. On first launch the page asks for your disc image (.iso);
after that it starts from the stored copy. On iPhone, add the page to the
home screen from Safari's Share menu.


## How it works

- **Memory model.** wasm32 pointers are 32 bits, so the port's
  u32/pointer round trips hold by construction. `src/host/host_emscripten.c`
  is the host backend. Static data is placed at 16 MB (`GLOBAL_BASE`) so
  pointers never look like packed message ids, and the GameCube
  high-bit heap check is skipped on wasm (`memory.c` patch).
- **Coroutines and the main loop.** minicoro's Emscripten fiber backend
  on ASYNCIFY. The same mechanism lets the blocking game loop yield to
  the browser: each Aurora frame waits for `requestAnimationFrame`, and
  GPU waits sleep through `emscripten_sleep`.
- **Graphics.** Aurora runs on emdawnwebgpu (Dawn's webgpu_cpp.h over the
  browser's WebGPU) with SDL3's Emscripten port; the render worker and
  pipeline-cache threads run inline. The changes are `__EMSCRIPTEN__` paths in `third_party/aurora/`.
- **Game content.** `mp6_fopen_utf8` routes read-only disc opens to
  `src/web/web_content.c`, an `fopencookie` stream whose reads await the
  page's provider (`packaging/pwa/content.js`), which reads the stored disc
  image (`Blob`s from the picked file or OPFS) via `mp6ContentSource`.
- **Prototype mismatches.** The decomp disagrees with itself about ~60
  prototypes (void vs int returns, s16 vs int, unprototyped shims).
  Native ABIs absorb this; wasm traps. The build therefore avoids LTO,
  emulates function-pointer casts, and rewrites wasm-ld's trapping
  "signature_mismatch" stubs into adapters after the link
  (`tools/wasm_sigfix.py`, run through the `tools/wasm_ld_wrap.c` wrapper).

## Known gaps

- Removing the home-screen app on iOS deletes its storage: saves and the
  stored disc image have to be set up again.
- Performance depends heavily on the device; ASYNCIFY and emulated pointer
  casts both cost speed.
