/* Tiled 2D bands keep their authored centre and tile density in a wide view. */
#include "game/disp.h"
#include "game/esprite.h"
#include "game/sprite.h"
#include "mp6_widescreen.h"
#include <math.h>
#include <string.h>

int16_t mp6_widescreen_sprite_band(int16_t group, int nativeCount, int tileMember,
                                 float tileWidth, float left, float right)
{
    HUSPR_GROUP *old, *next;
    HUSPRITE tile;
    HUSPR_GROUPID replacement;
    HUSPRID *retiredMembers;
    int pairs, count, i;
    float margin = mp6_widescreen_half_width_delta();
    if (group < 0 || group >= HUSPR_GROUP_MAX || nativeCount < 1 || nativeCount >= HUSPR_MAX ||
        tileMember < 0 || tileMember >= nativeCount || !isfinite(tileWidth) ||
        tileWidth <= 0 || !isfinite(margin) || !isfinite(left) || !isfinite(right)) return group;
    old = &HuSprGrpData[group];
    if (old->sprNum < nativeCount || old->sprId == NULL) return group;
    if (margin < 0) margin = 0;
    /* Respect the real sprite pool. An impossible request leaves the live
     * group intact, just like allocation failure below. */
    if (margin / tileWidth > (HUSPR_MAX - nativeCount) / 2) return group;
    pairs = (int)ceilf(margin / tileWidth);
    count = nativeCount + pairs * 2;
    if (old->sprNum == count) return group;
    i = old->sprId[tileMember];
    if (i <= 0 || i >= HUSPR_MAX) return group;
    tile = HuSprData[i];
    if (!tile.data || tile.bg || tile.data3D || tile.hook3D ||
        (tile.attr & (HUSPR_ATTR_FUNC | HUSPR_ATTR_3D))) return group;
    replacement = HuSprGrpCreate((s16)count);
    if (replacement == HUSPR_GROUP_NONE) return group;
    next = &HuSprGrpData[replacement];
    /* Allocate all new sprites before transferring originals. Rollback can
     * then destroy the replacement without touching the player's HUD. */
    for (i = 0; i < pairs * 2; ++i) {
        HUSPRID sprite = HuSprCreate(tile.data, tile.prio, tile.bank);
        if (sprite == HUSPR_NONE) {
            HuSprGrpKill(replacement);
            return group;
        }
        HuSprData[sprite] = tile;
        HuSprData[sprite].groupMtx = NULL;
        HuSprData[sprite].pos.x = (i & 1) ? right + tileWidth * (i / 2 + 1)
                                               : left - tileWidth * (i / 2 + 1);
        HuSprGrpMemberSet(replacement, (s16)(nativeCount + i), sprite);
    }
    for (i = 0; i < nativeCount; ++i) {
        next->sprId[i] = old->sprId[i];
        old->sprId[i] = HUSPR_NONE;
    }
    /* Equal-priority sprites draw in group order. Replacing the group itself
     * moves the entire native band in front of later portrait groups (M656),
     * even though every sprite priority is unchanged. Commit only the grown
     * member list: retain the original group ID, transform and matrix address.
     * The temporary group takes ownership of the retired extra tiles/list. */
    retiredMembers = old->sprId;
    old->sprId = next->sprId;
    next->sprId = retiredMembers;
    next->sprNum = old->sprNum;
    old->sprNum = (s16)count;
    HuSprGrpKill(replacement);
    return group;
}

#define SCROLL_FIELDS 2
#define SCROLL_REPEATS 2
/* Companions draw only where a margin shows them; 21:9 needs about 90 for
 * resultDll's 80-unit cells, out of a 400-sprite pool. */
#define SCROLL_POOL 128

typedef struct {
    const int32_t *ids;
    int count;
    float span;
    HUSPR_GROUPID group;
} ScrollField;

static ScrollField scrollFields[SCROLL_FIELDS];

static ScrollField *scroll_find(const int32_t *espIds)
{
    for (int i = 0; i < SCROLL_FIELDS; ++i)
        if (espIds && scrollFields[i].ids == espIds) return &scrollFields[i];
    return NULL;
}

