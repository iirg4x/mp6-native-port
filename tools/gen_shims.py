#!/usr/bin/env python3
"""
gen_shims.py -- null-platform SDK shim generator.

Reads port/planning/sdk_surface.json (the SDK call inventory) and the
decomp's own include/dolphin + include/msm headers, and emits
platform/null/shims_generated.c (--headless build) and
platform/null/shims_generated_aurora.c (default aurora build; tools/
build.py picks exactly one, per PLATFORM_SOURCES_COMMON -- see
OUT_FILE_AURORA below for why there must be two files at all): one
logging no-op function per SDK symbol that isn't hand-written or
provided for real elsewhere.

Design notes:

  - Macro-mediated call sites (e.g. VECSubtract -> PSVECSubtract when
    MTX_USE_PS is defined, the mtx.h default) are resolved to the real
    linked name via MACRO_RESOLUTION below; the macro name itself never
    reaches the compiled object so it gets no shim.
  - Pure-arithmetic / struct-field-write macros (OSRoundUp32B, PADButtonDown,
    CARDSetIconSpeed, ...) are pure preprocessor expansions with no function
    call at all -- MACRO_NO_SHIM_NEEDED lists them so the generator doesn't
    go looking for a nonexistent function.
  - A curated MANUAL_SYMBOLS set is excluded from generation entirely,
    because those symbols get real (non-stub) behavior hand-written in
    platform/null/shims_manual.c, platform/os/arena.c, etc. -- see that
    list for the reasoning per symbol.
  - Every remaining symbol's prototype is located in the header tree by a
    best-effort regex scan (handles multi-line prototypes and function-
    pointer parameters). The ENTIRE parameter-list text is copied verbatim
    from the header into the generated definition (parameter names, if
    present in the header, are reused as-is; C permits unnamed parameters
    in a definition too, so headers with bare types still work).
  - Symbols found only as a `static inline` header definition are skipped
    (the header already provides a working body).
  - Symbols not found anywhere get a K&R-style `int NAME() {}` fallback
    with an explanatory NOTE comment -- in practice only the header-less
    gsapi_* speech-engine calls.
"""
import json
import os
import re
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
NATIVE_ROOT = os.path.dirname(SCRIPT_DIR)                      # .../port/mp6-native
PORT_ROOT = os.path.dirname(NATIVE_ROOT)                       # .../port
SDK_SURFACE_JSON = os.path.join(PORT_ROOT, "planning", "sdk_surface.json")
AURORA_SURFACE_JSON = os.path.join(PORT_ROOT, "planning", "aurora_surface.json")
DECOMP_INCLUDE = os.path.join(PORT_ROOT, "..", "external_refs", "repos", "marioparty6", "include")
OUT_FILE = os.path.join(NATIVE_ROOT, "platform", "null", "shims_generated.c")
# A SECOND generated file, used by the default (aurora) build INSTEAD of
# OUT_FILE (tools/build.py picks exactly one, per PLATFORM_SOURCES_COMMON).
# Why a second file rather than just marking every stub in OUT_FILE
# `__attribute__((weak))` and letting Aurora's real, strong symbols
# silently win: empirically verified that this does NOT work on this
# toolchain. clang/lld's COFF "weak external" representation is an
# UNDEFINED symbol carrying an embedded DEFAULT, resolved against whatever
# ELSE is already linked -- but archive members (Aurora's
# libaurora_{core,gx,vi}.a) are only ever EXTRACTED to satisfy a plain,
# non-weak undefined reference. With a shim generated for every SDK
# symbol, EVERY reference to e.g. GXBegin from decomp's own .o files is
# accompanied by a weak-external one from shims_generated.o -- no PLAIN
# reference is left anywhere to force libaurora_gx.a's GXGeometry.cpp.obj
# to be extracted, so decomp's calls silently resolve to the logged no-op
# default and never reach Aurora at all (symptom: [SDK] GX.* log lines
# keep firing for names that are genuinely in aurora_surface.json's
# "implemented" list). Hence: for the symbols Aurora actually implements,
# generate NO shim at all in the aurora variant -- decomp's own plain
# reference is then the ONLY one in the whole link, which correctly
# forces the real archive member's extraction.
OUT_FILE_AURORA = os.path.join(NATIVE_ROOT, "platform", "null", "shims_generated_aurora.c")

sys.path.insert(0, SCRIPT_DIR)
import build as B  # noqa: E402 -- reuses ZIG/COMMON_FLAGS/header patching so the probe
                    # compile sees EXACTLY what the real build sees (same -D/-I/-include)

# ---------------------------------------------------------------------------
# Macro handling (see sdk_surface.json -> macro_mediated_calls, and
# sdk_surface.md section "Most of the MTX family is preprocessor aliasing").
# ---------------------------------------------------------------------------

# Macros that expand to pure arithmetic or a direct struct-field write --
# zero runtime/function-call footprint, so no shim is ever needed for them.
MACRO_NO_SHIM_NEEDED = {
    "OSRoundUp32B", "OSRoundDown32B", "OSTicksToMilliseconds", "OSMillisecondsToTicks",
    "PADButtonDown",
    "MTXDegToRad", "MTXRadToDeg", "MTXRotDeg", "MTXRotAxisDeg",
    "CARDSetIconSpeed", "CARDSetIconFormat", "CARDSetCommentAddress",
    "CARDSetIconAddress", "CARDSetBannerFormat", "CARDSetIconAnim",
}

