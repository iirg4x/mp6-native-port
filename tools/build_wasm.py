#!/usr/bin/env python3
"""WebAssembly (Emscripten) build of the MP6 native port -- the PWA spike.

Reuses tools/build.py's unit inventory and generated-header machinery, but
keeps its own build tree (build/wasm/) and compiler driver, so the Windows
and Android rows stay untouched.

    python tools/build_wasm.py --headless          # node-runnable headless boot
    node build/wasm/headless/mp6native_headless.js 600

wasm32 satisfies the port's u32<->pointer model by construction (32-bit
pointers). Coroutines use minicoro's emscripten fiber backend, which needs
-sASYNCIFY; the same mechanism lets the game's blocking main loop yield to
the browser (src/host/host_emscripten.c).
"""
import argparse
import concurrent.futures
import glob
import base64
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
NATIVE_ROOT = os.path.dirname(TOOLS)
PORT_ROOT = os.path.dirname(NATIVE_ROOT)
EMSDK = os.environ.get("EMSDK") or os.path.join(PORT_ROOT, "toolchain", "emsdk")
EMSCRIPTEN = os.path.join(EMSDK, "upstream", "emscripten")
EMCC = [sys.executable, os.path.join(EMSCRIPTEN, "emcc.py")]
EMXX = [sys.executable, os.path.join(EMSCRIPTEN, "em++.py")]

sys.path.insert(0, TOOLS)
import build as b  # noqa: E402

WASM_DIR = os.path.join(b.BUILD_DIR, "wasm")
# The browser build links Aurora from the vendored source (third_party/aurora;
# its web-only parts sit behind EMSCRIPTEN guards), configured by
# --configure-aurora into build/wasm/aurora-build.
AURORA_SRC = b.AURORA_SOURCE
AURORA_BUILD = os.path.join(WASM_DIR, "aurora-build")
AURORA_DEPS = os.path.join(AURORA_BUILD, "_deps")
# Third-party sources (headers) the Aurora tree was configured against.
DEPS_SRC = b.aurora_deps_cache()
PORT_FLAGS = ["--use-port=sdl3", "--use-port=emdawnwebgpu"]
WASM_DEVICE_BASE = "/mp6"


def _emscripten_libc_dir(name):
    candidates = [os.path.join(EMSCRIPTEN, "cache", "sysroot", "include")]
    candidates += sorted(glob.glob(os.path.join(EMSDK, "upstream", "lib", "clang", "*", "include")))
    for d in candidates:
        if os.path.exists(os.path.join(d, name)):
            return os.path.join(d, name)
    return None


def _guard_musl_math(msl_dir):
    """Decomp TUs `#define _MATH_H` to keep the MSL math.h stub out. musl
    guards its real math.h with that same name (mingw and bionic use
    _MATH_H_), so on this target the real header would vanish and every
    sin/cos call would become an implicit int-returning declaration -- a
    wasm signature mismatch that traps at runtime. Include musl's header
    with the decomp's guard lifted, once (FP_NAN marks it as included)."""
    real = _emscripten_libc_dir("math.h").replace("\\", "/")
    line = f'#include "{real}"'
    guarded = ("#ifndef FP_NAN\n#pragma push_macro(\"_MATH_H\")\n#undef _MATH_H\n"
               f"{line}\n#pragma pop_macro(\"_MATH_H\")\n#endif")
    for name in ("math.h", "humath.h"):
        path = os.path.join(msl_dir, name)
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        if line in text and "FP_NAN" not in text:
            b._write_if_changed(path, text.replace(line, guarded, 1))


