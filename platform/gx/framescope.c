/* MP6 native port -- FRAMESCOPE: per-draw GX state-call capture.
 *
 * Records EVERY relevant GX configuration call the GAME makes, tagged with
 * the current draw index, and dumps one complete frame's trace as text:
 * "which TEV inputs / kColor / texmap / swap tables was draw N actually
 * configured with" becomes a file you read, not a hypothesis you test one
 * printf at a time.
 *
 * Mechanism: dolphin_compat.h #define-renames the intercepted GX calls to
 * mp6_fs_* for DECOMP TUs only (this file and the bridge compile against
 * aurora's own headers and are not affected). Each wrapper appends a line
 * to a ring buffer and forwards to the real Aurora implementation. The
 * draw index increments on each GXBegin/display-list execute, mirroring
 * MP6_SKIP_DRAWS's counting so indices line up across both tools.
 *
 * Controls (env):
 *   MP6_FRAMESCOPE=N   capture frame N (the N-th aurora frame after boot)
 *                      and write build/framescope.txt, then keep running.
 *                      "0" or unset = disabled, zero overhead beyond one
 *                      integer check per call.
 *   MP6_FRAMESCOPE_OUT=path   override the output path.
 */
#include <dolphin/gx.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SAVESTATE CARVE-OUT: host-owned statics (RmlUi document
 * sources, UI framework state, debug-tool latches) must not be captured or
 * restored. Must sit AFTER this TU's own includes and at preprocessor TOP
 * LEVEL (build.py rejects a conditionally-nested include -- a platform
 * branch would silently uncarve the TU). See mp6_host_section.h. */
#include "mp6_host_section.h"


extern long mp6_tick_count;
extern u32 mp6_current_draw_index(void); /* aurora_bridge.c's per-frame draw-index counter */

#define FS_MAX_LINES 8192
#define FS_LINE_LEN  192

static char  fs_lines[FS_MAX_LINES][FS_LINE_LEN];
static int   fs_count = 0;
static int   fs_target_frame = -2; /* -2 = not parsed yet, -1 = disabled */
static long  fs_frame_no = 0;      /* incremented by mp6_fs_frame_end() */
static int   fs_armed = 0;

static void fs_parse_env(void)
{
    const char *e = getenv("MP6_FRAMESCOPE");
    fs_target_frame = (e && atoi(e) > 0) ? atoi(e) : -1;
}

int mp6_fs_active(void)
{
    if (fs_target_frame == -2) fs_parse_env();
    return fs_armed;
}

/* called from the bridge once per aurora frame (end-of-frame) */
void mp6_fs_frame_end(void)
{
    if (fs_target_frame == -2) fs_parse_env();
    fs_frame_no++; /* count always: CopyTex announcements stamp frame numbers
                      even when no capture frame is armed */
    if (fs_target_frame < 0) return;
    if (fs_frame_no == fs_target_frame) {
        fs_armed = 1;
        fs_count = 0;
        printf("[FRAMESCOPE] armed for frame %ld\n", fs_frame_no + 1);
    } else if (fs_armed) {
        /* frame just captured -- dump */
        const char *out = getenv("MP6_FRAMESCOPE_OUT");
        char path[260];
        snprintf(path, sizeof path, "%s", out ? out : "build/framescope.txt");
        FILE *f = fopen(path, "w");
        if (f) {
            int i;
            fprintf(f, "# framescope: frame %ld, tick %ld, %d calls captured\n",
                    fs_frame_no, mp6_tick_count, fs_count);
            for (i = 0; i < fs_count; i++) fputs(fs_lines[i], f);
            fclose(f);
            printf("[FRAMESCOPE] wrote %d lines to %s\n", fs_count, path);
        }
        fs_armed = 0;
        fs_target_frame = -1; /* one-shot */
        fflush(stdout);
    }
}

static void fs_log(const char *fmt, ...)
{
    va_list ap;
    char *dst;
    int off;
    if (!fs_armed || fs_count >= FS_MAX_LINES) return;
    dst = fs_lines[fs_count++];
    off = snprintf(dst, FS_LINE_LEN, "d%03d ", mp6_current_draw_index());
    va_start(ap, fmt);
    vsnprintf(dst + off, FS_LINE_LEN - off - 2, fmt, ap);
    va_end(ap);
    strcat(dst, "\n");
}

/* ---- intercepted wrappers: log + forward ---- */

void mp6_fs_GXSetTevColorIn(GXTevStageID st, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d)
{ fs_log("TevColorIn  st%d  a=%d b=%d c=%d d=%d", st, a, b, c, d); GXSetTevColorIn(st, a, b, c, d); }

void mp6_fs_GXSetTevAlphaIn(GXTevStageID st, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d)
{ fs_log("TevAlphaIn  st%d  a=%d b=%d c=%d d=%d", st, a, b, c, d); GXSetTevAlphaIn(st, a, b, c, d); }

