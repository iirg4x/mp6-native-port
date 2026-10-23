#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "game/gamework.h"

typedef struct ANIMDATA_s {
    int id;
} ANIMDATA;

typedef struct MdsingModelFixture_s {
    int animationId[4];
} MDSING_MODEL;

typedef struct MdsingSettingsFixture_s {
    s16 settingsRow;
    s16 boardNo;
    s16 difficulty;
    s16 minigamePack;
} MDSING_SETTINGS;

typedef struct OMOBJ_s {
    s16 work[1];
    void (*objFunc)(struct OMOBJ_s *);
    s16 mtnId[1];
} OMOBJ;

_Static_assert(sizeof(GW_COMMON) == 0x5D0, "fixture must use the real GW_COMMON layout");

GW_COMMON GwCommon;
MDSING_SETTINGS lbl_1_bss_1340;
MDSING_MODEL lbl_1_bss_E74[16];
ANIMDATA *lbl_1_bss_11F4[25];
Vec lbl_1_data_7A8[27];
OMOBJ *lbl_1_bss_24;

static ANIMDATA texture_storage[25];
static int animation_calls;
static int animation_models[8];
static ANIMDATA *animation_values[8];
static int display_calls;

void Hu3DAnimAnimSet(int model, ANIMDATA *animation)
{
    assert(animation_calls < 8);
    animation_models[animation_calls] = model;
    animation_values[animation_calls] = animation;
    ++animation_calls;
}

static void update_entry_display(s16 entryNo, s16 frame, s16 duration,
                                 Vec *target, float scale)
{
    assert((entryNo == 14 || entryNo == 15) && frame >= 0 && frame <= 11);
    assert(duration == 10 && scale == 1.5f);
    assert(target == &lbl_1_data_7A8[entryNo + 9]);
    ++display_calls;
}

/* MDSING_NATIVE_SETTINGS_SUBJECT */

static void reset_card_fixture(void)
{
    int i;

    animation_calls = 0;
    display_calls = 0;
    memset(&lbl_1_bss_1340, 0, sizeof(lbl_1_bss_1340));
    for (i = 0; i < 25; ++i) {
        texture_storage[i].id = i;
        lbl_1_bss_11F4[i] = &texture_storage[i];
    }
    for (i = 0; i < 4; ++i) {
        lbl_1_bss_E74[14].animationId[i] = 140 + i;
        lbl_1_bss_E74[15].animationId[i] = 150 + i;
    }
}

static void poison_unrelated_config_bits(void)
{
    /* These fields share the target bytes with the two settings bitfields. */
    GwCommon.confTurnNum = 0x7F;
    GwCommon.confBonusStar = 1;
    GwCommon.confTag = 1;
    GwCommon.storyMgInstDispF = 1;
    GwCommon.storyMgComDispF = 1;
    GwCommon.storyMessSpeed = 3;
    GwCommon.storySaveMode = 3;
}

static void test_all_valid_setting_pairs(void)
{
    int difficulty;
    int pack;
    OMOBJ object;

    for (difficulty = 0; difficulty < 4; ++difficulty) {
        for (pack = 0; pack < 5; ++pack) {
            reset_card_fixture();
            memset(&GwCommon, 0, sizeof(GwCommon));
            GwCommon.confSingleDiff = difficulty;
            GwCommon.storyMgPack = pack;
            poison_unrelated_config_bits();
            object.work[0] = 0;
            object.objFunc = NULL;
            object.mtnId[0] = 0;
            lbl_1_bss_24 = &object;

            fn_1_15280(&object);

            assert(lbl_1_bss_1340.settingsRow == 0);
            assert(lbl_1_bss_1340.difficulty == difficulty);
            assert(lbl_1_bss_1340.minigamePack == pack);
            assert(animation_calls == 8);
            for (int surface = 0; surface < 4; ++surface) {
                assert(animation_models[surface] == 140 + surface);
                assert(animation_values[surface] == &texture_storage[difficulty + 15]);
                assert(animation_models[4 + surface] == 150 + surface);
                assert(animation_values[4 + surface] == &texture_storage[pack + 19]);
            }
            assert(display_calls == 2);
            assert(object.work[0] == 1);
        }
    }
}

static void test_real_callback_completion(void)
{
    OMOBJ object;

    reset_card_fixture();
    memset(&GwCommon, 0, sizeof(GwCommon));
    GwCommon.confSingleDiff = 2;
    GwCommon.storyMgPack = 3;
    poison_unrelated_config_bits();
    object.work[0] = 0;
    object.objFunc = fn_1_15280;
    object.mtnId[0] = 0;
    lbl_1_bss_24 = &object;

    for (int frame = 0; frame < 12; ++frame) {
        fn_1_15280(&object);
    }

    assert(object.work[0] == 12);
    assert(object.objFunc == NULL);
    assert(object.mtnId[0] == 1);
    assert(display_calls == 24);
}

int main(void)
{
    test_all_valid_setting_pairs();
    test_real_callback_completion();
    return 0;
}