def common_flags(tree, configuration):
    files_root = f"{WASM_DEVICE_BASE}/GP6E01/files"
    fst_path = f"{WASM_DEVICE_BASE}/GP6E01/sys/fst.bin"
    opt = ["-O0", "-g"] if configuration == "debug" else ["-O2", "-g2"]
    # No LTO: decomp TUs disagree about integer widths in ~80 prototypes
    # (s16 vs int, u8 vs s32), which inside one LTO module become trapping
    # "bitcast_invalid" thunks. As separate objects they all lower to i32;
    # the remaining void/int and arity mismatches are adapted after the
    # link by tools/wasm_sigfix.py.
    return opt + [
        "-funsigned-char",
        "-fcommon",
        "-fwrapv",
        "-fno-strict-aliasing",
        "-D_GNU_SOURCE",  # musl hides M_PI etc. without it
        "-DTARGET_PC",
        "-DMP6_LEAKHUNT_DEBUG",
        "-w",
        "-Wno-error=implicit-function-declaration",
        "-Wno-error=implicit-int",
        "-Wno-error=int-conversion",
        "-Wno-error=incompatible-pointer-types",
        "-Wno-error=incompatible-function-pointer-types",
        "-I", os.path.join(tree, "msl_override"),
        "-I", os.path.join(b.PATCHED_SRC_DIR, "include"),
        "-I", os.path.join(tree, "patched_include"),
        "-I", b.DECOMP_INCLUDE,
        "-I", b.DECOMP_INC_DATA,
        "-I", b.SHIM_INCLUDE,
        "-I", b.HOST_INCLUDE,
        "-include", "dolphin_compat.h",
        f'-DMP6_DVD_FILES_ROOT="{files_root}"',
        f'-DMP6_DVD_FST_PATH="{fst_path}"',
        "-DMP6_DVD_ROOT_EXPLICIT=0",
        "-D__declspec(x)=__attribute__((x))",
    ]


def aurora_flags(configuration):
    """Mirror of build.py's AURORA_FLAGS (same include order and defines)
    against the wasm Aurora tree; SDL3 and webgpu headers come from ports."""
    opt = ["-O0", "-g"] if configuration == "debug" else ["-O2", "-g2"]
    deps_src = os.path.join(AURORA_BUILD, "_deps")
    return opt + ["-std=gnu11",
                  "-I", b.SHIM_INCLUDE,
                  "-I", b.HOST_INCLUDE,
                  "-I", os.path.join(AURORA_SRC, "include"),
                  "-I", os.path.join(DEPS_SRC, "fmt-src", "include"),
                  "-I", os.path.join(DEPS_SRC, "xxhash-src", "cmake_unofficial", ".."),
                  "-I", os.path.join(WASM_DIR, "deps", "imgui-src"),
                  ] + PORT_FLAGS + b.AURORA_DEFINES


def aurora_cxx_flags(configuration):
    flags = ["-std=c++20" if f == "-std=gnu11" else f for f in aurora_flags(configuration)]
    return flags + [f'-DMP6_PORT_VERSION="{b.mp6_port_version()}"', *b.mp6_port_release_defines(),
                    "-DAURORA_ENABLE_RMLUI",
                    "-DRMLUI_STATIC_LIB",
                    "-sUSE_ZLIB=1",
                    "-I", os.path.join(DEPS_SRC, "rmlui-src", "Include"),
                    "-I", os.path.join(DEPS_SRC, "abseil-cpp-src"),
                    "-I", AURORA_SRC]


def aurora_link_items():
    items = []
    for root, _dirs, files in os.walk(AURORA_BUILD):
        for name in files:
            if name.endswith(".a") and "CMakeFiles" not in root:
                items.append(os.path.join(root, name).replace("\\", "/"))
    return sorted(items)


