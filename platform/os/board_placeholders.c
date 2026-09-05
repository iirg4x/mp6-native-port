/* Native board-seam diagnostics.
 *
 * marioparty6 main at the revision pinned by docs/DECOMP_DEPENDENCY.md
 * implements every board routine that this Port previously supplied as a
 * logged placeholder. Keep the diagnostic ABI available with an empty
 * registry so the live diagnostics UI does not need a build-time special
 * case. If a future integration seam is unavoidable, it must be explicit,
 * logged, and registered here rather than masquerading as recovered code.
 */
#include "mp6_diag_probe.h"

int mp6_diag_seam_count(void)
{
    return 0;
}

int mp6_diag_seam(int index, const char **symbol, const char **result,
                  unsigned long *count)
{
    (void)index;
    (void)symbol;
    (void)result;
    (void)count;
    return 0;
}
