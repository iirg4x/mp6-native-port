#include "mp6_rel_state.h"
#define MP6_FILESEL_FILENAME_BSS(X) \
    X(lbl_1_bss_42E) \
    X(nameKeyboardNum) \
    X(lbl_1_bss_42A) \
    X(nameDefault) \
    X(lbl_1_bss_3AC) \
    X(nameMesTemp) \
    X(nameCharNo) \
    X(nameMes) \
    X(keyboard)
#define MP6_FILESEL_FILENAME_DATA(X) \
    X(keyboardMain) \
    X(keyboardNumSpc) \
    X(keyboardTbl) \
    X(nameSpriteConfig)
MP6_REL_RESET_DEFINE(mp6_filesel_filename_reset, MP6_FILESEL_FILENAME_BSS, MP6_FILESEL_FILENAME_DATA)

void mp6_filesel_filesel_reset(void);
void mp6_filesel_saveload_reset(void);
void mp6_filesel_reset(void)
{
    mp6_filesel_filename_reset();
    mp6_filesel_filesel_reset();
    mp6_filesel_saveload_reset();
}
