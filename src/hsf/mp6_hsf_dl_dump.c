/* MP6 native port: MP6_DL_DUMP diagnostics for HSF display-list builds
 * (moved out of game/hsfdraw.c: decomp files carry only the hook call).
 * See docs/DEBUGGING.md. Diagnostic only: never changes behavior. */
#include "dolphin.h"
#include "game/hu3d.h"
#include "mp6_hsf_dl_dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Display-list / vertex ground-truth dump, used to track down a
 * mode-select/file-select "vertices all over the place" report with
 * actual bytes instead of more theories: for the one object named by
 * MP6_DL_DUMP=<name> this dumps, at DL-BUILD time (MDObjMesh, i.e.
 * model-create), to build/dldump_<name>.txt:
 *   - the parsed face records (type/mat/index tuples) the DL was built from,
 *   - the vertex/st array contents summary (count, first entries, min/max
 *     magnitude, out-of-range-index scan over every face tuple),
 *   - the referenced materials' vtxMode/attrNum/flags (the vertex-descriptor
 *     selectors), and
 *   - the raw built DL bytes (first 1 KB hex) + per-batch dlOfs table.
 * Decoding those bytes against the descriptor the draw path will set is a
 * mechanical, offline check of stream-vs-descriptor alignment. One getenv,
 * zero output when unset. Diagnostic only: never changes behavior. */
static int mp6DlDumpParsed = 0;       /* 0 = env not parsed yet */
static const char *mp6DlDumpName = NULL;
static const char *mp6DlDumpEnv(void)
{
    if (!mp6DlDumpParsed) {
        mp6DlDumpName = getenv("MP6_DL_DUMP");
        mp6DlDumpParsed = 1;
    }
    return mp6DlDumpName;
}
static int mp6DlDumpWant(const char *objName)
{
    const char *want = mp6DlDumpEnv();
    return want && objName && strcmp(want, objName) == 0;
}

