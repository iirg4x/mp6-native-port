#include "mp6_rel_state.h"
#define MP6_M620_TEXT_64A8_BSS(X)
#define MP6_M620_TEXT_64A8_DATA(X) \
    X(lbl_1_data_1F0) \
    X(lbl_1_data_1FC) \
    X(lbl_1_data_238) \
    X(lbl_1_data_280) \
    X(lbl_1_data_4E8) \
    X(lbl_1_data_500) \
    X(lbl_1_data_504) \
    X(lbl_1_data_51C) \
    X(lbl_1_data_52C) \
    X(lbl_1_data_55C) \
    X(lbl_1_data_58C) \
    X(lbl_1_data_5BC) \
    X(lbl_1_data_5EC) \
    X(lbl_1_data_61C) \
    X(lbl_1_data_64C) \
    X(lbl_1_data_688) \
    X(lbl_1_data_6A0)
MP6_REL_RESET_DEFINE(mp6_m620_text_64A8_reset, MP6_M620_TEXT_64A8_BSS, MP6_M620_TEXT_64A8_DATA)