# Macros that resolve (via mtx.h's MTX_USE_PS default, or a plain wrapper
# #define) to a different real function -- shim the RESOLVED name instead.
MACRO_RESOLUTION = {
    "OSAlloc": "OSAllocFromHeap",
    "DVDRead": "DVDReadPrio",
    "DVDReadAsync": "DVDReadAsyncPrio",
    "VECSubtract": "PSVECSubtract",
    "VECScale": "PSVECScale",
    "VECSquareMag": "PSVECSquareMag",
    "MTXMultVec": "PSMTXMultVec",
    "MTXConcat": "PSMTXConcat",
    "VECMag": "PSVECMag",
    "MTXLookAt": "C_MTXLookAt",
    "MTXTrans": "PSMTXTrans",
    "MTXRotRad": "PSMTXRotRad",
    "MTXLightPerspective": "C_MTXLightPerspective",
    "MTXInverse": "PSMTXInverse",
    "MTXPerspective": "C_MTXPerspective",
    "MTXCopy": "PSMTXCopy",
    "MTXInvXpose": "PSMTXInvXpose",
    "VECDistance": "PSVECDistance",
    "VECHalfAngle": "C_VECHalfAngle",
    "MTXMultVecArray": "PSMTXMultVecArray",
    "MTXRotAxisRad": "PSMTXRotAxisRad",
    # mtx.h aliases these exactly like the ones above; they just aren't
    # called out by name in sdk_surface.json's macro_mediated_calls summary
    # (that list is a curated highlight, not exhaustive), so they're listed
    # here directly.
    "MTXIdentity": "PSMTXIdentity",
    "MTXScale": "PSMTXScale",
    "MTXOrtho": "C_MTXOrtho",
    "VECAdd": "PSVECAdd",
    "VECDotProduct": "PSVECDotProduct",
    "VECNormalize": "PSVECNormalize",
    "VECCrossProduct": "PSVECCrossProduct",
    # shim/include/dolphin_compat.h's own `#define GXSetArray
    # mp6_GXSetArray3` (see its comment there for the full story -- short
    # version: decomp's real 3-arg GXSetArray usage can't share a
    # compiled name with Aurora's real, differently-shaped 5-arg
    # GXSetArray, and this toolchain has neither a working --wrap nor a
    # safe link-order trick to reconcile that any other way). The --headless
    # build's own null shim needs to be generated under the RESOLVED name
    # too, matching what decomp's renamed call sites actually link against.
    "GXSetArray": "mp6_GXSetArray3",
    # shim/include/dolphin_compat.h's own `#define GXBegin mp6_GXBegin` /
    # `#define GXEnd mp6_GXEnd` -- a PURE rename (unlike GXSetArray3's
    # arity bridge above) that lets platform/gx/aurora_bridge.c track
    # GXBegin/GXEnd open/close balance and log the call site the moment an
    # imbalance is detected. The --headless build's own null shim needs to
    # be generated under the RESOLVED name too, matching what decomp's
    # renamed call sites actually link against.
    "GXBegin": "mp6_GXBegin",
    "GXEnd": "mp6_GXEnd",
    # shim/include/dolphin_compat.h's own `#define GXCallDisplayList
    # mp6_GXCallDisplayList` -- same PURE-rename shape as GXBegin/GXEnd
    # above, so the draw-call bisect harness (MP6_SKIP_DRAWS) can
    # count/hide display-list-execute draws too, not just immediate-mode
    # GXBegin/GXEnd ones.
    "GXCallDisplayList": "mp6_GXCallDisplayList",
    # shim/include/dolphin_compat.h's own
    # `#define GXBeginDisplayList mp6_GXBeginDisplayList` /
    # `#define GXEndDisplayList mp6_GXEndDisplayList` -- same PURE-rename
    # shape as GXCallDisplayList above, so aurora_bridge.c can track when
    # the game is recording a display list (its learned-size GXSetArray
    # machinery must not emit re-binds into the DL being recorded).
    "GXBeginDisplayList": "mp6_GXBeginDisplayList",
    "GXEndDisplayList": "mp6_GXEndDisplayList",
}

