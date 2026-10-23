#include "mp6_rel_state.h"
#define MP6_BOOT_BOOT_BSS(X) \
    X(OutViewObj) \
    X(BootObjMan) \
    X(BootCameraRot) \
    X(BootCameraCenter) \
    X(BootCameraZoom) \
    X(BootLightId) \
    X(TitleMdlId) \
    X(BootUnusedBss)
#define MP6_BOOT_BOOT_DATA(X)
MP6_REL_RESET_DEFINE(mp6_boot_boot_reset, MP6_BOOT_BOOT_BSS, MP6_BOOT_BOOT_DATA)

void mp6_boot_opening_reset(void);
void mp6_boot_reset(void)
{
    mp6_boot_boot_reset();
    mp6_boot_opening_reset();
}
