#include "mp6_rel_state.h"
#define MP6_MGMBATTLE_MAIN_BSS(X) \
    X(lbl_1_bss_678) \
    X(lbl_1_bss_674) \
    X(lbl_1_bss_672) \
    X(lbl_1_bss_670) \
    X(lbl_1_bss_66C) \
    X(lbl_1_bss_66A) \
    X(lbl_1_bss_668)
#define MP6_MGMBATTLE_MAIN_DATA(X) \
    X(lbl_1_data_8D8) \
    X(lbl_1_data_8DC) \
    X(lbl_1_data_8E0) \
    X(lbl_1_data_8E4) \
    X(lbl_1_data_8E8) \
    X(lbl_1_data_8EC) \
    X(lbl_1_data_8FC) \
    X(lbl_1_data_938) \
    X(lbl_1_data_944)
MP6_REL_RESET_DEFINE(mp6_mgmbattle_main_reset,
                     MP6_MGMBATTLE_MAIN_BSS,
                     MP6_MGMBATTLE_MAIN_DATA)
void mp6_mgmbattle_gameplay_reset(void);
void mp6_mgmbattle_preview_reset(void);
void mp6_mgmbattle_stage_reset(void);
void mp6_mgmbattle_window_reset(void);

void mp6_mgmbattle_reset(void)
{
    mp6_mgmbattle_gameplay_reset();
    mp6_mgmbattle_main_reset();
    mp6_mgmbattle_preview_reset();
    mp6_mgmbattle_stage_reset();
    mp6_mgmbattle_window_reset();
}
