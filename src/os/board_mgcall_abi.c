/* Caller-typed entry points for two board/mgcall.c exports.
 *
 * The recovered mgcall.c defines `BOOL mbMgCallSingleOnCheck(void)` and
 * `int mbMgCallVsEffCreate(void)`, but its callers were recovered as
 * `BOOL mbMgCallSingleOnCheck(u16 ovl)` (board/single.c) and
 * `void mbMgCallVsEffCreate(void)` (REL/w10Dll/board.c). PPC and x86-64 shrug
 * that off; WebAssembly does not, since wasm-ld replaces a direct call whose
 * type differs from the definition with a trap. build.py renames the two
 * definitions inside mgcall.c only, and these adapters give every caller the
 * type it was compiled against. Behavior is the decomp's own: the overlay id
 * single.c passes was never read by the original routine either.
 */
#include "dolphin/types.h"

BOOL mp6_mgcall_single_on_check(void);
int mp6_mgcall_vs_eff_create(void);

BOOL mbMgCallSingleOnCheck(u16 ovl)
{
    (void)ovl;
    return mp6_mgcall_single_on_check();
}

void mbMgCallVsEffCreate(void)
{
    (void)mp6_mgcall_vs_eff_create();
}
