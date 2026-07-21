/* MP6 native port -- Aurora integration bridge (default/non-headless
 * build only; see tools/build.py's --headless flag).
 *
 * Compiled against AURORA's OWN dolphin/aurora headers (AURORA_FLAGS in
 * tools/build.py: -I external_refs/repos/aurora/include + its transitive
 * dep include dirs), NEVER the decomp's include/ tree and NEVER
 * shim/include/dolphin_compat.h -- keeping this file's header universe
 * strictly on the Aurora side is what guarantees GXAttr/BOOL/u32/etc. here
 * mean exactly what Aurora's compiled libaurora_gx.a/libaurora_vi.a expect,
 * with zero risk of accidentally resolving a decomp copy of a same-named
 * header instead (both trees have `dolphin/gx/GXGeometry.h` at the same
 * relative path; -I order is deterministic per-TU, not per-#include, so
 * mixing both -I roots in one translation unit would silently pick
 * whichever tree happens to be listed first for EVERY `#include
 * <dolphin/...>` in this file, not just the ones we intend).
 *
 * Five jobs:
 *   1. GXSetArray arity bridge (a genuine signature drift between decomp's
 *      and Aurora's copy of dolphin/gx/GXGeometry.h -- see the comment
 *      above mp6_GXSetArray3 below).
 *   2. The VI frame-pacing bridge: decomp's game/main.c calls
 *      VIWaitForRetrace() once per main-loop iteration (the classic
 *      GameCube "wait for vblank" idiom); Aurora has no such call at all
 *      (aurora_surface.md: VIWaitForRetrace is one of the 8 VI gaps) --
 *      its equivalent is aurora_update() + aurora_begin_frame()/
 *      aurora_end_frame(), a different (event-pump-then-maybe-skip-frame)
 *      shape. This file is the adapter between the two: every
 *      VIWaitForRetrace() call ends the frame the PREVIOUS call opened,
 *      pumps SDL/Aurora events (a window-close request becomes a clean
 *      process exit), fires the game's registered pre/post-retrace
 *      callbacks (game/pad.c's PadReadVSync -- the game's actual per-frame
 *      controller sample point -- is one of these), then opens the next
 *      frame.
 *   3. The MTX/VEC rename bridge: Aurora ships a real, complete
 *      MTX/VEC/QUAT library (`lib/dolphin/mtx/*.c`), and the game must
 *      link against IT, never against generated no-op shims (a no-op
 *      matrix function silently produces a black screen the moment a draw
 *      depends on a computed matrix). Two sub-cases:
 *      (a) 9 names (`C_MTXOrtho`, `C_MTXPerspective`, `C_MTXLookAt`,
 *          `C_MTXLightPerspective`, `C_VECHalfAngle`, `PSVECDistance`,
 *          `PSVECMag`, `PSVECNormalize`, `PSVECSquareMag`) are symbols
 *          Aurora's library exports under the EXACT name decomp's own
 *          header resolves calls to -- listed in
 *          `../planning/aurora_surface.json`'s `implemented.MTX`
 *          (matching the GX/VI/PAD mechanism), no bridge code needed here
 *          at all, just linking `libaurora_mtx.a` and excluding them from
 *          shim generation.
 *      (b) 19 names decomp needs under a `PSMTX*`/`PSVEC*` spelling (the
 *          real, paired-single-optimized name in decomp's OWN header --
 *          `MTXFoo` is just a macro alias TO `PSMTXFoo` there) have NO
 *          symbol under that exact name anywhere in Aurora's library at
 *          all -- Aurora only ever defines the generic `C_MTXFoo`/
 *          `C_VECFoo` equivalent. These get a real, one-line,
 *          unconditional rename wrapper below (`gen_shims.py`'s
 *          `AURORA_HAND_BRIDGED` excludes these same 19 names from shim
 *          generation, matching `mp6_GXSetArray3`'s own precedent).
 *          `Mtx`/`Vec`/`ROMtx`/`Point3d`/`Quaternion` are plain float
 *          arrays/structs with no pointer members, byte-identical between
 *          decomp's and Aurora's copies of `dolphin/mtx/GeoTypes.h` -- no
 *          32-on-64-bit ABI risk the way `GXTlutObj`/`GXTexObj`/
 *          `PADStatus` had, so a plain pass-through is safe.
 *   4. GXBegin/GXEnd hardware-faithful tolerance: a PURE rename, same
 *      shape as (b) above, NOT an arity bridge like (1) -- decomp's and
 *      Aurora's GXBegin/GXEnd signatures already match exactly. Real,
 *      unpatched decomp code (e.g. game/hsfman.c's Hu3DZClear) never
 *      calls GXEnd() for some primitives because real hardware's GX FIFO
 *      ends a primitive by vertex count, not an explicit End signal --
 *      but Aurora does NOT infer completion from vertex count and FATALs
 *      on "GXBegin: called without matching GXEnd". Tracks a single
 *      open/close boolean and auto-closes (calling the REAL GXEnd()) any
 *      still-open primitive before the next GXBegin or at frame-end --
 *      see section 5 below for the full story, including the one case
 *      (game/sprput.c's HuSpr3DDisp) where this general tolerance wasn't
 *      enough on its own and needed a companion decomp patch too.
 *   5. Draw-call bisect harness: `MP6_SKIP_DRAWS
 *      "lo-hi"` hides a specific, index-addressed, per-frame range of draws
 *      (both immediate-mode GXBegin/GXEnd primitives and GXCallDisplayList
 *      display-list replays -- section 7 below) by bracketing the real,
 *      unmodified call with a zero-area GXSetScissor, NOT by skipping the
 *      call itself -- deliberately chosen so nothing about vertex-count/
 *      FIFO-buffer bookkeeping (section 5's own hard-won subject) is ever
 *      disturbed. A pure empirical elimination tool ("which draw call
 *      paints these exact pixels"), absent/unset by default with zero
 *      overhead beyond one counter increment per draw.
 *
 * Every other GX/VI/PAD/MTX/VEC symbol decomp calls (the ~100 direct-link
 * GX functions -- TEV, lighting, vertex submission, texture setup, matrix
 * loading, ...; VIInit/VIConfigure/VIGetTvFormat/VIFlush; all 7 real PAD
 * functions; the 9 MTX/VEC names in (a) above) needs NO bridge code at
 * all: it links straight against Aurora's compiled, real definition of
 * the same name. This is NOT a weak-symbol override (weak-vs-archive
 * resolution does not behave as needed on this toolchain: the linker
 * takes an already-seen weak definition over an archive member) --
 * tools/gen_shims.py's shims_generated_aurora.c (used only by the
 * default/aurora build, in place of shims_generated.c) simply never
 * generates a shim at all for any of these names, so decomp's own plain
 * reference is the ONLY one in the link and correctly pulls in Aurora's
 * real archive member. The ~15 GX + 8 VI gap symbols Aurora truly has no
 * definition for at all still get a (weak, but uncontested) logging
 * no-op from that same shims_generated_aurora.c.
 */
#include <aurora/aurora.h>
#include <aurora/event.h>

#include <dolphin/gx.h>
#include <dolphin/vi.h>
#include <dolphin/mtx.h>
#include <dolphin/pad.h>

#include "mp6_boot.h"
#include "mp6_shim_log.h"
#include "mp6_gxarray_registry.h"
#include "mp6_widescreen.h" /* WS2 (docs/WS2_DYNAMIC_WIDESCREEN.md): dynamic true-widescreen */
#include "mp6_savestate.h"  /* F5/F8 hotkeys -> queued, serviced at the frame boundary */
#include "host.h" /* mp6_host_monotonic_ns/mp6_host_sleep_ns/mp6_host_init
                   * -- the tick throttle's OS primitives
                   * (QPC/Sleep/timeBeginPeriod) live behind the host
                   * seam; the throttle's absolute-deadline math below is
                   * platform-independent. */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h> /* Section 6 below (console repositioning:
                      * GetConsoleWindow/GetConsoleProcessList/GetWindowRect/
                      * SetWindowPos) ONLY, plus its YieldProcessor use in
                      * the throttle spin. This is the ONE deliberate
                      * windows.h exception outside platform/host/: the
                      * console nicety is win32-only UI policy with no
                      * portable meaning, so it stays #ifdef'd here (this
                      * include, the YieldProcessor spin hint, and the
                      * whole mp6_bridge_post_window_init console body are
                      * the three, and only three, _WIN32 regions) rather
                      * than getting a seam surface. */
#else
#include <sched.h> /* sched_yield for the throttle's non-aarch64 spin fallback */
#endif /* _WIN32 */

/* SAVESTATE CARVE-OUT (docs/SAVESTATE.md). Two placement rules, BOTH
 * load-bearing and both learned the hard way:
 *  - AFTER this TU's own includes: it is a #pragma clang section redirecting
 *    every file-scope definition that FOLLOWS it. As a -include (before all
 *    headers) it also captured the decomp headers' C TENTATIVE definitions
 *    (dolphin/os.h's `u32 __OSBusClock;` et al), turning those common symbols
 *    into strong per-TU definitions and breaking the link.
 *  - At PREPROCESSOR TOP LEVEL, never inside any #if/#ifdef branch: an
 *    earlier revision sat inside the #else of the _WIN32 block above, so on
 *    Windows -- the only platform savestates run on -- this TU silently
 *    compiled UNCARVED and every static here (the SDL window cache, the
 *    tick-throttle deadline, the input-script cursor) was captured and
 *    restored like game state. tools/build.py's verify_host_section_sources()
 *    now rejects a conditionally-nested include, but the rule belongs here
 *    too, where the next editor will see it. */
#include "mp6_host_section.h"

/* GXTexObj/GXTlutObj ABI coherence: GXTexObj is the one member of the
 * "decomp shadow struct vs Aurora's real ABI" family (GXTlutObj,
 * PADStatus, GXTexObj -- see the header comment) that tools/build.py's
 * HEADER_CONTENT_PATCHES deliberately does NOT resize: decomp's TARGET_PC
 * shadow (include/dolphin/gx/GXStruct.h) declares GXTexObj as `u32
 * dummy[22]` (88 bytes) against Aurora's real internal write footprint of
 * exactly 64 bytes (aurora/lib/gfx/texture.hpp's GXTexObj_, itself
 * static_assert-guarded there to fit Aurora's OWN 64-byte GXTexObj) --
 * decomp's declaration is the OVERSIZED side, so every real call site
 * (game/hsfdraw.c, game/hsfanim.c, game/sprput.c, game/THPDraw.c,
 * game/thpmain.c, game/printfunc.c, board/config.c, REL/bootDll/opening.c,
 * REL/fileseldll/filesel.c -- all plain function-local `GXTexObj texObj;`
 * declarations, confirmed via grep, none stored as a struct field or
 * memcpy'd by sizeof anywhere) hands Aurora's real GXInitTexObj/
 * GXLoadTexObj/etc. a pointer backed by MORE bytes than they ever write,
 * not fewer -- the unsafe direction GXTlutObj/PADStatus actually needed
 * fixing. Confirmed safe by construction, not merely assumed: the two
 * static_asserts below pin the exact byte counts this conclusion depends
 * on (Aurora's real, always-TARGET_PC-compiled ABI, visible here because
 * this file -- unlike game code -- compiles against Aurora's OWN dolphin/
 * gx headers) so a future Aurora update that grows GXTexObj_/GXTlutObj_
 * past decomp's fixed 88/40-effective-byte allocations fails THIS build
 * loudly instead of silently reintroducing a stack overflow. */
_Static_assert(sizeof(GXTexObj) == 64, "Aurora's real GXTexObj grew past decomp's oversized-but-fixed 88-byte shadow (include/dolphin/gx/GXStruct.h) -- audit game/hsfdraw.c & co.'s local GXTexObj usage for overflow");
_Static_assert(sizeof(GXTlutObj) == 40, "Aurora's real GXTlutObj changed size -- re-check tools/build.py's HEADER_CONTENT_PATCHES GXTlutObj dummy[10] shadow-header fix still matches");

/* ---------------------------------------------------------------------
 * 1. GXSetArray arity bridge.
 *
 * decomp's OWN call sites (game/hsfdraw.c, game/hsfanim.c, game/sprput.c,
 * game/printfunc.c -- every one, confirmed via grep) use the REAL-hardware
 * 3-argument shape: GXSetArray(attr, data, stride), no explicit size --
 * real hardware addresses memory directly, there is no GPU buffer to
 * size. tools/build.py's patch_abi_struct_headers() ships a shadow
 * dolphin/gx/GXGeometry.h that declares exactly this 3-arg shape
 * unconditionally (decomp's OWN header still has a `#ifdef TARGET_PC`
 * branch here, it's just never how MP6's code actually calls it -- see
 * that function's own comment), so decomp compiles clean against it.
 *
 * Aurora's real GXSetArray takes 5 arguments: (attr, data, size, stride,
 * le). `size` is NOT cosmetic -- reading lib/dolphin/gx/GXGeometry.cpp
 * (writes the raw pointer + size into Aurora's internal FIFO command
 * stream) through to lib/gx/command_processor.cpp's GX_AURORA_LOAD_
 * ARRAYBASE handler (stores them into g_gxState.arrays[attr]) through to
 * lib/gx/command_processor.cpp's push_gx_draw() (`gfx::push_storage(
 * array.data, array.size)` for any indexed vertex attribute) confirms
 * `size` bytes are genuinely read from `data` later, at
 * aurora_end_frame() time, to build a real GPU-visible buffer -- exactly
 * the information the real GameCube API never needed and decomp's call
 * sites genuinely do not have on hand (the actual element count only
 * becomes known later, at the paired GXBegin(..., nverts) call).
 *
 * Passing a GUESSED, possibly-too-large size risks reading past the real
 * (often small stack or scratch-heap) allocation decomp's code actually
 * made -- a genuine out-of-bounds read, not a theoretical one. A size=0
 * fallback is PROVABLY crash-safe (Aurora's own lib/gfx/common.cpp
 * push()/push_storage() -- `if (length > 0) { target.append(data,
 * length); }`, data is never dereferenced at all for a 0-byte push), but
 * NOT actually harmless: push_gx_draw() (lib/gx/command_processor.cpp)
 * uploads exactly `array.size` bytes into the real GPU-visible storage
 * buffer the vertex shader indexes into for GX_INDEX8/GX_INDEX16
 * attributes -- size=0 uploads ZERO bytes, so every indexed fetch (every
 * HSF mesh vertex/normal/st/color, all of them GX_INDEX16) reads
 * out-of-range/zeroed data and the mesh vanishes.
 *
 * PRIMARY MECHANISM: mp6_gxarray_registry.h/.c
 * (platform/gx/gxarray_registry.c). decomp itself never has a real
 * element count on hand at this call site (real hardware never needed one
 * either), but platform/hsf/hsf_load_native.c -- which ALLOCATES every
 * vertex/normal/st/color buffer HSF meshes ever pass here -- knows each
 * one's real byte size at allocation time and registers it there; this
 * bridge looks the pointer up and uses the real size when known, falling
 * back to the learned-size mechanism below (and ultimately the still
 * crash-safe size=0) for any OTHER GXSetArray call site the registry
 * never saw (game/sprput.c, game/printfunc.c, game/hsfanim.c).
 *
 * The arity gap itself (3 args supplied, 5 needed) can't be closed with
 * GNU ld's usual --wrap trick on THIS toolchain: zig cc/c++'s own linker-
 * arg frontend rejects `-Wl,--wrap=X` outright ("unsupported linker arg:
 * --wrap", confirmed by direct testing, both via -Wl, and -Xlinker, and
 * regardless of -fuse-ld=lld); the underlying linker it actually invokes
 * for this target is lld-link (COFF/MSVC-style), which doesn't implement
 * GNU --wrap-style symbol interposition at all in the first place. A
 * link-ORDER trick (link our own strong GXSetArray before libaurora_gx.a,
 * hoping the archive member defining Aurora's real one is simply never
 * pulled in) was tested directly too and does NOT work either: that same
 * .cpp file also defines several OTHER functions decomp genuinely needs
 * directly, so the member gets pulled in regardless, and lld-link then
 * hard-errors with "duplicate symbol" rather than preferring either
 * definition.
 *
 * Fixed instead in shim/include/dolphin_compat.h (see its own section-3
 * comment): every decomp call site is renamed, at the preprocessor level,
 * to `mp6_GXSetArray3` -- a name Aurora's own build never defines, so
 * there is no collision to resolve at link time at all. This file
 * provides the actual mp6_GXSetArray3 definition for the aurora build
 * (gen_shims.py's MACRO_RESOLUTION covers the --headless build's own
 * null shim under the same resolved name).
 *
 * `le = true`: this whole port is a fresh recompile of the C source for a
 * native little-endian target, never a raw reinterpretation of original
 * big-endian disc bytes, so vertex arrays are already little-endian from
 * Aurora's point of view (byte-order conversion is scoped to
 * DVD-sourced/compiled-in data blobs, not to freshly-compiled-and-laid-out
 * arrays like the ones GXSetArray is handed here).
 * --------------------------------------------------------------------- */