# Symbols hand-written elsewhere with REAL (non-stub) behavior -- excluded
# from generation so there's no duplicate-symbol link error. See
# platform/null/shims_manual.c, platform/os/arena.c, platform/os/jmp_native.c,
# platform/os/dll_bridge.c, platform/os/malloc_direct.c for the definitions;
# each group's comment below explains why it must be manual.
MANUAL_SYMBOLS = {
    # varargs passthrough -- this IS the game's own diagnostic channel
    "OSReport", "OSWarn",
    # NOT hand-written at all -- src/game/fault.c provides a REAL OSPanic
    # (draws an on-screen error via its own framebuffer text renderer,
    # then calls PPCHalt()) that would duplicate-symbol-clash with a
    # shim. Excluded from generation so the game's own definition is the
    # only one; see PPCHalt below for how the actual process-exit happens.
    "OSPanic",
    # Likewise game/THPDraw.c provides REAL implementations of these 3
    # (game-side wrappers around lower-level dolphin GX/THP calls, same
    # pattern sdk_surface.md calls out for THPSimple's THP* wrappers) --
    # not SDK stubs, would also duplicate-symbol-clash if generated.
    "THPGXRestore", "THPGXYuv2RgbSetup", "THPGXYuv2RgbDraw",
    # real incrementing clock / tick shims (RTC-seeded, ticks since
    # 2000-01-01), plus a REAL OSTicksToCalendarTime -- the save-file tags
    # render calendar dates from it
    "OSGetTick", "OSGetTime", "OSGetTick64", "OSGetTime64",
    "OSTicksToCalendarTime",
    # heap/arena family -- backed by platform/os/arena.c's real bump allocator
    "OSGetArenaLo", "OSGetArenaHi", "OSSetArenaLo", "OSSetArenaHi",
    "OSInitAlloc", "OSCreateHeap", "OSSetCurrentHeap", "OSAllocFromHeap",
    "OSAllocFixed", "OSCheckHeap", "OSFreeToHeap", "OSFree",
    "OSDumpHeap",
    # VI frame-pacing / exit-after-N-ticks bridge
    "VIWaitForRetrace", "VIGetRetraceCount", "VIGetNextField", "VIInit",
    # GX bring-up needs a plausible non-null fifo pointer
    "GXInit",
    # REL loader bridge (fakes bootDll/selmenuDll/fileseldll as already-linked)
    "OSLink", "OSUnlink",
    # DVD: special-cased for the 3 synthetic REL paths, else "file not found"
    "DVDOpen", "DVDReadAsyncPrio", "DVDReadPrio", "DVDClose", "DVDGetDriveStatus",
    "DVDFastOpen", "DVDCancel", "DVDCancelAsync", "DVDGetCommandBlockStatus",
    "DVDConvertPathToEntrynum",
    # ARAM: synchronous fake-DMA-completion (platform/os/arena.c's ARAM buffer)
    "ARQPostRequest", "ARInit", "ARCheckInit", "ARGetSize", "ARQInit",
    # Real hardware never returns from this; src/game/fault.c's own
    # OSPanic (see above) calls it as its actual termination step, so it
    # needs to really end the process, not log-and-return like every
    # other CACHE/PPC* no-op.
    "PPCHalt",
    # THP full-motion-video playback is out of scope for this port.
    # THPInit is the one master gate every reachable THP entry point
    # (game/THPSimple.c's THPSimpleInit) unconditionally checks FIRST,
    # before touching DVD or parsing anything. Left to the generator it
    # would only ever be the "no prototype found" K&R fallback
    # (include/dolphin/thp/THPPlayer.h isn't reached by dolphin/thp.h's
    # own umbrella chain), whose `return 0`/FALSE happens to gate
    # correctly only by accident. Hand-written instead (with the real
    # `BOOL THPInit(void)` signature) so the intent -- and the residual
    # risk it does NOT cover -- is explicit; see platform/null/
    # shims_manual.c's own comment.
    "THPInit",
    # Real streamed-music playback, hand-written in
    # platform/audio/msm_bridge.c (its own file rather than growing
    # platform/null/shims_manual.c) -- excluded from generation here so
    # there's no duplicate-symbol clash. msmSysInit brings up the real
    # .pdt parser + audio backend; msmSysRegularProc is the per-frame
    # mixer pump (--headless only, see msm_bridge.c); the 8
    # msmStream*/AI* entries are the actual playback control API
    # game/audio.c's HuAudSStream*/HuAudBGM*/HuAudJingle* wrappers call.
    # See msm_bridge.c's own header comment for the full scope rationale.
    "msmSysInit", "msmSysRegularProc",
    "msmStreamPlay", "msmStreamStop", "msmStreamPauseAll", "msmStreamPause",
    "msmStreamSetParam", "msmStreamGetStatus", "msmStreamStopAll", "msmStreamSetMasterVolume",
    "AIGetDMAStartAddr", "AIInitDMA", "AIRegisterDMACallback",
    "AISetStreamPlayState", "AISetStreamVolLeft", "AISetStreamVolRight", "AIStartDMA",
    # Real SFX playback via the SEPARATE MP6_SND.msm bank (see
    # msm_bridge.c's header comment for the scope).
    # Deliberately NOT included: msmMus* (MIDI-sequencer music, out of
    # scope) and the 3D-positional-emitter SE family (msmSeSetListener/
    # msmSeUpdataListener/msmSeDelListener/msmSeGetEntryID/msmSeGetIndexPtr/
    # msmSeGetNumPlay) -- those stay the auto-generated logging no-op (no
    # reached call site uses them).
    "msmSePlay", "msmSeStop", "msmSeStopAll", "msmSeGetStatus",
    "msmSePauseAll", "msmSeSetParam", "msmSeSetMasterVolume",
    # REAL dynamic .msm group load/unload in msm_bridge.c. Left as no-op
    # stubs, per-scene SE groups are never actually loaded (boot.c's
    # HuAudSndGrpSetSet(MSM_GRP_MENU), objmain.c's HuAudDllSndGrpSet(overlay)
    # on every scene switch) and every SE id in those groups fails silently
    # with msm error -122.
    # msmSysSetOutputMode/msmSysSetAux/msmSysCheckInit remain generated
    # no-ops (aux effect buses / output modes stay out of scope).
    "msmSysLoadGroup", "msmSysLoadGroupBase", "msmSysDelGroupAll",
    "msmSysDelGroupBase", "msmSysGetSampSize", "msmSysSetGroupLoadMode",
    # The save-data/CARD gate. A generated stub (a bare `return 0` with
    # the memSize/sectorSize/CARDStat* OUT params left untouched) would
    # claim CARD_RESULT_READY -- a card successfully probed/mounted --
    # while never populating any of the data a real READY result promises
    # the caller. game/card.c's HuCardSlotCheck/HuCardMount read those
    # uninitialized locals right back (e.g. HuCardSectorSizeGet's
    # `u32 sectorSize;` declared-but-never-written), so
    # REL/fileseldll/saveload.c's FileCardMount compares GARBAGE stack
    # bytes against the real GC sector size (0x2000) and -- depending on
    # whatever happens to be on the stack -- unpredictably lands on its
    # own "this card can't be used with this game" (CARD_RESULT_WRONGDEVICE-
    # shaped) error path instead of the clean, standard, extremely common
    # "no memory card in this slot" one real hardware shows when a slot is
    # genuinely empty. Hand-written to honestly return CARD_RESULT_NOCARD
    # (-3) -- every caller in game/card.c already returns early on a
    # negative result without touching any output parameter, so this both
    # avoids the uninitialized-read hazard AND routes fileseldll down its
    # own already-correct, already-tested "no save card" UI instead of a
    # data-dependent wrong one. See platform/null/shims_manual.c's own
    # comment for the full call-chain trace.
    "CARDProbeEx", "CARDCheck", "CARDMount", "CARDGetSectorSize",
    # The same "stub dishonestly claims success" shape as the CARD gate
    # directly above, this time for the bongos/microphone peripheral. A
    # generated MICProbeEx stub would claim MIC_RESULT_READY (0)
    # unconditionally -- "a microphone IS plugged in and was successfully
    # probed" -- which this port cannot honor (no mic hardware is ever
    # emulated). game/mic.c's HuMCProbe loops on MICProbeEx until it sees
    # anything other than exactly -1 (MIC_RESULT_BUSY) and returns that
    # value straight through; every one of its 3 real call sites
    # (REL/fileseldll/filesel.c x2, game/mic.c) is a plain
    # `if (HuMCProbe(1) != 0) { <safe, mic-less path> } else { <full mic
    # init/mount> }` gate -- with a dishonest 0/READY default, EVERY
    # caller takes the FULL init branch instead, reaching HuMCInit ->
    # MCDVDRead -> memcpyFast, a real, deterministic access violation on
    # data no shim provides. Honest instead: MIC_RESULT_NOCARD (-3, the
    # same real SDK constant name CARD uses, dolphin/mic.h) for
    # MICProbeEx -- HuMCProbe's own retry loop only continues past BUSY
    # (-1), so a -3 returns immediately, no 500ms busy-loop tax either.
    # MICMount alongside it, matching the CARD gate's own
    # defense-in-depth reasoning (every current call site is already
    # probe-gated, so this alone isn't reachable today, but costs nothing
    # and protects a future direct caller).
    "MICProbeEx", "MICMount",
}

