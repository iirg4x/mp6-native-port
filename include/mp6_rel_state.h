#ifndef MP6_REL_STATE_H
#define MP6_REL_STATE_H

#include <string.h>

/* A native overlay remains linked between visits, unlike a freshly loaded REL.
 * Invoke the generated reset before its first entry and every subsequent entry,
 * never during play. The owner supplies explicit X-macro lists of its mutable
 * data and BSS; sizeof preserves native pointer widths and array dimensions.
 * Snapshots contain native values, not serialized PPC bytes. This resets
 * storage only: omOvlKill must first stop processes, destroy scene objects,
 * and free HU_MEMNUM_OVL allocations. It is not a resource destructor. */
#define MP6_REL_STATE_DECLARE(name) static unsigned char initial_##name[sizeof(name)];
#define MP6_REL_STATE_CAPTURE(name) memcpy(initial_##name, &(name), sizeof(name));
#define MP6_REL_STATE_RESTORE(name) memcpy(&(name), initial_##name, sizeof(name));
#define MP6_REL_STATE_ZERO(name) memset(&(name), 0, sizeof(name));

#define MP6_REL_RESET_DEFINE(function, bss, data) \
    void function(void) { \
        static int initialized; \
        data(MP6_REL_STATE_DECLARE) \
        if (!initialized) { \
            data(MP6_REL_STATE_CAPTURE) \
            initialized = 1; \
        } else { \
            data(MP6_REL_STATE_RESTORE) \
        } \
        bss(MP6_REL_STATE_ZERO) \
    }

#endif