/* LEARNED-SIZE MECHANISM (for arrays the HSF registry never saw):
 * non-HSF indexed consumers exist -- e.g. game/sprput.c's HuSpr3DDisp
 * registers a (col+1)*(row+1)-element vertex/texcoord grid via
 * GXSetArray, then submits a QUADS primitive that indexes into it
 * (GXPosition1x16/GXTexCoord1x16); with size=0 every indexed vertex
 * resolves to degenerate/empty data and the whole quad grid (e.g. a
 * message-window frame) collapses to nothing visible, while DIRECT-mode
 * draws (text glyphs) still render.
 *
 * Learn the real size instead of guessing OR hand-special-casing call
 * sites. decomp's own call PATTERN gives us everything needed, fully
 * generically (no knowledge of col/row/HUSPR_3DDATA required here):
 * GXSetArray(attr, data, stride) is always followed by a
 * GXBegin(..., nverts) before the array's contents are ever really read
 * (Aurora reads an array's bytes lazily, at aurora_end_frame() time, per
 * this file's own section-1 header comment above) -- cache (data, stride)
 * per attribute here; the very next mp6_GXBegin call (section 5 below)
 * computes nverts*stride and re-registers the REAL GXSetArray with that
 * size whenever it's bigger than whatever's already registered for this
 * exact pointer.
 *
 * Is nverts*stride always >= the true buffer size? For DIRECT-shaped
 * sequential indexing (index i used only for vertex i, 0..nverts-1) it's
 * EXACT. For HuSpr3DDisp's shared-grid-pool indexing specifically (proven
 * algebraically): the true size is (col+1)*(row+1) elements, nverts is
 * col*row*4; 4*col*row >= (col+1)*(row+1) for every integer col,row >= 1
 * (equality only at col=row=1; strictly bigger otherwise -- e.g.
 * col=8,row=1: nverts=32 vs true 18 elements), so this NEVER under-reads
 * (never reproduces the missing-geometry bug), and the modest overshoot
 * reads only slightly past the real allocation -- still safely inside
 * this port's own large pre-reserved game arena, not the OS heap, so
 * there is no unmapped-page/crash risk the way a truly arbitrary guessed
 * size would carry. Resetting to 0 whenever the (data, stride) pair
 * actually CHANGES (a genuinely different buffer, e.g. a different
 * sprite's own data3D) avoids ever carrying a stale, possibly-too-large
 * size over onto an unrelated, smaller allocation that happens to reuse
 * the same attribute slot. */

/* [ARRAYPROBE] -- env-gated (MP6_ARRAYPROBE=1) evidence probe for
 * "clean data, garbage pixels" vertex-corruption investigations.
 * Three questions, one run:
 *   (a) which GXSetArray binds take the authoritative registry path vs the
 *       learned-size fallback (a learned size is nverts*stride of some draw,
 *       NOT max-index*stride -- undersized for sparse indexed meshes);
 *   (b) does mp6_gx_array_slots_grow_for_draw() ever re-issue GXSetArray
 *       WHILE the game is recording a display list (hsfdraw.c's
 *       GXBeginDisplayList..GXEndDisplayList bracket)? Aurora's GXSetArray
 *       writes a GX_AURORA LOAD_ARRAYBASE+stride command sequence into
 *       whatever the FIFO currently targets -- during recording that is the
 *       game's DL buffer, i.e. bytes MDFaceCnt() never budgeted AND a
 *       replayed-every-frame rebind of that attribute to a foreign array;
 *   (c) the recording brackets themselves (buf, bytes written) so any
 *       injected bind can be attributed to the exact model DL being built.
 * The DL-recording flag lives here (not queried from Aurora) because this
 * file wraps GXBeginDisplayList/GXEndDisplayList anyway (dolphin_compat.h
 * renames, same pure-rename mechanism as mp6_GXCallDisplayList). */
static bool g_mp6InDlRecording = false;
static void *g_mp6DlRecBuf = NULL;
static int g_mp6ArrayProbeEnabled = -1; /* -1 = getenv not consulted yet */

static bool mp6_arrayprobe_enabled(void)
{
    if (g_mp6ArrayProbeEnabled < 0) {
        const char *e = getenv("MP6_ARRAYPROBE");
        g_mp6ArrayProbeEnabled = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return g_mp6ArrayProbeEnabled == 1;
}

/* Dedupe table so per-frame re-binds don't flood stdout: one line per
 * distinct (attr, data, path, size) tuple ever passed to Aurora. */
typedef struct {
    GXAttr attr;
    const void *data;
    u32 size;
    u8 path; /* 0=registry 1=learned 2=zero */
} MP6ArrayProbeSeen;
#define MP6_ARRAYPROBE_SEEN_MAX 4096
static MP6ArrayProbeSeen g_mp6ArrayProbeSeen[MP6_ARRAYPROBE_SEEN_MAX];
static int g_mp6ArrayProbeSeenCount = 0;

static void mp6_arrayprobe_log_setarray(GXAttr attr, const void *data, u8 stride,
                                        u8 path, u32 size)
{
    static const char *pathName[3] = { "REGISTRY", "LEARNED", "ZERO" };
    int i;
    if (!mp6_arrayprobe_enabled()) {
        return;
    }
    for (i = 0; i < g_mp6ArrayProbeSeenCount; i++) {
        MP6ArrayProbeSeen *s = &g_mp6ArrayProbeSeen[i];
        if (s->attr == attr && s->data == data && s->path == path && s->size == size) {
            return; /* already reported this exact bind shape */
        }
    }
    if (g_mp6ArrayProbeSeenCount < MP6_ARRAYPROBE_SEEN_MAX) {
        MP6ArrayProbeSeen *s = &g_mp6ArrayProbeSeen[g_mp6ArrayProbeSeenCount++];
        s->attr = attr;
        s->data = data;
        s->path = path;
        s->size = size;
    }
    printf("[ARRAYPROBE] SetArray attr=%d ptr=%p stride=%u path=%s size=%u inDL=%d\n",
           (int)attr, data, (unsigned)stride, pathName[path], (unsigned)size,
           g_mp6InDlRecording ? 1 : 0);
    fflush(stdout);
}

typedef struct {
    GXAttr attr;
    void  *data;
    u8     stride;
    u32    registeredSize;
    bool   used;
} MP6GxArraySlot;

#define MP6_GXARRAY_SLOT_MAX 32 /* GX_VA_MAX_ATTR is ~24 entries; generous headroom */
static MP6GxArraySlot g_gxArraySlots[MP6_GXARRAY_SLOT_MAX];
static int g_gxArraySlotCount = 0;

static MP6GxArraySlot *mp6_gx_array_slot_get(GXAttr attr)
{
    int i;
    for (i = 0; i < g_gxArraySlotCount; i++) {
        if (g_gxArraySlots[i].attr == attr) {
            return &g_gxArraySlots[i];
        }
    }
    if (g_gxArraySlotCount < MP6_GXARRAY_SLOT_MAX) {
        MP6GxArraySlot *slot = &g_gxArraySlots[g_gxArraySlotCount++];
        slot->attr = attr;
        slot->data = NULL;
        slot->stride = 0;
        slot->registeredSize = 0;
        slot->used = false;
        return slot;
    }
    /* MP6_GXARRAY_SLOT_MAX comfortably exceeds GX_VA_MAX_ATTR; should never
     * happen. Fall back to the caller treating this as untracked rather
     * than indexing out of bounds. */
    return NULL;
}

/* Called from mp6_GXBegin (section 5 below) -- grows any tracked array
 * whose declared size doesn't yet cover this draw's own vertex count. Safe
 * to call unconditionally on every GXBegin, even ones that don't use an
 * indexed attribute at all: an untouched slot (never GXSetArray'd, or
 * already sized big enough) is simply skipped; re-growing a slot that
 * genuinely belongs to a DIFFERENT, unrelated draw this same tick just
 * widens its declared (arena-backed, harmless) read window a bit further
 * than strictly needed for THIS specific call, never less. */
static void mp6_gx_array_slots_grow_for_draw(u16 nverts)
{
    int i;
    /* NEVER grow-and-re-bind while the game is recording a display list.
     * Aurora's GXSetArray is not a pure state setter: it writes a
     * GX_AURORA LOAD_ARRAYBASE + CP-stride command sequence into whatever
     * the FIFO currently targets, and during
     * GXBeginDisplayList..GXEndDisplayList that target is the game's own
     * DL buffer. The recorded re-bind then replays on EVERY
     * GXCallDisplayList of that batch, re-pointing the attribute
     * (POS/TEX0/CLR0...) at the learned slot's SPRITE/WINDOW array right
     * before the batch's indexed vertices fetch -- the object draws
     * window-grid garbage -- plus the injected ~22B/bind eat the DL slack
     * MDFaceCnt() budgeted (an "exactly-full DL" fingerprint).
     * MP6_ARRAYPROBE=1's "GROW ... <-- INJECTED-INTO-DL" line flags
     * exactly this.
     *
     * Skipping (not deferring) is correct because the learned slots exist
     * for IMMEDIATE-mode draws only (sprput/window quads): a batch being
     * recorded never consumes them at replay -- every GXCallDisplayList
     * consumer in the game (hsfdraw.c FaceDraw's per-object binds,
     * hsfanim.c's particle-draw binds) re-issues GXSetArray for its
     * indexed attributes immediately before the call. A model batch's
     * nverts was never a valid size hint for a sprite array in the first
     * place -- two unrelated consumers sharing an attribute slot. */
    if (g_mp6InDlRecording) {
        return;
    }
    for (i = 0; i < g_gxArraySlotCount; i++) {
        MP6GxArraySlot *slot = &g_gxArraySlots[i];
        u32 neededSize;
        if (!slot->used || slot->stride == 0 || slot->data == NULL) {
            continue;
        }
        neededSize = (u32)nverts * (u32)slot->stride;
        if (neededSize > slot->registeredSize) {
            slot->registeredSize = neededSize;
            if (mp6_arrayprobe_enabled()) {
                /* A re-issue DURING display-list recording writes the
                 * LOAD_ARRAYBASE command sequence into the game's DL buffer
                 * itself -- report loudly, this poisons every replay. */
                printf("[ARRAYPROBE] GROW attr=%d ptr=%p ->%u nverts=%u inDL=%d%s\n",
                       (int)slot->attr, slot->data, (unsigned)neededSize,
                       (unsigned)nverts, g_mp6InDlRecording ? 1 : 0,
                       g_mp6InDlRecording ? "  <-- INJECTED-INTO-DL (buf below)" : "");
                if (g_mp6InDlRecording) {
                    printf("[ARRAYPROBE] GROW-INJECT dlbuf=%p\n", g_mp6DlRecBuf);
                }
                fflush(stdout);
            }
            GXSetArray(slot->attr, slot->data, neededSize, slot->stride, /*le=*/true);
        }
    }
}

void mp6_GXSetArray3(GXAttr attr, void *data, u8 stride)
{
    uint32_t realSize = mp6_gxarray_lookup(data);
    MP6GxArraySlot *slot;
    if (realSize > 0) {
        /* Exact size from the HSF loader's registry -- authoritative. */
        MP6_LOG_ONCE("GX", "GXSetArray (real size, see mp6_gxarray_registry.h)");
        mp6_arrayprobe_log_setarray(attr, data, stride, /*path=REGISTRY*/0, realSize);
        GXSetArray(attr, data, realSize, stride, /*le=*/true);
        return;
    }
    /* Pointer not in the loader registry (non-HSF path, e.g. sprite
     * vertex arrays) -- fall back to the learned-size slot instead of a
     * flat size=0. */
    MP6_LOG_ONCE("GX", "GXSetArray (learned-size bridge, see aurora_bridge.c)");
    slot = mp6_gx_array_slot_get(attr);
    if (slot) {
        if (slot->data != data || slot->stride != stride) {
            /* Genuinely different buffer (or a reused attribute slot with a
             * different stride/interpretation) -- any previously-learned
             * size belonged to the OLD buffer and could be unsafely large
             * for this new one. Start over at 0 (the same safe fallback as
             * an unregistered pointer) until the next GXBegin (section 5)
             * tells us how much of THIS buffer this draw really needs. */
            slot->data = data;
            slot->stride = stride;
            slot->registeredSize = 0;
        }
        slot->used = true;
        mp6_arrayprobe_log_setarray(attr, data, stride, /*path=LEARNED*/1, slot->registeredSize);
        GXSetArray(attr, data, slot->registeredSize, stride, /*le=*/true);
    } else {
        mp6_arrayprobe_log_setarray(attr, data, stride, /*path=ZERO*/2, 0);
        GXSetArray(attr, data, /*size=*/0, stride, /*le=*/true);
    }
}

/* ---------------------------------------------------------------------
 * GXBeginDisplayList/GXEndDisplayList
 * recording-bracket wrappers -- the same PURE-rename mechanism as
 * mp6_GXCallDisplayList (dolphin_compat.h #define + gen_shims.py
 * MACRO_RESOLUTION entry; this file is compiled against Aurora's own
 * headers so the real symbols are visible here). Purpose: maintain
 * g_mp6InDlRecording so the array-size machinery above knows when a
 * GXSetArray re-issue would be recorded INTO the game's display list
 * rather than executed immediately, and (under MP6_ARRAYPROBE=1) log the
 * recording brackets for attribution. Forwarding is otherwise 1:1.
 * --------------------------------------------------------------------- */
void mp6_GXBeginDisplayList(void *list, u32 size)
{
    g_mp6InDlRecording = true;
    g_mp6DlRecBuf = list;
    if (mp6_arrayprobe_enabled()) {
        printf("[ARRAYPROBE] DLREC-BEGIN buf=%p size-arg=%u\n", list, (unsigned)size);
        fflush(stdout);
    }
    GXBeginDisplayList(list, size);
}

u32 mp6_GXEndDisplayList(void)
{
    u32 written = GXEndDisplayList();
    if (mp6_arrayprobe_enabled()) {
        printf("[ARRAYPROBE] DLREC-END buf=%p wrote=%u\n", g_mp6DlRecBuf, (unsigned)written);
        fflush(stdout);
    }
    g_mp6InDlRecording = false;
    g_mp6DlRecBuf = NULL;
    return written;
}

/* ---------------------------------------------------------------------
 * 2. VI frame-pacing bridge.
 * --------------------------------------------------------------------- */
static bool g_frameOpen = false;
static VIRetraceCallback g_preRetraceCB = NULL;
static VIRetraceCallback g_postRetraceCB = NULL;
static void *g_nextFrameBuffer = NULL;

/* The draw-call bisect harness's per-frame draw-index counter (section 7
 * below). Declared up here (rather than down in section 7 with the rest
 * of that harness) because VIWaitForRetrace below -- section 2, this same
 * job -- resets it once per tick; a plain file-scope C static must be
 * declared before its first use in the same translation unit. Section 7
 * has the full mechanism/rationale. */
static u32 g_mp6DrawIndex = 0;

/* Forward declaration -- defined in section 5 below (GXBegin/GXEnd
 * tolerance), called from VIWaitForRetrace's frame-end handling here in
 * section 2 so a primitive left open with no further GXBegin in the same
 * frame still gets closed before aurora_end_frame(), not just on the next
 * GXBegin call. See section 5's own comment for the full root-cause story. */
static void mp6_gx_close_stale_primitive(const char *context);

/* GameCube double-buffer "flip" idiom (game/init.c, game/sreset.c call
 * this with DemoFrameBuffer1/DemoCurrentBuffer). GXCopyDisp is a
 * documented no-op under Aurora -- the EFB->XFB copy + present happens
 * automatically inside aurora_end_frame() instead (aurora_surface.md
 * gotcha #4) -- and nothing in this slice ever reads
 * VIGetCurrentFrameBuffer/VIGetNextFrameBuffer back (neither is in
 * sdk_surface.json's needed-symbol list), so this is a pure store for
 * bookkeeping/debugging visibility, with no effect on what actually
 * reaches the screen. */
void VISetNextFrameBuffer(void *fb)
{
    g_nextFrameBuffer = fb;
}

/* No real "blank the display" concept is exposed by Aurora's simple
 * present-every-frame model; tracked only so a future milestone has
 * somewhere to hang real behavior (e.g. skipping the draw calls between
 * VISetBlack(TRUE) and the next VISetBlack(FALSE)) without another
 * signature change. */
static bool g_black = false;
void VISetBlack(BOOL black)
{
    g_black = black ? true : false;
    (void)g_black;
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb)
{
    VIRetraceCallback old = g_preRetraceCB;
    g_preRetraceCB = cb;
    return old;
}

/* game/pad.c registers PadReadVSync here -- this IS the game's real
 * per-frame controller sample point, so firing it for real (from
 * VIWaitForRetrace below) every tick matters for input to ever reach the
 * game at all, not just for cosmetic parity with real hardware. */
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb)
{
    VIRetraceCallback old = g_postRetraceCB;
    g_postRetraceCB = cb;
    return old;
}

/* Real hardware: progressive-scan/DTV detection. Aurora's own VIGetTvFormat
 * always reports NTSC (aurora_surface.md); game/init.c's one call site
 * (init.c:51) already short-circuits on "VIGetTvFormat() != 2" before ever
 * consulting this, so 0 ("not DTV") is the honest, harmless answer either
 * way -- this is one of the 8 VI gaps Aurora has no definition for at all. */