def collect_units(headless):
    units = []
    for src, flags, obj, flavor in b.collect_units(headless=headless, android=True):
        if src.replace("\\", "/").endswith("src/host/host_android.c"):
            src = os.path.join(NATIVE_ROOT, "src", "host", "host_emscripten.c")
            obj = obj.replace("host_android", "host_emscripten")
        units.append((src, flags, obj, flavor))
    units.append((os.path.join(NATIVE_ROOT, "src", "web", "web_content.c"),
                  [], "plat_web_content.o", "common"))
    if headless:
        units.append((os.path.join(NATIVE_ROOT, "src", "null", "wasm_headless_stubs.c"),
                      ["-DMP6_HEADLESS_BUILD"], "plat_wasm_headless_stubs.o", "common"))
    else:
        units.append((os.path.join(NATIVE_ROOT, "src", "web", "sdl_dialog_web.c"),
                      [], "plat_sdl_dialog_web.o", "aurora"))
        units.append((os.path.join(NATIVE_ROOT, "src", "web", "safe_area_web.c"),
                      [], "plat_safe_area_web.o", "aurora"))
        # The Android on-screen GameCube controller, shared as-is.
        units.append((os.path.join(NATIVE_ROOT, "src", "android", "touch_pad.cpp"),
                      ['-DIMGUI_USER_CONFIG="aurora/imgui_config.h"'], "plat_touch_pad_aurora.o", "aurora"))
    return units


def _deps_newer(obj, depfile):
    try:
        with open(depfile, "r", encoding="utf-8", errors="replace") as f:
            text = f.read().replace("\\\n", " ")
    except OSError:
        return True
    _, _, deps = text.partition(": ")  # not ":" -- the target has a drive letter
    obj_mtime = os.path.getmtime(obj)
    for dep in shlex.split(deps.replace("\\", "/"), posix=True):
        try:
            if os.path.getmtime(dep) > obj_mtime:
                return True
        except OSError:
            return True
    return False


def compile_unit(unit, obj_dir, flags_for):
    src, extra, obj_name, flavor = unit
    obj = os.path.join(obj_dir, obj_name)
    depfile = obj + ".d"
    cmd_record = obj + ".cmd"
    driver = EMXX if src.endswith(".cpp") else EMCC
    cmd = driver + flags_for(flavor, src) + extra + ["-MD", "-MF", depfile, "-c", src, "-o", obj]
    line = subprocess.list2cmdline(cmd)
    if os.path.exists(obj) and os.path.exists(cmd_record):
        with open(cmd_record, "r", encoding="utf-8") as f:
            same = f.read() == line
        if same and not _deps_newer(obj, depfile):
            return src, obj, True, "", True
    missing_return = b.missing_return_check(driver + flags_for(flavor, src) + extra, src)
    if missing_return:
        return src, obj, False, missing_return, False
    proc = subprocess.run(cmd, capture_output=True, text=True)
    ok = proc.returncode == 0 and os.path.exists(obj)
    if ok:
        with open(cmd_record, "w", encoding="utf-8") as f:
            f.write(line)
    return src, obj, ok, proc.stdout + proc.stderr, False


