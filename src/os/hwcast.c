/* MP6 native port -- the diagnostic half of include/mp6_hwcast.h.
 *
 * mp6_fctiwz_s32 is a pure inline that reproduces Gekko's `fctiwz`
 * saturation, and hardware saturated SILENTLY -- so this file exists purely
 * so a saturation is not silent HERE. Every saturation means some recovered
 * expression was handed +-inf or NaN, and every known instance traces to
 * Hu3D3Dto2D dividing by a collapsed camera-space z. That is a real upstream
 * defect that the cast fix deliberately does NOT hide: the cast now behaves
 * like the console, and the log says the input was degenerate.
 *
 * Rate limiting is per-process and generous-then-silent: the first
 * MP6_HWCAST_REPORT_MAX distinct-looking events are printed in full, then one
 * summary line, then nothing. A degenerate projection typically repeats every
 * frame for as long as the subject stays on the camera plane (the board's
 * per-player pan loop runs four times a tick), so an unlimited report would
 * bury the log it is supposed to make readable -- exactly the failure mode
 * the [SDK] shim log's MP6_LOG_ONCE policy already avoids.
 *
 * stderr, not stdout: stdout is the game-flow log the ua1 log-diff gate
 * normalizes and compares byte-for-byte, and this is host diagnostics about
 * a defect, not game narration. Nothing in the 600-tick boot baseline can
 * reach a projection pan, so the gate is unaffected either way -- keeping it
 * off stdout means it stays unaffected if that ever changes.
 */
#include "mp6_hwcast.h"

#include <stdio.h>

#define MP6_HWCAST_REPORT_MAX 8

static long g_hwcastSaturations;

void mp6_hwcast_saturation_report(float value, const char *site)
{
    g_hwcastSaturations++;
    if (g_hwcastSaturations <= MP6_HWCAST_REPORT_MAX) {
        /* %g so an infinity prints as "inf"/"-inf" and a quiet NaN as "nan":
         * which of the three it is says WHICH degeneracy happened -- a zero
         * divisor with a nonzero numerator gives an infinity, a zero divisor
         * with a zero numerator (or a NaN camera basis) gives a NaN. */
        fprintf(stderr, "[HWCAST] fctiwz saturation #%ld: %s converted %g "
                        "(hardware would saturate; the input is degenerate)\n",
                g_hwcastSaturations, site != NULL ? site : "<unknown site>",
                (double)value);
        if (g_hwcastSaturations == MP6_HWCAST_REPORT_MAX) {
            fprintf(stderr, "[HWCAST] further saturation reports suppressed\n");
        }
        fflush(stderr);
    }
}

long mp6_hwcast_saturation_count(void)
{
    return g_hwcastSaturations;
}