u32 VIGetDTVStatus(void)
{
    MP6_LOG_ONCE("VI", "VIGetDTVStatus");
    return 0;
}

/* Shared with the --headless build's own definition of these two (see
 * platform/null/shims_manual.c) -- both read the SAME mp6_tick_count this
 * file's VIWaitForRetrace below advances, so `build/mp6native.exe 600`
 * means "600 ticks" identically in both build modes. */
u32 VIGetRetraceCount(void)
{
    return (u32)mp6_tick_count;
}

u32 VIGetNextField(void)
{
    return (u32)(mp6_tick_count & 1);
}

/* ---------------------------------------------------------------------
 * 4. Keyboard-to-PAD bridge.
 *
 * Aurora's real PAD path is a real SDL_Gamepad; nothing guarantees one is
 * plugged in wherever this runs, so at least one obvious key must reach
 * the game as real input regardless.
 * Aurora DOES have its own keyboard-bind subsystem (PADSetKeyButtonBinding
 * et al, lib/dolphin/pad/pad.cpp) but every default binding is
 * PAD_KEY_INVALID and nothing in Aurora itself ever opts a port into it
 * (PADSetKeyboardActive has zero call sites anywhere in Aurora's own
 * source -- confirmed directly) -- so out of the box, keyboard presses
 * reach the game NOT AT ALL, gamepad or not.
 *
 * PADSetVirtualStatus (pad.h) is the simplest supported hook instead:
 * build a PADStatus from a direct SDL_GetKeyboardState() poll each frame
 * and hand it to Aurora, which OR-merges it into whatever PADRead() would
 * have returned from a real pad on the same port (pad.cpp's own
 * merge_virtual_status) -- purely additive, never overrides a real
 * controller if one actually is connected.
 *
 * Mapping (documented here since there is no in-game prompt for it):
 *   Enter or Space -> PAD_BUTTON_START -- satisfies BOTH the warning
 *                     screen's "press ANY button" check (game/REL/
 *                     bootDll/boot.c's BootWarningExec ORs all 4 ports'
 *                     HuPadBtnDown together, so any single real button
 *                     works) AND BootTitleExec's specific
 *                     PAD_BUTTON_START confirm check at the title screen.
 *   Z -> PAD_BUTTON_A, X -> PAD_BUTTON_B, arrow keys -> D-pad.
 * Port 0 only -- single-player boot-to-menu flow, no split-screen concern
 * this early. */

/* MP6_AUTO_START_TICKS: a direct, focus-independent alternative to
 * SendKeys/keybd_event-based driving (two independently-running
 * mp6native.exe windows sharing one input focus is a real source of
 * corrupted test runs -- see docs/TESTING.md's test-isolation protocol).
 * A comma-separated list of VIWaitForRetrace tick numbers
 * (mp6_tick_count, the same counter mp6_boot.h already exposes); on each
 * of those exact ticks, PAD_BUTTON_START is injected for that one frame
 * only, exactly as if a real button press had landed then -- satisfies
 * both real boot call sites (BootWarningExec's "any button"
 * OR-of-all-ports check, BootTitleExec's specific START confirm), with no
 * window focus, no OS-level key injection, and no dependency on which
 * window happens to be foreground. Parsed once, lazily, on first use;
 * absent/unset (the default) is a pure no-op -- every existing driving
 * flow (manual play, SDL keyboard input above) is completely unaffected.
 * 256 slots: driving a full opening-skip + title-start + file-select
 * session wants presses spread widely across tens of thousands of ticks
 * for robustness (exact timing of each gate isn't known in advance). */
#define MP6_AUTO_START_MAX 256
static int g_autoStartTicks[MP6_AUTO_START_MAX];
static int g_autoStartCount = -1; /* -1 = not yet parsed */

static void mp6_parse_auto_start_ticks(void)
{
    const char *env;
    g_autoStartCount = 0;
    env = getenv("MP6_AUTO_START_TICKS");
    if (!env) return;
    {
        const char *p = env;
        while (*p && g_autoStartCount < MP6_AUTO_START_MAX) {
            char *end;
            long v = strtol(p, &end, 10);
            if (end == p) break;
            g_autoStartTicks[g_autoStartCount++] = (int)v;
            p = end;
            while (*p == ',' || *p == ' ') p++;
        }
    }
    if (g_autoStartCount > 0) {
        int i;
        printf("[MP6-INPUT] MP6_AUTO_START_TICKS active, %d scheduled press(es):", g_autoStartCount);
        for (i = 0; i < g_autoStartCount; i++) printf(" %d", g_autoStartTicks[i]);
        printf("\n");
        fflush(stdout);
    }
}

static bool mp6_auto_start_due_now(void)
{
    int i;
    if (g_autoStartCount < 0) mp6_parse_auto_start_ticks();
    for (i = 0; i < g_autoStartCount; i++) {
        if (g_autoStartTicks[i] == (int)mp6_tick_count) return true;
    }
    return false;
}

/* A plain SDL_GetKeyboardState() poll alone has a missed-edge race:
 * decomp's input consumers check `HuPadBtnDown[0] & PAD_BUTTON_START`
 * every tick, fed by game/pad.c's PadReadVSync ->
 * PADButtonDown(oldBtn, newBtn) edge detector, which can only see a
 * transition if SOME tick's PADRead() call actually observed the button
 * held. SDL_GetKeyboardState() returns an INSTANTANEOUS snapshot; if a
 * press's key-down AND key-up both land in the SAME aurora_update() pump
 * (entirely possible -- synthetic presses can complete faster than one
 * ~16.67ms tick, and aurora_update() drains the whole SDL event queue in
 * one call), the keyboard state array reads back "not pressed" (the
 * LATEST, post-both-events state) even though a real, valid down-then-up
 * DID happen -- EVERY tick's poll can legitimately miss it, not just an
 * unlucky one. The result is probabilistic missed presses, not a broken
 * mapping.
 *
 * Fix: latch real key-down EVENTS (SDL_EVENT_KEY_DOWN, surfaced through
 * aurora_update()'s own AURORA_SDL_EVENT passthrough -- see
 * mp6_latch_key_down_event, called from VIWaitForRetrace's existing event
 * pump below) instead of relying solely on a once-per-tick state poll.
 * SDL only ever DROPS a keydown if the OS never delivered it at all; once
 * pumped, it is guaranteed to still be latched on THIS SAME tick's
 * mp6_pump_keyboard_to_pad() call (event pump always runs before the
 * keyboard pump within one VIWaitForRetrace -- see that function), so at
 * least one full PADSetVirtualStatus/PADRead/HuPadRead cycle sees the
 * button pressed no matter how fast the corresponding key-up followed. A
 * genuinely-HELD key keeps working exactly as before via the plain state
 * poll for as long as it's actually held; the latch only ever adds
 * coverage for the sub-tick-blip case the poll alone could miss. */
typedef struct { SDL_Scancode scancode; u16 button; } MP6KeyBind;
static const MP6KeyBind g_keyBinds[] = {
    { SDL_SCANCODE_RETURN, PAD_BUTTON_START },
    { SDL_SCANCODE_SPACE,  PAD_BUTTON_START },
    { SDL_SCANCODE_Z,      PAD_BUTTON_A },
    { SDL_SCANCODE_X,      PAD_BUTTON_B },
    { SDL_SCANCODE_UP,     PAD_BUTTON_UP },
    { SDL_SCANCODE_DOWN,   PAD_BUTTON_DOWN },
    { SDL_SCANCODE_LEFT,   PAD_BUTTON_LEFT },
    { SDL_SCANCODE_RIGHT,  PAD_BUTTON_RIGHT },
#ifdef __ANDROID__
    /* The Android BACK gesture/button. SDL's Java glue delivers it as a
     * normal key event (KEYCODE_BACK -> SDL_SCANCODE_AC_BACK --
     * SDL_androidkeyboard.c's translate table), and main_native.c sets
     * SDL_ANDROID_TRAP_BACK_BUTTON=1 so the activity's onBackPressed()
     * fallback can never finish() the process out from under a save
     * write. Mapping it onto PAD B gives BACK the GameCube "cancel/back
     * out" meaning inside every menu -- the back-button policy (exit =
     * HOME/recents, like any fullscreen game). Windows builds: this row
     * is compiled out. */
    { SDL_SCANCODE_AC_BACK, PAD_BUTTON_B },
#endif
};
#define MP6_KEYBIND_COUNT (sizeof(g_keyBinds)/sizeof(g_keyBinds[0]))

#ifdef __ANDROID__
/* On-screen touch controls (platform/android/touch_pad.cpp -- an
 * Android-only C++ TU; see its file header for the full design). The three
 * calls below are the entire integration surface: feed it SDL events, OR
 * its PAD state into the same virtual status everything else uses, draw
 * its overlay while a frame is open. */
extern void mp6_touch_pad_event(const SDL_Event *ev);
extern void mp6_touch_pad_collect(u16 *btnOut, s8 *stickXOut, s8 *stickYOut);
extern void mp6_touch_pad_draw(void);

/* ---------------------------------------------------------------------
 * Deferred PAD motor queue (Android only). dolphin_compat.h renames
 * every decomp PADControlMotor call site to mp6_PADControlMotor (same pure
 * -rename mechanism as mp6_GXCallDisplayList; full rationale at that
 * #define): on Android, aurora's PADControlMotor can reach the SYSTEM
 * VIBRATOR through SDL -- a JNI upcall -- and game code issues motor
 * commands from HuPrc COROUTINES (e.g. omOvlKill -> HuPadRumbleAllStop on
 * an overlay transition), whose arena stacks sit outside the
 * ART-registered thread stack; ART's JNI-entry stack-bounds check then
 * throws a spurious java.lang.StackOverflowError that aborts the process
 * at the next checked JNI call. NO JNI from coroutine stacks, ever. This
 * TU is compiled against aurora's own headers (never dolphin_compat.h),
 * so the rename cannot touch the REAL PADControlMotor call below -- the
 * exact mp6_GXBegin/mp6_GXCallDisplayList pattern.
 *
 * Semantics preserved per port: the LAST command a tick issues wins
 * (matching what back-to-back synchronous calls left as final motor
 * state); applied from VIWaitForRetrace -- main-loop context, real thread
 * stack -- at most one tick (<=16.7ms) after the game asked, well inside
 * physical rumble-transport latency. */
#define MP6_PAD_MOTOR_NONE 0xFFFFFFFFu
static u32 g_padMotorPending[4] = {
    MP6_PAD_MOTOR_NONE, MP6_PAD_MOTOR_NONE, MP6_PAD_MOTOR_NONE, MP6_PAD_MOTOR_NONE,
};

void mp6_PADControlMotor(s32 chan, u32 cmd)
{
    if (chan >= 0 && chan < 4) {
        g_padMotorPending[chan] = cmd;
    }
}

static void mp6_pad_motor_apply_pending(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (g_padMotorPending[i] != MP6_PAD_MOTOR_NONE) {
            PADControlMotor((u32)i, g_padMotorPending[i]);
            g_padMotorPending[i] = MP6_PAD_MOTOR_NONE;
        }
    }
}
#endif

/* Buttons newly key-down'd via a real SDL event since the last
 * mp6_pump_keyboard_to_pad() call -- consumed (cleared) every tick, not
 * sticky, so this never overrides a real key-up that follows on a later
 * tick; see the file comment above for why this exists alongside (not
 * instead of) the plain state poll. */
static u16 g_padKeyLatch = 0;

static void mp6_latch_key_down_event(const SDL_Event *sdlEvent)
{
    size_t i;
    if (sdlEvent->type != SDL_EVENT_KEY_DOWN) {
        return;
    }
    for (i = 0; i < MP6_KEYBIND_COUNT; i++) {
        if (sdlEvent->key.scancode == g_keyBinds[i].scancode) {
            g_padKeyLatch |= g_keyBinds[i].button;
        }
    }
}

/* Savestate hotkeys (docs/SAVESTATE.md): F5 saves, F8 loads -- the
 * emulator-conventional pair, chosen so muscle memory transfers and so
 * neither collides with g_keyBinds above (which is entirely letters/arrows
 * mapped to real pad buttons).
 *
 * Uses the same KEY_DOWN EVENT latch as the pad binds rather than a
 * SDL_GetKeyboardState poll, for the missed-edge reason this file already
 * documents for pad input: a key pressed and released between two polls is
 * invisible to a state poll but always produces an event. Here it matters
 * more than for pad input, not less -- a dropped savestate keypress reads
 * to the user as "the feature is broken," while a dropped pad frame just
 * reads as input lag.
 *
 * The request is only QUEUED here. It is serviced later, from
 * mp6_savestate_tick(), so that capture/restore happens at the frame
 * boundary where no HuPrc coroutine is mid-instruction -- servicing it
 * inside the event pump would capture whatever the SDL callback happened
 * to interrupt. */
static void mp6_latch_savestate_key_event(const SDL_Event *sdlEvent)
{
    if (sdlEvent->type != SDL_EVENT_KEY_DOWN) {
        return;
    }
    if (sdlEvent->key.repeat) {
        /* S4 (review): OS auto-repeat delivers a KEY_DOWN stream while the
         * key is held, and each one used to queue a fresh multi-second
         * capture/restore -- a one-second hold stacked ~30 of them and read
         * as "savestates hard-froze the game". One request per physical
         * press. */
        return;
    }
    if (sdlEvent->key.scancode == SDL_SCANCODE_F5) {
        printf("[SAVESTATE] F5 -- save queued for the next frame boundary\n");
        fflush(stdout);
        mp6_savestate_request_save();
    } else if (sdlEvent->key.scancode == SDL_SCANCODE_F8) {
        printf("[SAVESTATE] F8 -- load queued for the next frame boundary\n");
        fflush(stdout);
        mp6_savestate_request_load();
    }
}

/* In-game menu hotkey: F10 toggles the persistent RmlUi settings window
 * over the running game (docs/TESTING.md's launcher section; the ripped
 * F1/gamepad-Back/R+Start/3-finger bindings route through the UI's own
 * event path instead -- mp6_launcher_forward_sdl_event below). Same
 * KEY_DOWN-event latch discipline as the savestate hotkeys above.
 * mp6_launcher_toggle_menu() itself is inert in automation mode (launcher
 * TU guards on its own mode flag), so a scripted/ticked run can never
 * show UI -- the automation contract is untouched. */
static void mp6_latch_menu_key_event(const SDL_Event *sdlEvent)
{
    if (sdlEvent->type != SDL_EVENT_KEY_DOWN || sdlEvent->key.repeat) {
        return;
    }
    if (sdlEvent->key.scancode == SDL_SCANCODE_F10) {
        extern void mp6_launcher_toggle_menu(void);
        mp6_launcher_toggle_menu();
    }
}

/* ---------------------------------------------------------------------
 * 6. Deterministic input-script bridge (test tooling).
 *
 * SendKeys/keybd_event-driven testing has a real reliability problem
 * independent of anything in the game itself: window-focus races (a bare
 * ALT keypress can activate a window's system menu and swallow the very
 * next key), OS input-injection timing, and possible cross-talk between
 * multiple automated test runs against similarly-named windows all
 * introduce noise that's indistinguishable, from the outside, from a
 * genuine "did the game actually see this button" question.
 * --input-script "<spec>" removes ALL of that: it injects
 * PAD button state directly into the SAME virtual-status mechanism the
 * keyboard bridge above already uses (PADSetVirtualStatus), timed purely
 * by this process's own internal tick counter -- no window focus, no OS
 * input queue, no real keyboard hardware involved at any point. This is
 * strictly a TESTING tool (real players still use the keyboard bridge
 * above, unaffected); it exists so a scripted test run is 100%
 * reproducible frame-for-frame, independent of automation timing.
 *
 * Spec syntax: semicolon-separated steps, e.g.
 * "wait:180;press:start;wait:60;press:a" --
 *   wait:N   -- advance N ticks before the next step.
 *   press:X  -- latch button X (start/a/b/up/down/left/right) for
 *               exactly one tick (matching mp6_latch_key_down_event's own
 *               one-tick-then-consumed semantics above -- a script press
 *               is exactly as "instantaneous" as a real key-down event
 *               is), then immediately move to the next step. Multiple
 *               press steps with no wait between them all land on the
 *               SAME tick (OR'd together) -- add an explicit wait:1 if a
 *               script needs them on separate ticks instead.
 *   stick:X  -- tilt the ANALOG stick (up/down/left/right, +-72 of the
 *               +-~72 hardware range) for exactly one tick, same
 *               one-tick semantics as press. Needed because several
 *               menus (file-select name entry via
 *               REL/fileseldll/filename.c) read ONLY HuPadDStkRep --
 *               game/pad.c's PadADConv derives that from the analog
 *               stick alone, so press:up/down/left/right (dpad BUTTON
 *               bits) can never move those cursors. */
