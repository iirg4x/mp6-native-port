/* MP6 native port -- E. Gadd's Garage: the conveyors' path strip is solid.
 *
 * The white path strip along Garage's conveyor belts is the board model's
 * "ami1" material: an I4 strip texture whose material is 40% transparent
 * (invAlpha 0.4, opaque pass) in the game data, and nothing animates it. The
 * belts under it are separate "conhook"/"conv" models whose texture-scroll
 * animation runs when a ? space starts them. Drawn as the data says, the
 * strip shows the slats through it in uneven blotches; the project owner's
 * reference from the original shows it solid, and he chose to match that.
 *
 * Called once from w02dll's conveyor setup (after the board models exist):
 * every loaded model's "ami1" material at exactly that transparency is made
 * opaque. The belts' own animation is untouched, and no other material or
 * board is.
 */
#include "game/hu3d.h"
#include "game/hsfformat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp6_host_section.h"

void mp6_garage_conveyor_strip_solid(void)
{
    int i, changed = 0;
    for (i = 0; i < HU3D_MODEL_MAX; i++) {
        HSF_DATA *hsf = Hu3DData[i].hsf;
        s32 m;
        /* A hook model's hsf slot holds its hook function (a union). */
        if (hsf == NULL || (Hu3DData[i].attr & HU3D_ATTR_HOOKFUNC) || hsf->material == NULL) continue;
        for (m = 0; m < hsf->materialNum; m++) {
            HSF_MATERIAL *mat = &hsf->material[m];
            if (mat->name != NULL && strcmp(mat->name, "ami1") == 0 && fabsf(mat->invAlpha - 0.4f) < 0.01f &&
                (mat->pass & 0xF) == 0) {
                mat->invAlpha = 0.0f;
                changed++;
            }
        }
    }
    if (getenv("MP6_HSF_MAT_TRACE") != NULL) {
        printf("[HSF-MAT] Garage conveyor strip: %d material(s) made opaque\n", changed);
    }
}
