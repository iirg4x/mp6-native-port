#ifndef MP6_PPC_MATH_H
#define MP6_PPC_MATH_H

/* Native implementation in shims_manual.c; never use an implicit int return
 * for the PPC reciprocal-square-root helper in recovered math expressions. */
double __frsqrte(double x);

#endif