typedef enum { MP6_SCRIPT_WAIT, MP6_SCRIPT_PRESS, MP6_SCRIPT_STICK } MP6ScriptStepType;
typedef struct {
    MP6ScriptStepType type;
    u32 waitFrames; /* MP6_SCRIPT_WAIT */
    u16 button;     /* MP6_SCRIPT_PRESS */
    s8 stickX;      /* MP6_SCRIPT_STICK */
    s8 stickY;      /* MP6_SCRIPT_STICK */
} MP6ScriptStep;

#define MP6_SCRIPT_MAX_STEPS 256
static MP6ScriptStep g_script[MP6_SCRIPT_MAX_STEPS];
static int g_scriptStepCount = 0;
static int g_scriptCursor = 0;
static u32 g_scriptWaitRemaining = 0;
static bool g_scriptActive = false;

static u16 mp6_script_button_from_name(const char *name, size_t len)
{
    if (len == 5 && strncmp(name, "start", 5) == 0) return PAD_BUTTON_START;
    if (len == 1 && name[0] == 'a') return PAD_BUTTON_A;
    if (len == 1 && name[0] == 'b') return PAD_BUTTON_B;
    if (len == 2 && strncmp(name, "up", 2) == 0) return PAD_BUTTON_UP;
    if (len == 4 && strncmp(name, "down", 4) == 0) return PAD_BUTTON_DOWN;
    if (len == 4 && strncmp(name, "left", 4) == 0) return PAD_BUTTON_LEFT;
    if (len == 5 && strncmp(name, "right", 5) == 0) return PAD_BUTTON_RIGHT;
    printf("[TEST] input-script: unrecognized button name (len=%zu)\n", len);
    fflush(stdout);
    return 0;
}

void mp6_input_script_init(const char *spec)
{
    const char *p = spec;
    printf("[TEST] input-script armed: %s\n", spec);
    while (*p && g_scriptStepCount < MP6_SCRIPT_MAX_STEPS) {
        const char *sep = strchr(p, ';');
        size_t stepLen = sep ? (size_t)(sep - p) : strlen(p);
        const char *colon = (const char *)memchr(p, ':', stepLen);
        if (colon) {
            size_t keyLen = (size_t)(colon - p);
            const char *valStart = colon + 1;
            size_t valLen = stepLen - keyLen - 1;
            if (keyLen == 4 && strncmp(p, "wait", 4) == 0) {
                g_script[g_scriptStepCount].type = MP6_SCRIPT_WAIT;
                g_script[g_scriptStepCount].waitFrames = (u32)atoi(valStart);
                g_scriptStepCount++;
            } else if (keyLen == 5 && strncmp(p, "press", 5) == 0) {
                g_script[g_scriptStepCount].type = MP6_SCRIPT_PRESS;
                g_script[g_scriptStepCount].button = mp6_script_button_from_name(valStart, valLen);
                g_scriptStepCount++;
            } else if (keyLen == 5 && strncmp(p, "stick", 5) == 0) {
                /* analog-stick tilt step -- see the spec comment above. */
                MP6ScriptStep *st = &g_script[g_scriptStepCount];
                st->type = MP6_SCRIPT_STICK;
                st->button = 0;
                st->stickX = 0;
                st->stickY = 0;
                if (valLen == 2 && strncmp(valStart, "up", 2) == 0) st->stickY = 72;
                else if (valLen == 4 && strncmp(valStart, "down", 4) == 0) st->stickY = -72;
                else if (valLen == 4 && strncmp(valStart, "left", 4) == 0) st->stickX = -72;
                else if (valLen == 5 && strncmp(valStart, "right", 5) == 0) st->stickX = 72;
                else { printf("[TEST] input-script: unrecognized stick direction (len=%zu)\n", valLen); }
                g_scriptStepCount++;
            } else {
                printf("[TEST] input-script: unrecognized step keyword (len=%zu)\n", keyLen);
            }
        }
        if (!sep) break;
        p = sep + 1;
    }
    g_scriptCursor = 0;
    g_scriptWaitRemaining = 0;
    g_scriptActive = (g_scriptStepCount > 0);
    printf("[TEST] input-script: parsed %d step(s)\n", g_scriptStepCount);
    fflush(stdout);
}

/* Called once per tick, before mp6_pump_keyboard_to_pad's own status
 * computation -- advances the script state machine and returns any
 * button(s) that should be latched THIS tick (0 if none, or no script
 * armed at all). */
static s8 g_scriptTickStickX = 0; /* this tick's scripted analog tilt */
static s8 g_scriptTickStickY = 0;

static u16 mp6_input_script_advance(void)
{
    u16 pressedThisTick = 0;
    g_scriptTickStickX = 0;
    g_scriptTickStickY = 0;
    while (g_scriptActive && g_scriptCursor < g_scriptStepCount) {
        MP6ScriptStep *step = &g_script[g_scriptCursor];
        if (step->type == MP6_SCRIPT_WAIT) {
            if (g_scriptWaitRemaining == 0) {
                if (step->waitFrames == 0) {
                    g_scriptCursor++;
                    continue; /* zero-length wait -- no-op separator, move on now */
                }
                g_scriptWaitRemaining = step->waitFrames;
            }
            g_scriptWaitRemaining--;
            if (g_scriptWaitRemaining == 0) {
                g_scriptCursor++;
            }
            break; /* this tick is spent on the wait either way */
        } else if (step->type == MP6_SCRIPT_STICK) {
            g_scriptTickStickX = step->stickX; /* last stick step this tick wins */
            g_scriptTickStickY = step->stickY;
            g_scriptCursor++;
            /* keep looping: consume any further zero-gap steps too */
        } else {
            pressedThisTick |= step->button;
            g_scriptCursor++;
            /* keep looping: consume any further zero-gap press steps too */
        }
    }
    if (g_scriptActive && g_scriptCursor >= g_scriptStepCount) {
        g_scriptActive = false;
        printf("[TEST] input-script: complete\n");
        fflush(stdout);
    }
    return pressedThisTick;
}

static void mp6_pump_keyboard_to_pad(void)
{
    int numKeys = 0;
    const bool *keys = SDL_GetKeyboardState(&numKeys);
    PADStatus status;
    size_t i;
    memset(&status, 0, sizeof(status));

    if (keys) {
        for (i = 0; i < MP6_KEYBIND_COUNT; i++) {
            if (g_keyBinds[i].scancode < numKeys && keys[g_keyBinds[i].scancode]) {
                status.button |= g_keyBinds[i].button;
            }
        }
    }
    status.button |= g_padKeyLatch;
    g_padKeyLatch = 0;
    status.button |= mp6_input_script_advance();
    if (g_scriptTickStickX != 0 || g_scriptTickStickY != 0) { /* stick:X step */
        status.stickX = g_scriptTickStickX;
        status.stickY = g_scriptTickStickY;
    }
#ifdef __ANDROID__
    {
        /* The touch overlay's PAD state, OR-merged like every other
         * virtual source in this function. Touch analog tilt only applies
         * when no input-script stick step claimed this tick (scripts are
         * the deterministic-test path; a live thumb yields to them). */
        u16 touchBtn = 0;
        s8 touchSX = 0, touchSY = 0;
        mp6_touch_pad_collect(&touchBtn, &touchSX, &touchSY);
        status.button |= touchBtn;
        if (status.stickX == 0 && status.stickY == 0 && (touchSX != 0 || touchSY != 0)) {
            status.stickX = touchSX;
            status.stickY = touchSY;
        }
        /* U-A3: the touch overlay IS this device's controller -- a stock
         * Android install has no Bluetooth/USB pad. The merge just above
         * only feeds PADRead()'s per-tick BUTTON data (aurora's own
         * merge_virtual_status, gated on g_virtualPadActive) -- a
         * COMPLETELY SEPARATE signal from what platform/gx/ui/overlay.cpp's
         * "No controller assigned" warning actually checks:
         * PADGetIndexForPort()'s real-SDL-gamepad player index, OR'd with
         * PADGetKeyButtonBindings()'s g_keyboardBindings[port].m_mappingsSet
         * flag (aurora/lib/dolphin/pad/pad.cpp) -- neither of those two
         * consults g_virtualPadActive at all. Without this, a fresh Android
         * install (no persisted keyboard_bindings.dat, no real gamepad)
         * shows "Configure controller port 1 in Settings." forever, even
         * though every tap is already reaching the game correctly.
         * PADSetKeyboardActive() is the exact, side-effect-free lever
         * Aurora exposes for this: it only flips
         * g_keyboardBindings[port].m_mappingsSet, never touching the real
         * per-key scancode table (still 100% PAD_KEY_INVALID -- nothing
         * here opts into Aurora's OWN scancode poll, which stays exactly as
         * inert as it already is on Windows), so PADRead()'s own
         * scancode-loop stays a true no-op and no new input path opens up
         * -- this changes only whether the launcher considers the port
         * "configured", never what reaches the game. One-time: the flag
         * never needs re-arming once set. */
        {
            static bool touchPadMarkedConnected = false;
            if (!touchPadMarkedConnected) {
                PADSetKeyboardActive(PAD_CHAN0, TRUE);
                touchPadMarkedConnected = true;
            }
        }
    }
#endif
    if (mp6_auto_start_due_now()) {
        status.button |= PAD_BUTTON_START;
        printf("[MP6-INPUT] auto-injecting PAD_BUTTON_START at tick %ld\n", mp6_tick_count);
        fflush(stdout);
    }
    PADSetVirtualStatus(0, &status);
}

/* One VIWaitForRetrace() call = one Aurora frame cycle. See the file
 * header comment for the overall shape; order within a call:
 *   1. end the frame the PREVIOUS call opened (nothing to end on the
 *      very first call -- g_frameOpen starts false);
 *   2. pump SDL/Aurora events; AURORA_EXIT (window closed) -> clean exit;
 *   3. fire the game's pre-retrace callback (game/sreset.c's
 *      HuDvdErrDispIntFunc), matching real hardware's "just before
 *      retrace" timing;
 *   4. begin the next frame;
 *   5. refresh the virtual keyboard-PAD status, then fire the game's
 *      post-retrace callback (game/pad.c's PadReadVSync, which is what
 *      actually calls PADRead -- the virtual status has to be fresh
 *      before this fires, not after);
 *   6. advance the shared tick budget (mp6_tick_advance() -- NOT
 *      mp6_tick_and_maybe_exit(), which exits without ever calling
 *      aurora_shutdown() -- see mp6_boot.h's own comment for why that
 *      turns a clean tick-budget exit into an abort() here).
 *
 * Both the tick-budget exit and the window-close exit below share the
 * SAME clean-shutdown shape (aurora_shutdown() -> print -> exit(0)),
 * which produces a benign WARNING-level "Device lost: Device was
 * destroyed" -- a plain exit(0) mid-frame with the device still live
 * would instead surface as a FATAL in Aurora's static-destructor
 * teardown, which this port's log callback turns into an abort(). */
static void mp6_clean_shutdown_exit(const char *reason)
{
    /* Savestate (docs/SAVESTATE.md): after a restore, third-party TEARDOWN
     * state is not trustworthy, so do not run it.
     *
     * The savestate carve-out is a denylist -- it excludes the statics of
     * every TU we know is host-owned, but it cannot reach the static C
     * runtime's own globals, and each statically-linked third-party library
     * has to be carved out explicitly as it is discovered. Every layer fixed
     * so far revealed the next one at process exit: RmlUi's plugin registry,
     * then sqlite's shared-memory node list, then a remaining unidentified
     * static destructor -- all of them faulting in TEARDOWN, after the game
     * itself had run correctly all the way to the tick budget.
     *
     * Running a graceful shutdown over restored third-party state is
     * therefore doing unbounded work on data we already know may be stale,
     * purely to reach exit(). A restored process is a short-lived debugging
     * session; the honest move is to flush what the user cares about and let
     * the OS reclaim everything else. _exit() skips atexit handlers and
     * static destructors, which is exactly the code that was faulting.
     *
     * Deliberately scoped to restored processes ONLY: a normal run still
     * takes the full aurora_shutdown() path above, so the ordinary exit
     * behavior (and the benign "Device lost" WARNING it produces) is
     * completely unchanged. */
    if (mp6_savestate_was_restored()) {
        printf("[BOOT] %s -- savestate was restored this run, skipping third-party "
               "teardown (see docs/SAVESTATE.md), exiting 0\n", reason);
        mp6_savestate_guarded_exit(0); /* C15: the ONE exit seam for restored processes */
    }
    printf("[BOOT] %s -- shutting down Aurora, exiting 0\n", reason);
    fflush(stdout);
    { /* Tear the RmlUi document stacks down while the context is still
       * alive -- left to CRT static destructors they run AFTER
       * aurora_shutdown() has freed the context, an observed teardown UAF
       * (mp6_launcher_ui_teardown()'s comment, launcher_core.cpp has the
       * stack walk). No-op in automation mode (UI never initialized). */
        extern void mp6_launcher_ui_teardown(void);
        mp6_launcher_ui_teardown();
    }
    aurora_shutdown();
    exit(0);
}

/* ---------------------------------------------------------------------
 * 8. Fixed-60Hz tick throttle.
 *
 * WHY: this engine is a fixed-tick-per-frame design -- game logic advances
 * exactly one tick per VIWaitForRetrace call, with no delta-time scaling
 * anywhere in decomp code (real hardware's VI guaranteed the 60Hz cadence,
 * so the original game never needed any). The only frame-rate limiter this
 * port inherits is aurora_end_frame()'s vsync'd present: main_native.c
 * sets config.vsync = true, which Aurora's lib/webgpu/gpu.cpp
 * best_present_mode() maps to FifoRelaxed (or Fifo) -- i.e. the DISPLAY's
 * refresh rate, not the game's design rate. On a 60Hz display the two
 * accidentally coincide and everything paces correctly; on a high-refresh
 * display (e.g. ~176Hz) the whole game runs ~3x fast. The correct fix for
 * a fixed-tick engine is NOT delta-time surgery on game code (out of the
 * question under this project's decomp-read-only discipline anyway):
 * throttle the tick rate itself back to the design rate, here at the
 * single place the game blocks per frame. Presentation simply follows
 * ticks -- 60fps presented on a 176Hz vsync'd display is fine; each
 * present just lands on the next 176Hz vblank.
 *
 * MECHANISM: an absolute-deadline scheduler on the host monotonic clock
 * (mp6_host_monotonic_ns -- QueryPerformanceCounter underneath on win32).
 * Each tick advances a persistent next-deadline by exactly one period and
 * waits for it -- ABSOLUTE deadlines, so however long this tick's own work
 * took (game logic, and crucially aurora_end_frame()'s vsync present
 * block, <=1/176s ~= 5.7ms on this display, well inside the 16.67ms
 * budget) is absorbed into the same period rather than added on top of it
 * (the classic "sleep a fixed amount per frame" mistake, which would pace
 * at period+worktime and drift). There is therefore no double-throttle
 * against Aurora's vsync: vsync quantizes WHEN a present lands (176Hz
 * grid), this scheduler alone decides how many ticks happen per second --
 * and 176Hz vblanks always arrive faster than 60Hz deadlines. (Corollary,
 * documented not hidden: on a display REFRESHING SLOWER than MP6_TICK_HZ,
 * the vsync block itself would pace ticks down to the display rate --
 * deadlines would run perpetually late and the resnap rule below keeps
 * that graceful. Not this machine's case, 176 > 60.)
 *
 * LATE/RESNAP RULE: if this tick finds itself late by more than
 * MP6_TICK_RESNAP_PERIODS periods (debugger pause, disc-load stall, a
 * dragged window), the schedule re-anchors at now+period instead of
 * fast-forwarding through the backlog -- no spiral of death, no burst of
 * catch-up ticks played at max speed. Lateness of up to that many periods
 * is simply absorbed by running the next few ticks back-to-back (bounded,
 * at most 4 fast ticks), which keeps ordinary one-off scheduler hiccups
 * from accumulating into wall-clock drift.
 *
 * PRECISION: a coarse OS sleep alone (even at timeBeginPeriod(1)
 * resolution) has ~1-2ms of wakeup slop -- so this sleeps only until ~2ms
 * before the deadline, then spins on the monotonic clock for the
 * remainder (YieldProcessor/_mm_pause in the loop; worst case ~2ms of one
 * core per 16.67ms tick, typically ~1ms). The OS timer-resolution push
 * (timeBeginPeriod(1) via a runtime-resolved winmm.dll, atexit-paired
 * timeEndPeriod) is mp6_host_init() (platform/host/host_win32.c), called
 * once, lazily, at the moment the throttle first engages -- so
 * MP6_TICK_HZ=0 keeps loading no winmm and pushing no timer resolution,
 * exactly the env contract below. mp6_host_init's return value feeds the
 * "timeBeginPeriod(1) ok/UNAVAILABLE" status in the boot line.
 *
 * ENV CONTRACT (MP6_TICK_HZ):
 *   unset/empty -> 60 (the design rate; the default everyone gets).
 *   0           -> throttle fully disabled: VIWaitForRetrace does no QPC
 *                  reads, no timeBeginPeriod, no winmm load -- pure
 *                  free-run timing, the A/B and leakgate-comparability
 *                  escape hatch.
 *   other > 0   -> that tick rate (dev tool: slow-mo/fast-forward; values
 *                  above the display refresh are additionally capped by
 *                  the vsync present block, and MP6_TICK_HZ=0 -- not a
 *                  huge number -- is the real "uncapped" switch).
 *   invalid     -> 60, with a warning line (never silently 0: a typo'd
 *                  env var must not silently disable pacing).
 *
 * MP6_TICK_RATE_LOG=1 (default off): once per ~5s, prints one stderr line
 * with the measured tick rate over that window plus worst/mean scheduler
 * lateness -- a permanent env-gated diagnostic (matching
 * MP6_DIAG_DRAWCOUNT / MP6_AUTO_START_TICKS precedent: zero overhead
 * when unset).
 * --------------------------------------------------------------------- */