# Symbols whose RESOLVED (post-MACRO_RESOLUTION) name platform/gx/
# aurora_bridge.c defines directly and unconditionally for the aurora
# build (real logic, not a plain passthrough to an Aurora symbol of the
# exact same name -- see that file's own comment for mp6_GXSetArray3's
# full story). Excluded from the AURORA variant only, same reasoning as
# aurora_covered in main(): even a weak duplicate is unnecessary risk once
# something else already provides the real definition unconditionally.
# Still generated in the --headless variant (aurora_bridge.c isn't even
# compiled there), same as any other MACRO_RESOLUTION target.
#
# The MTX/VEC names below are a PLAIN rename bridge, not new logic --
# decomp's own header resolves MTXFoo/PSMTXFoo calls to a `PSMTX*`/
# `PSVEC*` symbol name Aurora's real (linked, see tools/build.py's
# AURORA_LINK_ITEMS) MTX library never defines under that literal name
# (only the generic `C_MTXFoo`/`C_VECFoo` equivalent) -- see platform/gx/
# aurora_bridge.c's header comment for the full story and how this
# differs from the names recorded in aurora_surface.json's
# implemented.MTX list instead (those ARE exported under the exact name
# decomp needs, no rename required, so ordinary aurora_covered exclusion
# is enough for them).
AURORA_HAND_BRIDGED = {
    "mp6_GXSetArray3",
    "PSMTXIdentity", "PSMTXCopy", "PSMTXConcat", "PSMTXInverse", "PSMTXInvXpose",
    "PSMTXReorder", "PSMTXTrans", "PSMTXScale", "PSMTXRotRad", "PSMTXRotAxisRad",
    "PSMTXMultVec", "PSMTXMultVecArray", "PSMTXMultVecSR", "PSMTXROMultVecArray",
    "PSVECAdd", "PSVECSubtract", "PSVECScale", "PSVECDotProduct", "PSVECCrossProduct",
    # GXBegin/GXEnd balance-diagnostic wrappers (see dolphin_compat.h and
    # aurora_bridge.c) -- aurora_bridge.c unconditionally defines both for
    # the aurora build, same reasoning as every other entry in this set.
    "mp6_GXBegin", "mp6_GXEnd",
    # The draw-call bisect harness's display-list-execute hook, same
    # reasoning as mp6_GXBegin/mp6_GXEnd immediately above.
    "mp6_GXCallDisplayList",
    # Display-list recording-bracket wrappers, same reasoning as
    # mp6_GXCallDisplayList immediately above.
    "mp6_GXBeginDisplayList", "mp6_GXEndDisplayList",
}