void mp6_fs_GXSetTevColorOp(GXTevStageID st, GXTevOp op, GXTevBias bias, GXTevScale sc, GXBool clamp, GXTevRegID out)
{ fs_log("TevColorOp  st%d  op=%d out=%d", st, op, out); GXSetTevColorOp(st, op, bias, sc, clamp, out); }

void mp6_fs_GXSetTevAlphaOp(GXTevStageID st, GXTevOp op, GXTevBias bias, GXTevScale sc, GXBool clamp, GXTevRegID out)
{ fs_log("TevAlphaOp  st%d  op=%d out=%d", st, op, out); GXSetTevAlphaOp(st, op, bias, sc, clamp, out); }

void mp6_fs_GXSetTevOrder(GXTevStageID st, GXTexCoordID tc, GXTexMapID tm, GXChannelID ch)
{ fs_log("TevOrder    st%d  texcoord=%d TEXMAP=%d chan=%d", st, tc, tm, ch); GXSetTevOrder(st, tc, tm, ch); }

void mp6_fs_GXSetNumTevStages(u8 n)
{ fs_log("NumTevStages %d", n); GXSetNumTevStages(n); }

void mp6_fs_GXSetTevKColor(GXTevKColorID id, GXColor c)
{ fs_log("KColor      id=%d  rgba=(%d,%d,%d,%d)", id, c.r, c.g, c.b, c.a); GXSetTevKColor(id, c); }

void mp6_fs_GXSetTevKColorSel(GXTevStageID st, GXTevKColorSel sel)
{ fs_log("KColorSel   st%d  sel=%d", st, sel); GXSetTevKColorSel(st, sel); }

void mp6_fs_GXSetTevKAlphaSel(GXTevStageID st, GXTevKAlphaSel sel)
{ fs_log("KAlphaSel   st%d  sel=%d", st, sel); GXSetTevKAlphaSel(st, sel); }

void mp6_fs_GXSetTevSwapMode(GXTevStageID st, GXTevSwapSel ras, GXTevSwapSel tex)
{ fs_log("SwapMode    st%d  ras=%d tex=%d", st, ras, tex); GXSetTevSwapMode(st, ras, tex); }

void mp6_fs_GXSetTevSwapModeTable(GXTevSwapSel sel, GXTevColorChan r, GXTevColorChan g, GXTevColorChan b, GXTevColorChan a)
{ fs_log("SwapTable   sel=%d  r<-%d g<-%d b<-%d a<-%d", sel, r, g, b, a); GXSetTevSwapModeTable(sel, r, g, b, a); }

/* MP6_FRAMESCOPE_TEXDUMP=1: while the capture frame is armed, also write each
 * loaded texture's raw (still GC-tiled) bytes to build/fs_tex_<ptr>_<w>x<h>_f<fmt>.bin
 * so the actual pixel content can be decoded and inspected offline
 * (tools/gc_tex_decode.py). Dedups by data pointer within the frame. */
static const void *fs_dumped[64];
static int fs_dumped_n = 0;

static u32 fs_tex_size(int w, int h, int fmt)
{
    /* bytes for the base mip, tile-rounded, per GX format */
    switch (fmt) {
    case 0: /* I4   */ return (u32)((w + 7) / 8) * ((h + 7) / 8) * 32;
    case 1: /* I8   */ return (u32)((w + 7) / 8) * ((h + 3) / 4) * 32;
    case 2: /* IA4  */ return (u32)((w + 7) / 8) * ((h + 3) / 4) * 32;
    case 3: /* IA8  */ return (u32)((w + 3) / 4) * ((h + 3) / 4) * 32;
    case 4: /* RGB565 */ return (u32)((w + 3) / 4) * ((h + 3) / 4) * 32;
    case 5: /* RGB5A3 */ return (u32)((w + 3) / 4) * ((h + 3) / 4) * 32;
    case 6: /* RGBA8  */ return (u32)((w + 3) / 4) * ((h + 3) / 4) * 64;
    case 8: /* C4   */ return (u32)((w + 7) / 8) * ((h + 7) / 8) * 32;
    case 9: /* C8   */ return (u32)((w + 7) / 8) * ((h + 3) / 4) * 32;
    case 14:/* CMPR */ return (u32)((w + 7) / 8) * ((h + 7) / 8) * 32;
    default: return 0;
    }
}

