#ifndef MP6_ANIM_NATIVE_H
#define MP6_ANIM_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#include "game/animdata.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a packed ANM from a verified game-heap allocation. */
ANIMDATA *mp6_anim_read(void *data);

/* Explicit-size seam for immutable packed arrays compiled into the game.
 * This is intentionally separate from mp6_anim_read: an unowned pointer
 * with no size is never parsed speculatively. */
ANIMDATA *mp6_anim_read_sized(void *data, size_t size);

/* Register an ANIMDATA graph that is already native (HuSprAnimMake). This
 * registration is mandatory: a later HuSprAnimRead(native) must never
 * reinterpret host pointers as packed big-endian offsets. */
void mp6_anim_register_native(ANIMDATA *anim);

/* Remove all cache identities for `anim` and return its distinct owned
 * packed backing allocation, if any. The caller frees that backing after
 * it has finished reading native bitmap metadata and before freeing anim. */
void *mp6_anim_unregister_for_free(ANIMDATA *anim);

/* Called by the central HuMemDirectFreeNum seam before a tagged bulk free.
 * It invalidates exactly the records whose key/value allocations carry the
 * reclaimed heap/tag; persistent static-array records remain registered. */
void mp6_anim_before_bulk_free(int heap, uint32_t tag);

/* Guard the ordinary direct-free seam as well. Proper sprite destruction
 * unregisters first; a match here means a caller is about to orphan either
 * the native graph or its packed backing allocation. */
void mp6_anim_before_direct_free(const void *ptr);

/* Test/diagnostic census of live cache records. */
size_t mp6_anim_cache_live_count(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_ANIM_NATIVE_H */
