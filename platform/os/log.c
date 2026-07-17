/* MP6 native port -- shim/boot logging.
 *
 * Single chokepoint for the "[SDK] name(...)" trace. Kept independent of
 * stdio buffering surprises by flushing after every line, since the whole
 * point is to watch boot progress live.
 */
#include <stdio.h>
#include "mp6_shim_log.h"

void mp6_shim_log(const char *family, const char *name)
{
    fprintf(stdout, "[SDK] %s.%s(...)\n", family, name);
    fflush(stdout);
}

void mp6_boot_log(const char *msg)
{
    fprintf(stdout, "[BOOT] %s\n", msg);
    fflush(stdout);
}