static void fs_texdump(const void *data, int w, int h, int fmt)
{
    static int fs_texdump_env = -1;
    char path[260];
    FILE *f;
    u32 sz;
    int i;
    if (fs_texdump_env == -1) {
        const char *e = getenv("MP6_FRAMESCOPE_TEXDUMP");
        fs_texdump_env = (e && atoi(e) > 0);
    }
    if (!fs_texdump_env || !data) return;
    for (i = 0; i < fs_dumped_n; i++) if (fs_dumped[i] == data) return;
    if (fs_dumped_n < 64) fs_dumped[fs_dumped_n++] = data;
    sz = fs_tex_size(w, h, fmt);
    if (!sz) return;
    snprintf(path, sizeof path, "build/fs_tex_%p_%dx%d_f%d.bin", data, w, h, fmt);
    f = fopen(path, "wb");
    if (f) { fwrite(data, 1, sz, f); fclose(f); }
}

void mp6_fs_GXLoadTexObj(GXTexObj *obj, GXTexMapID map)
{
    /* width/height/format via aurora's own accessors keeps this ABI-safe */
    fs_log("LoadTexObj  TEXMAP=%d  data=%p  %dx%d fmt=%d", map,
           GXGetTexObjData(obj), GXGetTexObjWidth(obj), GXGetTexObjHeight(obj), GXGetTexObjFmt(obj));
    if (fs_armed)
        fs_texdump(GXGetTexObjData(obj), GXGetTexObjWidth(obj), GXGetTexObjHeight(obj), GXGetTexObjFmt(obj));
    GXLoadTexObj(obj, map);
}

void mp6_fs_GXSetBlendMode(GXBlendMode t, GXBlendFactor s, GXBlendFactor d, GXLogicOp op)
{ fs_log("BlendMode   type=%d src=%d dst=%d", t, s, d); GXSetBlendMode(t, s, d, op); }

/* ---- TLUT / palettized-texture probes ----
 * A C8 (palettized) texture's rendered hue is decided entirely by which
 * TLUT bytes back it. These three wrappers pin that association: which
 * (data,fmt,entries) TLUT was loaded into which tlut slot, and which tlut
 * slot each CI texObj references. GXTlutObj is opaque at the C API level (no
 * accessors, unlike GXGetTexObj*), so mp6_fs_GXInitTlutObj records each
 * init's args in a tiny table keyed by the object POINTER; the
 * back-to-back InitTlutObj -> LoadTlut idiom every call site in this
 * codebase uses (game/sprput.c HuSprTexLoad, game/hsfdraw.c LoadTexture)
 * makes that lookup reliable. With MP6_FRAMESCOPE_TEXDUMP=1 the armed
 * frame also dumps each loaded TLUT's raw BE u16 entries to
 * build/fs_tlut_i<slot>_<ptr>_n<entries>_tf<fmt>.bin so the palette can
 * be decoded offline against the matching fs_tex C8 dump
 * (tools/gc_tex_decode.py --tlut). */
#define FS_TLUT_TRACK 8
static struct { const GXTlutObj *obj; const void *data; int fmt; int entries; } fs_tluts[FS_TLUT_TRACK];
static int fs_tlut_next = 0;

void mp6_fs_GXInitTlutObj(GXTlutObj *obj, const void *data, GXTlutFmt format, u16 entries)
{
    int i;
    for (i = 0; i < FS_TLUT_TRACK; i++) {
        if (fs_tluts[i].obj == obj) break;
    }
    if (i == FS_TLUT_TRACK) { i = fs_tlut_next; fs_tlut_next = (fs_tlut_next + 1) % FS_TLUT_TRACK; }
    fs_tluts[i].obj = obj; fs_tluts[i].data = data;
    fs_tluts[i].fmt = (int)format; fs_tluts[i].entries = (int)entries;
    fs_log("InitTlutObj obj=%p data=%p fmt=%d entries=%u", (void *)obj, data, format, entries);
    GXInitTlutObj(obj, data, format, entries);
}

void mp6_fs_GXLoadTlut(const GXTlutObj *obj, u32 idx)
{
    int i;
    for (i = 0; i < FS_TLUT_TRACK; i++) {
        if (fs_tluts[i].obj == obj) break;
    }
    if (i < FS_TLUT_TRACK) {
        fs_log("LoadTlut    slot=%u data=%p fmt=%d entries=%d", idx,
               fs_tluts[i].data, fs_tluts[i].fmt, fs_tluts[i].entries);
        if (fs_armed && fs_tluts[i].data && fs_tluts[i].entries > 0) {
            static int fs_tlutdump_env = -1;
            if (fs_tlutdump_env == -1) {
                const char *e = getenv("MP6_FRAMESCOPE_TEXDUMP");
                fs_tlutdump_env = (e && atoi(e) > 0);
            }
            if (fs_tlutdump_env) {
                char path[260];
                FILE *f;
                snprintf(path, sizeof path, "build/fs_tlut_i%u_%p_n%d_tf%d.bin",
                         idx, fs_tluts[i].data, fs_tluts[i].entries, fs_tluts[i].fmt);
                f = fopen(path, "wb");
                if (f) { fwrite(fs_tluts[i].data, 2, (size_t)fs_tluts[i].entries, f); fclose(f); }
            }
        }
    } else {
        fs_log("LoadTlut    slot=%u obj=%p (init not seen)", idx, (const void *)obj);
    }
    GXLoadTlut(obj, idx);
}