#define MP6_TICK_HZ_DEFAULT      60.0
#define MP6_TICK_RESNAP_PERIODS  4
#define MP6_TICK_SPIN_WINDOW_MS  2.0

/* The deadline math runs on the host seam's fixed nanosecond timebase
 * (mp6_host_monotonic_ns, 1e9 counts/second -- the win32 backend is QPC
 * underneath, scaled with exact integer math), so it is
 * platform-independent; the winmm timer-resolution push lives in
 * mp6_host_init(). */
#define MP6_TICK_NS_PER_SEC 1000000000ll

static double  g_tickHz = -1.0;          /* -1 = env not parsed yet; 0 = disabled */
static int64_t g_tickPeriodNs = 0;       /* ns per tick at g_tickHz */
static int64_t g_tickNextDeadline = 0;   /* ns; 0 = schedule not anchored yet */

/* Defined in launcher_core.cpp -- 1 in automation/straight-boot mode. Used
 * below to pick the no-env throttle default (automation = free-run). */
extern int mp6_launcher_is_automation(void);

static void mp6_tick_throttle_init(void)
{
    const char *env = getenv("MP6_TICK_HZ");
    int fromEnv = (env != NULL && *env != '\0');
    double hz;
    if (fromEnv) {
        char *end = NULL;
        double v = strtod(env, &end);
        if (end != env && v >= 0.0) {
            hz = v; /* explicit, incl. 0 = disable -- see the env contract above */
        } else {
            printf("[MP6-TICK] MP6_TICK_HZ='%s' is not a number >= 0 -- using the default %g Hz\n",
                   env, MP6_TICK_HZ_DEFAULT);
            hz = MP6_TICK_HZ_DEFAULT;
            fromEnv = 0;
        }
    } else {
        /* No explicit rate. Automation/straight-boot only needs to REACH a
         * state, not run in real time, so default the throttle OFF: reaching
         * mode-select drops from ~150s (60Hz, paced) to ~30s (free-run). Pairs
         * with automation's vsync-off default (main_native.c) so neither cap
         * applies. Interactive play keeps 60Hz for correct pacing; a real-time
         * gate (leakgate) sets MP6_TICK_HZ=60 to opt back in. */
        hz = mp6_launcher_is_automation() ? 0.0 : MP6_TICK_HZ_DEFAULT;
    }
    g_tickHz = hz;
    if (hz <= 0.0) {
        printf("[MP6-TICK] tick throttle DISABLED (%s) -- free-run timing "
               "(paced only by vsync, which automation also defaults off)\n",
               fromEnv ? "MP6_TICK_HZ=0" : "automation default; MP6_TICK_HZ=60 forces real-time");
        fflush(stdout);
        return;
    }
    g_tickPeriodNs = (int64_t)((double)MP6_TICK_NS_PER_SEC / hz + 0.5);
    if (g_tickPeriodNs < 1) g_tickPeriodNs = 1; /* absurd MP6_TICK_HZ (> 1e9) -- degenerate but safe */
    {
        /* The lazy timer-resolution push (winmm timeBeginPeriod(1) +
         * atexit timeEndPeriod) lives in mp6_host_init(); its return is
         * the status this boot line prints. Only reached when the
         * throttle actually engages (MP6_TICK_HZ=0 returned above),
         * preserving the env contract. */
        int timerResRaised = mp6_host_init();
        printf("[MP6-TICK] tick throttle active: %.3f Hz (period %.3f ms, timebase %lld ns/s, "
               "timeBeginPeriod(1) %s) -- MP6_TICK_HZ overrides, 0 disables\n",
               hz, 1000.0 / hz, (long long)MP6_TICK_NS_PER_SEC,
               timerResRaised ? "ok" : "UNAVAILABLE (coarser sleeps, longer spin tail)");
    }
    fflush(stdout);
}

/* Scheduler-lateness stats for the rate log below: how far past the
 * absolute deadline each tick's wait actually ended (0 for a perfectly-hit
 * deadline; includes genuine frame overruns). Reset every log window. */
static int64_t g_tickLateMaxNs = 0;
static int64_t g_tickLateSumNs = 0;
static long    g_tickLateSamples = 0;

static void mp6_tick_throttle_wait(void)
{
    int64_t now;
    if (g_tickHz < 0.0) {
        mp6_tick_throttle_init();
    }
    if (g_tickHz <= 0.0) {
        return; /* MP6_TICK_HZ=0: the legacy free-run timing path */
    }
    now = (int64_t)mp6_host_monotonic_ns();
    if (g_tickNextDeadline == 0) {
        /* First throttled tick: anchor the schedule one period out and let
         * this tick through immediately -- there is no meaningful "previous
         * tick" to pace against yet. */
        g_tickNextDeadline = now + g_tickPeriodNs;
        return;
    }
    g_tickNextDeadline += g_tickPeriodNs;
    if (now - g_tickNextDeadline > (int64_t)MP6_TICK_RESNAP_PERIODS * g_tickPeriodNs) {
        /* Late by more than the resnap budget (debugger pause, load stall,
         * window drag): re-anchor rather than fast-forward -- see the
         * section comment's LATE/RESNAP RULE. */
        g_tickNextDeadline = now + g_tickPeriodNs;
        return; /* already past even the NEW deadline's start point -- run now */
    }
    for (;;) {
        int64_t remain;
        now = (int64_t)mp6_host_monotonic_ns();
        remain = g_tickNextDeadline - now;
        if (remain <= 0) {
            break;
        }
        {
            double remainMs = (double)remain / 1000000.0;
            if (remainMs > MP6_TICK_SPIN_WINDOW_MS) {
                uint32_t sleepMs = (uint32_t)(remainMs - MP6_TICK_SPIN_WINDOW_MS);
                if (sleepMs > 0) {
                    mp6_host_sleep_ns((uint64_t)sleepMs * 1000000ull); /* was Sleep(sleepMs) */
                } else {
                    mp6_host_sleep_ns(0); /* 2..3ms band: just yield, re-check (was Sleep(0)) */
                }
            } else {
#ifdef _WIN32
                YieldProcessor(); /* _mm_pause -- final sub-2ms approach is a monotonic-clock spin */
#elif defined(__aarch64__)
                __asm__ __volatile__("yield"); /* aarch64's spin-loop hint -- the exact
                                                * analogue of YieldProcessor's _mm_pause */
#else
                sched_yield(); /* portable fallback -- coarser than a pause
                                * instruction but keeps the spin polite */
#endif
            }
        }
    }
    {
        /* now holds the loop-exit sample: how late past the deadline did we
         * actually resume? (0-lateness is impossible to distinguish from a
         * few-hundred-ns spin-exit granularity -- both count as "on time".) */
        int64_t late = now - g_tickNextDeadline;
        if (late < 0) late = 0;
        if (late > g_tickLateMaxNs) g_tickLateMaxNs = late;
        g_tickLateSumNs += late;
        g_tickLateSamples++;
    }
}

/* MP6_TICK_RATE_LOG=1: once per ~5s, one stderr line with the measured
 * tick rate over the elapsed window (Delta-tick / Delta-wall-QPC -- an
 * actual measurement, not the configured target) plus the scheduler
 * lateness stats gathered above. Works with the throttle DISABLED too
 * (MP6_TICK_HZ=0 prints the free-run rate; lateness stats just stay 0/0)
 * -- an A/B instrument for pacing investigations. */
static void mp6_tick_rate_log(void)
{
    static int     s_enabled = -1;   /* -1 = env not checked yet */
    static int64_t s_windowStartNs = 0;
    static long    s_windowStartTick = 0;
    int64_t now;
    double elapsed;
    if (s_enabled < 0) {
        const char *env = getenv("MP6_TICK_RATE_LOG");
        s_enabled = (env && *env && *env != '0') ? 1 : 0;
    }
    if (!s_enabled) {
        return;
    }
    now = (int64_t)mp6_host_monotonic_ns();
    if (s_windowStartNs == 0) {
        s_windowStartNs = now;
        s_windowStartTick = mp6_tick_count;
        return;
    }
    elapsed = (double)(now - s_windowStartNs) / (double)MP6_TICK_NS_PER_SEC;
    if (elapsed >= 5.0) {
        double rate = (double)(mp6_tick_count - s_windowStartTick) / elapsed;
        double lateMaxMs = (double)g_tickLateMaxNs / 1000000.0;
        double lateAvgMs = g_tickLateSamples > 0
            ? ((double)g_tickLateSumNs / (double)g_tickLateSamples) / 1000000.0
            : 0.0;
        fprintf(stderr, "[MP6-TICKRATE] window=%.2fs ticks=%ld rate=%.3f ticks/s "
                        "late(max=%.3fms avg=%.3fms n=%ld) tick=%ld\n",
                elapsed, (long)(mp6_tick_count - s_windowStartTick), rate,
                lateMaxMs, lateAvgMs, g_tickLateSamples, mp6_tick_count);
        fflush(stderr);
        s_windowStartNs = now;
        s_windowStartTick = mp6_tick_count;
        g_tickLateMaxNs = 0;
        g_tickLateSumNs = 0;
        g_tickLateSamples = 0;
    }
}

/* The minigame-stub's "black screen" half (see shim/include/mp6_boot.h).
 * platform/os/dll_bridge.c's
 * stub prolog sets mp6_dll_stub_black_screen_active the moment a
 * not-yet-decompiled minigame DLL is "loaded" -- checked here, once per
 * frame, right before the frame closes, so a plain opaque black quad
 * overrides whatever (if anything) the still-running boot-flow game logic
 * drew underneath. Deliberately a hand-written, fully self-contained GX
 * call sequence (own projection/vtxfmt/TEV setup, no shared state with
 * whatever the game left behind) rather than a call into any decomp
 * function -- this is PLATFORM code (matches this feature's own "wire at
 * dll_bridge.c's synthetic-REL layer" scope), and this exact recipe
 * (orthographic full-screen quad, single flat KONST-style color, no
 * texture) is copied faithfully from game/wipe.c's own WipeGXInit/
 * WipeNormalFade -- the same proven-safe "solid color covers the whole
 * frame" shape this codebase already uses for real screen wipes,
 * substituting a fixed opaque black instead of wipeData's animated
 * fade color. A single, properly self-paired GXBegin(4)/GXEnd() (no
 * display list involved), so the GXBegin/GXEnd tolerance machinery never
 * has to intervene on this shape. */
static void mp6_dll_stub_draw_black_screen(void)
{
    Mtx44 proj;
    Mtx modelview;
    GXColor black = { 0, 0, 0, 255 };
    /* WS4 (docs/WS4_WIDESCREEN_2D.md): this stub's own quad/viewport were
     * hardcoded to native 640, so a wide (Widescreen-on) window showed an
     * uncovered strip past pixel 640 on any still-undecompiled-minigame
     * black-screen stub. mp6_widescreen_render_width() returns exactly 640
     * (byte-identical) when disabled. */
    int w = mp6_widescreen_render_width();

    MTXOrtho(proj, 0, 480, 0, w, 0, 10);
    GXSetProjection(proj, GX_ORTHOGRAPHIC);
    MTXIdentity(modelview);
    GXLoadPosMtxImm(modelview, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetViewport(0, 0, w, 480, 0, 1);
    GXSetScissor(0, 0, w, 480);
    GXSetCullMode(GX_CULL_NONE);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaCompare(GX_GEQUAL, 1, GX_AOP_AND, GX_GEQUAL, 1);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_SPEC);
    GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_SPEC);

    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_U16, 0);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetTevColor(GX_COLOR1, black);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition2u16(0, 0);
    GXPosition2u16(w, 0);
    GXPosition2u16(w, 480);
    GXPosition2u16(0, 480);
    GXEnd();
}

void VIWaitForRetrace(void)
{
    if (g_frameOpen) {
        if (mp6_dll_stub_black_screen_active) {
            mp6_dll_stub_draw_black_screen();
        }
        /* Optional FPS overlay -- ImGui is already live between
         * aurora_begin_frame()/aurora_end_frame(), so this composits into
         * the frame about to present. Two-load early-out unless the user
         * enabled it in the launcher menu; automation runs (launcher
         * skipped) can never draw it. Runs on Android too now that the
         * launcher TUs compile there -- the same launcher-mode-only guard
         * inside means straight_boot/automation launches never touch it
         * on either platform. */
        { extern void mp6_launcher_frame_overlay(void); mp6_launcher_frame_overlay(); }
        mp6_gx_close_stale_primitive("closing it before aurora_end_frame()");
        aurora_end_frame();
        { extern void mp6_fs_frame_end(void); mp6_fs_frame_end(); } /* framescope */
        g_frameOpen = false;
    }

    /* Pace ticks to the design rate (section 8 above; default 60Hz,
     * MP6_TICK_HZ=0 restores the legacy free-run path).
     * Placed AFTER the present (frame N reaches the display as early as
     * possible; the vsync block it just paid is absorbed by the absolute
     * deadline) and BEFORE the event pump/keyboard-PAD refresh below, so
     * tick N+1's input is sampled at the START of its real 16.67ms slot,
     * not up to a full period stale. */
    mp6_tick_throttle_wait();
    mp6_tick_rate_log();

    const AuroraEvent *event = aurora_update();
    while (event != NULL && event->type != AURORA_NONE) {
        if (event->type == AURORA_EXIT) {
            mp6_clean_shutdown_exit("window closed");
        }
        if (event->type == AURORA_SDL_EVENT) {
            mp6_latch_key_down_event(&event->sdl);
            mp6_latch_savestate_key_event(&event->sdl);
            mp6_latch_menu_key_event(&event->sdl); /* F10 in-game menu toggle */
            { /* in-game UI + freecam event forwards -- both inert unless the
               * launcher/freecam actually armed them (launcher mode only). */
                extern void mp6_launcher_forward_sdl_event(const SDL_Event *ev);
                extern void mp6_freecam_input_event(const SDL_Event *ev);
                mp6_launcher_forward_sdl_event(&event->sdl);
                mp6_freecam_input_event(&event->sdl);
            }
#ifdef __ANDROID__
            mp6_touch_pad_event(&event->sdl); /* finger tracking (touch_pad.cpp) */
#endif
        }
        ++event;
    }
#ifdef __ANDROID__
    /* Apply deferred motor commands from main-loop context (see the
     * mp6_PADControlMotor queue above) -- right after the event pump, on
     * the thread's real stack, where a JNI-reaching rumble is legal. */
    mp6_pad_motor_apply_pending();
#endif

    /* WS2 (docs/WS2_DYNAMIC_WIDESCREEN.md): every tick (not just on a
     * detected resize event) -- mp6_widescreen_render_width() re-reads the
     * live window size fresh every call, and the decomp-side setter is a
     * cheap no-op whenever nothing changed since the last tick, so this is
     * the simplest robust way to converge a live interactive resize
     * without any separate resize-event bookkeeping. A true no-op (both
     * calls return 640 as a pure passthrough) when widescreen is disabled
     * -- the default -- so this line does not perturb any existing gate. */
    mp6_widescreen_apply_render_width(mp6_widescreen_render_width());

    /* WS14 (docs/WS14_DYNAMIC_EXTRUDE.md): the render-width/2D-layer sync
     * above already converges every tick, but WS11-13's own 3D backdrop
     * extrude + per-scene camera setup ran exactly ONCE, at scene load --
     * frozen at whatever window size happened to be current then. This
     * re-derives every REGISTERED 3D backdrop/camera from its own cached
     * native baseline using the CURRENT scale_factor(), every tick, so a
     * live interactive resize converges the 3D content too, not just the
     * 2D/render-target layers. A true no-op (a for-loop over permanently-
     * empty registries) when Widescreen is disabled -- see platform/hsf/
     * mp6_widescreen_extrude.c's own file header for the full mechanism. */
    mp6_widescreen_reapply();

    if (g_preRetraceCB) {
        g_preRetraceCB((u32)mp6_tick_count);
    }

    if (aurora_begin_frame()) {
        g_frameOpen = true;
#ifdef __ANDROID__
        /* Draw the touch overlay into the just-opened ImGui frame
         * (aurora ran ImGui::NewFrame inside the successful
         * aurora_begin_frame above; the overlay's foreground draw list is
         * frozen+composited over the game by the NEXT call's
         * aurora_end_frame). Guarded by the begin_frame result: on a
         * skipped frame (backgrounded/surface-lost) there is no open
         * ImGui frame to draw into. */
        mp6_touch_pad_draw();
#endif
    }
    /* Reset the draw-call bisect harness's
     * per-frame draw-index counter here, unconditionally (regardless of
     * aurora_begin_frame()'s result) -- decomp's own game logic issues the
     * same GXBegin/GXCallDisplayList calls every tick whether or not this
     * particular tick's frame actually gets presented (see the comment
     * immediately below), so "once per tick" is the right reset cadence
     * for MP6_SKIP_DRAWS="lo-hi" to mean the same, stable per-frame draw
     * range every single frame -- which is what makes a static screenshot
     * comparison against a reference image meaningful (a global,
     * never-reset counter would only ever hide ONE frame's worth of draws
     * once, for the life of the process). See section 7 below.
     *
     * MP6_DIAG_DRAWCOUNT (optional, throttled to 1 line/second): prints
     * the PREVIOUS frame's final count right before resetting -- lets a
     * bisect session immediately find a sane MP6_SKIP_DRAWS upper bound
     * (typical per-frame counts are ~60, far lower than a naive guess)
     * instead of trial-and-error against the actual per-frame ceiling. */
    if (getenv("MP6_DIAG_DRAWCOUNT") && (mp6_tick_count % 60) == 0) {
        printf("[MP6-DIAG-DRAWCOUNT] tick=%ld prev-frame draw count=%u\n", mp6_tick_count, g_mp6DrawIndex);
        fflush(stdout);
    }
    g_mp6DrawIndex = 0;
    /* aurora_begin_frame() returning false (window minimized, etc.) just
     * means this logical tick renders nothing new -- the NEXT call's step
     * 1 correctly sees g_frameOpen still false and skips ending a frame
     * that was never opened, then tries begin_frame() again. In a
     * normal run this returns true on every tick -- the frame loop
     * genuinely cycles. */

    { /* freecam: sample keyboard/stick + drain event deltas ONCE per tick,
       * at the same input-sampling point as the keyboard-PAD pump below
       * (shim/include/mp6_freecam.h; inert unless the Mods toggle is on). */
        extern void mp6_freecam_input_tick(void);
        mp6_freecam_input_tick();
    }
    mp6_pump_keyboard_to_pad();
    if (g_postRetraceCB) {
        g_postRetraceCB((u32)mp6_tick_count);
    }

    if (mp6_tick_advance()) {
        char reason[64];
        snprintf(reason, sizeof(reason), "reached %ld VIWaitForRetrace ticks (limit %d)",
                 mp6_tick_count, mp6_max_ticks);
        mp6_clean_shutdown_exit(reason);
    }
}

