#include "mp6_rel_state.h"
#define MP6_FILESEL_SAVELOAD_BSS(X)
#define MP6_FILESEL_SAVELOAD_DATA(X) \
    X(lbl_1_data_568)
MP6_REL_RESET_DEFINE(mp6_filesel_saveload_reset, MP6_FILESEL_SAVELOAD_BSS, MP6_FILESEL_SAVELOAD_DATA)
