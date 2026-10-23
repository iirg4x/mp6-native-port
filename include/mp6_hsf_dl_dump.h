/* MP6 native port: MP6_DL_DUMP diagnostics for HSF display-list builds
 * (src/hsf/mp6_hsf_dl_dump.c). Called by game/hsfdraw.c after an object's
 * display list is built; no output unless MP6_DL_DUMP is set. */
#ifndef MP6_HSF_DL_DUMP_H
#define MP6_HSF_DL_DUMP_H

#include "game/hu3d.h"

void mp6_hsf_dl_dump(HSF_OBJECT *objPtr, HSF_BUFFER *faceBuf, const void *DLBufStartP, s32 totalSize,
                     s32 DLTotalNum, const HSF_DRAWDATA *DrawData, s32 matChgCnt);

#endif
