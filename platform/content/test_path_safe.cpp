/* Focused unit test for mp6_content_path_is_safe_rel (SECURITY: FST-name
 * traversal gate). Exercises the real validator header directly -- no ISO is
 * forged, and no SDL/nod is linked. Build + run standalone, e.g.:
 *
 *     zig c++ platform/content/test_path_safe.cpp -o build/test_path_safe
 *     build/test_path_safe
 *
 * Mirrors setup/lib/test_path_safe.py so the C++ and Python gates stay in
 * lockstep.
 */
#include "content_path_safe.h"

#include <cstdio>
#include <cstring>

struct Case {
    const char *rel;
    int expected; /* 1 = must be accepted, 0 = must be rejected */
};

static const Case kCases[] = {
    /* traversal / escape attempts: all REJECTED */
    { "data/../../evil", 0 },
    { "/abs/evil", 0 },
    { "..\\win", 0 }, /* backslash + ".." */
    { "..", 0 },
    { ".", 0 },
    { "data/..", 0 },
    { "data/./x", 0 },
    { "data//x", 0 },
    { "data/", 0 },
    { "C:/x", 0 },
    { "C:\\x", 0 },
    { "data\\sub\\evil", 0 },
    { "data/sub/../../../x", 0 },
    { "data/e\nvil", 0 }, /* control byte */
    { "", 0 },
    /* legitimate wanted-set paths: all ACCEPTED */
    { "data/x.bin", 1 },
    { "data/sub/file.bin", 1 },
    { "opening.bnr", 1 },
    { "sound/MP6_SND.msm", 1 },
    { "mess/e/message.bin", 1 },
    { "data/...oddbutlegal", 1 }, /* "..." is a real name */
    { "data/.hidden", 1 },        /* leading dot is legal */
};

int main(void)
{
    int failures = 0;
    const int n = (int)(sizeof(kCases) / sizeof(kCases[0]));
    for (int i = 0; i < n; ++i) {
        int got = mp6_content_path_is_safe_rel(kCases[i].rel) ? 1 : 0;
        if (got != kCases[i].expected) {
            printf("  FAIL: mp6_content_path_is_safe_rel(\"%s\") = %d, expected %d\n",
                   kCases[i].rel, got, kCases[i].expected);
            ++failures;
        }
    }
    /* NUL embedded mid-string cannot occur in a C string, but a leading NUL
     * (empty) is covered above; verify a benign but tricky name too. */
    if (!mp6_content_path_is_safe_rel("data/a.b.c")) {
        printf("  FAIL: \"data/a.b.c\" should be accepted\n");
        ++failures;
    }
    if (failures) {
        printf("[test_path_safe] FAIL: %d case(s)\n", failures);
        return 1;
    }
    printf("[test_path_safe] PASS: %d cases -- traversal/absolute/drive/control "
           "names rejected, wanted-set paths accepted\n", n);
    return 0;
}