/* ---------------------------------------------------------------------
 * 3. MTX/VEC rename bridge -- see this file's header comment, case (b).
 *
 * Every one of these is a pure, unconditional rename to the real Aurora
 * function of the same math under its `C_MTX*`/`C_VEC*` name -- no
 * decomp-side logic lives here, just closing a naming gap. `Mtx`/`Vec`/
 * `ROMtx`/`Point3d` are plain arrays/structs (no pointer members), byte-
 * identical between decomp's and Aurora's `dolphin/mtx/GeoTypes.h`
 * (checked directly), so a plain pass-through is safe with no ABI risk.
 *
 * Aurora's OWN dolphin/mtx.h defines every one of these 19 names as a
 * macro chain (`PSMTXIdentity` -> `MTXIdentity` -> `C_MTXIdentity`, ...) --
 * useful for CALL sites, but it just as happily rewrites a *definition*
 * using the same identifier: `void PSMTXIdentity(...)` below would
 * otherwise preprocess into `void C_MTXIdentity(...)`, colliding with the
 * real one already in libaurora_mtx.a ("duplicate symbol", confirmed by
 * hitting it directly). #undef-ing each name first stops the macro from
 * touching this file's own definition while leaving the *call* to
 * `C_MTXFoo(...)` inside each body completely unaffected (that identifier
 * was never itself a macro). */
#undef PSMTXIdentity
#undef PSMTXCopy
#undef PSMTXConcat
#undef PSMTXInverse
#undef PSMTXInvXpose
#undef PSMTXReorder
#undef PSMTXTrans
#undef PSMTXScale
#undef PSMTXRotRad
#undef PSMTXRotAxisRad
#undef PSMTXMultVec
#undef PSMTXMultVecArray
#undef PSMTXMultVecSR
#undef PSMTXROMultVecArray
#undef PSVECAdd
#undef PSVECSubtract
#undef PSVECScale
#undef PSVECDotProduct
#undef PSVECCrossProduct

void PSMTXIdentity(Mtx m) { C_MTXIdentity(m); }
void PSMTXCopy(const Mtx src, Mtx dst) { C_MTXCopy(src, dst); }
void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab) { C_MTXConcat(a, b, ab); }
u32 PSMTXInverse(const Mtx src, Mtx inv) { return C_MTXInverse(src, inv); }
u32 PSMTXInvXpose(const Mtx src, Mtx invX) { return C_MTXInvXpose(src, invX); }
void PSMTXReorder(const Mtx src, ROMtx dest) { C_MTXReorder(src, dest); }
void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT) { C_MTXTrans(m, xT, yT, zT); }
void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS) { C_MTXScale(m, xS, yS, zS); }
void PSMTXRotRad(Mtx m, char axis, f32 rad) { C_MTXRotRad(m, axis, rad); }
void PSMTXRotAxisRad(Mtx m, const Vec* axis, f32 rad) { C_MTXRotAxisRad(m, axis, rad); }
void PSMTXMultVec(const Mtx m, const Vec* src, Vec* dst) { C_MTXMultVec(m, src, dst); }
void PSMTXMultVecArray(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    C_MTXMultVecArray(m, srcBase, dstBase, count);
}
void PSMTXMultVecSR(const Mtx m, const Vec* src, Vec* dst) { C_MTXMultVecSR(m, src, dst); }
void PSMTXROMultVecArray(const ROMtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    C_MTXROMultVecArray(m, srcBase, dstBase, count);
}
void PSVECAdd(const Vec* a, const Vec* b, Vec* ab) { C_VECAdd(a, b, ab); }
void PSVECSubtract(const Vec* a, const Vec* b, Vec* a_b) { C_VECSubtract(a, b, a_b); }
void PSVECScale(const Vec* src, Vec* dst, f32 scale) { C_VECScale(src, dst, scale); }
f32 PSVECDotProduct(const Vec* a, const Vec* b) { return C_VECDotProduct(a, b); }
void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* axb) { C_VECCrossProduct(a, b, axb); }

/* ---------------------------------------------------------------------
 * 5. GXBegin/GXEnd hardware-faithful tolerance + diagnostic. See this
 * file's header comment, case 4, and shim/include/dolphin_compat.h's own
 * section-4 comment for the rename mechanism that routes every decomp
 * call site here first.
 *
 * WHY: `game/hsfman.c`'s `Hu3DZClear()` -- real, unpatched, ORIGINAL
 * decomp source -- does `GXBegin(GX_QUADS, GX_VTXFMT0, 4)`, emits exactly
 * the 4 declared vertices, then returns with NO `GXEnd()` call anywhere
 * in the function. This is not a decompilation error: 2 more instances
 * of the EXACT same shape exist elsewhere in this codebase
 * (`game/wipe.c`'s `WipeDissolve`/`WipeViewShift`), so this is a
 * recurring, deliberate pattern in the original game. Real GameCube
 * hardware's GX FIFO parser ends a primitive purely by vertex count (N
 * declared by GXBegin, N vertices streamed, done) -- `GXEnd()` is
 * CPU-side bookkeeping ONLY, safely omittable whenever the caller
 * already streamed exactly the declared count, which `Hu3DZClear` does.
 * Aurora's own GX emulation, unlike real hardware, does NOT infer
 * completion from vertex count: it tracks an explicit open/close flag
 * cleared ONLY by an actual `GXEnd()` call, and FATALs the instant a
 * second `GXBegin` fires while that flag is still set -- so the FATAL
 * fires at some entirely innocent later call site that merely has the
 * bad luck of running next.
 *
 * The tolerance is bridge-side, NOT a decomp patch -- Hu3DZClear/
 * WipeDissolve/WipeViewShift are genuine, intentional original game code
 * (patching them would violate this project's own "decomp read-only,
 * patch queue for genuine bugs only" discipline; there is no bug in this
 * code to fix). `mp6_GXBegin` auto-closes any still-open primitive
 * (calling the REAL GXEnd()) before opening the new one -- matching real
 * hardware semantics exactly, since the FIFO has already moved past the
 * old primitive by vertex count long before this point. `VIWaitForRetrace`
 * (section 2 above) also auto-closes at frame-end, before
 * aurora_end_frame(), for the same reason, in case some future code path
 * leaves a primitive open with no further GXBegin call in the same frame
 * (not needed for TODAY's specific bug -- WipeNormalFade always runs
 * first -- but closes the general case, matching how real hardware never
 * cared about frame boundaries here either).
 * --------------------------------------------------------------------- */
static bool g_gxOpen = false;
static u32 g_gxCallNo = 0;      /* 1-based count of GXBegin calls seen so far */
static u32 g_gxOpenCallNo = 0;  /* which call number is currently open, if any */
static void *g_gxOpenRetAddr = NULL;
static GXPrimitive g_gxOpenType = (GXPrimitive)0;
static GXVtxFmt g_gxOpenFmt = (GXVtxFmt)0;
static u16 g_gxOpenCount = 0;
static bool g_gxOpenHidden = false; /* was this open primitive's scissor
                                     * zeroed by the draw-call bisect harness?
                                     * (section 7 below) -- if so, whichever
                                     * path actually closes it (mp6_GXEnd or
                                     * mp6_gx_close_stale_primitive) must
                                     * restore the scissor afterward. */

/* Once-per-distinct-call-site log throttle -- Hu3DZClear's own missing
 * GXEnd is hit every single frame once OpeningCreate's camera exists, so
 * an unthrottled log would print one line per frame for the rest of the
 * process's life. Matches MP6_LOG_ONCE's own "once per call site, not
 * once ever" spirit; sized generously for the small number of real call
 * sites this codebase has of this pattern (3 known total, so far). */
static void *g_gxToleratedSites[16];
static int g_gxToleratedSiteCount = 0;

/* A SECOND vertex-mismatch shape exists that this tolerance must NOT
 * handle (it gets a decomp patch instead): "[AURORA FATAL] GXEnd: vertex
 * count mismatch" out of game/hsfdraw.c's MDFaceDraw. (Aurora's own GXEnd
 * computes vtxSize = bytesWritten/nVerts by INTEGER DIVISION of the
 * actual byte count, so the sizes it prints are an arithmetic artifact
 * of the corruption, not the real configured vertex format.) MDFaceDraw's
 * per-face-type GXBegin calls are ANOTHER instance of the "real hardware
 * infers completion from vertex count, no GXEnd needed" pattern this
 * comment already describes for Hu3DZClear/HuSpr3DDisp above -- but this
 * one is NOT safe to leave to this general bridge tolerance, because the
 * whole per-material batch is recorded inside a GXBeginDisplayList/
 * GXEndDisplayList bracket, and Aurora's own fifo module (lib/gx/fifo.cpp)
 * tracks the display-list recording buffer (sDlBuffer/sDlWritePos)
 * completely separately from the immediate-mode buffer (sBufferSize) that
 * GXBegin/GXEnd's byte-count validation always reads from -- so by the time
 * THIS tolerance finally auto-closes the primitive (typically well after
 * the material's own GXEndDisplayList already returned), whatever unrelated
 * immediate-mode GX traffic ran in between gets misattributed as extra
 * bytes for that primitive. Fixed at the source instead (patches/decomp/
 * src/game/hsfdraw.c.patch): an explicit GXEnd() right after each of
 * MDFaceDraw's 3 vertex loops, BEFORE its own GXEndDisplayList() call --
 * guarantees Aurora's (buffer-blind) byte measurement always sees a 0-byte
 * delta at that exact point, a pure no-op on real hardware. */
/* ---------------------------------------------------------------------
 * 7. Draw-call bisect harness.
 *
 * When a visual bug resists code-reading, the assumption "we already
 * know which draw call paints these pixels" is itself unverified. This
 * harness answers that question empirically:
 * MP6_SKIP_DRAWS="lo-hi" hides every
 * draw whose PER-FRAME index (0-based, reset every tick -- see
 * VIWaitForRetrace's own comment above) falls in [lo, hi], every frame,
 * for both draw shapes that actually put pixels on screen:
 *   - immediate-mode GXBegin/GXEnd primitives (mp6_GXBegin below), and
 *   - GXCallDisplayList display-list replays (mp6_GXCallDisplayList below,
 *     dolphin_compat.h's hook for it) -- MOST real HSF face draws
 *     actually go through here (or
 *     GXFastCallDisplayList, redirected to the same place), not plain
 *     GXBegin, once a material repeats across consecutive faces.
 * One shared, monotonic g_mp6DrawIndex counts both kinds together in
 * whatever order they actually execute, so "draw 47" means the 47th
 * draw-shaped call of EITHER kind this frame, not two separate spaces.
 *
 * HOW HIDING WORKS, AND WHY NOT JUST SKIP THE CALL: for GXCallDisplayList,
 * simply not calling it at all would be safe (it writes a complete,
 * self-contained recorded byte stream to Aurora's FIFO -- external_refs/
 * repos/aurora/lib/dolphin/gx/GXDispList.cpp's GXCallDisplayList -- with no
 * external state left half-updated if skipped). But for GXBegin, skipping
 * the real call while still letting decomp's OWN subsequent GXPosition3f32/
 * GXColor4u8/etc. calls run (they are NOT bridged here, so they can't be
 * intercepted) would append real vertex-attribute bytes to Aurora's
 * immediate-mode buffer with NO preceding BEGIN header for the FIFO parser
 * to make sense of them against -- real corruption, not a clean no-op
 * (section 5 above is a long, hard-won lesson in exactly how unforgiving
 * this buffer's own bookkeeping is). So this harness NEVER skips a real
 * call. Instead it brackets the real call (both kinds, uniformly) with a
 * zero-area GXSetScissor(0,0,0,0) before and a restore-to-full-frame
 * GXSetScissor(0,0,640,480) after -- scissor is a pure rasterizer clip
 * (external_refs/repos/aurora/lib/dolphin/gx/GXCull.cpp's GXSetScissor),
 * completely orthogonal to vertex counts/FIFO byte bookkeeping, so this
 * cannot desync anything section 5 cares about. 640x480 matches this exact
 * file's own mp6_dll_stub_draw_black_screen (above) -- the established
 * full-frame constant already used elsewhere in this file, not a new guess.
 *
 * CAVEAT: this restores
 * scissor to a HARDCODED full-frame rect, not the true prior value (GX has
 * no "read back current register" call for CPU code to use -- real
 * hardware's own BP registers are write-only from the CPU's side, and nothing
 * in this codebase already shadows every GXSetScissor call site to track
 * "last requested rect" the way g_gxOpen* tracks GXBegin/GXEnd). If some
 * draw ahead of a skip range had deliberately set a non-full-frame scissor
 * for its own reasons, this harness would incorrectly widen it back to full
 * frame the moment the skip range ends. Fine for simple non-split-screen
 * scenes, and this is a debug-only, env-var-gated tool with zero effect
 * when unset -- but a real limitation for more complex use, flagged here
 * rather than silently assumed safe. */
static int g_mp6SkipDrawLo = -1;
static int g_mp6SkipDrawHi = -1;
static bool g_mp6SkipDrawParsed = false;
/* g_mp6DrawIndex itself is declared earlier, right after g_frameOpen in
 * section 2 above -- see that declaration's own comment for why. */

/* Public getter -- lets a TEMPORARY decomp-side diagnostic (added via
 * the normal patches/decomp queue, e.g. a FaceDraw material/attribute
 * print) correlate "which real material is this" against the exact
 * per-frame draw index MP6_SKIP_DRAWS bisects on,
 * without needing decomp code to reach into this file's statics directly.
 * Kept as a permanent, harmless one-line utility (matches this codebase's
 * general habit of leaving small, env-var-gated debug hooks in place, see
 * e.g. MP6_AUTO_START_TICKS/MP6_TEST_LOAD_DLL) even though any given
 * CALLER of it in decomp is typically temporary and reverted. */
u32 mp6_current_draw_index(void)
{
    return g_mp6DrawIndex;
}

static void mp6_parse_skip_draws(void)
{
    const char *env;
    g_mp6SkipDrawParsed = true;
    env = getenv("MP6_SKIP_DRAWS");
    if (!env || !*env) {
        return;
    }
    {
        char *end = NULL;
        long lo = strtol(env, &end, 10);
        long hi;
        if (end == env || *end != '-') {
            printf("[MP6-DRAWBISECT] MP6_SKIP_DRAWS='%s' not in 'lo-hi' form -- ignoring\n", env);
            fflush(stdout);
            return;
        }
        hi = strtol(end + 1, &end, 10);
        g_mp6SkipDrawLo = (int)lo;
        g_mp6SkipDrawHi = (int)hi;
        printf("[MP6-DRAWBISECT] MP6_SKIP_DRAWS active: hiding per-frame draw indices [%d, %d] "
               "(every frame; see docs/DEBUGGING.md)\n", g_mp6SkipDrawLo, g_mp6SkipDrawHi);
        fflush(stdout);
    }
}

