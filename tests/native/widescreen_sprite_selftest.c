#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "game/sprite.h"
#include "mp6_widescreen.h"

static HUSPRITE sprites[HUSPR_MAX];
HUSPRITE *HuSprData = sprites;
HUSPR_GROUP HuSprGrpData[HUSPR_GROUP_MAX];
static ANIMDATA anim;
static float margin;
static int refs, allocations, fail_after = -1, fail_group;
float mp6_widescreen_half_width_delta(void) { return margin; }

HUSPRID HuSprCreate(ANIMDATA *data, s16 prio, s16 bank)
{
    if (fail_after == 0) return HUSPR_NONE;
    if (fail_after > 0) --fail_after;
    for (int i = 1; i < HUSPR_MAX; ++i) if (!sprites[i].data) {
        memset(&sprites[i], 0, sizeof(sprites[i]));
        sprites[i].data = data; sprites[i].prio = prio; sprites[i].bank = bank;
        sprites[i].scale.x = sprites[i].scale.y = 1;
        ++refs; ++allocations;
        return i;
    }
    return HUSPR_NONE;
}
HUSPR_GROUPID HuSprGrpCreate(s16 count)
{
    if (fail_group) return HUSPR_GROUP_NONE;
    for (int i = 0; i < HUSPR_GROUP_MAX; ++i) if (!HuSprGrpData[i].sprNum) {
        HUSPR_GROUP *g = &HuSprGrpData[i];
        memset(g, 0, sizeof(*g));
        g->sprNum = count; g->sprId = malloc(count * sizeof(*g->sprId));
        assert(g->sprId);
        for (int j = 0; j < count; ++j) g->sprId[j] = HUSPR_NONE;
        g->scale.x = g->scale.y = 1;
        ++allocations;
        return i;
    }
    return HUSPR_GROUP_NONE;
}
void HuSprGrpMemberSet(HUSPR_GROUPID group, s16 member, HUSPRID id)
{
    assert(HuSprGrpData[group].sprId[member] == HUSPR_NONE);
    HuSprGrpData[group].sprId[member] = id;
}
void HuSprGrpKill(HUSPR_GROUPID group)
{
    HUSPR_GROUP *g = &HuSprGrpData[group];
    for (int i = 0; i < g->sprNum; ++i) if (g->sprId[i] != HUSPR_NONE) {
        HUSPRITE *sp = &sprites[g->sprId[i]];
        assert(sp->data); sp->data = NULL; --refs;
    }
    free(g->sprId); g->sprId = NULL; g->sprNum = 0;
}
static HUSPR_GROUPID espGroup;
HUSPR_GROUPID espGrpIDGet(void) { return espGroup; }
static s16 extend(s16 group) { return mp6_widescreen_sprite_band(group, 11, 0, 64, 0, 608); }

