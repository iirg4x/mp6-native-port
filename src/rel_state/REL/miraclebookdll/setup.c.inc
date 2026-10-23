#include "mp6_rel_state.h"
#define MP6_MIRACLEBOOK_SETUP_BSS(X) \
    X(lbl_1_bss_4) \
    X(lbl_1_bss_0)
#define MP6_MIRACLEBOOK_SETUP_DATA(X)
MP6_REL_RESET_DEFINE(mp6_miraclebook_setup_reset, MP6_MIRACLEBOOK_SETUP_BSS, MP6_MIRACLEBOOK_SETUP_DATA)

void mp6_miraclebook_setup_reset(void);
void mp6_miraclebook_book_reset(void);
void mp6_miraclebook_reset(void)
{
    mp6_miraclebook_setup_reset();
    mp6_miraclebook_book_reset();
}
