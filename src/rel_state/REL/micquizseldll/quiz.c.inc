#include "mp6_rel_state.h"
#define MP6_MICQUIZSEL_QUIZ_BSS(X)
#define MP6_MICQUIZSEL_QUIZ_DATA(X) \
    X(lbl_1_data_0) \
    X(lbl_1_data_1C) \
    X(lbl_1_data_48) \
    X(lbl_1_data_5C) \
    X(lbl_1_data_A0) \
    X(lbl_1_data_E8) \
    X(lbl_1_data_12C) \
    X(lbl_1_data_17C) \
    X(lbl_1_data_1CC) \
    X(lbl_1_data_1D4) \
    X(lbl_1_data_1F4) \
    X(lbl_1_data_1F8) \
    X(lbl_1_data_1FC) \
    X(lbl_1_data_208) \
    X(lbl_1_data_20A) \
    X(lbl_1_data_20C) \
    X(lbl_1_data_20E) \
    X(lbl_1_data_210) \
    X(lbl_1_data_218) \
    X(lbl_1_data_21A) \
    X(lbl_1_data_23C) \
    X(lbl_1_data_21C)
MP6_REL_RESET_DEFINE(mp6_micquizsel_quiz_reset, MP6_MICQUIZSEL_QUIZ_BSS, MP6_MICQUIZSEL_QUIZ_DATA)

void mp6_micquizsel_quiz_reset(void);
void mp6_micquizsel_reset(void)
{
    mp6_micquizsel_quiz_reset();
}