def link_flags(configuration, headless):
    flags = [
        "-sASYNCIFY=1",
        "-sASYNCIFY_STACK_SIZE=262144",
        "-sALLOW_MEMORY_GROWTH=1",
        "-sINITIAL_MEMORY=256MB",  # grows on demand; iOS Safari is strict about up-front size
        "-sMAXIMUM_MEMORY=2GB",
        "-sSTACK_SIZE=2MB",
        "-sUSE_ZLIB=1",
        # The OS low-memory globals (__OSBusClock & co.) are tentative
        # definitions in dolphin/os.h; a few TUs emit them as strong
        # zero-initialized definitions under LTO. All copies are identical.
        "-Wl,--allow-multiple-definition",
        # The same prototype disagreements reach function-pointer calls
        # (event/process callback tables), which trap in wasm when the
        # pointer's type differs from the target's. Emulated casts give
        # them native-ABI tolerance at some indirect-call cost.
        "-sEMULATE_FUNCTION_POINTER_CASTS=1",
        # Static data starts at 16 MB, like the Windows image (0x10000000+)
        # sits above every packed message id: the game tells a message
        # pointer from a dir<<16|num id by magnitude (window.c's
        # MP6_MESSNUM_ENCODED_MAX), and wasm data would otherwise start
        # near address 0.
        "-sGLOBAL_BASE=16777216",
        "-sENVIRONMENT=node" if headless else "-sENVIRONMENT=web",
    ]
    if not headless:
        pwa = os.path.join(NATIVE_ROOT, "packaging", "pwa")
        flags += ["--pre-js", os.path.join(pwa, "content.js"),
                  "-lidbfs.js",  # saves and settings persist in IndexedDB
                  # Launcher stylesheets and fonts, opened as relative res/...
                  "--preload-file", os.path.join(NATIVE_ROOT, "res").replace("\\", "/") + "@/res",
                  # (includes res/initial_pipeline_cache.db, the shader seed)
                  ] + [
                  "-sINCOMING_MODULE_JS_API=canvas,arguments,print,printErr,setStatus,"
                  "onRuntimeInitialized,preRun,locateFile"]
    if headless:
        flags += ["-sNODERAWFS=1", "-sEXIT_RUNTIME=1",
                  "--pre-js", os.path.join(NATIVE_ROOT, "packaging", "pwa", "node_env.pre.js")]
    flags += ["-O0", "-g"] if configuration == "debug" else ["-O2", "-g2"]
    if not headless:
        # The disc-image importer (content_import.cpp) needs nod, which has
        # no wasm build yet. Its entry points are only reachable from the
        # content-setup browse dialogs, which are stubs on the web
        # (src/web/sdl_dialog_web.c); a call would abort with the name.
        flags += ["-sERROR_ON_UNDEFINED_SYMBOLS=0"]
    return flags


AURORA_TARGETS = ["aurora_core", "aurora_gx", "aurora_gd", "aurora_main", "aurora_pad", "aurora_si",
                  "aurora_card", "aurora_mtx", "aurora_vi", "aurora_os", "aurora_ms"]


def _rmtree(path):
    def writable(func, target, _exc):
        os.chmod(target, 0o700)  # git marks its object files read-only
        func(target)
    shutil.rmtree(path, onexc=writable)


def _apply_patch(tree, patch):
    # Its own repository: inside this worktree, `git apply` would resolve
    # paths against the port and silently skip an ignored build/ tree.
    subprocess.run(["git", "init", "-q"], cwd=tree, check=True)
    subprocess.run(["git", "apply", "--ignore-whitespace", "--check", patch], cwd=tree, check=True)
    subprocess.run(["git", "apply", "--ignore-whitespace", patch], cwd=tree, check=True)


