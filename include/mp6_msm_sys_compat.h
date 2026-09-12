/* Exact group-loader declaration hidden by the decomp's two unrelated
 * msm.h headers sharing one include guard.  Native callers must not fall back
 * to implicit-int rules. */
#ifndef MP6_MSM_SYS_COMPAT_H
#define MP6_MSM_SYS_COMPAT_H

#include <dolphin/types.h>

s32 msmSysLoadGroup(s32 grp, void *buf, BOOL flag);

#endif
