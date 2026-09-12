#ifndef MP6_SELFTEST_ASSERT_H
#define MP6_SELFTEST_ASSERT_H

#include <stdio.h>
#include <stdlib.h>

/* Test-only checks, including deliberate negative controls. A failed check
 * must report and exit, not enter the CRT abort/crash-report path and leave an
 * unattended Windows test waiting for a dialog. Never used by game builds.
 * Include after system headers so their assert definitions cannot replace it.
 * Checks remain enabled even if a test accidentally inherits NDEBUG. */
static inline void mp6_selftest_assert_fail(const char *expression, const char *file, int line) {
    fprintf(stderr, "Assertion failed: %s (%s:%d)\n", expression, file, line);
    exit(1);
}

#undef assert
#define assert(expression) ((expression) ? (void)0 : mp6_selftest_assert_fail(#expression, __FILE__, __LINE__))

#endif
