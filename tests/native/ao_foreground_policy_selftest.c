#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mp6_ao_foreground_policy.h"

int main(void)
{
    HU3D_MODEL model={0}; HSF_OBJECT object={0}; HSF_CONSTDATA constant={0};
    HSF_MATERIAL materials[2]={{0}}; HSF_FACE face={0};
    HU3D_DRAW_OBJ draw={0};
    draw.model=&model; draw.object=&object;
    object.constData=&constant; object.mesh.material=materials;
    face.mat=0x1001; /* High face bits are not part of the material index. */
    HSF_MATERIAL *m=&materials[1];
    assert(!mp6_ao_foreground_eligible(&draw,&face)); /* Opaque */
    m->pass=1; object.name="leaf0"; model.attr=33556480;
    assert(mp6_ao_foreground_eligible(&draw,&face)); /* Live DK tree cp37 */
    object.name="unregistered-event-prop";
    assert(mp6_ao_foreground_eligible(&draw,&face)); /* No asset-name dependency */
    m->pass=0; m->invAlpha=0.5f;
    assert(mp6_ao_foreground_eligible(&draw,&face));
    m->invAlpha=0; constant.attr=HU3D_CONST_ALTBLEND;
    assert(mp6_ao_foreground_eligible(&draw,&face));
    constant.attr=0; m->pass=0x10;
    assert(!mp6_ao_foreground_eligible(&draw,&face)); /* Hilite bits, not XLU */
    m->pass=1;
    const unsigned excludedMaterial[]={HSF_MATERIAL_MATHOOK,HSF_MATERIAL_REFLECTMODEL,
        HSF_MATERIAL_SRCCOL,HSF_MATERIAL_ADDCOL,HSF_MATERIAL_INVCOL,
        HSF_MATERIAL_DISABLE_ZWRITE,HSF_MATERIAL_NEAR};
    for (unsigned i=0;i<sizeof(excludedMaterial)/sizeof(*excludedMaterial);i++) {
        m->flags=excludedMaterial[i]; assert(!mp6_ao_foreground_eligible(&draw,&face));
        m->flags=0; object.flags=excludedMaterial[i]; assert(!mp6_ao_foreground_eligible(&draw,&face));
        object.flags=0;
    }
    const unsigned excludedModel[]={HU3D_ATTR_HOOKFUNC,HU3D_ATTR_REFLECT_MODEL,
        HU3D_ATTR_ZCMP_OFF,HU3D_ATTR_ZWRITE_OFF,HU3D_ATTR_COLOR_NOUPDATE};
    for (unsigned i=0;i<sizeof(excludedModel)/sizeof(*excludedModel);i++) {
        model.attr=excludedModel[i]; assert(!mp6_ao_foreground_eligible(&draw,&face));
    }
    model.attr=0; object.flags=HSF_MATERIAL_NOCULL;
    assert(mp6_ao_foreground_eligible(&draw,&face));
    HSF_DATA hsf={0}; HSF_OBJECT root={0};
    model.hsf=&hsf; hsf.root=&root; root.name="mizu3";
    assert(mp6_ao_water_surface(&draw));
    object.flags=HSF_MATERIAL_DISABLE_ZWRITE|HSF_MATERIAL_ADDCOL;
    assert(mp6_ao_water_surface(&draw));
    root.name="tree"; object.name="suimen2";
    assert(!mp6_ao_water_surface(&draw));
    root.name="mizu3"; model.attr=HU3D_ATTR_HOOKFUNC;
    assert(!mp6_ao_water_surface(&draw));
    puts("AO foreground policy: event props included; cutouts, special blends and hooks excluded");
    return 0;
}