void mp6_hsf_dl_dump(HSF_OBJECT *objPtr, HSF_BUFFER *faceBuf, const void *DLBufStartP, s32 totalSize,
                     s32 DLTotalNum, const HSF_DRAWDATA *DrawData, s32 matChgCnt)
{
    if (!mp6DlDumpEnv()) {
        return;
    }
    if (mp6DlDumpEnv() && mp6DlDumpEnv()[0] == '*') { /* sweep mode: one line per built object */
        HSF_FACE *fp = faceBuf->data;
        HuVecF *vtx = objPtr->mesh.vertex ? (HuVecF *)objPtr->mesh.vertex->data : NULL;
        s32 vtxN = objPtr->mesh.vertex ? objPtr->mesh.vertex->count : 0;
        s32 stN = objPtr->mesh.st ? objPtr->mesh.st->count : 0;
        s32 fi, k, badV = 0, badS = 0;
        float mn = 1e30f, mx = -1e30f;
        for (fi = 0; fi < vtxN; fi++) {
            float ax = vtx[fi].x < 0 ? -vtx[fi].x : vtx[fi].x;
            float ay = vtx[fi].y < 0 ? -vtx[fi].y : vtx[fi].y;
            float az = vtx[fi].z < 0 ? -vtx[fi].z : vtx[fi].z;
            float m = ax > ay ? ax : ay; if (az > m) m = az;
            if (m < mn) mn = m; if (m > mx) mx = m;
        }
        for (fi = 0; fi < faceBuf->count; fi++) {
            HSF_FACE *f = &fp[fi];
            s32 nIdx = ((f->typeSrc & HSF_FACE_MASK) == HSF_FACE_QUAD) ? 4 :
                       ((f->typeSrc & HSF_FACE_MASK) == HSF_FACE_TRI) ? 3 : 0;
            for (k = 0; k < nIdx; k++) {
                if (f->index[k].vertex < 0 || f->index[k].vertex >= vtxN) badV++;
                if (stN > 0 && (f->index[k].st < -1 || f->index[k].st >= stN)) badS++;
            }
            if ((f->typeSrc & HSF_FACE_MASK) == HSF_FACE_TRISTRIP) {
                HSF_FACE_INDEX *sd = f->strip.data;
                u32 sc = f->strip.count, m2;
                for (m2 = 0; m2 < 3; m2++) {
                    if (f->strip.index[m2].vertex < 0 || f->strip.index[m2].vertex >= vtxN) badV++;
                }
                for (m2 = 0; sd && m2 < sc; m2++) {
                    if (sd[m2].vertex < 0 || sd[m2].vertex >= vtxN) badV++;
                    if (stN > 0 && (sd[m2].st < -1 || sd[m2].st >= stN)) badS++;
                }
            }
        }
        printf("[DL-SWEEP] obj=%s faces=%d vtxN=%d dl=%d/%d badV=%d badS=%d vmag=[%g..%g]%s\n",
               objPtr->name ? objPtr->name : "(null)", faceBuf->count, vtxN,
               (int)totalSize, (int)DLTotalNum, (int)badV, (int)badS, mn, mx,
               ((int)totalSize > (int)DLTotalNum || badV || badS) ? "  <-- ANOMALY" : "");
        fflush(stdout);
    }
    if (mp6DlDumpWant(objPtr->name)) { /* DL/vertex ground-truth dump -- see mp6DlDumpWant above */
        char path[128];
        FILE *df;
        snprintf(path, sizeof(path), "build/dldump_%s.txt", objPtr->name);
        df = fopen(path, "w");
        if (df) {
            HSF_FACE *fp = faceBuf->data;
            HuVecF *vtx = objPtr->mesh.vertex ? (HuVecF *)objPtr->mesh.vertex->data : NULL;
            s32 vtxN = objPtr->mesh.vertex ? objPtr->mesh.vertex->count : 0;
            HuVec2f *stp = objPtr->mesh.st ? (HuVec2f *)objPtr->mesh.st->data : NULL;
            s32 stN = objPtr->mesh.st ? objPtr->mesh.st->count : 0;
            s32 nrmN = objPtr->mesh.normal ? objPtr->mesh.normal->count : 0;
            s32 colN = objPtr->mesh.color ? objPtr->mesh.color->count : 0;
            s32 fi, k, badV = 0, badS = 0;
            float mn = 1e30f, mx = -1e30f;
            const u8 *dl = (const u8 *)DLBufStartP;
            fprintf(df, "obj=%s faces=%d vtxN=%d stN=%d nrmN=%d colN=%d dlSize=%d dlCap=%d batches=%d\n",
                    objPtr->name, faceBuf->count, vtxN, stN, nrmN, colN, (int)totalSize, (int)DLTotalNum, (int)matChgCnt);
            for (fi = 0; fi < vtxN; fi++) {
                float ax = vtx[fi].x < 0 ? -vtx[fi].x : vtx[fi].x;
                float ay = vtx[fi].y < 0 ? -vtx[fi].y : vtx[fi].y;
                float az = vtx[fi].z < 0 ? -vtx[fi].z : vtx[fi].z;
                float m = ax > ay ? ax : ay; if (az > m) m = az;
                if (m < mn) mn = m; if (m > mx) mx = m;
            }
            fprintf(df, "vtx magnitude min=%g max=%g\n", mn, mx);
            for (fi = 0; fi < vtxN && fi < 8; fi++) {
                fprintf(df, "  vtx[%d]=(%g, %g, %g)\n", fi, vtx[fi].x, vtx[fi].y, vtx[fi].z);
            }
            for (fi = 0; fi < stN && fi < 8; fi++) {
                fprintf(df, "  st[%d]=(%g, %g)\n", fi, stp[fi].x, stp[fi].y);
            }
            for (fi = 0; fi < faceBuf->count; fi++) {
                HSF_FACE *f = &fp[fi];
                s32 nIdx = ((f->typeSrc & HSF_FACE_MASK) == HSF_FACE_QUAD) ? 4 :
                           ((f->typeSrc & HSF_FACE_MASK) == HSF_FACE_TRI) ? 3 : 0;
                for (k = 0; k < nIdx; k++) {
                    if (f->index[k].vertex < 0 || f->index[k].vertex >= vtxN) badV++;
                    if (stN > 0 && (f->index[k].st < -1 || f->index[k].st >= stN)) badS++;
                }
                if (fi < 24) {
                    fprintf(df, "face[%d] type=%04x mat=%d", fi, (unsigned)f->typeSrc, f->mat & 0xFFF);
                    if ((f->typeSrc & HSF_FACE_MASK) == HSF_FACE_TRISTRIP) {
                        u32 m2;
                        for (m2 = 0; m2 < 3; m2++) {
                            fprintf(df, " (v=%d n=%d c=%d st=%d)", f->strip.index[m2].vertex,
                                    f->strip.index[m2].normal, f->strip.index[m2].color, f->strip.index[m2].st);
                        }
                        fprintf(df, " STRIP count=%u:", (unsigned)f->strip.count);
                        for (m2 = 0; f->strip.data && m2 < f->strip.count && m2 < 8; m2++) {
                            fprintf(df, " (v=%d n=%d c=%d st=%d)", f->strip.data[m2].vertex,
                                    f->strip.data[m2].normal, f->strip.data[m2].color, f->strip.data[m2].st);
                        }
                    } else {
                        for (k = 0; k < (nIdx ? nIdx : 3); k++) {
                            fprintf(df, " (v=%d n=%d c=%d st=%d)", f->index[k].vertex,
                                    f->index[k].normal, f->index[k].color, f->index[k].st);
                        }
                    }
                    {
                        HSF_MATERIAL *fm = &objPtr->mesh.material[f->mat & 0xFFF];
                        fprintf(df, " | mat vtxMode=%d attrNum=%d flags=%08x pass=%d\n",
                                fm->vtxMode, (int)fm->attrNum, (unsigned)fm->flags, fm->pass);
                    }
                }
            }
            fprintf(df, "out-of-range indices: vertex=%d st=%d (over %d faces)\n", (int)badV, (int)badS, faceBuf->count);
            for (fi = 0; fi < matChgCnt && fi < 8; fi++) {
                fprintf(df, "batch[%d] dlOfs=%u flags=%x\n", fi, (unsigned)DrawData[fi].dlOfs, (unsigned)DrawData[fi].flags);
            }
            for (fi = 0; fi < (s32)totalSize && fi < 1024; fi++) {
                fprintf(df, "%02x%s", dl[fi], ((fi & 15) == 15) ? "\n" : " ");
            }
            fprintf(df, "\n");
            fclose(df);
            printf("[DL-DUMP] wrote %s (faces=%d dlSize=%d)\n", path, faceBuf->count, (int)totalSize);
            fflush(stdout);
        }
    }
}
