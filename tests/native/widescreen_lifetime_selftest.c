#include <assert.h>
#include "lifetime_subject.inc"

HU3D_MODEL *Hu3DData;
static int motionReapplyCalls, motionForgetCalls, tileForgetCalls;
void mp6_widescreen_motion_reapply(void) { ++motionReapplyCalls; }
void mp6_widescreen_motion_forget(int16_t modelId) { (void)modelId; ++motionForgetCalls; }
void mp6_widescreen_tile_forget(int16_t modelId) { (void)modelId; ++tileForgetCalls; }
static int kills;
void Hu3DModelKill(HU3D_MODELID modelId)
{
    ++kills;
    mp6_widescreen_forget_model(modelId);
    Hu3DData[modelId].hsf = NULL;
}

int main(void)
{
    static HU3D_MODEL models[HU3D_MODEL_MAX];
    static HSF_DATA data;
    static HSF_OBJECT object;
    static HuVecF vertex;
    int pass, i;
    Hu3DData = models;
    for (pass = 0; pass < 100; ++pass) {
        /* Deliberately reuse ALL arena addresses, not just the model slot. */
        models[3].hsf = models[4].hsf = &data;
        g_mp6WsModelNum = g_mp6WsRepeatNum = g_mp6WsSelectiveNum = 2;
        g_mp6WsBorderNum = g_mp6WsSplitNum = g_mp6WsRepositionNum = 2;
        g_mp6WsExtendNum = g_mp6WsFloorDupNum = 2;
        for (i = 0; i < 2; ++i) {
            int id = 3 + i;
            g_mp6WsModels[i].modelId = g_mp6WsRepeat[i].modelId = id;
            g_mp6WsSelective[i].modelId = g_mp6WsBorder[i].modelId = id;
            g_mp6WsSplit[i].modelId = g_mp6WsReposition[i].modelId = id;
            g_mp6WsExtend[i].modelId = g_mp6WsFloorDup[i].dupModelId = id;
            g_mp6WsModels[i].hsf = g_mp6WsRepeat[i].hsf = &data;
            g_mp6WsSelective[i].hsf = g_mp6WsBorder[i].hsf = &data;
            g_mp6WsSplit[i].hsf = g_mp6WsReposition[i].hsf = &data;
            g_mp6WsExtend[i].hsf = g_mp6WsFloorDup[i].dupHsf = &data;
            g_mp6WsRepeat[i].ptr = g_mp6WsSelective[i].ptr = &vertex;
            g_mp6WsBorder[i].ptr = g_mp6WsSplit[i].ptr = &vertex;
            g_mp6WsReposition[i].obj = g_mp6WsExtend[i].obj = &object;
            g_mp6WsRepeat[i].native = malloc(12);
            g_mp6WsSelective[i].native = malloc(12);
            g_mp6WsBorder[i].native = malloc(12);
            g_mp6WsSplit[i].native = malloc(12);
        }
        mp6_widescreen_forget_model(3);
        mp6_widescreen_forget_model(3); /* teardown is idempotent */
        models[3].hsf = NULL;
        assert(!mp6_ws_model_live(3, g_mp6WsExtend[0].hsf)); /* NULL != live */
        models[3].hsf = &data; /* reconstructed at identical addresses */
        assert(!mp6_ws_model_live(3, g_mp6WsModels[0].hsf));
        assert(!g_mp6WsRepeat[0].native && !g_mp6WsSelective[0].native);
        assert(!g_mp6WsBorder[0].native && !g_mp6WsSplit[0].native);
        assert(g_mp6WsRepeat[1].native && g_mp6WsSelective[1].native);
        assert(g_mp6WsBorder[1].native && g_mp6WsSplit[1].native);
        assert(mp6_ws_model_alloc() == &g_mp6WsModels[0]);
        assert(mp6_ws_repeat_alloc() == &g_mp6WsRepeat[0]);
        assert(mp6_ws_selective_alloc() == &g_mp6WsSelective[0]);
        assert(mp6_ws_border_alloc() == &g_mp6WsBorder[0]);
        assert(mp6_ws_split_alloc() == &g_mp6WsSplit[0]);
        assert(mp6_ws_reposition_alloc() == &g_mp6WsReposition[0]);
        assert(mp6_ws_extend_alloc() == &g_mp6WsExtend[0]);
        assert(mp6_ws_extend_find(&object) == &g_mp6WsExtend[1]);
        assert(mp6_ws_repeat_find(&vertex) == &g_mp6WsRepeat[1]);
        kills = 0;
        mp6_widescreen_floor_border_fill_dup_kill_all();
        assert(kills == 1 && models[3].hsf == &data && !models[4].hsf);
        assert(!mp6_ws_extend_find(&object) && !mp6_ws_repeat_find(&vertex));
    }
    assert(!mp6_ws_model_live(-1, &data));
    assert(!mp6_ws_model_live(HU3D_MODEL_MAX, &data));
    Hu3DData = NULL;
    assert(!mp6_ws_model_live(3, &data));
    return 0;
}
