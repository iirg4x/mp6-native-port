/* MP6 native port -- Aurora integration bridge (default/non-headless
 * build only; see tools/build.py's --headless flag).
 *
 * Compiled against AURORA's OWN dolphin/aurora headers (AURORA_FLAGS in
 * tools/build.py), NEVER the decomp's include/ tree and NEVER
 * shim/include/dolphin_compat.h -- keeping this file's header universe
 * strictly on the Aurora side guarantees GXAttr/BOOL/u32/etc. here mean
 * exactly what Aurora's compiled libraries expect, with zero risk of
 * accidentally resolving a decomp copy of a same-named header instead
 * (both trees have `dolphin/gx/GXGeometry.h` at the same relative path,
 * and -I order is deterministic per-TU, not per-#include).
 *
 * Five jobs:
 *   1. GXSetArray arity bridge (a genuine signature drift between decomp's
 *      and Aurora's copy of dolphin/gx/GXGeometry.h -- see the comment
 *      above mp6_GXSetArray3 below).
 *   2. The VI frame-pacing bridge: decomp's game/main.c calls
 *      VIWaitForRetrace() once per main-loop iteration (the classic
 *      GameCube "wait for vblank" idiom); Aurora has no such call at all
 *      -- its equivalent is aurora_update() + aurora_begin_frame()/
 *      aurora_end_frame(), a different (event-pump-then-maybe-skip-frame)
 *      shape. This file is the adapter: every VIWaitForRetrace() call ends
 *      the frame the PREVIOUS call opened, pumps SDL/Aurora events (a
 *      window-close request becomes a clean process exit), fires the
 *      game's registered pre/post-retrace callbacks (game/pad.c's
 *      PadReadVSync -- the game's actual per-frame controller sample point
 *      -- is one of these), then opens the next frame.
 *   3. The MTX/VEC rename bridge: Aurora ships a real, complete
 *      MTX/VEC/QUAT library, and the game must link against IT, never
 *      against generated no-op shims (a no-op matrix function silently
 *      produces a black screen the moment a draw depends on a computed
 *      matrix). 9 names are symbols Aurora's library exports under the
 *      exact name decomp's header resolves calls to, so no bridge code is
 *      needed -- just linking libaurora_mtx.a. 19 other names decomp needs
 *      under a `PSMTX*`/`PSVEC*` spelling have no symbol under that exact
 *      name in Aurora's library at all (Aurora only defines the generic
 *      `C_MTXFoo`/`C_VECFoo` equivalent), so these get a real, one-line,
 *      unconditional rename wrapper below. `Mtx`/`Vec`/`ROMtx`/`Point3d`/
 *      `Quaternion` are plain float arrays/structs with no pointer
 *      members, byte-identical between decomp's and Aurora's copies of
 *      `dolphin/mtx/GeoTypes.h` -- no ABI risk, so a plain pass-through is
 *      safe.
 *   4. GXBegin/GXEnd hardware-faithful tolerance: a pure rename, not an
 *      arity bridge like (1) -- decomp's and Aurora's GXBegin/GXEnd
 *      signatures already match exactly. Real, unpatched decomp code
 *      (e.g. game/hsfman.c's Hu3DZClear) never calls GXEnd() for some
 *      primitives because real hardware's GX FIFO ends a primitive by
 *      vertex count, not an explicit End signal -- but Aurora does NOT
 *      infer completion from vertex count and FATALs on "GXBegin: called
 *      without matching GXEnd". Tracks a single open/close boolean and
 *      auto-closes any still-open primitive before the next GXBegin or at
 *      frame-end -- see section 5 below for the full story, including the
 *      one case (game/sprput.c's HuSpr3DDisp) where this general
 *      tolerance wasn't enough on its own and needed a companion decomp
 *      patch too.
 *   5. Draw-call bisect harness: `MP6_SKIP_DRAWS "lo-hi"` hides a specific,
 *      index-addressed, per-frame range of draws by bracketing the real,
 *      unmodified call with a zero-area GXSetScissor, NOT by skipping the
 *      call itself -- so nothing about vertex-count/FIFO-buffer
 *      bookkeeping is ever disturbed. A pure empirical elimination tool
 *      ("which draw call paints these exact pixels"), absent/unset by
 *      default with zero overhead beyond one counter increment per draw.
 *
 * Every other GX/VI/PAD/MTX/VEC symbol decomp calls needs NO bridge code
 * at all: it links straight against Aurora's compiled, real definition of
 * the same name. This is NOT a weak-symbol override (weak-vs-archive
 * resolution does not behave as needed on this toolchain) --
 * tools/gen_shims.py's shims_generated_aurora.c simply never generates a
 * shim at all for any of these names, so decomp's own plain reference is
 * the ONLY one in the link and correctly pulls in Aurora's real archive
 * member. The handful of GX + VI gap symbols Aurora truly has no
 * definition for at all still get a (weak, but uncontested) logging
 * no-op from that same file.
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
#include "mp6_widescreen.h" /* dynamic true-widescreen support */
#include "mp6_enhancements.h" /* mp6_enh_widescreen -- the switch's one front door */
#include "mp6_savestate.h"  /* F5/F8 hotkeys -> queued, serviced at the frame boundary */
#include "mp6_unlocked_fps.h" /* Mods-page Unlocked FPS: retained-stream frame interpolation
                               * (platform/gx/frame_interp.c); hooks at the frame boundaries
                               * + the tick throttle's idle window below, all single-compare
                               * no-ops while the feature is off */
#include "mp6_frame_gate.h" /* Android: no simulation work without a presentable frame */
#include "mp6_parse.h" /* strict operational env/script numbers */
#include "mp6_events.h" /* the game-event bus the input script's waitev/pressuntil steps block on */
#include "mp6_console.h" /* dev console: keyboard ownership, the stat sampler's hooks,
                          * and the runtime overrides for this file's own diag levers */
#include "mp6_display.h" /* the output the window is on + live present sync -- section 2b
                          * below is this seam's one implementation */
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

/* SAVESTATE CARVE-OUT: host-owned statics in this TU must land in the
 * carve-out section (see mp6_host_section.h), and the #include below has
 * two load-bearing placement rules, both learned the hard way:
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

/* Live adapter capability, parsed by main_native.c from Aurora's own startup
 * log.  This TU is in the savestate host-state carve-out, so a state captured
 * on another GPU cannot overwrite the limit negotiated by this process. */
static int g_mp6AuroraMaxTextureDimension2D;

void mp6_aurora_record_max_texture_dimension_2d(int dimension)
{
    g_mp6AuroraMaxTextureDimension2D = dimension;
}

int mp6_aurora_queried_max_texture_dimension_2d(void)
{
    return g_mp6AuroraMaxTextureDimension2D;
}

/* ---------------------------------------------------------------------
 * 1. GXSetArray arity bridge.
 *
 * decomp's own call sites use the real-hardware 3-argument shape:
 * GXSetArray(attr, data, stride), no explicit size -- real hardware
 * addresses memory directly, there is no GPU buffer to size. Aurora's
 * real GXSetArray takes 5 arguments: (attr, data, size, stride, le).
 * `size` is NOT cosmetic: Aurora reads exactly that many bytes from
 * `data` later, at aurora_end_frame() time, to build a real GPU-visible
 * buffer -- exactly the information the real GameCube API never needed
 * and decomp's call sites don't have on hand (the element count only
 * becomes known later, at the paired GXBegin(..., nverts) call).
 *
 * A guessed, too-large size risks reading past the real allocation --
 * a genuine out-of-bounds read. A size=0 fallback is provably crash-safe
 * (Aurora never dereferences `data` for a 0-byte push) but not harmless:
 * it uploads zero bytes, so every indexed fetch for a GX_INDEX8/16
 * attribute (every HSF mesh vertex/normal/st/color) reads out-of-range
 * data and the mesh vanishes.
 *
 * PRIMARY MECHANISM: mp6_gxarray_registry.h/.c
 * (platform/gx/gxarray_registry.c). decomp never has a real element count
 * on hand here, but platform/hsf/hsf_load_native.c -- which allocates
 * every vertex/normal/st/color buffer HSF meshes pass here -- knows each
 * one's real byte size at allocation time and registers it there; this
 * bridge looks the pointer up and uses the real size when known, falling
 * back to the learned-size mechanism below (and ultimately the still
 * crash-safe size=0) for any call site the registry never saw.
 *
 * The arity gap itself (3 args supplied, 5 needed) can't be closed with
 * GNU ld's usual --wrap trick on this toolchain: zig cc/c++'s linker
 * frontend rejects `-Wl,--wrap=X` outright, and the linker it actually
 * invokes (lld-link, COFF/MSVC-style) doesn't implement --wrap-style
 * symbol interposition at all. A link-order trick (linking a strong
 * GXSetArray before libaurora_gx.a) doesn't work either: that archive
 * member also defines other functions decomp genuinely needs, so it gets
 * pulled in regardless and produces a duplicate-symbol error.
 *
 * Fixed instead in shim/include/dolphin_compat.h: every decomp call site
 * is renamed, at the preprocessor level, to `mp6_GXSetArray3` -- a name
 * Aurora's own build never defines, so there is no link-time collision to
 * resolve at all. This file provides the actual mp6_GXSetArray3
 * definition for the aurora build. `le = true`: this whole port is a
 * fresh recompile for a native little-endian target, so vertex arrays are
 * already little-endian from Aurora's point of view.
 * --------------------------------------------------------------------- */
/* LEARNED-SIZE MECHANISM (for arrays the HSF registry never saw):
 * non-HSF indexed consumers exist -- e.g. game/sprput.c's HuSpr3DDisp
 * registers a (col+1)*(row+1)-element vertex/texcoord grid via
 * GXSetArray, then submits a QUADS primitive that indexes into it; with
 * size=0 the whole quad grid (e.g. a message-window frame) collapses to
 * nothing visible.
 *
 * Learn the real size instead of guessing. GXSetArray(attr, data,
 * stride) is always followed by a GXBegin(..., nverts) before the
 * array's contents are ever really read -- cache (data, stride) per
 * attribute here; the very next mp6_GXBegin call (section 5 below)
 * computes nverts*stride and re-registers the real GXSetArray with that
 * size whenever it's bigger than whatever's already registered.
 *
 * Is nverts*stride always >= the true buffer size? For sequential
 * indexing it's exact. For HuSpr3DDisp's shared-grid-pool indexing
 * (proven algebraically): the true size is (col+1)*(row+1) elements,
 * nverts is col*row*4, and 4*col*row >= (col+1)*(row+1) for every integer
 * col,row >= 1, so this never under-reads, and the modest overshoot reads
 * only slightly past the real allocation -- still safely inside this
 * port's own large pre-reserved game arena, not the OS heap. Resetting to
 * 0 whenever the (data, stride) pair actually changes avoids carrying a
 * stale, too-large size onto an unrelated, smaller allocation that
 * happens to reuse the same attribute slot. */

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
    /* env latches the initial value; the dev console may override it live. */
    return mp6_console_cvar_get(MP6_CVAR_ARRAYPROBE, g_mp6ArrayProbeEnabled) != 0;
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

/* Retained-frame interpolation identity tap.  The forced-include compatibility
 * header renames only decomp/windowed GXLoadPosMtxImm call sites to this name;
 * this TU sees Aurora's real declaration and forwards immediately after
 * recording the active Hu3D camera/model context. */
