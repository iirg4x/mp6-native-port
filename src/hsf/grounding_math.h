#ifndef MP6_GROUNDING_MATH_H
#define MP6_GROUNDING_MATH_H
#include <math.h>
typedef struct { float x,y,z; } GroundPoint;
typedef struct { GroundPoint a,b,c; } GroundTri;

/* Vertical intersection of the actual rendered triangle. Do not extrapolate
 * beyond its footprint or treat steep walls as floor. */
static int ground_height(const GroundTri *t, float x, float z, float *y)
{
    float bx=t->b.x-t->a.x, bz=t->b.z-t->a.z;
    float cx=t->c.x-t->a.x, cz=t->c.z-t->a.z;
    float by=t->b.y-t->a.y, cy=t->c.y-t->a.y;
    float d=bx*cz-bz*cx;
    float nx=by*cz-bz*cy, nz=bx*cy-by*cx;
    if (!isfinite(d) || d*d < 1e-8f || nx*nx+nz*nz > d*d*0.04f) return 0;
    float u=((x-t->a.x)*cz-(z-t->a.z)*cx)/d;
    float v=(bx*(z-t->a.z)-bz*(x-t->a.x))/d;
    if (u < -1e-5f || v < -1e-5f || u+v > 1.00001f) return 0;
    *y=t->a.y+u*by+v*cy;
    return isfinite(*y);
}

#endif