FAMILY_ORDER = [
    "OS", "GX", "MTX", "VI", "DVD", "CACHE", "CARD", "PAD", "MSM", "AI",
    "AR", "THP", "SI", "LC", "MUSYX", "DSP", "EXI",
]

# Symbols NOT in sdk_surface.json at all -- that inventory's own
# methodology scoped to "include/ headers excluding include/game" reached
# via dolphin.h's own umbrella chain, which never includes dolphin/mic.h,
# dolphin/m2s.h, or dolphin/demo/DEMOStats.h (same story as dolphin/thp.h,
# already probed for above), so game/mic.c's and game/init.c's calls into
# these families only ever surface as link-time undefined symbols. MIC
# (GameCube microphone peripheral, several voice-recognition minigames)
# and M2S have real headers (probed for below, so the normal generator
# path finds real prototypes); the gsapi_* calls are a 3rd-party
# speech-recognition engine with NO header anywhere in the tree
# (grep -rl "gsapi_" include/ -- zero hits) -- these fall through to the
# K&R-fallback path further down exactly like any other unresolved
# symbol.
EXTRA_NEEDED_SYMBOLS = {
    "MIC": ["MICGetButton", "MICGetDeviceID", "MICGetSamples", "MICGetSamplesLeft",
            "MICInit", "MICIsActive", "MICMount", "MICProbeEx", "MICSetGain",
            "MICStart", "MICStop", "MICUnmount", "MICUpdateIndex"],
    "M2S": ["M2SClose", "M2SGetSamples", "M2SGetSamplesLeft", "M2SAdvanceBuffer",
            "M2SInit", "M2SOpen", "M2SSetActiveChannel", "M2SSetBuffer", "M2SSetMode",
            "M2SSetPrerecordSamples", "M2SSetShifts", "M2SStart", "M2SStop"],
    "DEMO": ["DEMOPrintStats", "DEMOUpdateStats"],
    "OS": ["gsapi_Close", "gsapi_ContextActivate", "gsapi_ContextDeActivate",
           "gsapi_ContextSetCtxData", "gsapi_ContextSetGcdData", "gsapi_ContextSetParam",
           "gsapi_ContextSetWrdData", "gsapi_EngineClose", "gsapi_EngineOpen",
           "gsapi_EngineRestart", "gsapi_EngineSessionDataExport", "gsapi_EngineSessionDataFree",
           "gsapi_EngineSessionDataImport", "gsapi_EngineSetMode", "gsapi_EngineSetParam",
           "gsapi_EngineStart", "gsapi_EngineStop", "gsapi_Init", "gsapi_LanguageLoadBuffer",
           "gsapi_LanguageUnLoad", "gsapi_NotifySetCallback", "gsapi_SetUserData"],
}

# Deliberately NOT under BUILD_DIR: empirically, when the main input file
# sits as a sibling of PATCHED_INCLUDE (both directly under BUILD_DIR),
# clang/zig resolves `#include "msm.h"` to the ORIGINAL decomp copy
# instead of the patched shadow in PATCHED_INCLUDE, even though -I lists
# PATCHED_INCLUDE first -- some path-prefix-based quirk in header
# resolution (reproduced multiple ways; moving the main file anywhere
# outside BUILD_DIR's tree reliably avoids it).
PROBE_DIR = os.path.join(NATIVE_ROOT, ".gen_shims_tmp")
PROBE_C = os.path.join(PROBE_DIR, "gen_shims_probe.c")
PROBE_I = os.path.join(PROBE_DIR, "gen_shims_probe.i")