void mp6_GXLoadPosMtxImm(const void *mtx, u32 id)
{
    mp6_fi_stream_note_pos_mtx();
    GXLoadPosMtxImm((void *)mtx, id);
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
    /* Display-list bytes RECORDED this frame -- the return value is already
     * here, so the console's scenerendering panel can report authored-vs-
     * replayed DL volume for free. */
    if (mp6_console_stats_active) mp6_console_note_dl_recorded((unsigned)written);
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
        int invalid = 0;
        while (*p && g_autoStartCount < MP6_AUTO_START_MAX) {
            const char *start;
            uint32_t value;
            while (*p == ',' || *p == ' ') p++;
            if (*p == '\0') break;
            start = p;
            while (*p && *p != ',' && *p != ' ') p++;
            if (!mp6_parse_u32_span(start, (size_t)(p - start), 0, INT_MAX, &value)) {
                fprintf(stderr, "[MP6-INPUT] invalid MP6_AUTO_START_TICKS token -- schedule disabled\n");
                g_autoStartCount = 0;
                invalid = 1;
                break;
            }
            g_autoStartTicks[g_autoStartCount++] = (int)value;
        }
        while (*p == ',' || *p == ' ') p++;
        if (!invalid && *p != '\0') {
            fprintf(stderr, "[MP6-INPUT] MP6_AUTO_START_TICKS exceeds %d entries -- schedule disabled\n",
                    MP6_AUTO_START_MAX);
            g_autoStartCount = 0;
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
        if ((long)g_autoStartTicks[i] == mp6_tick_count) return true;
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

/* Drop host input sampled on a timeline that will not advance.  Used by both
 * savestate restore and Android's background/surface-loss gate: the collector
 * TUs are correctly carved out of the game image, so their live fingers and
 * deltas must be cleared explicitly at either discontinuity. */
void mp6_aurora_input_reset_transients(void)
{
    g_padKeyLatch = 0;
    mp6_freecam_input_savestate_reset();
#ifdef __ANDROID__
    mp6_touch_pad_savestate_reset();
#endif
}

static void mp6_latch_key_down_event(const SDL_Event *sdlEvent)
{
    size_t i;
    if (sdlEvent->type != SDL_EVENT_KEY_DOWN) {
        return;
    }
    /* While the dev console owns the keyboard, a keystroke is text -- not a
     * button. Guarding the LATCH as well as the poll below matters because the
     * latch is sticky until the next pump: without it, the last character
     * typed before a command is submitted would still be delivered to the game
     * one tick later. Always false in automation (the console is unavailable
     * there, see shim/include/mp6_console.h). */
    if (mp6_console_captures_input()) {
        return;
    }
    for (i = 0; i < MP6_KEYBIND_COUNT; i++) {
        if (sdlEvent->key.scancode == g_keyBinds[i].scancode) {
            g_padKeyLatch |= g_keyBinds[i].button;
        }
    }
}

/* Savestate hotkeys: F5 saves, F8 loads -- the
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
        /* OS auto-repeat delivers a KEY_DOWN stream while the key is held,
         * and each one used to queue a fresh multi-second capture/restore
         * -- a one-second hold stacked ~30 of them and read as "savestates
         * hard-froze the game". One request per physical press. */
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

/* Dev console hotkey (shim/include/mp6_console.h). Grave/backtick is the
 * console key every developer already has muscle memory for; F9 is the
 * keyboard-layout escape hatch, since on AZERTY/JIS the key left of "1" is not
 * a backtick at all. Neither collides: F5/F8 are the savestate pair above,
 * F10/F1 open the in-game menu, and g_keyBinds is letters and arrows only.
 *
 * The bar has THREE states and the bare key cycles them (closed -> bar ->
 * bar+log -> closed). SHIFT is the shortcut straight to the scrollback and
 * straight back, so reading the log never costs a trip through closed --
 * which matters because closing the bar is also what returns the keyboard to
 * the game, and losing input for one frame while hunting for a log line is
 * exactly the kind of papercut a debug tool must not have.
 *
 * Both entry points are inert until the launcher grants availability, which
 * happens inside mp6_launcher_frame_overlay()'s launcher-mode guard -- so a
 * tick-budget, --input-script or MP6_AUTO_START_TICKS run cannot open the
 * console even if these keys are pressed. Same KEY_DOWN-event discipline as
 * the two latches above: a state poll would miss a fast tap. */
static void mp6_latch_console_key_event(const SDL_Event *sdlEvent)
{
    if (sdlEvent->type != SDL_EVENT_KEY_DOWN || sdlEvent->key.repeat) {
        return;
    }
    if (sdlEvent->key.scancode == SDL_SCANCODE_GRAVE ||
        sdlEvent->key.scancode == SDL_SCANCODE_F9) {
        if ((sdlEvent->key.mod & SDL_KMOD_SHIFT) != 0) {
            mp6_console_toggle_log();
        } else {
            mp6_console_toggle();
        }
    }
}

/* ---------------------------------------------------------------------
 * 6. Deterministic input-script bridge (test tooling) -- MOVED.
 *
 * The engine (spec syntax, parser, per-tick state machine and the
 * event-bound waitev/pressuntil steps) now lives in
 * platform/os/input_script.c, which is in PLATFORM_SOURCES_COMMON so the
 * --headless build gets it too. Read that file's header comment for the
 * full design and for why the move was necessary: nothing in the engine was
 * ever windowed -- it reads this process's tick counter and the in-process
 * event bus and returns PAD bits -- yet living here made `--input-script` an
 * Aurora-only feature, which forced every board-reaching drive through the
 * shared GPU lock at ~6 minutes a run.
 *
 * What stayed here is the only genuinely Aurora-side part: OR-merging this
 * tick's scripted buttons and analog tilt into the same virtual PADStatus
 * (PADSetVirtualStatus) that the keyboard bridge above feeds. See
 * mp6_pump_keyboard_to_pad() below.
 * --------------------------------------------------------------------- */

static void mp6_pump_keyboard_to_pad(void)
{
    int numKeys = 0;
    const bool *keys = SDL_GetKeyboardState(&numKeys);
    PADStatus status;
    size_t i;
    memset(&status, 0, sizeof(status));

    /* THE DEV CONSOLE OWNS THE KEYBOARD WHILE IT IS OPEN.
     *
     * This function polls SDL_GetKeyboardState directly and consults neither
     * ImGui's io.WantCaptureKeyboard nor RmlUi focus -- it never had to,
     * because PADBlockInput() shuts the pad off downstream whenever a launcher
     * document is visible. That is a downstream guard on a different signal,
     * and relying on it for a TEXT FIELD is how "typing stat fps also presses
     * the buttons s, t, a, f and p" happens. So the console gets its own guard
     * here, at the source: no state poll, and the sticky down-edge latch is
     * dropped rather than deferred to the next tick.
     *
     * mp6_input_script_advance() below stays UNCONDITIONAL. The scripted-input
     * path is the automation path, the console is unavailable in automation by
     * construction, and gating it here would silently freeze every gate that
     * drives the game with --input-script. */
    if (mp6_console_captures_input()) {
        keys = NULL;
        g_padKeyLatch = 0;
    }

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
    {   /* stick:X step -- the analog half of the same one advance above */
        s8 scriptStickX = 0, scriptStickY = 0;
        mp6_input_script_stick_get(&scriptStickX, &scriptStickY);
        if (scriptStickX != 0 || scriptStickY != 0) {
            status.stickX = scriptStickX;
            status.stickY = scriptStickY;
        }
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
        /* On Android, the touch overlay IS this device's controller -- a
         * stock Android install has no Bluetooth/USB pad. The merge just
         * above
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
static void mp6_present_rate_log_final(void); /* defined with the rate-log block below */

static void mp6_clean_shutdown_exit(const char *reason)
{
    mp6_present_rate_log_final(); /* MP6_PRESENT_RATE_LOG=1: the begin/end/tick
                                   * totals line both Unlocked FPS gates quote */
    /* After a savestate restore, third-party TEARDOWN state is not
     * trustworthy, so do not run it.
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
               "teardown, exiting 0\n", reason);
        mp6_savestate_guarded_exit(0); /* the ONE exit seam for restored processes */
    }
    printf("[BOOT] %s -- shutting down Aurora, exiting 0\n", reason);
    fflush(stdout);
    { /* Persist the window's last position (video.window_x/_y) exactly ONCE
       * per session, here -- never per SDL_EVENT_WINDOW_MOVED, which would
       * rewrite the config file on every pixel of a drag. Launcher-mode-gated
       * inside the launcher accessor, so automation can never write a config.
       * Deliberately AFTER the savestate `_exit` branch above: a restored
       * process's window state is not this session's own. */
        extern int mp6_display_window_pos_get(int *x, int *y);
        extern void mp6_launcher_note_window_position(int x, int y);
        int wx = 0, wy = 0;
        if (mp6_display_window_pos_get(&wx, &wy)) {
            mp6_launcher_note_window_position(wx, wy);
        }
    }
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
 * anywhere in decomp code (real hardware's VI guaranteed the 60Hz cadence).
 * The only frame-rate limiter this port inherits is aurora_end_frame()'s
 * vsync'd present, which paces to the DISPLAY's refresh rate, not the
 * game's design rate -- on a high-refresh display (e.g. ~176Hz) the whole
 * game would run ~3x fast. The fix is not delta-time surgery on game code
 * (out of the question under this project's decomp-read-only discipline):
 * throttle the tick rate itself back to the design rate, here at the
 * single place the game blocks per frame.
 *
 * MECHANISM: an absolute-deadline scheduler on the host monotonic clock.
 * Each tick advances a persistent next-deadline by exactly one period and
 * waits for it -- ABSOLUTE deadlines, so however long this tick's own work
 * took is absorbed into the same period rather than added on top of it
 * (the classic "sleep a fixed amount per frame" mistake, which would pace
 * at period+worktime and drift). There is no double-throttle against
 * Aurora's vsync: vsync quantizes WHEN a present lands, this scheduler
 * alone decides how many ticks happen per second, and a faster-refreshing
 * display's vblanks always arrive before the next 60Hz deadline.
 *
 * LATE/RESNAP RULE: if a tick finds itself late by more than
 * MP6_TICK_RESNAP_PERIODS periods (debugger pause, disc-load stall, a
 * dragged window), the schedule re-anchors at now+period instead of
 * fast-forwarding through the backlog -- no spiral of death, no burst of
 * catch-up ticks played at max speed. Lateness up to that many periods is
 * absorbed by running the next few ticks back-to-back (bounded, at most 4
 * fast ticks).
 *
 * PRECISION: a coarse OS sleep alone has ~1-2ms of wakeup slop -- so this
 * sleeps only until ~2ms before the deadline, then spins on the monotonic
 * clock for the remainder (YieldProcessor/_mm_pause in the loop). The OS
 * timer-resolution push (timeBeginPeriod(1), atexit-paired
 * timeEndPeriod) happens once, lazily, at the moment the throttle first
 * engages.
 *
 * ENV CONTRACT (MP6_TICK_HZ):
 *   unset/empty -> 60 (the design rate; the default everyone gets).
 *   0           -> throttle fully disabled: pure free-run timing, the A/B
 *                  and leakgate-comparability escape hatch.
 *   other > 0   -> that tick rate (dev tool: slow-mo/fast-forward).
 *   invalid     -> 60, with a warning line (never silently 0: a typo'd
 *                  env var must not silently disable pacing).
 *
 * MP6_TICK_RATE_LOG=1 (default off): once per ~5s, prints one stderr line
 * with the measured tick rate over that window plus worst/mean scheduler
 * lateness -- a permanent env-gated diagnostic, zero overhead when unset.
 * --------------------------------------------------------------------- */
#define MP6_TICK_HZ_DEFAULT      60.0
#define MP6_TICK_HZ_MIN          0.01
#define MP6_TICK_HZ_MAX          1000.0
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
        double v;
        if (mp6_parse_double_strict(env, 0.0, MP6_TICK_HZ_MAX, &v) &&
            (v == 0.0 || v >= MP6_TICK_HZ_MIN)) {
            hz = v; /* explicit, incl. 0 = disable -- see the env contract above */
        } else {
            printf("[MP6-TICK] MP6_TICK_HZ='%s' is invalid (expected 0 or %.2f..%.0f) -- using the default %g Hz\n",
                   env, MP6_TICK_HZ_MIN, MP6_TICK_HZ_MAX, MP6_TICK_HZ_DEFAULT);
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

/* Idle-budget stats: how much of each tick's period was still unspent when the
 * tick's own work handed control back to the throttle. This is the quantity
 * that decides whether an Unlocked-FPS idle window EXISTS -- the replay path is
 * offered the window only while this exceeds MP6_TICK_SPIN_WINDOW_MS -- so a
 * present rate pinned at the tick rate is attributable here before any
 * replay-side reason is consulted. Reset every log window, like the lateness
 * stats above. */
static int64_t g_tickSlackMaxNs = 0;
static int64_t g_tickSlackMinNs = 0;
static int64_t g_tickSlackSumNs = 0;
static long    g_tickSlackSamples = 0;
static long    g_tickSlackStarved = 0; /* ticks with no idle window at all */

/* Where a tick's non-idle time actually goes, so a starved idle window can
 * name its consumers instead of being blamed on "the scene is heavy":
 *   game     -- everything outside VIWaitForRetrace: decomp game logic plus
 *               the GX command emission it performs
 *   endframe -- the present block: overlay composite plus aurora_end_frame(),
 *               i.e. aurora's own FIFO process + GPU submit
 *   seal     -- mp6_fi_note_frame_end(): the retained-stream walk and pairing.
 *               PORT-SIDE cost that exists only because the feature is on, so
 *               it is the one bucket a fix here may legitimately attack.
 *   vipost   -- the rest of VIWaitForRetrace after the throttle returns
 * Sampled only while MP6_TICK_RATE_LOG is on (four QPC reads/tick otherwise
 * skipped); reset every log window with the stats above. */
static int64_t g_phGameNs, g_phEndFrameNs, g_phSealNs, g_phViPostNs;
static long    g_phSamples = 0;
static int64_t g_phLastReturnNs = 0; /* when the previous VIWaitForRetrace returned */

/* The overlay composite's own sub-bracket, carved OUT of the endframe bucket
 * above rather than added beside it.
 *
 * mp6_launcher_frame_overlay() runs inside the endframe block, and the dev
 * console's whole panel refresh happens inside that call -- so without this
 * split the console's `stat unit` page would report its own cost as "GX
 * submit" and an instrument would be measuring itself. The measured precedent
 * for how badly that goes is in this tree: shim/include/mp6_frame_dump.h
 * records a full-screen readback costing ~9.8ms/frame and consuming the entire
 * tick idle window, so every captured frame came back replay=0 -- the
 * instrument destroyed the population it was observing.
 *
 * g_phEndFrameNs keeps its existing meaning (the WHOLE present block) so the
 * [MP6-TICKRATE] line is unchanged; the console subtracts this bucket from it
 * and reports the remainder as submit and this as its own excluded row. */
static int64_t g_phOverlayNs;

/* This tick's raw lateness and idle slack, kept alongside the window SUMS
 * above. The sums are what [MP6-TICKRATE] averages; the console needs the
 * per-tick values, because a percentile cannot be recovered from a mean. Two
 * stores per tick, unconditional. */
static int64_t g_tickLastLateNs = 0;
static int64_t g_tickLastSlackNs = 0;

static int mp6_tick_clock_now(int64_t *out)
{
    uint64_t now = mp6_host_monotonic_ns();
    if (out == NULL || now > (uint64_t)INT64_MAX) return 0;
    *out = (int64_t)now;
    return 1;
}

static int mp6_tick_deadline_add(int64_t base, int64_t delta, int64_t *out)
{
    if (out == NULL || base < 0 || delta <= 0 || base > INT64_MAX - delta) return 0;
    *out = base + delta;
    return 1;
}

static void mp6_tick_throttle_overflow(void)
{
    fprintf(stderr, "[MP6-TICK] monotonic deadline range exhausted -- disabling throttle safely\n");
    g_tickHz = 0.0;
    g_tickNextDeadline = 0;
}

static void mp6_tick_throttle_wait(void)
{
    int64_t now;
    int fiDeclined;
    if (g_tickHz < 0.0) {
        mp6_tick_throttle_init();
    }
    if (g_tickHz <= 0.0) {
        return; /* MP6_TICK_HZ=0: the legacy free-run timing path */
    }
    if (!mp6_tick_clock_now(&now)) {
        mp6_tick_throttle_overflow();
        return;
    }
    if (g_tickNextDeadline == 0) {
        /* First throttled tick: anchor the schedule one period out and let
         * this tick through immediately -- there is no meaningful "previous
         * tick" to pace against yet. */
        if (!mp6_tick_deadline_add(now, g_tickPeriodNs, &g_tickNextDeadline)) {
            mp6_tick_throttle_overflow();
        }
        return;
    }
    if (!mp6_tick_deadline_add(g_tickNextDeadline, g_tickPeriodNs,
                               &g_tickNextDeadline)) {
        mp6_tick_throttle_overflow();
        return;
    }
    if (now > g_tickNextDeadline &&
        now - g_tickNextDeadline > (int64_t)MP6_TICK_RESNAP_PERIODS * g_tickPeriodNs) {
        /* Late by more than the resnap budget (debugger pause, load stall,
         * window drag): re-anchor rather than fast-forward -- see the
         * section comment's LATE/RESNAP RULE. */
        if (!mp6_tick_deadline_add(now, g_tickPeriodNs, &g_tickNextDeadline)) {
            mp6_tick_throttle_overflow();
        }
        return; /* already past even the NEW deadline's start point -- run now */
    }
    {
        /* `now` was sampled on entry and g_tickNextDeadline is this tick's
         * deadline, so their difference IS this tick's leftover budget. */
        int64_t slack = g_tickNextDeadline - now;
        if (slack < 0) slack = 0;
        g_tickLastSlackNs = slack; /* per-tick, for the console's percentiles */
        if (g_tickSlackSamples == 0 || slack < g_tickSlackMinNs) g_tickSlackMinNs = slack;
        if (slack > g_tickSlackMaxNs) g_tickSlackMaxNs = slack;
        if (slack > INT64_MAX - g_tickSlackSumNs) g_tickSlackSumNs = INT64_MAX;
        else g_tickSlackSumNs += slack;
        g_tickSlackSamples++;
        if ((double)slack / 1000000.0 <= MP6_TICK_SPIN_WINDOW_MS) g_tickSlackStarved++;
    }
    /* One idle window, one replay verdict per decline.
     *
     * The sleep below truncates to whole milliseconds, so whenever the window
     * ends in the 2..3ms band the loop degenerates into mp6_host_sleep_ns(0)
     * yields -- and it used to re-offer the window to mp6_fi_idle_present on
     * every one of them. Measured on the w01 board: 500-1000 calls per tick.
     * That is not merely wasted work. Each declined call runs
     * fi_budget_decay(), which shrinks the measured replay-cost estimate by
     * an eighth, so several hundred declines annihilate it inside a single
     * tick; admission then alternates between "budget 0, admit anything" and
     * "budget 5ms, refuse everything" instead of tracking the real cost.
     *
     * A decline is final for this window by construction: slack only shrinks
     * as the deadline approaches, so a replay that does not fit now cannot fit
     * later in the same window. (A SUCCESSFUL replay still re-offers the
     * window -- that is how a light scene fits its eight.) */
    fiDeclined = 0;
    for (;;) {
        int64_t remain;
        if (!mp6_tick_clock_now(&now)) {
            mp6_tick_throttle_overflow();
            return;
        }
        remain = g_tickNextDeadline - now;
        if (remain <= 0) {
            break;
        }
        {
            double remainMs = (double)remain / 1000000.0;
            if (remainMs > MP6_TICK_SPIN_WINDOW_MS) {
                /* Unlocked FPS (shim/include/mp6_unlocked_fps.h): spend the
                 * idle window presenting interpolated frames instead of
                 * sleeping through it. Pass the throttle's actual absolute
                 * deadline, not this iteration's relative remainder: replay
                 * admission re-samples the clock after spacing/rewrite/event
                 * work and must never manufacture extra simulation slack.
                 * Declines (feature off, snapshots not ready, or insufficient
                 * time/resources) fall through to the ordinary sleep below;
                 * the sub-2ms spin tail never attempts a present. */
                if (!fiDeclined) {
                    if (mp6_fi_idle_present(g_tickNextDeadline, g_tickPeriodNs)) {
                        continue;
                    }
                    fiDeclined = 1; /* final for this window -- see above */
                }
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
        g_tickLastLateNs = late; /* per-tick, for the console's percentiles */
        if (late > g_tickLateMaxNs) g_tickLateMaxNs = late;
        if (late > INT64_MAX - g_tickLateSumNs) g_tickLateSumNs = INT64_MAX;
        else g_tickLateSumNs += late;
        g_tickLateSamples++;
    }
}

/* Present accounting (MP6_PRESENT_RATE_LOG=1, the exact MP6_TICK_RATE_LOG
 * shape below): every aurora_begin_frame()/aurora_end_frame() pair is
 * counted -- the real per-tick frame here in VIWaitForRetrace, and each
 * extra interpolated present frame_interp.c pushes through
 * mp6_present_counters_add(). With Unlocked FPS OFF this makes the no-op
 * contract measurable: begin-frames == tick-count exactly, end-frames ==
 * tick-count - 1 (the final opened frame is discarded by process exit --
 * it exists for every run and is not an extra present). With the feature
 * ON, the present rate visibly exceeds the tick rate. */
static long g_mp6BeginFrames = 0;
static long g_mp6EndFrames = 0;

void mp6_present_counters_add(long begins, long ends)
{
    g_mp6BeginFrames += begins;
    g_mp6EndFrames += ends;
    /* The dev console's present sampler rides the SAME funnel, so it sees every
     * present the user actually sees -- the real frame end (0,1), the real
     * frame begin (1,0) and every interpolated present frame_interp.c pushes
     * (1,1) -- with no second interception and no new call site to keep in
     * sync. It returns before reading a clock while the console is closed. */
    mp6_console_note_present(begins, ends);
}

/* Read-only accessor for the same two counters (the console's fps panel cross-
 * checks its measured rate against them; they are the Unlocked-FPS off-proof's
 * own quantities). */
void mp6_present_counters_get(long *begins, long *ends)
{
    if (begins != NULL) *begins = g_mp6BeginFrames;
    if (ends != NULL) *ends = g_mp6EndFrames;
}

/* The clock every stat-unit number is relative to. */
void mp6_tick_config_get(double *hz, long long *periodNs)
{
    if (hz != NULL) *hz = (g_tickHz > 0.0) ? g_tickHz : 0.0;
    if (periodNs != NULL) *periodNs = (long long)g_tickPeriodNs;
}

/* The stat-unit HUD's RenderRes row. The window size is the real backing
 * pixel size (the same SDL query mp6_widescreen_render_width() uses); the
 * render size is that times the session's SSAA factor, which is what aurora
 * actually allocates the content framebuffer at (aurora-patches/0018, applied
 * at aurora_initialize and restart-pending, so reading it here is exact rather
 * than a live guess). Both zeroed when the window is not readable yet -- the
 * HUD then omits the row rather than printing a made-up percentage. */
void mp6_render_res_get(int *winW, int *winH, int *renderW, int *renderH)
{
    /* Section 6's aspect window, declared there and used here -- the same
     * handle mp6_widescreen_render_width() queries, so the two can never
     * disagree about which window "the window" is. */
    extern SDL_Window *mp6_aspect_window(void);
    SDL_Window *win = mp6_aspect_window();
    int w = 0, h = 0;
    float ssaa;
    extern float mp6_launcher_cfg_ssaa(void);
    if (win == NULL || !SDL_GetWindowSizeInPixels(win, &w, &h) ||
        w <= 0 || h <= 0) {
        w = 0;
        h = 0;
    }
    ssaa = mp6_launcher_cfg_ssaa();
    if (!(ssaa > 0.0f)) ssaa = 1.0f;
    if (winW != NULL) *winW = w;
    if (winH != NULL) *winH = h;
    if (renderW != NULL) *renderW = (int)((float)w * ssaa + 0.5f);
    if (renderH != NULL) *renderH = (int)((float)h * ssaa + 0.5f);
}

/* =======================================================================
 * 2b. The host OUTPUT the window is on, and the present sync that paces it.
 *     (shim/include/mp6_display.h holds the seam's contract; this is its
 *     one implementation.)
 *
 * WHY THESE FOUR THINGS ARE ONE MODULE. They are all the same fact seen from
 * different sides: with present sync ON, the present cadence equals the
 * refresh rate of whichever output the window sits on. So "what does the FPS
 * badge mean", "which output should the window open on", "the window moved to
 * another output" and "present sync just changed" cannot be answered
 * independently -- each one changes the answer to the others, and each one
 * invalidates the same cached display query.
 *
 * WHY THE STATE LIVES IN THIS TU. Every static below is live host state: an
 * SDL display id, a desktop position, a monotonic cache stamp, the live
 * present-sync flag. This TU is already inside the savestate carve-out (the
 * `#include "mp6_host_section.h"` at preprocessor depth 0 near the top, plus
 * a HOST_STATE_SECTION_SOURCES entry in tools/build.py), so these statics are
 * excluded from capture and restore by construction. A new TU would have
 * needed both halves of that arrangement, and the build-time check exists
 * precisely because the include once sat inside an `#ifdef _WIN32` and
 * silently compiled UNCARVED on the only platform savestates run on.
 *
 * WHY NOTHING HERE PATCHES AURORA. `external_refs/repos/aurora` is a SINGLE
 * shared checkout for every worktree, and `setup/lib/step_aurora.py`'s
 * build_fingerprint() hashes the calling lane's own aurora-patches/* bytes
 * against a stamp stored INSIDE that shared checkout -- so adding a patch
 * here would fail the next link in every other lane. Hence: the present mode
 * is scraped from a log line aurora already prints, and the forced surface
 * reconfigure is reached through aurora_enable_vsync(), which already exists,
 * is already exported, and already pushes the RefreshSurface event that leads
 * to resize_swapchain_internal(..., force=true).
 * ======================================================================= */

/* aurora's exported present-sync entry point (external_refs/repos/aurora/
 * include/aurora/gfx.h:34, impl lib/webgpu/gpu.cpp:1331). Declared locally
 * with the same C-linkage-seam discipline platform/gx/console/console_stats.c
 * uses for aurora_get_stats/aurora_get_fps: this TU compiles against aurora's
 * include path but the gfx header's C half spells the parameter `bool`
 * without including <stdbool.h>, so it cannot be included from a C TU. `_Bool`
 * is the identical one-byte type the C++ side compiled, so the ABI matches
 * exactly -- an `int` here would be a 4-byte-for-1-byte mismatch that happens
 * to work on this ABI and would be a real bug on another. */
extern void aurora_enable_vsync(_Bool enabled);

/* Live present sync. aurora tracks NO queryable vsync state -- gpu.cpp:1331
 * writes g_graphicsConfig.surfaceConfiguration.presentMode and nothing else --
 * so this side must own it. Seeded from what aurora was actually initialized
 * with (config + MP6_VSYNC resolved by main_native.c), never guessed. */
static int s_dispVsyncOn = 1;
/* What the session actually BOOTED with. Not readable from the config: in
 * automation MP6_VSYNC overrides it, and main_native.c is the only place that
 * has resolved config-vs-env by the time aurora is initialized. Kept so the
 * `vsync` console command can say what changed since launch. */
static int s_dispVsyncBoot = 1;

/* The exact wgpu present mode aurora logged at startup, and whether the label
 * is still that exact value. After a runtime toggle only the CLASS is known:
 * best_present_mode() (gpu.cpp:272-295) picks FifoRelaxed-or-Fifo for on and
 * Mailbox-or-Immediate-or-Fifo for off, from surface capabilities this side
 * cannot see. */
static char s_dispPresentMode[24] = "";
static int  s_dispPresentModeExact = 0;

/* Cross-output follow state. s_dispLastDisplay is the SDL display id the
 * window was last SEEN on; it is mutated in exactly two places -- the launch
 * SEED and the one crossing action -- which is what makes the reconfigure
 * edge-triggered by construction rather than by a per-frame comparison.
 *
 * THE SEED IS NOT A FORMALITY. Measured: on this machine SDL emits no
 * SDL_EVENT_WINDOW_DISPLAY_CHANGED at window creation, so with the latch
 * starting empty the FIRST genuine crossing was consumed as the initial latch
 * and did nothing -- only the second one acted (build/verify/r34.out:12117
 * logged the drag BACK as "#1"). Seeding from SDL_GetDisplayForWindow at init
 * makes the first crossing a real edge. */
static SDL_DisplayID s_dispLastDisplay = 0;
static int  s_dispHaveLastDisplay = 0;
static long s_dispFollowCount = 0;

/* The second half of the crossing's present-mode FLIP, owed to the next event
 * drain. See mp6_display_follow_crossing() for why one Configure is not
 * enough and why the pair cannot be issued back to back. */
static int s_dispPendingRestore = 0;

/* The launch position mp6_display_resolve_launch_pos() decided, kept so it can
 * be RE-APPLIED after aurora has created the window.
 *
 * WHY IT HAS TO BE RE-APPLIED. aurora's create_window() treats any negative
 * coordinate as "unset" -- lib/window.cpp:314-318 maps `posX < 0 || posY < 0`
 * to SDL_WINDOWPOS_UNDEFINED -- and on a desktop whose highest-refresh output
 * is arranged to the LEFT of the primary, every position on that output has a
 * negative x. So the resolver's answer was silently discarded at exactly the
 * output it exists to reach: measured 100% failure on the 240 Hz panel
 * (build/verify/r2a.out, r2b.out) against a working positive-coordinate
 * control (r2c.out). The checkout is shared with every other lane and must
 * stay byte-unchanged, so the fix is port-side: let aurora place the window
 * wherever it likes and move it afterwards with SDL_SetWindowPosition, which
 * has no such sentinel. config.windowPosX/Y still carries the value too, so a
 * positive answer is honored at creation and the move below is then a no-op. */
static int s_dispLaunchX = 0, s_dispLaunchY = 0;
static int s_dispHaveLaunchPos = 0;

/* Last window position seen, for video.window_x/_y persistence. Updated from
 * SDL_EVENT_WINDOW_MOVED and flushed to the config exactly once, at clean
 * shutdown -- never a per-move file write. */
static int s_dispWinX = -1, s_dispWinY = -1;
static int s_dispHaveWinPos = 0;

/* Memoized answer to "is this the highest-refresh attached output", which is
 * the only part of the info query that walks every display. The badge asks
 * twice per second; the stamp keeps that from becoming a per-present walk,
 * and the two events that can invalidate it (a crossing, a vsync toggle)
 * clear it explicitly so a stale answer cannot outlive its cause. */
static uint64_t s_dispCacheStampNs = 0;
static int      s_dispCacheValid = 0;
static int      s_dispCacheIsHighest = 0;

static void mp6_display_cache_invalidate(void)
{
    s_dispCacheValid = 0;
    s_dispCacheStampNs = 0;
}

/* The exact rate in thousandths of a Hz. SDL3 gives a rational pair alongside
 * the float, and it is the pair that is authoritative: a 240 Hz panel reports
 * 240/1 exactly there while Win32_VideoController rounds the same panel to
 * 239, and a nominal "75 Hz" virtual display can genuinely be 74.973. Falls
 * back to the float only when the denominator is zero (SDL's own
 * "unspecified" encoding). */
static int mp6_display_milli_hz(const SDL_DisplayMode *mode)
{
    if (mode == NULL) return 0;
    if (mode->refresh_rate_denominator > 0 && mode->refresh_rate_numerator > 0) {
        double exact = (double)mode->refresh_rate_numerator /
                       (double)mode->refresh_rate_denominator;
        return (int)(exact * 1000.0 + 0.5);
    }
    if (mode->refresh_rate > 0.0f) {
        return (int)((double)mode->refresh_rate * 1000.0 + 0.5);
    }
    return 0;
}

int mp6_display_info_get(Mp6DisplayInfo *out)
{
    extern SDL_Window *mp6_aspect_window(void);
    SDL_Window *win;
    SDL_DisplayID id;
    const SDL_DisplayMode *mode;
    const char *name;
    Mp6DisplayInfo info;
    int milli;

    if (out == NULL) return 0;
    win = mp6_aspect_window();
    if (win == NULL) return 0;
    id = SDL_GetDisplayForWindow(win);
    if (id == 0) return 0;
    mode = SDL_GetCurrentDisplayMode(id);
    if (mode == NULL) return 0;
    milli = mp6_display_milli_hz(mode);
    if (milli <= 0) return 0;

    memset(&info, 0, sizeof(info));
    info.valid = 1;
    name = SDL_GetDisplayName(id);
    snprintf(info.name, sizeof(info.name), "%s", (name != NULL) ? name : "display");
    info.w = mode->w;
    info.h = mode->h;
    info.refreshMilliHz = milli;
    info.refreshHz = (milli + 500) / 1000;
    info.vsyncOn = s_dispVsyncOn ? 1 : 0;

    /* isHighestRefresh: one bounded walk of the attached outputs, memoized
     * for 500 ms. Deliberately a >-comparison against this display's own
     * exact rate, so an identical-rate second monitor does NOT flag the
     * window as being on a slower output. */
    {
        uint64_t now = mp6_host_monotonic_ns();
        if (!s_dispCacheValid || now < s_dispCacheStampNs ||
            now - s_dispCacheStampNs >= 500000000ull) {
            int count = 0;
            SDL_DisplayID *ids = SDL_GetDisplays(&count);
            int highest = 1;
            if (ids != NULL) {
                int i;
                for (i = 0; i < count; ++i) {
                    const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(ids[i]);
                    if (mp6_display_milli_hz(m) > milli) {
                        highest = 0;
                        break;
                    }
                }
                SDL_free(ids);
            }
            s_dispCacheIsHighest = highest;
            s_dispCacheValid = 1;
            s_dispCacheStampNs = now;
        }
        info.isHighestRefresh = s_dispCacheIsHighest;
    }

    /* The present-mode label. Exact while it is still aurora's own logged
     * value; the requested class once a runtime toggle has moved it. */
    if (s_dispPresentModeExact && s_dispPresentMode[0] != '\0') {
        snprintf(info.presentMode, sizeof(info.presentMode), "%s", s_dispPresentMode);
        info.presentModeExact = 1;
    } else {
        snprintf(info.presentMode, sizeof(info.presentMode), "%s",
                 s_dispVsyncOn ? "FIFO" : "IMMEDIATE");
        info.presentModeExact = 0;
    }

    *out = info;
    return 1;
}

int mp6_display_vsync_enabled(void)
{
    return s_dispVsyncOn ? 1 : 0;
}

long mp6_display_follow_count(void)
{
    return s_dispFollowCount;
}

void mp6_display_note_init_present_mode(const char *token)
{
    size_t i = 0;
    char buf[24];
    if (token == NULL) return;
    /* Bounded, alphanumeric-only copy, upper-cased. aurora formats the wgpu
     * enum name with {fmt} (gpu.cpp:1191), so the token is a bare identifier
     * ("Fifo", "FifoRelaxed", "Mailbox", "Immediate") -- anything else means
     * the line was not what this expects and the label stays a class. */
    while (token[i] != '\0' && i + 1u < sizeof(buf)) {
        char c = token[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9'))) {
            break;
        }
        buf[i] = (char)((c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c);
        ++i;
    }
    buf[i] = '\0';
    if (i == 0) return;
    snprintf(s_dispPresentMode, sizeof(s_dispPresentMode), "%s", buf);
    s_dispPresentModeExact = 1;
    mp6_display_cache_invalidate();
}

/* WHY A LIVE presentMode CHANGE CANNOT LAND MID-FRAME. aurora_enable_vsync()
 * assigns the mode and then push_custom_event(RefreshSurface) -- it does not
 * reconfigure anything itself. The reconfigure runs inside
 * window::process_event's RefreshSurface arm (aurora/lib/window.cpp:209-214)
 * when that event is DEQUEUED. This port has exactly two dequeue sites, and
 * both sit outside an open frame:
 *   - the game loop's mp6_dispatch_aurora_events(aurora_update()) below, which
 *     runs with g_frameOpen == false (the previous frame closed it; the next
 *     aurora_begin_frame has not run);
 *   - the pre-boot launcher menu's own walk (launcher_core.cpp), which runs
 *     before its aurora_begin_frame().
 * frame_interp.c's replay path only SDL_PumpEvents() and never dequeues, so an
 * interpolated present cannot trigger it either. The cost is one tick of
 * latency; the benefit is that gfx::gpu_synchronize()'s render-worker drain
 * can never intersect an open frame. */
void mp6_display_set_vsync(int on)
{
    const int want = on ? 1 : 0;
    if (want == s_dispVsyncOn) return; /* re-selecting the current value is free */
    s_dispVsyncOn = want;
    /* The label can no longer claim aurora's logged mode: on -> FifoRelaxed
     * or Fifo, off -> Mailbox or Immediate or Fifo, chosen from surface
     * capabilities this side cannot read (gpu.cpp:272-295). */
    s_dispPresentModeExact = 0;
    mp6_display_cache_invalidate();
    aurora_enable_vsync(want ? (_Bool)1 : (_Bool)0);
    printf("[MP6-DISPLAY] present sync %s -- requesting %s (surface reconfigures "
           "on the next event drain, between frames)\n",
           want ? "ON" : "OFF", want ? "FIFO" : "IMMEDIATE");
    fflush(stdout);
}

/* THE LAUNCH SEED -- the first of the latch's two writers. Priming it with the
 * output the window actually came up on is what makes the FIRST genuine
 * crossing an edge; see the latch's own comment for the measurement that
 * proved the unseeded latch swallowed it. */
static void mp6_display_seed_latch_to(SDL_DisplayID id)
{
    if (id == 0) return;
    s_dispLastDisplay = id;
    s_dispHaveLastDisplay = 1;
    mp6_display_cache_invalidate();
}

/* THE SWAPCHAIN-FOLLOW ACTION -- the one place a crossing is acted on, and the
 * one place besides the launch seed that writes the latch.
 *
 * WHY A CROSSING NEEDS ANY ACTION. A monitor drag changes no pixel dimension,
 * so aurora's resize_swapchain_internal() takes its
 * `if (!force && !sizeChanged) return;` early-out (gpu.cpp:1269-1274) and the
 * DXGI surface keeps its ORIGINAL output association: presents stay pinned at
 * the old display's refresh indefinitely.
 *
 * WHY ONE FORCED Configure IS NOT ENOUGH, MEASURED. The first attempt called
 * aurora_enable_vsync() with the CURRENT state, reasoning that the only effect
 * would be refresh_surface(false) -> resize_swapchain_internal(force=true) ->
 * g_surface.Configure(). It reached that path and did nothing: the crossing
 * logged, aurora even logged its own "Display scale changed to 1.25", and
 * [MP6-PRESENTRATE] stayed at 74.8-75.2 presents/s across four consecutive 5 s
 * windows with the window verified on the 240 Hz output (build/verify/r34.out:
 * 13113 + r34.err). Dawn's D3D swapchain reuses the existing DXGI swapchain
 * across a Configure whose parameters are unchanged, and the parameters
 * include the present mode; reuse keeps the original output association.
 *
 * WHAT DOES WORK, ALSO MEASURED. Toggling the present mode away and back --
 * `vsync 0` then `vsync 1` by hand at the same window position -- took the same
 * process to 238.4-240.2 presents/s (r34.err). A different presentMode is a
 * different swapchain configuration, so Dawn must create a real one, and the
 * new one is created against the output the HWND is on NOW.
 *
 * WHY THE PAIR CANNOT BE ISSUED BACK TO BACK. aurora_enable_vsync() assigns
 * g_graphicsConfig.surfaceConfiguration.presentMode and pushes a RefreshSurface
 * custom event; the Configure happens when that event is DEQUEUED (window.cpp:
 * 209-214). Two calls in a row would leave presentMode back at its original
 * value before EITHER event was drained, so both Configures would see the
 * unchanged configuration -- precisely the case that provably does nothing. The
 * flip is therefore split across drains: this function requests !current, and
 * mp6_display_pump_pending_reconfigure() restores current at the next drain
 * boundary, one drain later. Cost: two forced reconfigures on one edge, and one
 * or two presented frames at the other sync mode.
 *
 * EDGE-TRIGGERED BY CONSTRUCTION: the only callers are the DISPLAY_CHANGED
 * observer's changed branch and the launch placement, both of which compare
 * against the latch first. A forced reconfigure runs gfx::gpu_synchronize() --
 * a full render-worker drain plus framebuffer recreation -- so doing it per
 * frame would be catastrophic; doing it twice per crossing costs two drains. */
static void mp6_display_follow_crossing(SDL_DisplayID was, SDL_DisplayID now,
                                        const char *why)
{
    /* Kill switch as an ENV LEVER, not a config key: this is a bug fix, not a
     * preference, so it must not grow a settings row that invites leaving it
     * off. It exists only so a bisect can attribute a visual regression to the
     * forced reconfigure. */
    const char *followEnv = getenv("MP6_SWAPCHAIN_FOLLOW");
    s_dispLastDisplay = now;
    s_dispHaveLastDisplay = 1;
    ++s_dispFollowCount;
    mp6_display_cache_invalidate();
    /* The label can no longer claim aurora's logged boot mode: the surface is
     * about to be re-created against a different output, and best_present_mode()
     * re-picks within each pair from THAT surface's capabilities
     * (gpu.cpp:272-295), which this side cannot read. */
    s_dispPresentModeExact = 0;
    if (followEnv != NULL && followEnv[0] == '0') {
        printf("[MP6-DISPLAY] swapchain follow: display %u -> %u (%s), "
               "SUPPRESSED (MP6_SWAPCHAIN_FOLLOW=0)\n",
               (unsigned)was, (unsigned)now, why);
        fflush(stdout);
        return;
    }
    printf("[MP6-DISPLAY] swapchain follow: display %u -> %u (%s), forcing a real "
           "surface reconfigure via a present-mode flip (#%ld)\n",
           (unsigned)was, (unsigned)now, why, s_dispFollowCount);
    fflush(stdout);
    aurora_enable_vsync(s_dispVsyncOn ? (_Bool)0 : (_Bool)1);
    s_dispPendingRestore = 1;
}

/* The flip's second half, owed to the NEXT event drain. Called once per drain
 * from both SDL event walks, immediately after aurora_update() has returned --
 * i.e. after the drain in which the first half's RefreshSurface was processed,
 * so the restore is a genuinely different configuration and forces a second
 * real swapchain creation.
 *
 * NOT a per-frame reconfigure: the flag is set in exactly one place (the
 * crossing action) and cleared here, so on every frame of a session with no
 * crossing this function is one integer compare and a return. */
void mp6_display_pump_pending_reconfigure(void)
{
    if (!s_dispPendingRestore) return;
    s_dispPendingRestore = 0;
    aurora_enable_vsync(s_dispVsyncOn ? (_Bool)1 : (_Bool)0);
    printf("[MP6-DISPLAY] swapchain follow: restoring present sync %s -- second "
           "Configure of flip #%ld (the one that re-associates the output)\n",
           s_dispVsyncOn ? "ON" : "OFF", s_dispFollowCount);
    fflush(stdout);
}

/* Observes the two window events this module cares about and nothing else.
 * SDL-typed, so it is declared at its call sites rather than in
 * shim/include/mp6_display.h (which console_stats.c includes in the headless
 * build too) -- the same local-extern idiom mp6_launcher_forward_sdl_event
 * already uses one function below. */
void mp6_display_note_sdl_event(const SDL_Event *ev)
{
    if (ev == NULL) return;

    if (ev->type == SDL_EVENT_WINDOW_MOVED) {
        s_dispWinX = ev->window.data1;
        s_dispWinY = ev->window.data2;
        s_dispHaveWinPos = 1;
        return;
    }

    if (ev->type != SDL_EVENT_WINDOW_DISPLAY_CHANGED) return;

    {
        const SDL_DisplayID now = (SDL_DisplayID)ev->window.data1;
        if (now == 0) return;
        /* The latch is seeded at init from the window's own launch output, so
         * an unseeded latch here means SDL answered nothing at init -- treat
         * this event as the seed rather than as an edge, exactly as before. */
        if (!s_dispHaveLastDisplay) {
            mp6_display_seed_latch_to(now);
            return;
        }
        if (now == s_dispLastDisplay) return;
        mp6_display_follow_crossing(s_dispLastDisplay, now, "window moved");
    }
}

/* The last window position seen this session, for the config flush at clean
 * shutdown. Returns 0 when no move was ever observed, so a window that never
 * moved does not overwrite a saved position with its launch value. */
int mp6_display_window_pos_get(int *outX, int *outY)
{
    if (!s_dispHaveWinPos) return 0;
    if (outX != NULL) *outX = s_dispWinX;
    if (outY != NULL) *outY = s_dispWinY;
    return 1;
}

/* --- the `vsync` console command ---------------------------------------
 * Registered from THIS TU, not from console_core.c's console_register_builtins:
 * console_core.c is in PLATFORM_SOURCES_COMMON and links in the --headless
 * build, where aurora_enable_vsync does not exist at all. Registering it here
 * keeps the headless link free of any aurora reference by construction rather
 * than by an #ifdef in a both-mode file.
 *
 * A command, not a cvar: console_core.c's kCvars model is for integers a
 * consumer POLLS, whereas this is a one-shot apply with a side effect. Keeping
 * the two models distinct is why `toggles` does not and must not list it. */
static void mp6_display_cmd_vsync(int argc, const char *const *argv)
{
    Mp6DisplayInfo info;
    if (argc >= 2 && argv[1] != NULL &&
        (argv[1][0] == '0' || argv[1][0] == '1') && argv[1][1] == '\0') {
        mp6_display_set_vsync(argv[1][0] == '1');
    } else if (argc >= 2) {
        mp6_console_log("vsync: expected 0 or 1 (no argument reports the live state)");
        return;
    }
    mp6_console_log("present sync: %s  (booted %s)", s_dispVsyncOn ? "ON" : "OFF",
                    s_dispVsyncBoot ? "ON" : "OFF");
    mp6_console_log("boot present mode: %s%s",
                    (s_dispPresentMode[0] != '\0') ? s_dispPresentMode : "(not logged)",
                    s_dispPresentModeExact ? "  (exact, from aurora's own log)"
                                           : "  (superseded by a runtime toggle)");
    if (mp6_display_info_get(&info)) {
        mp6_console_log("output: %s %dx%d @ %.3f Hz%s", info.name, info.w, info.h,
                        (double)info.refreshMilliHz / 1000.0,
                        info.isHighestRefresh ? "" : "  (NOT the highest-refresh output)");
        mp6_console_log("present mode now: %s%s", info.presentMode,
                        info.presentModeExact ? "" : "  (requested class)");
    }
    mp6_console_log("swapchain follows: %ld cross-output reconfigure(s) this session",
                    s_dispFollowCount);
}

void mp6_display_init(int vsyncOn)
{
    extern SDL_Window *mp6_aspect_window(void);
    SDL_Window *win;
    s_dispVsyncOn = vsyncOn ? 1 : 0;
    s_dispVsyncBoot = s_dispVsyncOn;
    mp6_display_cache_invalidate();
    /* SEED THE CROSS-OUTPUT LATCH from the output the window actually came up
     * on. Requires a real SDL_Window, which is why main_native.c calls this
     * AFTER mp6_bridge_window_policy_init() (the earliest point the handle is
     * stashed) rather than immediately after aurora_initialize(). A NULL handle
     * degrades to the old behaviour: the first DISPLAY_CHANGED event seeds
     * instead, and is not treated as an edge. */
    win = mp6_aspect_window();
    if (win != NULL) mp6_display_seed_latch_to(SDL_GetDisplayForWindow(win));
    mp6_console_register("vsync",
                         "vsync [0|1] -- present sync; no argument reports the live state",
                         mp6_display_cmd_vsync);
}

/* R2's SECOND HALF: put the window where the resolver decided, now that a real
 * SDL_Window exists. See s_dispLaunchX's comment for why aurora's own config
 * channel cannot carry a negative coordinate, and therefore cannot reach the
 * output this whole feature exists for.
 *
 * A no-op unless it has something to do: no resolved position (automation, or
 * `video.display: "primary"`), no window, or the window is already exactly
 * there because the coordinates were positive and aurora honored them.
 *
 * THE MOVE IS A CROSSING and is handled as one. aurora created and SHOWED the
 * window on the OS default output and configured the surface against it, so
 * moving to a different output leaves the swapchain pinned to the old one --
 * the same defect a user's drag hits. The latch was seeded by
 * mp6_display_init() one call earlier, so the comparison here is against the
 * genuine launch output and the flip runs exactly once. */
void mp6_display_apply_launch_pos(void)
{
    extern SDL_Window *mp6_aspect_window(void);
    SDL_Window *win;
    SDL_DisplayID before, after;
    int x, y, curX = 0, curY = 0;

    if (!s_dispHaveLaunchPos) return;
    win = mp6_aspect_window();
    if (win == NULL) return;
    x = s_dispLaunchX;
    y = s_dispLaunchY;

    /* CAPTION SAFETY, now computable. The resolver centres a CLIENT rectangle
     * inside the target's usable bounds without knowing the frame's borders,
     * because no window exists yet to measure them. Here one does, so nudge the
     * client origin down/right by exactly the caption and left border when the
     * frame would otherwise start above or left of the usable area -- the same
     * arithmetic (and the same failure it prevents: a window that looks
     * borderless because its title bar is parked off-desktop) as the rescue in
     * this file's window-policy section. */
    {
        SDL_Point centre;
        SDL_DisplayID target;
        SDL_Rect usable;
        int w = 0, h = 0, bt = 0, bl = 0, bb = 0, br = 0;
        SDL_GetWindowSize(win, &w, &h);
        centre.x = x + w / 2;
        centre.y = y + h / 2;
        target = SDL_GetDisplayForPoint(&centre);
        if (target != 0 && SDL_GetDisplayUsableBounds(target, &usable)) {
            if (!SDL_GetWindowBordersSize(win, &bt, &bl, &bb, &br)) { bt = 0; bl = 0; }
            if (y - bt < usable.y) y = usable.y + bt;
            if (x - bl < usable.x) x = usable.x + bl;
        }
    }

    if (SDL_GetWindowPosition(win, &curX, &curY) && curX == x && curY == y) {
        printf("[MP6-DISPLAY] window already at (%d,%d) -- aurora honored the "
               "resolved position, no move needed\n", x, y);
        fflush(stdout);
        return;
    }

    before = SDL_GetDisplayForWindow(win);
    if (!SDL_SetWindowPosition(win, x, y)) {
        printf("[MP6-DISPLAY] SDL_SetWindowPosition(%d,%d) failed (%s) -- the "
               "window stays where the OS put it\n", x, y, SDL_GetError());
        fflush(stdout);
        return;
    }
    /* Block until the move is real: the answer to "which output is it on now"
     * is the whole point of the next few lines, and SDL_SetWindowPosition is
     * documented as possibly asynchronous (SDL_video.h:1762). */
    SDL_SyncWindow(win);
    SDL_GetWindowPosition(win, &curX, &curY);
    after = SDL_GetDisplayForWindow(win);
    printf("[MP6-DISPLAY] moved the window to (%d,%d) after aurora created it "
           "(landed at (%d,%d)) -- aurora's create path discards negative "
           "positions\n", x, y, curX, curY);
    fflush(stdout);
    if (after != 0 && after != before) {
        mp6_display_follow_crossing(before, after, "launch placement");
    }
}

/* --- launch output selection (video.display / video.window_x / _y) -----
 *
 * WHY BEFORE aurora_initialize. aurora's own aurora_initialize() calls
 * window::initialize(), then create_window() (possibly twice, via the backend
 * fallback loop -- so the position must live in its config, not be applied
 * once), and then window::show_window(), all inside itself. By the time
 * main_native.c regains control the window is already visible on whichever
 * output the OS default chose. There is no later moment.
 *
 * WHY IT QUITS THE VIDEO SUBSYSTEM AGAIN. aurora's window::initialize() sets
 * SDL_HINT_ORIENTATIONS *before* its own SDL_InitSubSystem(SDL_INIT_VIDEO)
 * (lib/window.cpp:380-402). Leaving the subsystem initialized here would turn
 * that into a post-init hint set. Quitting drops the refcount to zero so
 * aurora's init is a genuinely fresh one and the hint keeps its ordering. The
 * consequence shapes the return value: SDL_DisplayIDs are not valid across
 * the quit, which is exactly why this returns an ABSOLUTE DESKTOP POSITION
 * and not SDL_WINDOWPOS_CENTERED_DISPLAY(id). */
#ifndef __ANDROID__
static int mp6_display_pick_by_name(const SDL_DisplayID *ids, int count,
                                    const char *want)
{
    int i;
    if (want == NULL || want[0] == '\0') return -1;
    for (i = 0; i < count; ++i) {
        const char *name = SDL_GetDisplayName(ids[i]);
        if (name == NULL) continue;
        if (SDL_strcasecmp(name, want) == 0) return i;
    }
    /* Substring, case-insensitive, as a second pass -- a user typing
     * "ULTRAGEAR" for "LG ULTRAGEAR 27GR93U" should work.
     *
     * This pass is also what makes a TRUNCATED stored name still resolve:
     * video.display is char[64], so a longer SDL display name is saved as its
     * own prefix, and a prefix matches here at the start of the haystack. Not
     * a happy accident -- it is why the fallback is a substring scan rather
     * than a second exact compare. */
    for (i = 0; i < count; ++i) {
        const char *name = SDL_GetDisplayName(ids[i]);
        const char *hay;
        size_t nWant;
        if (name == NULL) continue;
        nWant = strlen(want);
        for (hay = name; *hay != '\0'; ++hay) {
            if (SDL_strncasecmp(hay, want, nWant) == 0) return i;
        }
    }
    return -1;
}
#endif /* !__ANDROID__ */

int mp6_display_resolve_launch_pos(int launcherMode, unsigned int reqW,
                                   unsigned int reqH, int *outX, int *outY)
{
#ifdef __ANDROID__
    (void)launcherMode; (void)reqW; (void)reqH; (void)outX; (void)outY;
    return 0; /* one display, and no desktop coordinates to speak of */
#else
    extern const char *mp6_launcher_cfg_display(void);
    extern int mp6_launcher_cfg_window_pos(int *x, int *y);
    const char *envDisplay = getenv("MP6_WINDOW_DISPLAY");
    const char *cfgDisplay = "auto";
    int savedX = -1, savedY = -1, haveSaved = 0;
    int count = 0, chosen = -1, ok = 0;
    SDL_DisplayID *ids = NULL;
    const char *reason = "highest refresh";
    int effW, effH;

    if (outX == NULL || outY == NULL) return 0;
    /* Automation placement must stay bit-for-bit what it was: no config read,
     * no enumeration, no printed line -- unless the run explicitly opts in
     * with the env lever, the same shape MP6_WINDOW_SIZE already has. */
    if (!launcherMode && (envDisplay == NULL || envDisplay[0] == '\0')) return 0;

    if (launcherMode) {
        cfgDisplay = mp6_launcher_cfg_display();
        haveSaved = mp6_launcher_cfg_window_pos(&savedX, &savedY);
    }
    if (envDisplay == NULL || envDisplay[0] == '\0') {
        /* "primary" is the explicit opt-out: today's OS-default placement,
         * byte-for-byte, and the control arm every measurement compares
         * against. */
        if (cfgDisplay != NULL && SDL_strcasecmp(cfgDisplay, "primary") == 0) return 0;
    }

    /* aurora's own create_window() defaulting, mirrored (lib/window.cpp:
     * 301-312): a zero request becomes 1280x960, and anything smaller than
     * 640x480 is clamped up. Centering against the wrong rectangle centers in
     * the wrong place. */
    effW = (reqW > 0u) ? (int)reqW : 1280;
    effH = (reqH > 0u) ? (int)reqH : 960;
    if (effW < 640) effW = 640;
    if (effH < 480) effH = 480;

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return 0;
    ids = SDL_GetDisplays(&count);
    if (ids == NULL || count <= 0) {
        if (ids != NULL) SDL_free(ids);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return 0;
    }

    /* RESOLUTION ORDER, one place, no hidden precedence:
     *   MP6_WINDOW_DISPLAY  >  saved window_x/y  >  video.display  >  no change */
    if (envDisplay != NULL && envDisplay[0] != '\0') {
        /* A 1-based index or a name/substring, mirroring MP6_WINDOW_SIZE's
         * lever shape. This is the ONLY way an automation run opts in. */
        char *end = NULL;
        long idx = strtol(envDisplay, &end, 10);
        if (end != envDisplay && (end == NULL || *end == '\0') &&
            idx >= 1 && idx <= (long)count) {
            chosen = (int)idx - 1;
        } else {
            chosen = mp6_display_pick_by_name(ids, count, envDisplay);
        }
        if (chosen >= 0) reason = "MP6_WINDOW_DISPLAY";
    }

    if (chosen < 0 && haveSaved && savedX > -32768 && savedY > -32768) {
        /* STALE-POSITION REFUSAL. A saved position is only usable while the
         * desktop it described still exists: unplugging a monitor would
         * otherwise leave the window permanently invisible. Require the
         * window's CENTRE to land inside some output's usable bounds. */
        const int cx = savedX + effW / 2, cy = savedY + effH / 2;
        int i, inside = 0;
        for (i = 0; i < count; ++i) {
            SDL_Rect usable;
            if (!SDL_GetDisplayUsableBounds(ids[i], &usable)) continue;
            if (cx >= usable.x && cx < usable.x + usable.w &&
                cy >= usable.y && cy < usable.y + usable.h) {
                inside = 1;
                break;
            }
        }
        if (inside) {
            *outX = savedX;
            *outY = savedY;
            /* Stash for the post-creation re-apply -- see
             * mp6_display_apply_launch_pos(). A saved position on a
             * left-of-primary output is negative too, so this path needs the
             * bypass just as much as the auto one. */
            s_dispLaunchX = savedX;
            s_dispLaunchY = savedY;
            s_dispHaveLaunchPos = 1;
            printf("[MP6-DISPLAY] chose saved position (%d,%d) for a %dx%d window "
                   "-- saved position\n", savedX, savedY, effW, effH);
            fflush(stdout);
            SDL_free(ids);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return 1;
        }
        printf("[MP6-DISPLAY] saved position (%d,%d) is off-desktop -- falling back "
               "to auto\n", savedX, savedY);
        fflush(stdout);
    }

    if (chosen < 0 && launcherMode && cfgDisplay != NULL &&
        SDL_strcasecmp(cfgDisplay, "auto") != 0) {
        chosen = mp6_display_pick_by_name(ids, count, cfgDisplay);
        if (chosen >= 0) {
            reason = "video.display";
        } else {
            printf("[MP6-DISPLAY] video.display=\"%s\" matches no attached output "
                   "-- falling back to auto\n", cfgDisplay);
            fflush(stdout);
        }
    }

    if (chosen < 0) {
        /* "auto": the attached output with the highest refresh. Ties keep the
         * first enumerated one, which is SDL's primary-first order. */
        int i, bestMilli = -1;
        for (i = 0; i < count; ++i) {
            const int milli = mp6_display_milli_hz(SDL_GetCurrentDisplayMode(ids[i]));
            if (milli > bestMilli) {
                bestMilli = milli;
                chosen = i;
            }
        }
        reason = "highest refresh";
    }

    if (chosen >= 0) {
        SDL_Rect usable;
        if (SDL_GetDisplayUsableBounds(ids[chosen], &usable)) {
            const char *name = SDL_GetDisplayName(ids[chosen]);
            const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(ids[chosen]);
            /* USABLE bounds, not full bounds: they exclude the taskbar, so a
             * centered window's caption cannot start underneath it. The two
             * clamps keep a window LARGER than the output from being pushed
             * above/left of the usable origin, which is the exact shape of the
             * "looks borderless, frame parked off-desktop" failure. */
            int x = usable.x + (usable.w - effW) / 2;
            int y = usable.y + (usable.h - effH) / 2;
            if (x < usable.x) x = usable.x;
            if (y < usable.y) y = usable.y;
            *outX = x;
            *outY = y;
            /* Stash for the post-creation re-apply. THIS is the path the whole
             * feature exists for and the one aurora's negative-position
             * sentinel silently discarded. */
            s_dispLaunchX = x;
            s_dispLaunchY = y;
            s_dispHaveLaunchPos = 1;
            printf("[MP6-DISPLAY] chose \"%s\" %dx%d@%.3f Hz at (%d,%d) for a %dx%d "
                   "window -- %s\n",
                   (name != NULL) ? name : "display",
                   (mode != NULL) ? mode->w : 0, (mode != NULL) ? mode->h : 0,
                   (double)mp6_display_milli_hz(mode) / 1000.0, x, y, effW, effH,
                   reason);
            fflush(stdout);
            ok = 1;
        }
    }

    SDL_free(ids);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return ok;
#endif /* __ANDROID__ */
}

static int mp6_present_rate_log_enabled(void)
{
    static int s_enabled = -1; /* -1 = env not checked yet */
    if (s_enabled < 0) {
        const char *env = getenv("MP6_PRESENT_RATE_LOG");
        s_enabled = (env && *env && *env != '0') ? 1 : 0;
    }
    /* env latches the initial value; the dev console may override it live. */
    return mp6_console_cvar_get(MP6_CVAR_PRESENT_RATE_LOG, s_enabled);
}

static void mp6_present_rate_log(void)
{
    static int64_t s_windowStartNs = 0;
    static long s_windowStartEnds = 0;
    int64_t now;
    double elapsed;
    if (!mp6_present_rate_log_enabled()) {
        return;
    }
    now = (int64_t)mp6_host_monotonic_ns();
    if (s_windowStartNs == 0) {
        s_windowStartNs = now;
        s_windowStartEnds = g_mp6EndFrames;
        return;
    }
    elapsed = (double)(now - s_windowStartNs) / (double)MP6_TICK_NS_PER_SEC;
    if (elapsed >= 5.0) {
        double rate = (double)(g_mp6EndFrames - s_windowStartEnds) / elapsed;
        fprintf(stderr, "[MP6-PRESENTRATE] window=%.2fs presents=%ld rate=%.3f presents/s "
                        "(cum: begin-frames=%ld end-frames=%ld ticks=%ld)\n",
                elapsed, g_mp6EndFrames - s_windowStartEnds, rate,
                g_mp6BeginFrames, g_mp6EndFrames, mp6_tick_count);
        fflush(stderr);
        s_windowStartNs = now;
        s_windowStartEnds = g_mp6EndFrames;
    }
}

/* The one-shot totals line at clean shutdown -- the quantitative anchor for
 * the Unlocked FPS off-proof (begin-frames == ticks exactly, end-frames ==
 * ticks-1 structurally) and on-proof (both counters far above ticks). */
static void mp6_present_rate_log_final(void)
{
    if (!mp6_present_rate_log_enabled()) {
        return;
    }
    fprintf(stderr, "[MP6-PRESENTRATE] final: begin-frames=%ld end-frames=%ld ticks=%ld\n",
            g_mp6BeginFrames, g_mp6EndFrames, mp6_tick_count);
    fflush(stderr);
}

/* MP6_TICK_RATE_LOG=1: once per ~5s, one stderr line with the measured
 * tick rate over the elapsed window (Delta-tick / Delta-wall-QPC -- an
 * actual measurement, not the configured target) plus the scheduler
 * lateness stats gathered above. Works with the throttle DISABLED too
 * (MP6_TICK_HZ=0 prints the free-run rate; lateness stats just stay 0/0)
 * -- an A/B instrument for pacing investigations. */
static int mp6_tick_rate_log_enabled(void)
{
    static int s_enabled = -1;   /* -1 = env not checked yet */
    if (s_enabled < 0) {
        const char *env = getenv("MP6_TICK_RATE_LOG");
        s_enabled = (env && *env && *env != '0') ? 1 : 0;
    }
    /* env latches the initial value; the dev console may override it live. */
    return mp6_console_cvar_get(MP6_CVAR_TICK_RATE_LOG, s_enabled);
}

/* Phase stamp: the monotonic clock while the phase accounting is armed, 0
 * otherwise. A 0 sample makes every phase delta below fold to zero, so the
 * accounting is inert -- not merely cheap -- with both consumers off.
 *
 * The predicate is widened, NOT duplicated: the dev console's `stat unit` page
 * wants exactly these buckets, so it arms the same clock rather than adding a
 * second one. Off, this remains the deliberately-skipped QPC read the block
 * above documents. */
static int64_t mp6_tick_phase_now(void)
{
    return (mp6_tick_rate_log_enabled() || mp6_console_stats_armed())
               ? (int64_t)mp6_host_monotonic_ns() : 0;
}

static void mp6_tick_phase_add(int64_t *bucket, int64_t from, int64_t to)
{
    if (from > 0 && to > from) *bucket += to - from;
}

static void mp6_tick_rate_log(void)
{
    static int64_t s_windowStartNs = 0;
    static long    s_windowStartTick = 0;
    int64_t now;
    double elapsed;
    if (!mp6_tick_rate_log_enabled()) {
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
        double slackAvgMs = g_tickSlackSamples > 0
            ? ((double)g_tickSlackSumNs / (double)g_tickSlackSamples) / 1000000.0
            : 0.0;
        double phN = (g_phSamples > 0) ? (double)g_phSamples : 1.0;
        fprintf(stderr, "[MP6-TICKRATE] window=%.2fs ticks=%ld rate=%.3f ticks/s "
                        "late(max=%.3fms avg=%.3fms n=%ld) "
                        "idle-slack(min=%.3fms avg=%.3fms max=%.3fms starved=%ld/%ld) "
                        "phase-avg-ms(game=%.3f endframe=%.3f seal=%.3f vipost=%.3f n=%ld) "
                        "tick=%ld\n",
                elapsed, (long)(mp6_tick_count - s_windowStartTick), rate,
                lateMaxMs, lateAvgMs, g_tickLateSamples,
                (double)g_tickSlackMinNs / 1000000.0, slackAvgMs,
                (double)g_tickSlackMaxNs / 1000000.0,
                g_tickSlackStarved, g_tickSlackSamples,
                (double)g_phGameNs / phN / 1e6, (double)g_phEndFrameNs / phN / 1e6,
                (double)g_phSealNs / phN / 1e6, (double)g_phViPostNs / phN / 1e6,
                g_phSamples, mp6_tick_count);
        fflush(stderr);
        s_windowStartNs = now;
        s_windowStartTick = mp6_tick_count;
        g_tickLateMaxNs = 0;
        g_tickLateSumNs = 0;
        g_tickLateSamples = 0;
        g_tickSlackMaxNs = 0;
        g_tickSlackMinNs = 0;
        g_tickSlackSumNs = 0;
        g_tickSlackSamples = 0;
        g_tickSlackStarved = 0;
        g_phGameNs = g_phEndFrameNs = g_phSealNs = g_phViPostNs = 0;
        g_phOverlayNs = 0;
        g_phSamples = 0;
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
    /* This stub's own quad/viewport were hardcoded to native 640, so a
     * wide (widescreen-on) window showed an uncovered strip past pixel 640
     * on any still-undecompiled-minigame black-screen stub.
     * mp6_widescreen_render_width() returns exactly 640 (byte-identical)
     * when disabled. */
    int w = mp6_widescreen_render_width();

    MTXOrtho(proj, 0, 480, 0, w, 0, 10);
    GXSetProjection(proj, GX_ORTHOGRAPHIC);
    MTXIdentity(modelview);
    /* Route through the same FI identity seam as decomp calls. This host-only
     * black-screen draw records an explicit invalid key, keeping the retained
     * stream's matrix/key cardinality aligned without making it interpolable. */
    mp6_GXLoadPosMtxImm(modelview, GX_PNMTX0);
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

static void mp6_dispatch_aurora_events(const AuroraEvent *event)
{
    /* The cross-output flip's second half (section 2b). Runs here because this
     * is a DRAIN BOUNDARY: aurora_update() has already processed the
     * RefreshSurface the first half pushed, so the restore issued now is a
     * genuinely different surface configuration rather than a duplicate of one
     * Dawn would reuse. One integer compare on every other frame. */
    mp6_display_pump_pending_reconfigure();
    while (event != NULL && event->type != AURORA_NONE) {
        if (event->type == AURORA_EXIT) {
            mp6_clean_shutdown_exit("window closed");
        }
        if (event->type == AURORA_SDL_EVENT) {
            mp6_latch_key_down_event(&event->sdl);
            mp6_latch_savestate_key_event(&event->sdl);
            mp6_latch_menu_key_event(&event->sdl); /* F10 in-game menu toggle */
            mp6_latch_console_key_event(&event->sdl); /* ` / F9 dev console toggle */
            { /* in-game UI + freecam event forwards -- both inert unless the
               * launcher/freecam actually armed them (launcher mode only). */
                extern void mp6_launcher_forward_sdl_event(const SDL_Event *ev);
                extern void mp6_freecam_input_event(const SDL_Event *ev);
                mp6_launcher_forward_sdl_event(&event->sdl);
                mp6_freecam_input_event(&event->sdl);
            }
            /* Cross-output swapchain follow + window-position cache (section
             * 2b). Inert until the window actually crosses to a different
             * display or is moved, so an automation run prints nothing and
             * reconfigures nothing. Declared locally rather than in
             * shim/include/mp6_display.h because it is SDL-typed and that
             * header is included by a TU that also links --headless. */
            {
                extern void mp6_display_note_sdl_event(const SDL_Event *ev);
                mp6_display_note_sdl_event(&event->sdl);
            }
#ifdef __ANDROID__
            mp6_touch_pad_event(&event->sdl); /* finger tracking (touch_pad.cpp) */
#endif
        }
        ++event;
    }
}

#ifdef __ANDROID__
typedef struct {
    const AuroraEvent *events;
} Mp6AndroidFrameGate;

static void mp6_frame_gate_pump_events(void *user)
{
    Mp6AndroidFrameGate *gate = (Mp6AndroidFrameGate *)user;
    const AuroraEvent *event;
    gate->events = aurora_update();
    /* Exit is a host lifecycle event and must remain responsive even while no
     * surface exists.  Game-facing key/touch/freecam delivery is deferred to
     * the successful try-begin below. */
    for (event = gate->events; event != NULL && event->type != AURORA_NONE; ++event) {
        if (event->type == AURORA_EXIT) mp6_clean_shutdown_exit("window closed");
    }
}

static int mp6_frame_gate_try_begin(void *user)
{
    Mp6AndroidFrameGate *gate = (Mp6AndroidFrameGate *)user;
    if (!aurora_begin_frame()) {
        /* Events pumped while there is no presentable frame are deliberately
         * not delivered to game/UI callbacks.  Also forget pre-suspend finger
         * state and one-tick latches: their UP/CANCELED events may be among the
         * discarded batches, and retaining them would create a stuck input on
         * the first resumed tick. */
        mp6_aurora_input_reset_transients();
        return 0;
    }
    mp6_dispatch_aurora_events(gate->events);
    return 1;
}

static void mp6_frame_gate_idle(void *user)
{
    (void)user;
    /* Keep a failed surface-refresh/admission loop from busy-spinning while
     * still pumping AURORA_EXIT with millisecond-scale responsiveness. */
    mp6_host_sleep_ns(1000000ull);
}
#endif

void VIWaitForRetrace(void)
{
    int frameBegan = 0;
    /* Tick-phase attribution (see the g_ph* block above). tEntry closes the
     * game's own slice, which began when the previous call returned. */
    int64_t tEntry = mp6_tick_phase_now(), tEndFrame = 0, tSeal = 0, tThrottleOut = 0;
    int64_t tOverlayIn = 0, tOverlayOut = 0; /* the overlay's own excluded bucket */
    /* g_phLastReturnNs is overwritten at the bottom of this function, so this
     * tick's game slice has to be taken while it still names the PREVIOUS
     * return -- the same instant mp6_tick_phase_add uses two lines below. */
    int64_t tPrevReturn = g_phLastReturnNs;
#ifdef __ANDROID__
    Mp6AndroidFrameGate androidGate = { NULL };
#endif
    mp6_tick_phase_add(&g_phGameNs, g_phLastReturnNs, tEntry);
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
        /* Bracketed into g_phOverlayNs (see its declaration): the dev console
         * composites inside this call, and an instrument that reported its own
         * cost as renderer time would be lying about the thing it exists to
         * measure. g_phEndFrameNs still covers the WHOLE block, so
         * [MP6-TICKRATE] is unchanged; the console subtracts. */
        tOverlayIn = mp6_tick_phase_now();
        { extern void mp6_launcher_frame_overlay(void); mp6_launcher_frame_overlay(); }
        tOverlayOut = mp6_tick_phase_now();
        mp6_gx_close_stale_primitive("closing it before aurora_end_frame()");
        aurora_end_frame();
        tEndFrame = mp6_tick_phase_now();
        mp6_present_counters_add(0, 1); /* MP6_PRESENT_RATE_LOG accounting */
        mp6_fi_note_frame_end(); /* Unlocked FPS: seal tick N's retained GX stream,
                                  * snapshot camera-cut history, and timestamp its present */
        tSeal = mp6_tick_phase_now();
        mp6_tick_phase_add(&g_phEndFrameNs, tEntry, tEndFrame);
        mp6_tick_phase_add(&g_phOverlayNs, tOverlayIn, tOverlayOut);
        mp6_tick_phase_add(&g_phSealNs, tEndFrame, tSeal);
        { extern void mp6_fs_frame_end(void); mp6_fs_frame_end(); } /* framescope */
        { /* MP6_FRAME_DUMP (shim/include/mp6_frame_dump.h): capture the frame
           * that was just presented. Same hook point as framescope right
           * above -- after aurora_end_frame(), so the readback the lever
           * enqueues is ordered strictly behind this frame's own work.
           * replayFrame=0: this is the real per-tick frame; frame_interp.c
           * makes the same call with 1 for each interpolated present, so a
           * burst captures the true presented sequence, not just the ticks. */
            extern void mp6_frame_dump_present(int replayFrame);
            mp6_frame_dump_present(0);
        }
        { /* MP6_SHADOW_DUMP (shim/include/mp6_shadow_dump.h): right after
           * aurora_end_frame(), same hook point as framescope right above --
           * any shadow copy THIS tick made is guaranteed already resolved. */
            extern void mp6_shadow_dump_tick(void);
            mp6_shadow_dump_tick();
        }
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
    mp6_present_rate_log();
    tThrottleOut = mp6_tick_phase_now(); /* the idle window is over; vipost starts */

#ifdef __ANDROID__
    /* SDL/Aurora blocks here while the activity is paused.  If a transient
     * surface loss makes begin_frame decline, stay in this host-only gate and
     * pump lifecycle events until a real frame opens.  No retrace callback,
     * PAD sample, Freecam integration, widescreen/game write, or tick advance
     * occurs inside the gate. */
    (void)mp6_frame_gate_wait(mp6_frame_gate_pump_events,
                              mp6_frame_gate_try_begin,
                              mp6_frame_gate_idle, &androidGate);
    frameBegan = 1;
    /* Apply deferred motor commands from main-loop context (see the
     * mp6_PADControlMotor queue above) -- right after the event pump, on
     * the thread's real stack, where a JNI-reaching rumble is legal. */
    mp6_pad_motor_apply_pending();
#else
    mp6_dispatch_aurora_events(aurora_update());
#endif

    /* Runs every tick (not just on a detected resize event) --
     * mp6_widescreen_render_width() re-reads the live window size fresh
     * every call, and the decomp-side setter is a cheap no-op whenever
     * nothing changed since the last tick, so this is the simplest robust
     * way to converge a live interactive resize without any separate
     * resize-event bookkeeping. A true no-op (both calls return 640 as a
     * pure passthrough) when widescreen is disabled -- the default -- so
     * this line does not perturb any existing gate. */
    mp6_widescreen_apply_render_width(mp6_widescreen_render_width());

    /* The render-width/2D-layer sync above already converges every tick,
     * but the 3D backdrop extrude + per-scene camera setup (platform/hsf/
     * mp6_widescreen_extrude.c) ran exactly ONCE, at scene load -- frozen
     * at whatever window size happened to be current then. This
     * re-derives every REGISTERED 3D backdrop/camera from its own cached
     * native baseline using the CURRENT scale_factor(), every tick, so a
     * live interactive resize converges the 3D content too, not just the
     * 2D/render-target layers. A true no-op (a for-loop over permanently-
     * empty registries) when widescreen is disabled -- see platform/hsf/
     * mp6_widescreen_extrude.c's own file header for the full mechanism. */
    mp6_widescreen_reapply();

    if (g_preRetraceCB) {
        g_preRetraceCB((u32)mp6_tick_count);
    }

#ifndef __ANDROID__
    frameBegan = aurora_begin_frame() ? 1 : 0;
#endif
    if (frameBegan) {
        g_frameOpen = true;
        mp6_present_counters_add(1, 0); /* MP6_PRESENT_RATE_LOG accounting */
        mp6_fi_note_frame_begin(); /* Unlocked FPS: arm retained-stream capture */
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
    {
        /* env latches the initial value once (it used to re-getenv every tick);
         * the dev console may override it live. */
        static int s_drawCountEnv = -1;
        if (s_drawCountEnv < 0) {
            const char *e = getenv("MP6_DIAG_DRAWCOUNT");
            s_drawCountEnv = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
        }
        if (mp6_console_cvar_get(MP6_CVAR_DIAG_DRAWCOUNT, s_drawCountEnv) &&
            (mp6_tick_count % 60) == 0) {
            printf("[MP6-DIAG-DRAWCOUNT] tick=%ld prev-frame draw count=%u\n", mp6_tick_count, g_mp6DrawIndex);
            fflush(stdout);
        }
    }
    /* Same instant, same reason: the console's scenerendering panel reports the
     * PREVIOUS complete frame, so it snapshots the per-frame GX census here,
     * right before the draw index is reset. */
    mp6_console_note_frame_reset();
    g_mp6DrawIndex = 0;
    /* On Windows, aurora_begin_frame() returning false (window minimized, etc.) just
     * means this logical tick renders nothing new -- the NEXT call's step
     * 1 correctly sees g_frameOpen still false and skips ending a frame
     * that was never opened, then tries begin_frame() again. In a
     * normal run this returns true on every tick -- the frame loop
     * genuinely cycles. Android never reaches this point without a successful
     * begin: its lifecycle gate above suspends the logical tick instead. */

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

    g_phLastReturnNs = mp6_tick_phase_now();
    mp6_tick_phase_add(&g_phViPostNs, tThrottleOut, g_phLastReturnNs);
    if (tEntry > 0) g_phSamples++;

    /* Publish THIS tick's phase slices to the console's sampler. The buckets
     * above are window SUMS that mp6_tick_rate_log() zeroes every 5s, so the
     * individual ticks are gone by the time anyone asks for a percentile --
     * this hands over the per-tick deltas instead. Called unconditionally: it
     * also refreshes the one relaxed int the GX census hook sites read, and it
     * returns immediately while the console is closed (every argument is 0
     * then anyway, since the phase clock is not armed). */
    mp6_console_note_tick_phase(
        (tPrevReturn > 0 && tEntry > tPrevReturn) ? tEntry - tPrevReturn : 0,
        (tEndFrame > tEntry) ? tEndFrame - tEntry : 0,
        (tOverlayOut > tOverlayIn) ? tOverlayOut - tOverlayIn : 0,
        (tSeal > tEndFrame) ? tSeal - tEndFrame : 0,
        (g_phLastReturnNs > tThrottleOut) ? g_phLastReturnNs - tThrottleOut : 0,
        g_tickLastLateNs, g_tickLastSlackNs);

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
    int lo, hi;
    if (!g_mp6SkipDrawParsed) {
        mp6_parse_skip_draws();
    }
    /* env parses "lo-hi" once; the dev console can move either end live
     * (`set skipdrawlo 40` / `set skipdrawhi 60`), which turns a bisect from an
     * edit-restart-look loop into a live one. -1 on either end = off. */
    lo = mp6_console_cvar_get(MP6_CVAR_SKIP_DRAW_LO, g_mp6SkipDrawLo);
    hi = mp6_console_cvar_get(MP6_CVAR_SKIP_DRAW_HI, g_mp6SkipDrawHi);
    if (lo < 0 || hi < lo) {
        return false;
    }
    return (int)idx >= lo && (int)idx <= hi;
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
    /* Primitive-type histogram + vertex total for the console's scenerendering
     * panel: both arguments are already in hand, so this intercepts nothing
     * new. One relaxed int load when the console is closed. */
    if (mp6_console_stats_active) mp6_console_note_prim((int)type, nverts);
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
    if (mp6_console_stats_active) mp6_console_note_dl_call(nbytes); /* replayed DL bytes */
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
 *     (b)'s AuroraSetViewportPolicy call itself now happens LATER --
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
 *     viewport metrics.
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

/* Stashed by mp6_bridge_window_policy_init below (the earliest point a
 * real SDL_Window* is available), consumed by
 * mp6_widescreen_render_width() further down to query the LIVE window
 * size on every call -- interactive resizes must keep tracking, not just
 * the size at boot, and this is the one place a real SDL_Window* is
 * available this early with no extra plumbing. */
static SDL_Window *g_mp6AspectWindow = NULL;

/* The one reader outside this section is mp6_render_res_get() (section 2,
 * compiled above this point), which needs the SAME handle so the HUD's
 * RenderRes row and mp6_widescreen_render_width() can never disagree about
 * which window they mean. */
SDL_Window *mp6_aspect_window(void) { return g_mp6AspectWindow; }

void mp6_bridge_window_policy_init(void *sdlWindowPtr)
{
    SDL_Window *window = (SDL_Window *)sdlWindowPtr;
    const char *freeAspect = getenv("MP6_FREE_ASPECT");
    bool aspectLocked = !(freeAspect != NULL && freeAspect[0] != '\0' && freeAspect[0] != '0');
    g_mp6AspectWindow = window; /* see the static's own comment above -- stash before the NULL check so later queries are consistent even if this call is ever a no-op below */
    /* This runs BEFORE config is ever loaded (right after
     * aurora_initialize()), so the only widescreen signal available this
     * early is the MP6_WIDESCREEN env lever (mirroring MP6_FREE_ASPECT's
     * own env-only early decision) -- mp6_widescreen_enabled() already
     * consults it internally (see that
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

/* The CONTENT half of the launcher-aspect fix above -- AuroraSetViewportPolicy
 * itself, deferred from "before the launcher" to "right before GameMain()"
 * so the RmlUi launcher (menu + persistent overlay) always composes
 * against the real window/display surface, on every aspect.
 *
 * aspectLockedCfg is the resolved user preference: g_cfg.aspectLocked in
 * launcher mode (mp6_launcher_cfg_aspect_locked(), launcher_core.cpp,
 * mirroring the existing mp6_launcher_cfg_backend()/_vsync() pattern) or a
 * fixed 1 (today's automation-mode default, matching this function's own
 * earlier behavior when config is never read) otherwise. MP6_FREE_ASPECT
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
    /* mp6_widescreen_set_enabled() (platform/main_native.c) must run
     * BEFORE this function for this to see the right value -- both are
     * called back-to-back, right before GameMain(), same call-timing
     * contract as every other setting in this file. MP6_FREE_ASPECT keeps
     * absolute top priority over BOTH widescreen and aspectLocked
     * (unchanged escape hatch: raw anisotropic stretch, for A/B
     * comparisons only). */
    bool widescreen = mp6_widescreen_enabled() && !envFree;

    if (envFree) {
        AuroraSetViewportPolicy(AURORA_VIEWPORT_STRETCH);
        printf("[MP6-WINDOW] content aspect policy: FREE STRETCH (MP6_FREE_ASPECT) -- "
               "non-4:3 window shapes will distort the scene\n");
    } else if (widescreen) {
        /* THE key trick: stay on FIT, never switch to STRETCH. FIT
         * letterboxes/pillarboxes the GX render target to match Aurora's
         * own logical-fb aspect (aurora lib/gx/gx.cpp
         * vi::configured_fb_size(), driven by whatever RenderMode the
         * game's own VIConfigure call currently holds) inside the real
         * window. Widescreen widens THAT logical aspect itself
         * (mp6_widescreen_apply_render_width(), called from the game's
         * own HuSysInit and every tick thereafter) to track the live
         * window aspect -- so FIT's fitted rectangle converges to the
         * full window with no visible bars, because the thing being fit
         * is already the right shape, not because fitting was disabled.
         * This is what makes this a TRUE-WIDE render (the GX
         * viewport/scissor genuinely widen) rather than an earlier
         * discarded approach (STRETCH + an anamorphic projection widen --
         * visibly distorts every 2D/HUD element, since STRETCH scales the
         * whole already-composited frame non-uniformly). */
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
 * Dynamic true-widescreen -- implementation of
 * shim/include/mp6_widescreen.h's contract.
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
    /* THE ONE ON/OFF ANSWER every widescreen consumer reads -- window-shape
     * policy, content-aspect policy, render_width/scale_factor/
     * half_width_delta, mp6_widescreen_extrude.c's whole gate, and every
     * decomp-side patch that calls this directly. Resolved here, once, so no
     * two of them can disagree.
     *
     * Three sources:
     *
     *   1. g_mp6WidescreenEnabled -- the LIVE latch. main_native.c sets it
     *      once right before GameMain() from mp6_enh_widescreen() (see that
     *      call site), and the settings window re-sets it the instant the
     *      user flips the row mid-session. It is the latch, not the seam,
     *      that a live toggle can move within a tick: settings.cpp applies
     *      the change BEFORE cfg_save() republishes to the seam.
     *
     *   2. the ENHANCEMENTS SEAM (shim/include/mp6_enhancements.h), which
     *      resolves MP6_ENH_WIDESCREEN -> MP6_ENH_PRESET -> the published
     *      config -> RETAIL. This is consulted HERE and not only through the
     *      latch because mp6_bridge_window_policy_init() runs right after
     *      aurora_initialize(), long before main_native.c's latch call -- so
     *      without it the window shape at boot could not see either
     *      MP6_ENH_* lever, only the legacy one below. Automation publishes
     *      nothing to the seam, so this term is 0 on every automated boot
     *      and every existing gate stays byte-unchanged.
     *
     *   3. MP6_WIDESCREEN -- the pre-existing per-feature lever, unchanged.
     *      Same shape as MP6_AUTO_START_TICKS/--input-script/MP6_FREE_ASPECT:
     *      it forces widescreen ON in either mode, and unset it has zero
     *      effect. docs/SETTINGS.md's promise that the legacy levers keep
     *      their own priority at their own consumption site is exactly this
     *      OR term. */
    if (g_mp6WidescreenEnabled) {
        return 1;
    }
    if (mp6_enh_widescreen()) {
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
    /* This used to clamp at a hardcoded 1280.0f ("sane upper bound -- an
     * extreme sliver window can't blow out a texture/array bound"), which
     * silently pinned the render width at
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
        /* Self-sync, closing a real early-boot race found by testing at
         * an extreme aspect (crash repro: MP6_WINDOW_SIZE=5120x480,
         * "[AURORA FATAL] WebGPU error 2: Viewport width (40960.000000)
         * exceeds the maximum (16384)" -- 40960 == 5120*8). Root cause:
         * several decomp-side camera patches (boot.c/opening.c/filesel.c/
         * mdsel.c/actman.c/sequence.c widens, mirroring hsfman.c's own
         * established pattern) pass THIS function's return value
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