def _reset_foreign_cmake_cache(tree, source):
    """A tree configured from another source directory (the former
    build/wasm/aurora-src copy) cannot be re-pointed; start its cache over."""
    cache = os.path.join(tree, "CMakeCache.txt")
    if not os.path.isfile(cache):
        return
    with open(cache, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    if "CMAKE_HOME_DIRECTORY:INTERNAL=" + source.replace("\\", "/") not in text:
        os.remove(cache)
        shutil.rmtree(os.path.join(tree, "CMakeFiles"), ignore_errors=True)


def configure_aurora(jobs):
    """Configure the vendored Aurora source with Emscripten and build it.
    Dependency sources come from an existing desktop fetch (no downloads);
    Dear ImGui gets its web-only patch (compat/aurora/deps/) on a copy."""
    import aurora_deps
    imgui = os.path.join(WASM_DIR, "deps", "imgui-src")
    if os.path.exists(imgui):
        _rmtree(imgui)
    source_deps = b.aurora_deps_cache()
    shutil.copytree(os.path.join(source_deps, "imgui-src"), imgui, ignore=shutil.ignore_patterns(".git"))
    _apply_patch(imgui, os.path.join(aurora_deps.PATCH_DIR, "imgui-emdawnwebgpu.patch"))
    deps = source_deps.replace("\\", "/")
    cmake = shutil.which("cmake", path=os.path.join(os.environ.get("ProgramFiles", ""), "CMake", "bin")) or "cmake"
    sources = {"ABSEIL-CPP": "abseil-cpp-src", "XXHASH": "xxhash-src", "FMT": "fmt-src", "ZLIB": "zlib-src",
               "PNG": "png-src", "FREETYPE": "freetype-src", "SQLITE3": "sqlite3-src", "ZSTD": "zstd-src",
               "RMLUI": "rmlui-src", "TRACY": "tracy-src"}
    _reset_foreign_cmake_cache(AURORA_BUILD, AURORA_SRC)
    cfg = [sys.executable, os.path.join(EMSCRIPTEN, "emcmake.py"), cmake, "-S", AURORA_SRC, "-B", AURORA_BUILD,
           "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=OFF",
           "-DCMAKE_C_FLAGS=-sUSE_ZLIB=1", "-DCMAKE_CXX_FLAGS=-sUSE_ZLIB=1",
           "-DAURORA_ENABLE_RMLUI=ON", "-DAURORA_CACHE_USE_ZSTD=OFF", "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
           f"-DFETCHCONTENT_SOURCE_DIR_IMGUI={imgui}"]
    cfg += [f"-DFETCHCONTENT_SOURCE_DIR_{k}={deps}/{v}" for k, v in sources.items()]
    env = dict(os.environ, MSYS2_ARG_CONV_EXCL="*")
    subprocess.run(cfg, check=True, env=env)
    subprocess.run([cmake, "--build", AURORA_BUILD, "-j", str(jobs), "--target"] + AURORA_TARGETS,
                   check=True, env=env)


def prepare_wasm_ld_wrapper():
    """An LLVM bin/ mirror whose wasm-ld.exe is tools/wasm_ld_wrap.c (see
    there and tools/wasm_sigfix.py): hardlinks to the real tools, plus a lib/
    junction so clang still finds its resource headers."""
    real_bin = os.path.join(EMSDK, "upstream", "bin")
    root = os.path.join(WASM_DIR, "llvm")
    wrap_bin = os.path.join(root, "bin")
    os.makedirs(wrap_bin, exist_ok=True)
    lib = os.path.join(root, "lib")
    if not os.path.exists(lib):
        subprocess.run(["cmd", "/c", "mklink", "/J", lib, os.path.join(EMSDK, "upstream", "lib")],
                       check=True, capture_output=True)
    for name in os.listdir(real_bin):
        src = os.path.join(real_bin, name)
        dst = os.path.join(wrap_bin, "wasm-ld-real.exe" if name == "wasm-ld.exe" else name)
        if os.path.isfile(src) and not os.path.exists(dst):
            os.link(src, dst)
    wrapper = os.path.join(wrap_bin, "wasm-ld.exe")
    source = os.path.join(TOOLS, "wasm_ld_wrap.c")
    if not os.path.exists(wrapper) or os.path.getmtime(wrapper) < os.path.getmtime(source):
        subprocess.run([b.ZIG, "cc", "-O2", "-target", "x86_64-windows-gnu", source, "-o", wrapper],
                       check=True)
    return wrap_bin


def link_stamp(objs, headless):
    """The savestate build fingerprint (src/os/savestate.c refuses a state
    from any other build): a hash of everything the link consumes."""
    digest = hashlib.sha1()
    for path in sorted(objs) + ([] if headless else aurora_link_items()):
        digest.update(os.path.basename(path).encode())
        with open(path, "rb") as f:
            digest.update(hashlib.sha1(f.read()).digest())
    digest.update(" ".join(link_flags("release", headless)).encode())
    return "web-" + digest.hexdigest()




def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--headless", action="store_true")
    ap.add_argument("--configuration", choices=["debug", "release"], default="release")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--link-only", action="store_true")
    ap.add_argument("--max-failures", type=int, default=40)
    ap.add_argument("--configure-aurora", action="store_true",
                    help="(re)create and build the Emscripten Aurora tree first")
    args = ap.parse_args()

    if not os.path.exists(EMCC[1]):
        print(f"[FATAL] Emscripten not found at {EMSCRIPTEN} (set EMSDK)")
        return 1
    if args.configure_aurora:
        configure_aurora(args.j)
    if not args.headless and not os.path.exists(os.path.join(AURORA_BUILD, "libaurora_core.a")):
        print(f"[FATAL] wasm Aurora not built at {AURORA_BUILD}")
        return 1

    mode = "headless" if args.headless else "web"
    tree = os.path.join(WASM_DIR, mode)
    obj_dir = os.path.join(tree, f"obj-{args.configuration}")
    os.makedirs(obj_dir, exist_ok=True)

    b.patch_headers(dst_root=os.path.join(tree, "patched_include"))
    b.patch_msl_override(dst_dir=os.path.join(tree, "msl_override"),
                         find_real=_emscripten_libc_dir,
                         libc_desc="the Emscripten sysroot/clang-resource headers")
    _guard_musl_math(os.path.join(tree, "msl_override"))
    b.patch_abi_struct_headers(dst_root=os.path.join(tree, "patched_include"),
                               extra_patches=b.ANDROID_HEADER_CONTENT_PATCHES)
    b.apply_patches.apply_all(decomp_root=b.DECOMP)

    base = common_flags(tree, args.configuration)
    units = collect_units(args.headless)
    print(f"Total translation units: {len(units)}")

    a_c = aurora_flags(args.configuration)
    a_cxx = aurora_cxx_flags(args.configuration)

    extra = []

    def flags_for(flavor, src=""):
        if flavor.startswith("aurora"):  # aurora / aurora_cpp_rmlui
            return (a_cxx if src.endswith(".cpp") else a_c) + extra
        return base + extra

    objs, failures, compiled, skipped = [], [], 0, 0
    if args.link_only:
        objs = [os.path.join(obj_dir, u[2]) for u in units]
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.j) as ex:
            for src, obj, ok, log, was_skipped in ex.map(
                    lambda u: compile_unit(u, obj_dir, flags_for), units):
                objs.append(obj)
                if not ok:
                    failures.append((src, log))
                elif was_skipped:
                    skipped += 1
                else:
                    compiled += 1
    print(f"Compiled: {compiled}  Skipped(up-to-date): {skipped}  Failed: {len(failures)}")
    if failures:
        for src, log in failures[:args.max_failures]:
            print(f"\n--- {src} ---\n{log.strip()[:4000]}")
        print(f"\n{len(failures)} file(s) failed to compile. Not linking.")
        return 1

    stamp_src = os.path.join(tree, "mp6_link_stamp.c")
    with open(stamp_src, "w", encoding="utf-8") as f:
        f.write(f'const char mp6_link_stamp[] = "{link_stamp(objs, args.headless)}";\n')
    stamp_obj = os.path.join(obj_dir, "mp6_link_stamp.o")
    proc = subprocess.run(EMCC + ["-c", stamp_src, "-o", stamp_obj], capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stderr)
        return 1

    out_name = "mp6native_headless.js" if args.headless else "mp6native.js"
    # Link into a staging folder and move the finished files in afterwards: the
    # dev server keeps serving this folder, and a device that loads mid-link
    # would otherwise get a new .wasm with the old .js (a LinkError).
    staging = os.path.join(tree, ".staging")
    if os.path.exists(staging):
        _rmtree(staging)
    os.makedirs(staging)
    out = os.path.join(staging, out_name)
    rsp = os.path.join(obj_dir, "link.rsp")
    with open(rsp, "w", encoding="utf-8") as f:
        f.write("\n".join(o.replace("\\", "/") for o in objs + [stamp_obj]))
    link_driver = EMCC if args.headless else EMXX
    extra_items = [] if args.headless else aurora_link_items() + PORT_FLAGS
    lflags = link_flags(args.configuration, args.headless)
    link_map = os.path.join(obj_dir, "link.map")
    cmd = link_driver + ["@" + rsp, "-o", out] + extra_items + lflags + ["-Wl,-Map=" + link_map.replace("\\", "/")]
    # EM_IGNORE_SANITY: the wrapper root differs from the compile-time LLVM
    # root, and emcc would otherwise treat that as a toolchain change and
    # wipe its whole cache (system libraries and ports) on every link.
    env = dict(os.environ, EM_LLVM_ROOT=prepare_wasm_ld_wrapper(), EM_IGNORE_SANITY="1",
               MP6_WASM_SIGFIX=f"{sys.executable}|{os.path.join(TOOLS, 'wasm_sigfix.py')}")
    print(f"Linking {out} ...")
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    link_log = os.path.join(tree, f"link-{args.configuration}.log")
    with open(link_log, "w", encoding="utf-8") as f:
        f.write(proc.stdout + proc.stderr)
    mismatches = proc.stderr.count("function signature mismatch")
    print(proc.stderr[-4000:])
    print(f"Link log: {link_log} ({mismatches} signature mismatch warning(s))")
    if proc.returncode != 0:
        print("LINK FAILED")
        return 1
    stem = os.path.splitext(out_name)[0]
    for name in os.listdir(staging):
        if name.startswith(stem):
            src, dst = os.path.join(staging, name), os.path.join(tree, name)
            for attempt in range(10):
                try:
                    os.replace(src, dst)
                    break
                except PermissionError:
                    # Windows: the dev server may have the old file open
                    # (no delete sharing); retry, then overwrite in place.
                    if attempt == 9:
                        shutil.copyfile(src, dst)
                        os.remove(src)
                    else:
                        time.sleep(0.3)
    out = os.path.join(tree, out_name)
    state_layout = b.state_ranges(link_map, obj_dir)
    b.patch_state_ranges(os.path.join(tree, stem + ".wasm"), state_layout)
    print(f"Savestate layout: {len(state_layout)} game data range(s)")
    if not args.headless:
        for name in ("index.html", "manifest.webmanifest", "sw.js"):
            shutil.copy2(os.path.join(NATIVE_ROOT, "packaging", "pwa", name), os.path.join(tree, name))
        shutil.copytree(os.path.join(NATIVE_ROOT, "packaging", "pwa", "icons"), os.path.join(tree, "icons"),
                        dirs_exist_ok=True)
        # The home-screen icon: iOS's Add to Home Screen showed its letter
        # fallback instead of fetching icons/icon-180.png from this server, so
        # the page carries the 180x180 icon inline (nothing to fetch) and the
        # default /apple-touch-icon.png path iOS probes exists as well.
        touch_icon = os.path.join(NATIVE_ROOT, "packaging", "pwa", "icons", "icon-180.png")
        shutil.copy2(touch_icon, os.path.join(tree, "apple-touch-icon.png"))
        with open(touch_icon, "rb") as f:
            touch_uri = "data:image/png;base64," + base64.b64encode(f.read()).decode("ascii")
        page = os.path.join(tree, "index.html")
        with open(page, encoding="utf-8") as f:
            html = f.read()
        link = '<link rel="apple-touch-icon" sizes="180x180" href="icons/icon-180.png">'
        if link not in html:
            raise SystemExit("index.html: apple-touch-icon link not found")
        with open(page, "w", encoding="utf-8", newline="\n") as f:
            f.write(html.replace(link, f'<link rel="apple-touch-icon" sizes="180x180" href="{touch_uri}">'))
        # The service worker serves the installed app from its cache and
        # checks this in the background: a new value means a new build.
        digest = hashlib.sha256()
        for name in ("index.html", "manifest.webmanifest", "mp6native.js", "mp6native.wasm", "mp6native.data"):
            with open(os.path.join(tree, name), "rb") as f:
                digest.update(hashlib.sha256(f.read()).digest())
        with open(os.path.join(tree, "version.json"), "w", encoding="utf-8") as f:
            json.dump({"v": digest.hexdigest()[:16],
                       "bytes": sum(os.path.getsize(os.path.join(tree, n)) for n in ("mp6native.js", "mp6native.wasm", "mp6native.data"))}, f)
    print(f"Built {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