void mp6_fs_GXInitTexObjCI(GXTexObj *obj, const void *data, u16 width, u16 height, GXCITexFmt format,
                           GXTexWrapMode wrapS, GXTexWrapMode wrapT, GXBool mipmap, u32 tlut)
{
    fs_log("InitTexObjCI data=%p %ux%u fmt=%d tlutSlot=%u", data, width, height, format, tlut);
    GXInitTexObjCI(obj, data, width, height, format, wrapS, wrapT, mipmap, tlut);
}

/* ---- texgen/texmtx probes: day/night sheet-region selection ----
 * The Hu3D texture-anim machinery selects a sub-region of a multi-frame
 * sheet purely through a 2x4 texture matrix (hsfdraw.c's ANIM2D branch:
 * scale+trans from the anim layer) -- these two wrappers make the applied
 * matrix and the texcoord-gen wiring visible per draw, so "wrong region"
 * can be split into 'game computed the wrong matrix' vs 'matrix never
 * consumed'. GXSetTexCoordGen is a header inline over GXSetTexCoordGen2,
 * so intercepting the 2-suffix symbol catches every call shape. */
void mp6_fs_GXLoadTexMtxImm(const void *mtx, u32 id, GXTexMtxType type)
{
    const f32 (*m)[4] = (const f32 (*)[4])mtx;
    fs_log("LoadTexMtx  id=%u type=%d [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
           id, type, m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3]);
    GXLoadTexMtxImm(mtx, id, type);
}

void mp6_fs_GXSetTexCoordGen2(GXTexCoordID dst, GXTexGenType fn, GXTexGenSrc src, u32 mtx, GXBool normalize, u32 postMtx)
{
    fs_log("TexCoordGen coord=%d fn=%d src=%d mtx=%u", dst, fn, src, mtx);
    GXSetTexCoordGen2(dst, fn, src, mtx, normalize, postMtx);
}

/* EFB-copy probes: track WHEN GXCopyTex fires relative to the draws that
 * consume its dest. Rare calls, so the first few also go to stdout
 * unconditionally (they may happen outside the armed frame); after that
 * only the armed-frame trace records them. */
static struct { s16 x, y, w, h; } fs_copy_src;
static struct { u16 w, h; int fmt, mip; } fs_copy_dst;

void mp6_fs_GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht)
{
    fs_copy_src.x = left; fs_copy_src.y = top; fs_copy_src.w = wd; fs_copy_src.h = ht;
    fs_log("TexCopySrc  %d,%d %dx%d", left, top, wd, ht);
    GXSetTexCopySrc(left, top, wd, ht);
}

void mp6_fs_GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap)
{
    fs_copy_dst.w = wd; fs_copy_dst.h = ht; fs_copy_dst.fmt = fmt; fs_copy_dst.mip = mipmap;
    fs_log("TexCopyDst  %dx%d fmt=%d mip=%d", wd, ht, fmt, mipmap);
    GXSetTexCopyDst(wd, ht, fmt, mipmap);
}

void mp6_fs_GXCopyTex(void *dest, GXBool clear)
{
    /* Announce policy: first 32 unconditionally, then only NOVEL dest
     * pointers (new copy targets are the interesting events; the per-frame
     * BookFrameBuf[0] repeat would otherwise drown the log). */
    static int announced = 0;
    static const void *seen[16];
    static int seen_n = 0;
    int novel = 1, i;
    for (i = 0; i < seen_n; i++) if (seen[i] == dest) { novel = 0; break; }
    if (novel && seen_n < 16) seen[seen_n++] = dest;
    if (announced < 32 || novel) {
        announced++;
        printf("[FRAMESCOPE] CopyTex #%d frame=%ld draw=%u dest=%p src=%d,%d %dx%d dst=%dx%d fmt=%d clear=%d%s\n",
               announced, fs_frame_no, mp6_current_draw_index(), dest,
               fs_copy_src.x, fs_copy_src.y, fs_copy_src.w, fs_copy_src.h,
               fs_copy_dst.w, fs_copy_dst.h, fs_copy_dst.fmt, clear,
               novel ? "  <-- NOVEL DEST" : "");
        fflush(stdout);
    }
    fs_log("CopyTex     dest=%p clear=%d", dest, clear);
    GXCopyTex(dest, clear);
}

void mp6_fs_GXSetZMode(GXBool en, GXCompare fn, GXBool upd)
{ fs_log("ZMode       en=%d fn=%d upd=%d", en, fn, upd); GXSetZMode(en, fn, upd); }
