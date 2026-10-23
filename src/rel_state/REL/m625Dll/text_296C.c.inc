#include "mp6_rel_state.h"
#define MP6_M625_TEXT_296C_BSS(X) \
    X(lbl_1_bss_1D8) \
    X(lbl_1_bss_1D4) \
    X(lbl_1_bss_1D0) \
    X(lbl_1_bss_1CC) \
    X(lbl_1_bss_1C8)
#define MP6_M625_TEXT_296C_DATA(X) \
    X(lbl_1_data_1E8) \
    X(lbl_1_data_218) \
    X(lbl_1_data_248) \
    X(lbl_1_data_278) \
    X(lbl_1_data_2B8) \
    X(lbl_1_data_36C) \
    X(lbl_1_data_3E4)
MP6_REL_RESET_DEFINE(mp6_m625_text_296C_reset, MP6_M625_TEXT_296C_BSS, MP6_M625_TEXT_296C_DATA)