/* esprite slots are members of one group with member index == slot. */
static HUSPRID scroll_native(const ScrollField *field, int index)
{
    HUSPR_GROUP *esp = &HuSprGrpData[espGrpIDGet()];
    int32_t slot = field->ids[index];
    if (slot < 0 || slot >= esp->sprNum || !esp->sprId) return HUSPR_NONE;
    return esp->sprId[slot];
}

/* Horizontal reach of the sprite's current pattern from its position. */
static float scroll_reach(const HUSPRITE *sprite)
{
    const ANIMDATA *anim = sprite->data;
    if (!anim || !anim->pat || anim->patNum < 1) return 0;
    const ANIMPAT *pat = &anim->pat[0];
    return fmaxf(pat->centerX, pat->sizeX - pat->centerX) * fabsf(sprite->scale.x);
}

void mp6_widescreen_esp_scroll_begin(const int32_t *espIds, int count, float span)
{
    /* A field left behind by an overlay that never reached its teardown no
     * longer owns its group: drop it without killing anything. */
    ScrollField *field = scroll_find(espIds);
    for (int i = 0; !field && i < SCROLL_FIELDS; ++i)
        if (!scrollFields[i].ids) field = &scrollFields[i];
    if (!field || count < 1 || !isfinite(span) || span <= 0) return;
    *field = (ScrollField){espIds, count, span, HUSPR_GROUP_NONE};
    mp6_widescreen_esp_scroll_update(espIds);
}

void mp6_widescreen_esp_scroll_update(const int32_t *espIds)
{
    ScrollField *field = scroll_find(espIds);
    float margin = mp6_widescreen_half_width_delta();
    int used = 0;
    if (!field) return;
    if (!isfinite(margin) || margin < 0) margin = 0;
    if (margin <= 0 && field->group == HUSPR_GROUP_NONE) return;
    if (field->group == HUSPR_GROUP_NONE) {
        field->group = HuSprGrpCreate(SCROLL_POOL);
        if (field->group == HUSPR_GROUP_NONE) {
            field->ids = NULL;
            return;
        }
    }
    HUSPR_GROUP *group = &HuSprGrpData[field->group];
    for (int i = 0; margin > 0 && i < field->count; ++i) {
        HUSPRID source = scroll_native(field, i);
        if (source <= 0 || source >= HUSPR_MAX) continue;
        const HUSPRITE *native = &HuSprData[source];
        if (!native->data || native->bg || native->data3D || native->hook3D ||
            (native->attr & (HUSPR_ATTR_FUNC | HUSPR_ATTR_3D))) continue;
        float reach = scroll_reach(native);
        for (int copy = 0; copy < 2 * SCROLL_REPEATS && used < SCROLL_POOL; ++copy) {
            float x = native->pos.x + ((copy & 1) ? 1.0f : -1.0f) * (copy / 2 + 1) * field->span;
            if (x + reach <= -margin || x - reach >= HU_DISP_WIDTH + margin) continue;
            HUSPRID sprite = group->sprId[used];
            if (sprite == HUSPR_NONE) {
                /* Each companion holds its own reference to the shared
                 * animation, so teardown order against the field is free. */
                sprite = HuSprCreate(native->data, native->prio, native->bank);
                if (sprite == HUSPR_NONE) break;
                HuSprGrpMemberSet(field->group, (s16)used, sprite);
            }
            ANIMDATA *data = HuSprData[sprite].data;
            HuSprData[sprite] = *native;
            HuSprData[sprite].data = data;
            HuSprData[sprite].groupMtx = NULL;
            HuSprData[sprite].pos.x = x;
            ++used;
        }
    }
    for (int k = used; k < SCROLL_POOL; ++k)
        if (group->sprId[k] != HUSPR_NONE) HuSprData[group->sprId[k]].attr |= HUSPR_ATTR_DISPOFF;
}

void mp6_widescreen_esp_scroll_forget(const int32_t *espIds)
{
    ScrollField *field = scroll_find(espIds);
    if (!field) return;
    if (field->group != HUSPR_GROUP_NONE) HuSprGrpKill(field->group);
    memset(field, 0, sizeof(*field));
}