int main(void)
{
    HUSPRITE originals[11];
    s16 ids[11], group = HuSprGrpCreate(11);
    const s16 nativeGroup = group;
    for (int i = 0; i < 11; ++i) {
        ids[i] = HuSprCreate(&anim, 12, 2);
        sprites[ids[i]].pos.x = i == 10 ? 496 : i * 64 + (i >= 8 ? 32 : 0);
        sprites[ids[i]].time = i + 0.5f;
        sprites[ids[i]].work[1] = i;
        originals[i] = sprites[ids[i]];
        HuSprGrpMemberSet(group, i, ids[i]);
    }
    HuSprGrpData[group].pos.y = 240;
    HuSprGrpData[group].work[2] = 17;
    assert(extend(group) == group && refs == 11);
    /* Native M656 creates the strip before the equal-priority portraits.
     * Their group order is the tie-breaker: growing/shrinking must not move
     * the strip past either portrait or invalidate a retained matrix pointer. */
    s16 markerGroups[2];
    for (int i=0; i<2; ++i) {
        markerGroups[i]=HuSprGrpCreate(1);
        HuSprGrpMemberSet(markerGroups[i],0,HuSprCreate(&anim,12,0));
        assert(markerGroups[i] > nativeGroup);
    }
    Mtx *matrix = &HuSprGrpData[group].mtx;
    for (int step = 0; step < 102; ++step) {
        margin = (float[]){104, 240, 104, 0, 240, 0}[step%6];
        group = extend(group);
        assert(group == nativeGroup && &HuSprGrpData[group].mtx == matrix);
        assert(group < markerGroups[0] && group < markerGroups[1]);
        HUSPR_GROUP *g = &HuSprGrpData[group];
        int extra = 2 * (int)ceilf(margin / 64);
        assert(g->sprNum == 11 + extra && refs == 13 + extra);
        assert(g->pos.y == 240 && g->work[2] == 17);
        for (int i = 0; i < 11; ++i) {
            assert(g->sprId[i] == ids[i]);
            assert(memcmp(&sprites[ids[i]], &originals[i], sizeof(HUSPRITE)) == 0);
        }
        for (int i = 0; i < extra; ++i) {
            HUSPRITE *sp = &sprites[g->sprId[11+i]];
            assert(sp->pos.x == ((i & 1) ? 608 + 64 * (i / 2 + 1) : -64 * (i / 2 + 1)));
            assert(sp->scale.x == 1 && sp->scale.y == 1 && sp->bank == 2 && sp->prio == 12);
        }
        int before = allocations;
        for (int i = 0; i < 500; ++i) assert(extend(group) == group);
        assert(before == allocations);
    }
    margin = 240;
    fail_after = 3;
    assert(extend(group) == group && refs == 13 && HuSprGrpData[group].sprNum == 11);
    fail_after = -1; fail_group = 1;
    assert(extend(group) == group && refs == 13);
    fail_group = 0;
    assert(mp6_widescreen_sprite_band(group, 11, 0, 0, 0, 608) == group);
    margin = INFINITY; assert(extend(group) == group);
    margin = 100000; assert(extend(group) == group);
    margin = 240;
    sprites[ids[0]].attr |= HUSPR_ATTR_FUNC;
    assert(extend(group) == group);
    sprites[ids[0]].attr = HUSPR_ATTR_DISPOFF;
    group = extend(group);
    for (int i = 11; i < HuSprGrpData[group].sprNum; ++i)
        assert(sprites[HuSprGrpData[group].sprId[i]].attr & HUSPR_ATTR_DISPOFF);
    HuSprGrpKill(group);
    for (int i=0; i<2; ++i) HuSprGrpKill(markerGroups[i]);
    assert(refs == 0 && extend(group) == group);

    /* resultDll's scrolling field: copies of the whole 697-unit wrap span
     * appear only where a margin shows them, track the native pose every
     * tick, and are released before the field's own sprites. */
    static ANIMPAT pat = {1, 40, 40, 80, 80, NULL};
    anim.pat = &pat; anim.patNum = 1;
    espGroup = HuSprGrpCreate(3);
    int32_t field[3];
    const float startX[3] = {-80, 300, 616};
    for (int i = 0; i < 3; ++i) {
        HuSprGrpMemberSet(espGroup, (s16)i, HuSprCreate(&anim, 256, 0));
        field[i] = i;
        sprites[HuSprGrpData[espGroup].sprId[i]].pos.x = startX[i];
        sprites[HuSprGrpData[espGroup].sprId[i]].drawNo = 64;
    }
    margin = 0;
    mp6_widescreen_esp_scroll_begin(field, 3, 697);
    assert(refs == 3);
    margin = 240;
    for (int tick = 0; tick < 3; ++tick) {
        for (int i = 0; i < 3; ++i) sprites[HuSprGrpData[espGroup].sprId[i]].pos.x = startX[i] + tick;
        mp6_widescreen_esp_scroll_update(field);
        int shown = 0;
        float xs[2];
        for (int i = 0; i < HUSPR_MAX; ++i) {
            HUSPRITE *sp = &sprites[i];
            if (!sp->data || sp->prio != 256 || (sp->attr & HUSPR_ATTR_DISPOFF)) continue;
            int native = 0;
            for (int j = 0; j < 3; ++j) native |= HuSprGrpData[espGroup].sprId[j] == i;
            if (native) continue;
            assert(shown < 2 && sp->drawNo == 64 && sp->data == &anim);
            xs[shown++] = sp->pos.x;
        }
        assert(shown == 2 && refs == 5);
        assert((xs[0] == 617 + tick && xs[1] == -81 + tick) || (xs[1] == 617 + tick && xs[0] == -81 + tick));
    }
    margin = 0;
    mp6_widescreen_esp_scroll_update(field);
    for (int i = 0; i < HUSPR_MAX; ++i)
        if (sprites[i].data && sprites[i].prio == 256 && i > HuSprGrpData[espGroup].sprId[2])
            assert(sprites[i].attr & HUSPR_ATTR_DISPOFF);
    mp6_widescreen_esp_scroll_forget(field);
    assert(refs == 3);
    mp6_widescreen_esp_scroll_update(field);
    assert(refs == 3);
    HuSprGrpKill(espGroup);
    assert(refs == 0);
    return 0;
}