static bool mp6_draw_should_skip(u32 idx)
{
    if (!g_mp6SkipDrawParsed) {
        mp6_parse_skip_draws();
    }
    if (g_mp6SkipDrawLo < 0) {
        return false;
    }
    return (int)idx >= g_mp6SkipDrawLo && (int)idx <= g_mp6SkipDrawHi;
}

/* Once-per-distinct-call-site log throttle, matching g_gxToleratedSites's
 * own precedent exactly (section 5 above) -- a stable per-frame skip range
 * would otherwise log identically every single frame for as long as the
 * process idles at the title screen. */
static void *g_mp6SkipLoggedSites[64];
static int g_mp6SkipLoggedSiteCount = 0;

static void mp6_draw_hide_begin(u32 idx, void *retAddr, const char *kind)
{
    bool alreadyLogged = false;
    int i;
    for (i = 0; i < g_mp6SkipLoggedSiteCount; i++) {
        if (g_mp6SkipLoggedSites[i] == retAddr) {
            alreadyLogged = true;
            break;
        }
    }
    if (!alreadyLogged) {
        char sym[256];
        mp6_symbolize_addr(retAddr, sym, sizeof(sym));
        printf("[MP6-DRAWBISECT] hiding draw #%u (%s, ret=%p -> %s)\n", idx, kind, retAddr, sym);
        fflush(stdout);
        if (g_mp6SkipLoggedSiteCount < (int)(sizeof(g_mp6SkipLoggedSites) / sizeof(g_mp6SkipLoggedSites[0]))) {
            g_mp6SkipLoggedSites[g_mp6SkipLoggedSiteCount++] = retAddr;
        }
    }
    GXSetScissor(0, 0, 0, 0);
}

static void mp6_draw_hide_end(void)
{
    GXSetScissor(0, 0, 640, 480);
}

static void mp6_gx_close_stale_primitive(const char *context)
{
    if (!g_gxOpen) {
        return;
    }
    bool alreadyLogged = false;
    for (int i = 0; i < g_gxToleratedSiteCount; i++) {
        if (g_gxToleratedSites[i] == g_gxOpenRetAddr) {
            alreadyLogged = true;
            break;
        }
    }
    if (!alreadyLogged) {
        printf("[GX-TOLERANCE] auto-closing GXBegin call #%u (ret=%p, type=%d fmt=%d n=%u) "
               "that never got its own GXEnd -- %s. This matches real hardware (the GX FIFO "
               "ends a primitive by vertex count, not an explicit End signal).\n",
               (unsigned)g_gxOpenCallNo, g_gxOpenRetAddr, (int)g_gxOpenType, (int)g_gxOpenFmt,
               (unsigned)g_gxOpenCount, context);
        fflush(stdout);
        if (g_gxToleratedSiteCount < (int)(sizeof(g_gxToleratedSites) / sizeof(g_gxToleratedSites[0]))) {
            g_gxToleratedSites[g_gxToleratedSiteCount++] = g_gxOpenRetAddr;
        }
    }
    GXEnd();
    g_gxOpen = false;
    if (g_gxOpenHidden) { /* mirror mp6_GXEnd's own restore -- this path
                            * closes a primitive that never got an explicit
                            * GXEnd at all, so it must do the restore too. */
        mp6_draw_hide_end();
        g_gxOpenHidden = false;
    }
}

void mp6_GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts)
{
    void *ret = __builtin_return_address(0);
    u32 drawIdx;
    g_gxCallNo++;
    mp6_gx_close_stale_primitive("closing it before the next GXBegin");
    /* See mp6_GXSetArray3's own comment (section 1 above) -- grows any
     * GXSetArray-registered array to cover this draw's real vertex count
     * before the draw itself opens. */
    mp6_gx_array_slots_grow_for_draw(nverts);
    drawIdx = g_mp6DrawIndex++;
    g_gxOpen = true;
    g_gxOpenCallNo = g_gxCallNo;
    g_gxOpenRetAddr = ret;
    g_gxOpenType = type;
    g_gxOpenFmt = vtxfmt;
    g_gxOpenCount = nverts;
    g_gxOpenHidden = mp6_draw_should_skip(drawIdx);
    if (g_gxOpenHidden) {
        mp6_draw_hide_begin(drawIdx, ret, "GXBegin");
    }
    GXBegin(type, vtxfmt, nverts);
}

void mp6_GXEnd(void)
{
    if (!g_gxOpen) {
        /* A stray GXEnd with nothing open is the mirror image of the same
         * "real hardware doesn't track this the way Aurora does" gap --
         * tolerate it silently rather than forwarding to Aurora's real
         * GXEnd(), which could just as easily assert in the opposite
         * direction. Not observed in practice (this branch has never
         * fired in any run so far), but safe by construction either way. */
        return;
    }
    g_gxOpen = false;
    GXEnd();
    if (g_gxOpenHidden) { /* restore the scissor this draw's own
                            * mp6_GXBegin zeroed -- see section 7 above. */
        mp6_draw_hide_end();
        g_gxOpenHidden = false;
    }
}

/* dolphin_compat.h's own `#define
 * GXCallDisplayList mp6_GXCallDisplayList` routes every decomp (and
 * GXFastCallDisplayList) call here first -- the display-list-replay half
 * of the draw-call bisect harness, same counter/hide mechanism as
 * mp6_GXBegin/mp6_GXEnd above, just self-contained in one call (no
 * separate open/close pairing exists for this shape). */
void mp6_GXCallDisplayList(const void *list, u32 nbytes)
{
    void *ret = __builtin_return_address(0);
    u32 drawIdx = g_mp6DrawIndex++;
    bool hidden = mp6_draw_should_skip(drawIdx);
    if (hidden) {
        mp6_draw_hide_begin(drawIdx, ret, "GXCallDisplayList");
    }
    GXCallDisplayList(list, nbytes);
    if (hidden) {
        mp6_draw_hide_end();
    }
}

/* ---------------------------------------------------------------------
 * W. Window mode & aspect policy.
 *
 * Two independent concerns, neither needing an Aurora-side change:
 *
 * (a) Window placement: a window whose caption sits off-screen LOOKS
 *     borderless while its full normal styles are present the whole time.
 *     Aurora treats only NEGATIVE window positions as "unset"
 *     (-> SDL_WINDOWPOS_UNDEFINED); an exact 0,0 is honored, and SDL
 *     positions are CLIENT-AREA coordinates on Windows -- pinning the
 *     drawable area to the screen's top-left corner puts the entire
 *     caption + left frame edge off-screen. Fixed config-side in
 *     main_native.c (windowPosX/Y = -1 -> OS-default placement);
 *     mp6_bridge_window_policy_init below additionally RESCUES any window
 *     whose caption still lands off-screen (defensive only).
 *
 * (b) Free-stretch distortion: Aurora's default content-framebuffer policy
 *     (window.cpp's g_frameBufferAspectFit=false -- note gx.hpp's own
 *     viewportPolicy default is AURORA_VIEWPORT_FIT but window.cpp's flag
 *     only flips when AuroraSetViewportPolicy() is actually CALLED, which
 *     nothing did) sizes the content framebuffer to the raw window pixel
 *     size. The game's 640x480 logical viewport/scissor then scale by the
 *     per-axis fb/logical ratios (lib/gx/gx.cpp map_logical_viewport), so
 *     a non-4:3 window stretches the whole scene anisotropically -- the
 *     reported "zoomed/inconsistent FOV" component. EFB-copy destinations
 *     (lib/dolphin/gx/GXFrameBuffer.cpp scale_copy_dst) use the same
 *     per-axis ratios, so title/book-page copies were being anisotropically
 *     rescaled too. Fix: call the PUBLIC AuroraSetViewportPolicy(
 *     AURORA_VIEWPORT_FIT) -- the content fb then fits the VI-configured
 *     aspect (MP6 passes &GXNtsc480IntDf: 640x480, so exactly 4:3, always
 *     -- vi.cpp configured_fb_size()) and Aurora's own present blit
 *     (lib/aurora.cpp end_frame -> calculate_present_viewport) centers it
 *     in the surface with the bars cleared black (the present pass's
 *     loadOp=Clear covers the whole surface; the content blit only covers
 *     the fitted viewport) -- a real letterbox/pillarbox, computed from
 *     the CONTENT aspect, engaged for any window shape.
 *
 *     A5: (b)'s AuroraSetViewportPolicy call itself now happens LATER --
 *     see mp6_bridge_apply_content_aspect_policy() below, called from
 *     main_native.c right before GameMain() instead of from here. Reason:
 *     aurora::rmlui's presentation-dimension calc (lib/rmlui.cpp
 *     presentation_dimensions_from_window_size) consumes the SAME
 *     window::get_window_size() fb_width/fb_height that (b) retargets --
 *     so engaging the fit here, before the RmlUi launcher menu ever ran,
 *     quietly composed the WHOLE launcher (menu/wordmark/watermark/
 *     disc-info/version-info) for a phantom letterboxed 4:3 sub-rectangle
 *     instead of the real window/display surface. Invisible on the
 *     desktop default 1024x768 window (already 4:3, so the fit was a
 *     no-op) but severe on any non-4:3 physical surface -- confirmed on a
 *     Galaxy S22+ (~21.5:9 landscape): the menu, wordmark, and version/
 *     status text all rendered as if the screen were a narrow 4:3 box,
 *     leaving partyboard's own (verbatim, working-as-designed) `@media
 *     (max-height: 640dp)` mobile layout to reflow against the WRONG
 *     viewport metrics. Full story: docs/A5_LAUNCHER_ASPECT.md.
 *
 * Belt and braces: (b) alone already guarantees an undistorted scene at
 * ANY window shape, but the window would still
 * freely resize into shapes that waste most of their area on bars -- so
 * interactive resizes are ALSO constrained to 4:3 via
 * SDL_SetWindowAspectRatio (SDL3 enforces it during the user's drag, on
 * the client area, which is exactly the region the game fills). Maximize/
 * snap/tiling-WM shapes bypass such constraints by design; those fall
 * through to (b)'s letterbox. MP6_FREE_ASPECT=1 (env) skips both aspect
 * measures -- a deliberate escape hatch to unconstrained free-stretch
 * scaling, for A/B comparisons; the placement fix stays unconditional.
 * This resize constraint is a WINDOW-shape preference only (it does not
 * touch g_frameBufferAspectFit / RmlUi sizing at all), so unlike (b)'s
 * content fit it stays applied from the very first frame -- re-applied,
 * config-aware, by mp6_launcher_apply_display() (launcher_core.cpp) once
 * the user's saved aspectLocked setting is known.
 * --------------------------------------------------------------------- */

/* WS2 (docs/WS2_DYNAMIC_WIDESCREEN.md): stashed by mp6_bridge_window_policy_init
 * below (the earliest point a real SDL_Window* is available), consumed by
 * mp6_widescreen_render_width() further down to query the LIVE window
 * size on every call -- interactive resizes must keep tracking, not just
 * the size at boot, and this is the one place a real SDL_Window* is
 * available this early with no extra plumbing (same stash WS1's own
 * discarded mechanism used, for the same reason). */
static SDL_Window *g_mp6AspectWindow = NULL;

void mp6_bridge_window_policy_init(void *sdlWindowPtr)
{
    SDL_Window *window = (SDL_Window *)sdlWindowPtr;
    const char *freeAspect = getenv("MP6_FREE_ASPECT");
    bool aspectLocked = !(freeAspect != NULL && freeAspect[0] != '\0' && freeAspect[0] != '0');
    g_mp6AspectWindow = window; /* WS2: see the static's own comment above -- stash before the NULL check so later queries are consistent even if this call is ever a no-op below */
    /* WS2 (docs/WS2_DYNAMIC_WIDESCREEN.md): this runs BEFORE config is ever
     * loaded (right after aurora_initialize(), same as before WS2), so the
     * only widescreen signal available this early is the MP6_WIDESCREEN
     * env lever (mirroring MP6_FREE_ASPECT's own env-only early decision)
     * -- mp6_widescreen_enabled() already consults it internally (see that
     * function). A real config-driven Widescreen selection unlocks the
     * window shape later too, via mp6_launcher_apply_display()
     * (launcher_core.cpp) once the saved setting is actually known --
     * exactly the same "early env-only guess, later config-aware
     * re-apply" shape aspectLocked itself already has. */
    bool widescreen = mp6_widescreen_enabled();

    if (window == NULL) {
        return; /* headless-ish/unexpected: nothing to police */
    }

    /* Window side only here: keep interactive resizes at 4:3 (or free,
     * under MP6_FREE_ASPECT or Widescreen) from the very first frame. The
     * CONTENT framebuffer fit (AuroraSetViewportPolicy) is applied
     * separately and later -- see mp6_bridge_apply_content_aspect_policy()
     * below. */
    if (widescreen) {
        SDL_SetWindowAspectRatio(window, 0.0f, 0.0f);
        printf("[MP6-WINDOW] window resize policy: FREE (Widescreen active) -- "
               "the render/camera/2D-HUD track whatever shape the window ends up\n");
    } else if (aspectLocked) {
        if (!SDL_SetWindowAspectRatio(window, 4.0f / 3.0f, 4.0f / 3.0f)) {
            printf("[MP6-WINDOW] SDL_SetWindowAspectRatio failed (%s) -- "
                   "letterboxed present (once gameplay starts) still guarantees "
                   "an undistorted scene\n",
                   SDL_GetError());
        }
        printf("[MP6-WINDOW] window resize policy: 4:3 constrained "
               "(MP6_FREE_ASPECT=1 restores free stretch)\n");
    } else {
        SDL_SetWindowAspectRatio(window, 0.0f, 0.0f);
        printf("[MP6-WINDOW] window resize policy: FREE (MP6_FREE_ASPECT set) -- "
               "non-4:3 window shapes will distort the scene once gameplay starts\n");
    }

    /* Off-screen-caption rescue (defensive: with main_native.c's
     * windowPosX/Y=-1 the OS-default placement already keeps the whole
     * frame visible; this catches any WM/session-restore corner case so
     * "looks borderless at launch" can't come back). SDL positions are
     * client-area coords; the caption occupies [top - borderTop, top). */
    {
        int wx = 0, wy = 0;
        int borderTop = 0, borderLeft = 0, borderBottom = 0, borderRight = 0;
        SDL_Rect usable;
        SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        if (SDL_GetWindowPosition(window, &wx, &wy) &&
            display != 0 && SDL_GetDisplayUsableBounds(display, &usable)) {
            /* Can fail on some backends/timing -- treat unknown borders as
             * zero-height (then only a client area itself above the usable
             * top triggers a move, which is still correct). */
            if (!SDL_GetWindowBordersSize(window, &borderTop, &borderLeft,
                                          &borderBottom, &borderRight)) {
                borderTop = 0;
                borderLeft = 0;
            }
            if (wy - borderTop < usable.y || wx - borderLeft < usable.x) {
                int nx = (wx - borderLeft < usable.x) ? usable.x + borderLeft : wx;
                int ny = (wy - borderTop < usable.y) ? usable.y + borderTop : wy;
                if (SDL_SetWindowPosition(window, nx, ny)) {
                    printf("[MP6-WINDOW] caption was off-screen (client at %d,%d, "
                           "frame top/left extend %d/%d px) -- moved to %d,%d so the "
                           "title bar is visible from launch\n",
                           wx, wy, borderTop, borderLeft, nx, ny);
                }
            }
        }
    }
    fflush(stdout);
}

/* A5: the CONTENT half of (b) above -- AuroraSetViewportPolicy itself,
 * deferred from "before the launcher" to "right before GameMain()" so the
 * RmlUi launcher (menu + persistent overlay) always composes against the
 * real window/display surface, on every aspect (docs/A5_LAUNCHER_ASPECT.md).
 *
 * aspectLockedCfg is the resolved user preference: g_cfg.aspectLocked in
 * launcher mode (mp6_launcher_cfg_aspect_locked(), launcher_core.cpp,
 * mirroring the existing mp6_launcher_cfg_backend()/_vsync() pattern) or a
 * fixed 1 (today's automation-mode default, matching this function's own
 * pre-A5 behavior when config is never read) otherwise. MP6_FREE_ASPECT
 * keeps absolute priority in both directions, same as before.
 *
 * Call once, right before GameMain(), on EVERY boot path (automation,
 * launcher.skip, and interactive Play alike) -- identical final
 * gameplay-time state to pre-A5, just applied later: zero frames render
 * between the old call site and this one on any path (the launcher menu
 * loop and any skip-mode gap both render zero frames of their own before
 * GameMain() starts), so the GAME's own first frame is unaffected. */
