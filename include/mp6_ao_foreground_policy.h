#ifndef MP6_AO_FOREGROUND_POLICY_H
#define MP6_AO_FOREGROUND_POLICY_H

#include "game/hu3d.h"
#include <string.h>

/* Towering Treetop's day/night water asset. Its three animated meshes write
 * depth, unlike ordinary transparent foliage. Identify the authored model,
 * not generic names such as "test" or every depth-writing cutout. */
static inline int mp6_ao_water_surface(const HU3D_DRAW_OBJ *draw)
{
    const HSF_DATA *hsf=draw->model->hsf;
    return hsf && hsf->root && hsf->root->name &&
           !strcmp(hsf->root->name,"mizu3") &&
           !(draw->model->attr & (HU3D_ATTR_HOOKFUNC|HU3D_ATTR_COLOR_NOUPDATE));
}

/* Match FaceDraw's ordinary alpha-blended, depth-tested/non-writing branch.
 * Such a surface contributes color, but the scene depth belongs to something
 * behind it. Applying that receiver's AO to this color produces foreground
 * leaks. This is a draw-state property, not a board/model/texture allowlist.
 * Despite their names, ZWRITE_OFF / DISABLE_ZWRITE force the depth-writing
 * cutout branch in FaceDraw; keep that existing behavior unchanged. */
static inline int mp6_ao_foreground_eligible(const HU3D_DRAW_OBJ *draw,
                                           const HSF_FACE *face)
{
    const HSF_OBJECT *o=draw->object;
    const HSF_MATERIAL *m=o->mesh.material+(face->mat&0xFFF);
    const HSF_CONSTDATA *c=o->constData;
    const unsigned flags=o->flags|m->flags;
    if (flags&(HSF_MATERIAL_MATHOOK|HSF_MATERIAL_REFLECTMODEL|HSF_MATERIAL_SRCCOL|
               HSF_MATERIAL_ADDCOL|HSF_MATERIAL_INVCOL|HSF_MATERIAL_DISABLE_ZWRITE|HSF_MATERIAL_NEAR) ||
        draw->model->attr&(HU3D_ATTR_HOOKFUNC|HU3D_ATTR_REFLECT_MODEL|HU3D_ATTR_ZCMP_OFF|
                          HU3D_ATTR_ZWRITE_OFF|HU3D_ATTR_COLOR_NOUPDATE)) return 0;
    return m->invAlpha!=0 || (m->pass&0xF) || (c->attr&HU3D_CONST_ALTBLEND);
}

#endif
