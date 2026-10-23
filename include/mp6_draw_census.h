/* MP6 native port: MP6_DRAW_CENSUS (src/hsf/mp6_draw_census.c), the per-model
 * Hu3DExec draw census. Diagnostic only; no output unless the variable is set. */
#ifndef MP6_DRAW_CENSUS_H
#define MP6_DRAW_CENSUS_H

#include "dolphin/types.h"

/* Nonzero during the two ticks starting at MP6_DRAW_CENSUS=<tick>. */
int mp6_draw_census_active(void);
/* Hu3DExec entry dump: cameras, layerNum[HU3D_LAYER_MAX], live models. */
void mp6_draw_census_exec_entry(const s16 *layerNum);
/* Nonzero during the two ticks starting at MP6_DRAW_CENSUS_OBJ=<tick>. */
int mp6_draw_census_obj_active(void);

#endif