def get_preprocessed_text():
    """Runs the REAL preprocessor (same flags as the real build: same -D,
    -I, -include compat header) over a probe TU that just includes
    dolphin.h + msm.h, and returns the fully macro-/#ifdef-resolved output
    as one string.

    Why preprocess instead of regex-scanning raw header TEXT directly:
    a raw-text scan is blind to conditional compilation and can grab a
    declaration from an inactive `#ifdef TARGET_PC` branch (e.g.
    GXLoadPosMtxImm/GXLoadNrmMtxImm/GXLoadTexMtxImm each have TWO
    incompatible prototypes 2 lines apart, gated on TARGET_PC; several
    GXVert.h vertex functions -- GXEnd, GXColor1x8/1x16, GXPosition1x8/1x16,
    GXNormal1x16 -- are `static inline` MMIO pokes in the #else branch but
    plain extern prototypes in the #ifdef TARGET_PC one), generating a shim
    that conflicts with the header's ACTUALLY-active declaration at real
    compile time. Preprocessing with the exact same flags first means
    whatever this script sees is, by construction, exactly what the real
    compile sees -- no separate conditional-evaluation logic to keep in
    sync by hand.
    """
    os.makedirs(B.BUILD_DIR, exist_ok=True)
    os.makedirs(PROBE_DIR, exist_ok=True)
    B.patch_headers()
    B.patch_msl_override()
    B.patch_abi_struct_headers()  # GXTlutObj/PADStatus ABI-size shadow patch;
                                    # doesn't change any function prototype text, but
                                    # kept here too so the probe sees exactly the same
                                    # PATCHED_INCLUDE tree a real build does.
    # dolphin.h's own umbrella chain doesn't reach dolphin/thp.h, and the
    # msm/*.h implementation-detail headers (msmsys.h, msmmus.h, msmse.h,
    # msmstream.h -- msmSysCheckInit, msmMusFdoutEnd, ... live there, not in
    # the top-level msm.h) aren't pulled in by msm.h either -- both are
    # reached by game code through their own separate #include lines, so
    # the probe needs them explicitly too.
    # game/THPDraw.h and game/msm.h separately declare a few symbols
    # (THPGXRestore/THPGXYuv2RgbSetup/THPGXYuv2RgbDraw, msmSysSetAux) that
    # the top-level dolphin/msm headers don't -- game code reaches them via
    # "game/THPDraw.h" / (quote-search-resolved-to-game/) "msm.h", not the
    # top-level headers of the same short name.
    #
    # Top-level msm.h is listed FIRST deliberately, THEN #undef MSM_H to
    # force game/msm.h to ALSO fully expand despite sharing that guard:
    # the two headers declare ~30 of the same functions with incompatibly
    # different types (see dolphin_compat.h's comment), and
    # platform/null/shims_generated.c's OWN preamble (plain "#include
    # dolphin.h"+"msm.h", from platform/null/ -- not include/game/, so its
    # own "msm.h" always resolves to the TOP-LEVEL one) is what the
    # generated shims actually get compiled against -- generating
    # signatures from whichever header duplicate wins the probe's OWN
    # guard race must match that, or the generated .c conflicts with its
    # own preamble. -E (preprocess-only, no semantic checks) tolerates
    # #include-ing both despite the now-duplicate declarations just fine;
    # find_prototype() takes the FIRST match per name, so this yields
    # top-level msm.h's signature for every overlapping name (matching
    # shims_generated.c's reality) while still discovering game/msm.h's
    # OWN unique additions (msmSysSetAux, not declared in top-level msm.h
    # at all, so there's no first-match ambiguity for it either way).
    with open(PROBE_C, "w", encoding="utf-8", newline="\n") as f:
        f.write(
            "#include \"dolphin.h\"\n"
            "#include \"dolphin/thp.h\"\n"
            # dolphin/mic.h isn't part of dolphin.h's own umbrella include
            # chain either (like dolphin/thp.h) -- game/mic.c (the GameCube
            # microphone peripheral, used by a handful of voice-recognition
            # minigames) reaches it directly. Missed by the ORIGINAL
            # sdk_surface.json analysis entirely (no "MIC" family there),
            # only surfacing as link-time undefined symbols once game/mic.c
            # actually got compiled.
            "#include \"dolphin/mic.h\"\n"
            "#include \"dolphin/m2s.h\"\n"
            "#include \"dolphin/demo/DEMOStats.h\"\n"
            "#include \"msm.h\"\n"
            "#undef MSM_H\n"
            "#include \"game/msm.h\"\n"
            "#include \"game/THPDraw.h\"\n"
            "#include \"msm/msmsys.h\"\n"
            "#include \"msm/msmmus.h\"\n"
            "#include \"msm/msmse.h\"\n"
            "#include \"msm/msmstream.h\"\n"
            "#include \"msm/msmfio.h\"\n"
            "#include \"msm/msmmem.h\"\n"
        )
    # MP6_GENSHIMS_PROBE: dolphin_compat.h renames some SDK identifiers to
    # port interposers (framescope's mp6_fs_GX*, card_native's mp6_CARDInit).
    # The probe must see the PRISTINE SDK declarations or those symbols
    # silently degrade to K&R fallback stubs (caught when a fresh regen
    # produced `int GXCopyTex()` where the committed file had the real,
    # typed prototype). The compat header exempts its rename blocks under
    # this define, which exists ONLY for this probe -- never the real build.
    cmd = [B.ZIG, "cc"] + B.COMMON_FLAGS + ["-DMP6_GENSHIMS_PROBE", "-E", PROBE_C, "-o", PROBE_I]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        print("Probe preprocessing FAILED:")
        print(proc.stdout)
        print(proc.stderr)
        sys.exit(1)
    with open(PROBE_I, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    # Strip GNU line-marker directives (`# <num> "<path>" <flags>`) -- the
    # ONLY `#`-prefixed lines that survive preprocessing into -E output
    # (every real #define/#include/#ifdef is already consumed by this
    # point). Left in, one can end up INSIDE the backward-scan window
    # find_prototype() uses to grab a declaration's return-type text
    # (walking back to the previous `;`/`{`/`}`, which a line marker
    # contains none of), silently splicing garbage like
    # `# 154 "dolphin/mtx.h"` into a generated function's signature/return
    # statement -- genuinely invalid C that still LOOKS plausible enough
    # in a diff to be easy to miss.
    text = re.sub(r"^#.*$", "", text, flags=re.MULTILINE)
    return text


PROTO_CACHE = {}


def find_matching_paren(text, open_idx):
    depth = 0
    i = open_idx
    while i < len(text):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def find_prototype(name, text):
    """Scan the fully preprocessed (conditionals-resolved) text for
    `<rettype> NAME(<params>)`. Returns (kind, rettype, paramtext) where
    kind is 'proto', 'inline', or None if not found anywhere. `rettype` has
    trailing whitespace stripped; `paramtext` is the raw text between the
    outermost parens (verbatim). Since the input is preprocessor OUTPUT,
    each symbol appears at most once (whichever #ifdef branch was
    actually active for this build's flags) -- no risk of picking a
    declaration from an inactive branch."""
    if name in PROTO_CACHE:
        return PROTO_CACHE[name]
    pattern = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"\s*\(")
    result = (None, None, None)
    for m in pattern.finditer(text):
        open_idx = m.end() - 1
        close_idx = find_matching_paren(text, open_idx)
        if close_idx < 0:
            continue
        start = m.start()
        back = start
        while back > 0 and text[back - 1] not in ";{}":
            back -= 1
        rettype_and_mods = text[back:start].strip()
        if not rettype_and_mods:
            continue  # e.g. matched the name used as an argument, not a decl
        paramtext = text[open_idx + 1:close_idx]
        after = text[close_idx + 1:close_idx + 200].lstrip()
        is_inline = ("inline" in rettype_and_mods.split()) or rettype_and_mods.startswith("__inline")
        if after.startswith("{"):
            kind = "inline" if is_inline else "definition"
        elif after.startswith(";"):
            kind = "proto"
        else:
            continue
        rettype = re.sub(r"\b(static|inline|__inline|extern)\b", "", rettype_and_mods).strip()
        if kind in ("inline", "definition") and not is_inline:
            kind = "proto"
        result = (kind, rettype, paramtext)
        if kind == "proto":
            break
    PROTO_CACHE[name] = result
    return result