void mp6_bridge_apply_content_aspect_policy(int aspectLockedCfg)
{
    const char *freeAspect = getenv("MP6_FREE_ASPECT");
    bool envFree = freeAspect != NULL && freeAspect[0] != '\0' && freeAspect[0] != '0';
    bool aspectLocked = aspectLockedCfg != 0 && !envFree;
    /* WS2 (docs/WS2_DYNAMIC_WIDESCREEN.md): mp6_widescreen_set_enabled()
     * (platform/main_native.c) must run BEFORE this function for this to
     * see the right value -- both are called back-to-back, right before
     * GameMain(), same call-timing contract as every other setting in
     * this file. MP6_FREE_ASPECT keeps absolute top priority over BOTH
     * Widescreen and aspectLocked (unchanged escape hatch: raw anisotropic
     * stretch, for A/B comparisons only). */
    bool widescreen = mp6_widescreen_enabled() && !envFree;

    if (envFree) {
        AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
        printf("[MP6-WINDOW] content aspect policy: FREE STRETCH (MP6_FREE_ASPECT) -- "
               "non-4:3 window shapes will distort the scene\n");
    } else if (widescreen) {
        /* THE key trick (docs/WS2_DYNAMIC_WIDESCREEN.md section 2): stay
         * on FIT, never switch to STRETCH. FIT letterboxes/pillarboxes
         * the GX render target to match Aurora's own logical-fb aspect
         * (aurora lib/gx/gx.cpp vi::configured_fb_size(), driven by
         * whatever RenderMode the game's own VIConfigure call currently
         * holds) inside the real window. WS2 widens THAT logical aspect
         * itself (mp6_widescreen_apply_render_width(), called from the
         * game's own HuSysInit and every tick thereafter) to track the
         * live window aspect -- so FIT's fitted rectangle converges to
         * the full window with no visible bars, because the thing being
         * fit is already the right shape, not because fitting was
         * disabled. This is what makes WS2 a TRUE-WIDE render (the GX
         * viewport/scissor genuinely widen) rather than WS1's discarded
         * approach (STRETCH + an anamorphic projection widen -- visibly
         * distorts every 2D/HUD element, since STRETCH scales the whole
         * already-composited frame non-uniformly). */
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        printf("[MP6-WINDOW] content aspect policy: WIDESCREEN (dynamic true-wide, "
               "render_width=%d) -- 3D + 2D both track the live window aspect, no "
               "stretch, no pillarbox bars\n",
               mp6_widescreen_render_width());
    } else if (aspectLocked) {
        /* Sets a plain state flag + requests a deferred swapchain refit;
         * no GX FIFO write, no frame needs to be open. */
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        printf("[MP6-WINDOW] content aspect policy: 4:3 locked (letterboxed "
               "present; MP6_FREE_ASPECT=1 restores free stretch)\n");
    } else {
        AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
        printf("[MP6-WINDOW] content aspect policy: FREE STRETCH -- non-4:3 "
               "window shapes will distort the scene\n");
    }
    fflush(stdout);
}

/* ---------------------------------------------------------------------
 * WS2. Dynamic true-widescreen (docs/WS2_DYNAMIC_WIDESCREEN.md) --
 * implementation of shim/include/mp6_widescreen.h's contract.
 *
 * Every accessor below is computed FRESH on every call. No field here is
 * a cache that a resize event invalidates -- there IS no cache -- so
 * "recomputed on resize" falls out for free: the very next call (the very
 * next tick's HuSprDispInit/Hu3DCameraCreate/etc., or the per-tick
 * mp6_widescreen_apply_render_width() refresh below) sees the new window
 * shape.
 * --------------------------------------------------------------------- */
static bool g_mp6WidescreenEnabled = false;

void mp6_widescreen_set_enabled(int enabled)
{
    g_mp6WidescreenEnabled = enabled != 0;
}

int mp6_widescreen_enabled(void)
{
    /* MP6_WIDESCREEN: automation-compatible test/verification lever, same
     * shape as MP6_AUTO_START_TICKS/--input-script/MP6_FREE_ASPECT.
     * Automation mode NEVER reads mp6_config.json (docs/TESTING.md's
     * automation contract), so g_mp6WidescreenEnabled is always false on
     * every automation boot regardless of anything a real user
     * configured -- without this lever, a scripted/ticked verification
     * run could never reach or screenshot Widescreen mode at all. Unset
     * (the default): zero effect, every existing automated gate is
     * byte-unchanged. Consulted here (not re-checked at every call site)
     * so every consumer -- window-shape policy, content-aspect policy,
     * render_width/scale_factor/half_width_delta, and every decomp-side
     * patch that calls mp6_widescreen_enabled() directly -- agrees on one
     * answer. */
    if (g_mp6WidescreenEnabled) {
        return 1;
    }
    {
        const char *forceWide = getenv("MP6_WIDESCREEN");
        return (forceWide != NULL && forceWide[0] != '\0' && forceWide[0] != '0') ? 1 : 0;
    }
}

/* Rounds down to the nearest multiple of 16 -- matches the reference ROM
 * hack's own texture/framebuffer-width-alignment constraint
 * (WS_REFERENCE_STUDY.md section 7: 848 = round16(853.33)), generalized
 * to any width instead of hardcoded to one. */
static int mp6_align16_down(float v)
{
    int i = (int)v;
    return i & ~15;
}

int mp6_widescreen_render_width(void)
{
    int w, h;
    float liveAspect, raw;
    if (!mp6_widescreen_enabled() || g_mp6AspectWindow == NULL ||
        !SDL_GetWindowSizeInPixels(g_mp6AspectWindow, &w, &h) || w <= 0 || h <= 0) {
        return 640; /* native/disabled, or can't read the window yet -- never guess wide */
    }
    liveAspect = (float)w / (float)h;
    raw = 480.0f * liveAspect; /* == 640 * (liveAspect / (4/3)) -- see mp6_widescreen.h */
    if (raw < 640.0f) {
        raw = 640.0f; /* never narrower than native (a taller-than-4:3 window doesn't shrink the render) */
    }
    /* WS6 (docs/WS6_OVERLAY_CAMERAS.md): this used to clamp at a hardcoded
     * 1280.0f ("sane upper bound -- an extreme sliver window can't blow out
     * a texture/array bound"), which silently pinned the render width at
     * 32:9-class ultra-wide aspects (raw ~1707 at 2560x720) below what the
     * live window actually needed -- the game then rendered narrower than
     * the window, leaving a black band on the right even with every
     * overlay camera widened. Investigated what that cap actually
     * protected: nothing real. GXCopyDisp/GXSetDispCopySrc/GXSetDispCopyDst
     * are literal no-op stubs under Aurora (aurora/lib/dolphin/gx/
     * GXFrameBuffer.cpp -- there is no GameCube-style fixed EFB/XFB buffer
     * here at all); the one real texture allocation in that file,
     * copy_tex()'s scale_copy_dst(), sizes its destination texture against
     * the REAL GPU render-target size (the window's own backing texture,
     * gfx::get_render_target_size()) -- this "logical" fbWidth is only
     * ever used as a ratio denominator (targetWidth/logicalFbWidth), never
     * as an allocation size itself.
     *
     * The user asked for NO arbitrary policy cap -- the only bound should
     * be the GPU's own real hardware limit. Aurora's public API
     * (include/aurora/aurora.h) doesn't expose maxTextureDimension2D, and
     * the value lives on an internal C++ wgpu::Device this port's plain-C
     * bridge can't reach directly without patching + rebuilding the
     * vendored Aurora library itself (a heavy, independent CMake/Ninja/Dawn
     * step -- setup/lib/step_aurora.py's own docstring calls a from-scratch
     * Aurora build "20-60 minutes", utterly unlike this project's other
     * steps) -- a disproportionate, independently-risky lift for one
     * accessor. Reached the same information WITHOUT touching Aurora's
     * source at all: aurora/lib/webgpu/gpu.cpp already logs
     * "Using limits:\n  maxTextureDimension2D: N" via Log.info() straight
     * from the real adapter's negotiated device limits at startup, and
     * this port already registers an AuroraLogCallback that sees every
     * line (main_native.c's mp6_aurora_log_callback, config.logLevel =
     * LOG_INFO) -- so that callback now also scans for this exact
     * substring and stashes N (mp6_aurora_queried_max_texture_dimension_2d(),
     * shim/include/mp6_boot.h). This IS the live device value, genuinely
     * queried at this run's own startup, not a compile-time guess. Falls
     * back to 8192 only if that line was somehow never seen (e.g. called
     * before aurora_initialize() -- never happens in practice, since this
     * function needs g_mp6AspectWindow, itself only set after aurora_
     * initialize() returns) -- 8192 is the WebGPU specification's OWN
     * guaranteed baseline default for maxTextureDimension2D (every
     * conformant implementation must support at least this much), not an
     * arbitrary number, so even the fallback path is spec-derived rather
     * than a guess. */
    {
        int gpuMax = mp6_aurora_queried_max_texture_dimension_2d();
        float clampF = (float)(gpuMax > 0 ? gpuMax : 8192);
        if (raw > clampF) {
            raw = clampF;
        }
    }
    {
        int aligned = mp6_align16_down(raw);
        int result = aligned < 640 ? 640 : aligned;
        /* WS6 (docs/WS6_OVERLAY_CAMERAS.md): self-sync, closing a real
         * early-boot race this lane found by testing at an extreme aspect
         * (crash repro: MP6_WINDOW_SIZE=5120x480, "[AURORA FATAL] WebGPU
         * error 2: Viewport width (40960.000000) exceeds the maximum
         * (16384)" -- 40960 == 5120*8). Root cause: several decomp-side
         * camera patches (this lane's own boot.c/opening.c/filesel.c/
         * mdsel.c/actman.c/sequence.c widens, mirroring hsfman.c's own
         * established WS2 pattern) pass THIS function's return value
         * directly as a viewport width. Aurora's own map_logical_viewport
         * (aurora/lib/gx/gx.cpp) independently rescales every viewport by
         * targetWidth/logicalFbWidth, where logicalFbWidth comes from
         * RenderMode->fbWidth (via VIConfigure) -- kept in sync with this
         * function's own return value once per tick (aurora_bridge.c's
         * VIWaitForRetrace) and once at boot (game/init.c.patch's
         * HuSysInit). Both of those sync points can legitimately still be
         * stale relative to THIS specific call: HuSysInit's own sync runs
         * before g_mp6AspectWindow is guaranteed stashed (this function
         * still reads native 640 then, a no-op sync) and the very first
         * camera setup in the whole game (BootObjectSetup, bootDll's
         * prolog) runs before even one VIWaitForRetrace tick has ever
         * executed -- so RenderMode->fbWidth can still be native 640 at
         * the exact moment a decomp camera patch asks this function for a
         * viewport width and gets back the live (already-wide) value,
         * which Aurora then scales AGAIN by (targetWidth=5120)/
         * (logicalFbWidth=640, stale) = 8x, doubling the widen. Fully
         * idempotent and cheap to call unconditionally on every query (not
         * just once per tick): mp6_widescreen_apply_render_width() itself
         * early-returns the instant RenderMode->fbWidth already matches
         * (the normal case, every call after the first this tick), so this
         * adds one integer comparison to the steady-state path and a
         * real (rare, boot-only) VIConfigure() call exactly when it's
         * needed. Guarantees any camera/viewport call that just asked
         * "how wide?" gets an answer Aurora's own scaling math already
         * agrees with, at the exact moment it asks, regardless of tick
         * boundaries. A true no-op when Widescreen is disabled (this
         * whole function already early-returned 640 above in that case,
         * never reaching here). */
        mp6_widescreen_apply_render_width(result);
        return result;
    }
}

float mp6_widescreen_scale_factor(void)
{
    if (!mp6_widescreen_enabled()) {
        return 1.0f;
    }
    return (float)mp6_widescreen_render_width() / 640.0f;
}

float mp6_widescreen_half_width_delta(void)
{
    if (!mp6_widescreen_enabled()) {
        return 0.0f;
    }
    return (576.0f * mp6_widescreen_scale_factor() - 576.0f) / 2.0f;
}

/* Called every tick (VIWaitForRetrace below) so a live interactive resize
 * converges: mp6_widescreen_render_width() re-reads the CURRENT window
 * size every time, and mp6_widescreen_apply_render_width() (decomp-side,
 * patches/decomp/src/game/init.c.patch) is a cheap no-op whenever nothing
 * actually changed since the last tick. */
extern void mp6_widescreen_apply_render_width(int newFbWidth);

/* ---------------------------------------------------------------------
 * 6. Debug-console placement.
 *
 * tools/build.py never sets an explicit /SUBSYSTEM flag, so the linker
 * infers CONSOLE from the presence of a plain `main` (shim/mp6_boot.h's
 * own main_native.c) -- launching mp6native.exe with no parent console
 * (the ordinary "just run the .exe" path, not from an existing terminal)
 * makes Windows allocate a brand-new conhost window for our stdout/
 * OSReport traffic, entirely separate from the SDL/Aurora game window
 * this file bridges to. Both default to "let Windows decide" placement
 * (this port never calls SDL_SetWindowPosition; Aurora's own
 * lib/window.cpp passes SDL_WINDOWPOS_UNDEFINED), so they can land close
 * enough to visibly overlap: a debug/console strip bleeding into the
 * game window's bottom edge. That overlap is two independent OS-level
 * top-level windows landing too close together -- nothing is drawn into
 * the game's own presented framebuffer -- which is why the fix is OS
 * window placement, not a rendering change: once the game window exists (called from platform/
 * main_native.c right after aurora_initialize() returns), move the
 * console to a screen rect that provably does not intersect the game
 * window's rect. This never touches window CONTENT/compositing, only
 * its on-screen OS position, and only for a console this process itself
 * allocated: GetConsoleProcessList returning exactly 1 (only us) is the
 * standard, documented way to detect a freshly-allocated conhost with no
 * other attached process; a console INHERITED from an interactive shell
 * (PowerShell/Windows Terminal launched it) is the user's own terminal
 * and this deliberately leaves it completely alone rather than yanking
 * it around the screen. */
void mp6_bridge_post_window_init(void *sdlWindowPtr)
{
#ifndef _WIN32
    /* No console window concept on this platform -- the whole body below
     * is win32-only UI policy; stdout goes to the logcat pump instead. */
    (void)sdlWindowPtr;
#else
    SDL_Window *window = (SDL_Window *)sdlWindowPtr;
    HWND console = GetConsoleWindow();
    if (console == NULL || window == NULL) {
        return; /* no console (unusual subsystem/launch context) -- nothing to place */
    }

    DWORD ownerPids[2];
    DWORD attachedCount = GetConsoleProcessList(ownerPids, 2);
    if (attachedCount != 1) {
        printf("[BOOT] console is shared with another process (an interactive shell, most "
               "likely) -- leaving its window position alone\n");
        fflush(stdout);
        return;
    }

    int wx = 0, wy = 0, ww = 0, wh = 0;
    if (!SDL_GetWindowPosition(window, &wx, &wy) || !SDL_GetWindowSize(window, &ww, &wh)) {
        return;
    }

    SDL_Rect usable;
    SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    if (display == 0 || !SDL_GetDisplayUsableBounds(display, &usable)) {
        /* Fallback: no display-bounds info -- synthesize a generous rect
         * that still lets the "directly below" placement below work. */
        usable.x = 0;
        usable.y = 0;
        usable.w = wx + ww + 640;
        usable.h = wy + wh + 480;
    }

    RECT consoleRect;
    if (!GetWindowRect(console, &consoleRect)) {
        return;
    }
    const int cw = consoleRect.right - consoleRect.left;
    const int ch = consoleRect.bottom - consoleRect.top;
    if (cw <= 0 || ch <= 0) {
        /* A redirected-output launch (this port's own test harnesses use
         * Start-Process -RedirectStandardOutput/-RedirectStandardError,
         * confirmed to hit this exact case) can hand back a degenerate
         * console window with no real screen area -- nothing meaningful
         * to reposition, and SetWindowPos with a zero size is a pointless
         * no-op at best. Bail out rather than move a window that isn't
         * really occupying any screen space. */
        return;
    }
    const int gap = 8;

    /* Prefer directly below the game window -- the exact placement that
     * fixes the reported "bottom edge" overlap most intuitively -- then
     * to the right, then clamp into the usable area's bottom-right
     * corner as a last resort on a display too small for either (still a
     * strict improvement over "wherever Windows happened to cascade it",
     * even though a vanishingly small display could theoretically still
     * clip at that point). */
    int x, y;
    if (wy + wh + gap + ch <= usable.y + usable.h) {
        x = wx;
        y = wy + wh + gap;
    } else if (wx + ww + gap + cw <= usable.x + usable.w) {
        x = wx + ww + gap;
        y = wy;
    } else {
        x = usable.x + usable.w - cw;
        y = usable.y + usable.h - ch;
    }
    if (x < usable.x) x = usable.x;
    if (y < usable.y) y = usable.y;

    SetWindowPos(console, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    printf("[BOOT] repositioned debug console to (%d,%d) (size %dx%d) so it can't overlap the "
           "game window at (%d,%d) size %dx%d\n", x, y, cw, ch, wx, wy, ww, wh);
    fflush(stdout);
#endif /* _WIN32 -- see the #ifndef at the top of this function */
}
