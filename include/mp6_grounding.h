#ifndef MP6_GROUNDING_H
#define MP6_GROUNDING_H
#include "game/hu3d.h"

/* Port-only, AO-gated STATIC scenery corrections. Actors, guides, board-space
 * graphics and effects retain their original shared coordinate system.
 * No gameplay positions or asset buffers are modified. */
void mp6_ground_w01_begin(HU3D_MODELID terrain);
void mp6_ground_w01_scenery(HU3D_MODELID model);
void mp6_ground_w01_tree(HU3D_MODELID model);
void mp6_ground_end(void);
void mp6_ground_object(HU3D_MODEL *model, HSF_OBJECT *object, Mtx view, int descendants);
/* Draw-only log/tree base fitting and background/path/link lift to the authored start floor;
 * returns original storage when disabled. Path/actor matrices, log/tree tops
 * and asset vertex buffers stay intact. */
HuVecF *mp6_ground_vertices(HU3D_MODEL *model, HSF_OBJECT *object);
int mp6_ground_is_scenery(HU3D_MODEL *model);
#endif