def default_return(rettype):
    rt = rettype.strip()
    rt = re.sub(r"\s+", " ", rt)
    if rt in ("void",):
        return None
    if rt in ("BOOL",):
        return "1"
    if rt.endswith("*"):
        return "0"
    if rt in ("f32", "f64", "float", "double"):
        return "0"
    return "0"


def make_stub(name, family, rettype, paramtext):
    paramtext = paramtext.strip()
    if paramtext == "" or paramtext == "void":
        params_out = "void"
    else:
        params_out = paramtext
    ret = default_return(rettype)
    lines = []
    # `weak` is kept as cheap defense-in-depth (never observed to be load-
    # bearing on this toolchain -- see OUT_FILE_AURORA's own comment for why
    # the REAL aurora/headless split is a generation-time exclusion, not
    # this attribute) in case some future aurora_surface.json update adds a
    # symbol here that should have been excluded and wasn't: better a silent
    # weak-symbol coexistence than a hard duplicate-symbol link error.
    lines.append(f"__attribute__((weak)) {rettype} {name}({params_out})")
    lines.append("{")
    lines.append(f'    MP6_LOG_ONCE("{family}", "{name}");')
    if ret is not None:
        lines.append(f"    return ({rettype.strip()}){ret};")
    lines.append("}")
    return "\n".join(lines)


def make_unknown_stub(name, family):
    return (
        f"/* TODO: no prototype found in include/dolphin or include/msm for {name}; "
        f"K&R empty-parens fallback accepts any args. Weak (see make_stub's comment). */\n"
        f"__attribute__((weak)) int {name}()\n"
        "{\n"
        f'    MP6_LOG_ONCE("{family}", "{name}");\n'
        "    return 0;\n"
        "}"
    )


def load_aurora_covered_symbols():
    """Returns the set of symbol names (flattened across all families --
    names are globally unique in this SDK) with a real, linkable
    definition somewhere in Aurora, per aurora_surface.json's own
    'implemented' inventory. See OUT_FILE_AURORA's comment for why the
    aurora build variant must NOT generate a shim for any of these."""
    with open(AURORA_SURFACE_JSON, "r", encoding="utf-8") as f:
        aurora = json.load(f)
    covered = set()
    for names in aurora["implemented"].values():
        covered.update(names)
    return covered


