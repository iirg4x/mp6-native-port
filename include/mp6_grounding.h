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
/* Exact registered W01 ground artwork: color receives the underlying lawn's
 * AO, but the tiny anti-z-fighting offset must not become an AO occluder. */
int mp6_ground_is_decal(HU3D_MODEL *model, HSF_OBJECT *object);
/* Exact W01 terrain sky meshes are protected from background AO. The
 * predicate is AO-gated and fail-closed on model/HSF/object lifetime changes. */
int mp6_ground_is_background(HU3D_MODEL *model, HSF_OBJECT *object);
/* Exact normal-W01 tree body identities remain depth occluders while the AO
 * foreground replay restores their unshaded RGB, removing authored concave
 * crease lines without making other tree parts or 2D artwork unshaded. */
int mp6_ground_is_ao_unshaded(HU3D_MODEL *model, HSF_OBJECT *object);
#endif
