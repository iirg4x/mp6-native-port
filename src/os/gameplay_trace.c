/* MP6 native port -- gameplay state trace; see include/mp6_gameplay_trace.h. */
#include "dolphin.h"
#include "game/object.h"
#include "game/hu3d.h"
#include "game/gamework.h"
#include "game/mgdata.h"
#include "mp6_gameplay_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp6_host_section.h" /* diagnostic state of the running process */

extern OMOBJMAN *mp6_om_objman_get(void);
extern u32 mp6_colman_state_hash(u32 hash);
extern s32 rnd_seed;

static int g_traceOn = -1; /* 2: exit when the traced minigame ends */
static int g_traceModelsAt = -1;
static int g_traceOverlay = -1;
static int g_traceTick;

static u32 hash_bytes(u32 hash, const void *data, size_t bytes)
{
    const u8 *p = data;
    while (bytes--) hash = (hash ^ *p++) * 16777619u;
    return hash;
}

static u32 hash_objects(int print)
{
    OMOBJMAN *manager = mp6_om_objman_get();
    u32 hash = 2166136261u;
    if (manager == NULL || manager->property == NULL) return hash;
    OMOBJWORK *work = manager->property;
    for (int i = 0; i < work->objMax; ++i) {
        OMOBJ *object = &work->objData[i];
        if (object->stat & OM_STAT_DELETED) continue;
        hash = hash_bytes(hash, &i, sizeof(i));
        hash = hash_bytes(hash, &object->trans, sizeof(object->trans));
        hash = hash_bytes(hash, &object->rot, sizeof(object->rot));
        hash = hash_bytes(hash, &object->scale, sizeof(object->scale));
        for (int w = 0; w < 4; ++w) {
            /* Objects often keep a work pointer here. Arena addresses (0x8xxxxxxx,
             * like the GameCube's) move with heap layout, not with play. */
            u32 value = object->work[w];
            if ((value & 0xF0000000u) != 0x80000000u) hash = hash_bytes(hash, &value, sizeof(value));
        }
        if (print) {
            printf("[GPLAY-OBJECT] t=%d id=%d trans=%.4f,%.4f,%.4f rot=%.4f,%.4f,%.4f scale=%.4f,%.4f,%.4f work=%x,%x,%x,%x\n",
                   g_traceTick, i, object->trans.x, object->trans.y, object->trans.z,
                   object->rot.x, object->rot.y, object->rot.z,
                   object->scale.x, object->scale.y, object->scale.z,
                   (unsigned)object->work[0], (unsigned)object->work[1],
                   (unsigned)object->work[2], (unsigned)object->work[3]);
        }
    }
    return hash;
}

static u32 hash_models(int print)
{
    u32 hash = 2166136261u;
    for (int i = 0; i < HU3D_MODEL_MAX; ++i) {
        HU3D_MODEL *model = &Hu3DData[i];
        if (model->hsf == NULL) continue;
        hash = hash_bytes(hash, &i, sizeof(i));
        hash = hash_bytes(hash, &model->pos, sizeof(model->pos));
        hash = hash_bytes(hash, &model->rot, sizeof(model->rot));
        hash = hash_bytes(hash, &model->scale, sizeof(model->scale));
        if (print) {
            printf("[GPLAY-MODEL] t=%d id=%d pos=%.4f,%.4f,%.4f rot=%.4f,%.4f,%.4f scale=%.4f,%.4f,%.4f\n",
                   g_traceTick, i, model->pos.x, model->pos.y, model->pos.z,
                   model->rot.x, model->rot.y, model->rot.z,
                   model->scale.x, model->scale.y, model->scale.z);
        }
    }
    return hash;
}

void mp6_gameplay_trace_tick(void)
{
    if (g_traceOn < 0) {
        const char *on = getenv("MP6_GAMEPLAY_TRACE");
        const char *at = getenv("MP6_GAMEPLAY_TRACE_MODELS");
        g_traceOn = on && on[0] && on[0] != '0' ? (on[0] == '2' ? 2 : 1) : 0;
        g_traceModelsAt = at && at[0] ? atoi(at) : -1;
    }
    if (!g_traceOn) return;
    if (omcurovl < 0 || MgNoGet(omcurovl) < 0) {
        if (g_traceOverlay >= 0) {
            printf("[GPLAY-END] t=%d\n", g_traceTick);
            fflush(stdout);
            if (g_traceOn == 2) exit(0);
        }
        g_traceOverlay = -1;
        return;
    }
    if (g_traceOverlay != omcurovl) {
        g_traceOverlay = omcurovl;
        g_traceTick = 0;
    }
    printf("[GPLAY] t=%d r=%08x,%08x c=%08x o=%08x m=%08x p=%08x\n", g_traceTick,
           (unsigned)mp6_frand_seed_get(), (unsigned)rnd_seed,
           (unsigned)mp6_colman_state_hash(2166136261u), (unsigned)hash_objects(g_traceTick == g_traceModelsAt),
           (unsigned)hash_models(g_traceTick == g_traceModelsAt),
           (unsigned)hash_bytes(2166136261u, GwPlayer, sizeof(GwPlayer)));
    ++g_traceTick;
}