def main():
    with open(SDK_SURFACE_JSON, "r", encoding="utf-8") as f:
        data = json.load(f)
    aurora_covered = load_aurora_covered_symbols()

    print("Preprocessing dolphin.h + msm.h with the real build flags...")
    preprocessed = get_preprocessed_text()
    print(f"  ({len(preprocessed)} bytes)")

    needed = {}   # name -> family label (first family wins)
    skipped_macro_inert = []
    skipped_macro_resolved = []
    skipped_manual = []
    skipped_inline = []

    for family in FAMILY_ORDER:
        fam = data["families"].get(family)
        if not fam:
            continue
        for name in fam["symbols"].keys():
            if name in MACRO_NO_SHIM_NEEDED:
                skipped_macro_inert.append(name)
                continue
            target = MACRO_RESOLUTION.get(name)
            if target:
                skipped_macro_resolved.append((name, target))
                name = target
            if name in MANUAL_SYMBOLS:
                skipped_manual.append(name)
                continue
            needed.setdefault(name, family)

    extra_count = 0
    for family, names in EXTRA_NEEDED_SYMBOLS.items():
        for name in names:
            if name in MANUAL_SYMBOLS or name in needed:
                continue
            needed[name] = family
            extra_count += 1

    generated = []          # --headless variant: every needed symbol
    generated_aurora = []   # aurora variant: skips aurora_covered entirely
    unresolved = []
    inline_ok = []
    aurora_excluded = []

    for name in sorted(needed.keys()):
        family = needed[name]
        kind, rettype, paramtext = find_prototype(name, preprocessed)
        if kind == "inline":
            inline_ok.append(name)
            continue
        if kind == "proto":
            stub = make_stub(name, family, rettype, paramtext)
        else:
            unresolved.append(name)
            stub = make_unknown_stub(name, family)
        generated.append(stub)
        if name in aurora_covered or name in AURORA_HAND_BRIDGED:
            aurora_excluded.append(name)
        else:
            generated_aurora.append(stub)

    header = """/* AUTO-GENERATED by tools/gen_shims.py -- DO NOT HAND-EDIT.
 * Regenerate with: python tools/gen_shims.py
 *
 * One logging no-op per SDK symbol not hand-written in
 * platform/null/shims_manual.c / platform/os/*.c -- see this script's own
 * MANUAL_SYMBOLS set for the full list and the per-group reasoning.
 *
 * Used by the --headless build ONLY (tools/build.py) -- every needed
 * symbol gets a shim here, including the ~100+ GX/VI/PAD ones the
 * default (aurora) build instead gets for real from Aurora's own linked
 * libraries (see shims_generated_aurora.c, generated alongside this file,
 * for that build's own variant, and its own header comment for why a
 * SEPARATE file -- not a run-time or weak-symbol distinction -- is what
 * that split needs on this toolchain).
 */
#include "mp6_shim_log.h"
#include "dolphin.h"
#include "dolphin/thp.h"
#include "dolphin/mic.h"
#include "dolphin/m2s.h"
#include "dolphin/demo/DEMOStats.h"
#include "msm.h"

"""
    body = "\n\n".join(generated) + "\n"

    os.makedirs(os.path.dirname(OUT_FILE), exist_ok=True)
    with open(OUT_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
        f.write(body)

    header_aurora = """/* AUTO-GENERATED by tools/gen_shims.py -- DO NOT HAND-EDIT.
 * Regenerate with: python tools/gen_shims.py
 *
 * Used by the DEFAULT (aurora) build ONLY -- see OUT_FILE_AURORA's own
 * comment in gen_shims.py for the full story of why this file excludes
 * every symbol aurora_surface.json's 'implemented' inventory says Aurora
 * provides a real, linkable definition for (confirmed empirically:
 * generating a shim for these, even a weak one, silently prevents
 * Aurora's own real implementation from ever being linked in at all --
 * COFF/lld-link's weak-external resolution never triggers archive
 * extraction on its own). Everything else -- the true GX/VI gaps Aurora
 * has no definition for anywhere -- still gets a shim here, identical to
 * shims_generated.c's own (still logging).
 */
#include "mp6_shim_log.h"
#include "dolphin.h"
#include "dolphin/thp.h"
#include "dolphin/mic.h"
#include "dolphin/m2s.h"
#include "dolphin/demo/DEMOStats.h"
#include "msm.h"

"""
    body_aurora = "\n\n".join(generated_aurora) + "\n"
    with open(OUT_FILE_AURORA, "w", encoding="utf-8", newline="\n") as f:
        f.write(header_aurora)
        f.write(body_aurora)

    print(f"Total SDK symbols in sdk_surface.json: {data['totals']['total_symbols']}")
    print(f"Extra symbols (outside sdk_surface.json, found via link errors): {extra_count}")
    print(f"Macro (no shim needed, pure arithmetic/field-write): {len(skipped_macro_inert)}")
    print(f"Macro (resolved to a real target): {len(skipped_macro_resolved)}")
    print(f"Manual (hand-written elsewhere): {len(set(skipped_manual))}")
    print(f"Generated (found real prototype): {len(generated) - len(unresolved)}")
    print(f"Generated but UNRESOLVED (K&R fallback, no header proto found): {len(unresolved)}")
    if unresolved:
        print("  Unresolved:", ", ".join(unresolved))
    print(f"Skipped (already `static inline` in a header, no shim needed): {len(inline_ok)}")
    if inline_ok:
        print("  Inline-ok:", ", ".join(inline_ok))
    print(f"Aurora-covered (excluded from the aurora variant only, real Aurora symbol used instead): "
          f"{len(aurora_excluded)}")
    if aurora_excluded:
        print("  Aurora-covered:", ", ".join(sorted(aurora_excluded)))
    print(f"Wrote {OUT_FILE} ({len(generated)} symbols)")
    print(f"Wrote {OUT_FILE_AURORA} ({len(generated_aurora)} symbols)")


if __name__ == "__main__":
    sys.exit(main())
