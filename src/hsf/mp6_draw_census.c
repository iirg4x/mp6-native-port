/* MP6 native port: MP6_DRAW_CENSUS, the per-model Hu3DExec draw census
 * (moved out of game/hsfman.c: decomp files carry only the hook calls).
 * See docs/DEBUGGING.md. */
#include "dolphin.h"
#include "game/hu3d.h"
#include "mp6_draw_census.h"

#include <stdio.h>
#include <stdlib.h>

/* Title-wordmark draw census: an earlier framescope census proved the
 * three BootTitleCreate models (TITLE_HSF_PARTY/SIX/STAR) never load a
 * single texture on the title frame -- i.e. they never reach ObjDraw at
 * all -- but framescope can only see GX calls, not WHY the Hu3DExec model
 * loop skipped a model. This census prints, for MP6_DRAW_CENSUS=<tick>
 * (two consecutive ticks starting there, so one full Hu3DExec pass is
 * guaranteed even if the first armed tick lands mid-frame):
 *   - one entry-state dump per Hu3DExec call (live cameras, layerNum[],
 *     and every live model's routing state: layer, cameraBit, attr bits,
 *     scale, motion id, object count), and
 *   - one verdict line per live model on its OWN layer's pass per camera,
 *     evaluating the loop's real skip chain in the loop's real order.
 * Zero output and one getenv (cached) when the env var is unset.
 * Diagnostic only: prints state, never changes behavior. */
extern long mp6_tick_count;
static long mp6CensusTick = -2; /* -2 = env not parsed yet, -1 = disabled */
int mp6_draw_census_active(void)
{
    if (mp6CensusTick == -2) {
        const char *e = getenv("MP6_DRAW_CENSUS");
        mp6CensusTick = (e && atol(e) > 0) ? atol(e) : -1;
    }
    return mp6CensusTick > 0 && mp6_tick_count >= mp6CensusTick &&
           mp6_tick_count < mp6CensusTick + 2;
}

/* Entry-state dump for one Hu3DExec call: live cameras, the layers' model
 * counts and every live model's routing state. */
void mp6_draw_census_exec_entry(const s16 *layerNum)
{
    if (!mp6_draw_census_active()) {
        return;
    }
    {
        HU3D_MODEL *cmP;
        s16 ci;
        printf("[CENSUS] tick=%ld Hu3DExec entry, cameraExistF=%04x\n",
               mp6_tick_count, (unsigned)Hu3DCameraExistF);
        for (ci = 0; ci < HU3D_CAM_MAX; ci++) {
            if (Hu3DCamera[ci].fov != -1) {
                printf("[CENSUS]  cam%d fov=%.1f near=%.1f far=%.1f aspect=%.2f vp=(%.0f,%.0f %.0fx%.0f) "
                       "pos=(%.1f,%.1f,%.1f) target=(%.1f,%.1f,%.1f)\n",
                       ci, Hu3DCamera[ci].fov, Hu3DCamera[ci].near, Hu3DCamera[ci].far,
                       Hu3DCamera[ci].aspect, Hu3DCamera[ci].viewportX, Hu3DCamera[ci].viewportY,
                       Hu3DCamera[ci].viewportW, Hu3DCamera[ci].viewportH,
                       Hu3DCamera[ci].pos.x, Hu3DCamera[ci].pos.y, Hu3DCamera[ci].pos.z,
                       Hu3DCamera[ci].target.x, Hu3DCamera[ci].target.y, Hu3DCamera[ci].target.z);
            }
        }
        printf("[CENSUS]  layerNum:");
        for (ci = 0; ci < HU3D_LAYER_MAX; ci++) {
            printf(" %d", layerNum[ci]);
        }
        printf("\n");
        for (cmP = &Hu3DData[0], ci = 0; ci < HU3D_MODEL_MAX; ci++, cmP++) {
            if (!cmP->hsf) {
                continue;
            }
            printf("[CENSUS]  model%d layer=%d camBit=%04x attr=%08x motAttr=%08x objN=%d motId=%d "
                   "scale=(%.2f,%.2f,%.2f) pos=(%.1f,%.1f,%.1f)\n",
                   ci, cmP->layerNo, (unsigned)cmP->cameraBit, (unsigned)cmP->attr,
                   (unsigned)cmP->motAttr, cmP->hsf->objectNum, cmP->motId,
                   cmP->scale.x, cmP->scale.y, cmP->scale.z, cmP->pos.x, cmP->pos.y, cmP->pos.z);
        }
        fflush(stdout);
    }
}

/* MP6_DRAW_CENSUS_OBJ=<tick>: the per-object companion (hsfdraw.c's tree
 * walk, hsfanim.c's texture-anim picks, sprput.c's ANIM texture binds), over
 * the same two-tick window. */
static long mp6ObjCensusTick = -2; /* -2 = env not parsed yet, -1 = disabled */
int mp6_draw_census_obj_active(void)
{
    if (mp6ObjCensusTick == -2) {
        const char *e = getenv("MP6_DRAW_CENSUS_OBJ");
        mp6ObjCensusTick = (e && atol(e) > 0) ? atol(e) : -1;
    }
    return mp6ObjCensusTick > 0 && mp6_tick_count >= mp6ObjCensusTick &&
           mp6_tick_count < mp6ObjCensusTick + 2;
}
