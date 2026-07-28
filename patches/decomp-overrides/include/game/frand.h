/* MP6 native port -- ABI CORRECTION override of the decomp's include/game/frand.h.
 *
 * NOTE ON THIS DIRECTORY'S USUAL ROLE: patches/decomp-overrides/ is normally the
 * "foreign WIP shield" (pristine `git show HEAD:<rel>` snapshots -- see
 * tools/build.py resolve_source()/apply_decomp_override_headers()). THIS file is
 * the other legitimate use: a deliberate one-line correction to a decomp
 * declaration that the decomp's own config records as wrong. It is not a
 * snapshot and must not be refreshed from the decomp checkout.
 *
 * ONE CHANGE vs. the decomp header:
 *     -u32 frandmod(u32 arg0);
 *     +s32 frandmod(s32 arg0);
 *
 * WHY -- the shipped GameCube binary provably uses the SIGNED signature.
 * The decomp project already knows this and recorded it; it just never
 * propagated the correction into this shared header:
 *
 *     config/GP6E01/tu_declarations.json, sources[1] (src/REL/mdpartydll/stage.c),
 *     allowed_import_prototypes:
 *         "frandmod": { "declaration": "s32 frandmod(s32 modulus);",
 *                       "authority":    "dol" }
 *
 * Corroborated straight out of orig/GP6E01/sys/main.dol at
 * CharModelLandDustCreate (0x8005F968), for `alphaBase = -(frandmod(4)+8)`:
 *
 *     8005FAA8  li    r3, 4
 *     8005FAAC  bl    frandmod
 *     8005FAB0  addi  r0, r3, 8
 *     8005FAB4  neg   r0, r0
 *     8005FABC  xoris r0, r0, 0x8000   ; <- SIGNED int->double idiom
 *     8005FAD0  fsubs f0, f0, f1       ;    (magic 0x4330000080000000)
 *     8005FADC  stfs  f0, 0x30(r3)     ;    landEffParam.alphaBase
 *
 * mwcc emits that `xoris ...,0x8000` + 0x4330000080000000 pair only for
 * s32->float and omits it for u32->float (verified by compiling both forms
 * with the period compiler, external_refs/Compilers/GC/2.7/mwcceppc.exe,
 * -O4,p -proc gekko -fp hardware). The DOL has the signed form.
 *
 * WHAT THE WRONG u32 SIGNATURE COSTS THIS PORT. Any expression that makes the
 * result negative BEFORE it reaches a float evaluates in unsigned arithmetic and
 * converts a near-2^32 value instead of a small negative one. That is a real,
 * measured crash and a large field of latent corruption:
 *
 *   CRASH (measured, not inferred): charman.c's
 *   `landEffParam.alphaBase = -(frandmod(4)+8);` stores 4294967296.0f
 *   (bits 0x4f800000) rather than -8.0f..-11.0f. Instrumented run: all 12
 *   land-dust particles, alphaBase bit-identical 0x4f800000 in 20/20 samples,
 *   color.a a perfectly normal 255, rest of the EFFECTPARAM intact. Downstream,
 *   EffectParticleHook's `s16 color = particleDataP->color.a + param[i].alphaBase;`
 *   narrows ~4.295e9 into an s16 -- UB, which this toolchain traps as a hard
 *   panic ("(float) is outside the range of representable values of type 'short'").
 *
 *   SILENT CORRUPTION (same defect, no trap, so it would have gone unnoticed) --
 *   every one of these targets a float/f32 and is wrong ~half the time:
 *     src/game/charman.c:2109-2111  dustEffParam.vel.{x,y,z} = frandmod(10)-5;
 *     src/game/charman.c:2171-2173  particleDataP->vel.{x,y,z} = scale*(frandmod(100)-50);
 *     src/game/charman.c:2253       coinEffParam.vel.y = 0.1f*(frandmod(100)-50);
 *     src/game/charman.c:2599-2694  pos.{x,z} = modelP->pos.{x,z}+(frandmod(50)-25);
 *     src/game/charman.c:2922-2933  randX/randZ = frandmod(180)-90; and the
 *                                   CharEffectCryCreate offsets
 *     src/game/charman.c:589-623    pos.{x,z} += modelP->scale.x*(frandmod(50)-25);
 *     src/REL/mdpartydll/stage.c:992,1161  data->accel = frandmod(100)-50 /
 *                                   -frandmod(100)-50, then PSVECNormalize'd
 *     src/REL/mdseldll/mdsel.c:1283 -frandmod(1000)
 *   (Sites that subtract a FLOAT literal -- e.g. actman.c:68 `frandmod(20)-10.0f`,
 *   colman.c:2574 -- are already safe: the u32 converts to float before the
 *   subtraction. Those are unaffected either way.)
 *
 * WHY THE PROTOTYPE AND NOT A CAST AT THE CRASH SITE. Casting the three
 * charman.c alphaBase lines fixes only the one site that happens to trap and
 * leaves every silent site above still corrupt. Fixing the declaration restores
 * what the DOL actually does, everywhere, and needs no edit to any decomp source
 * except frand.c's own definition (see patches/decomp/src/game/frand.c.patch).
 *
 * WHY NOT A SATURATING float->s16 CONVERSION AT THE NARROWING. It would mask the
 * defect and silently BREAK the effect: PPC-faithful saturation of ~4.295e9 gives
 * (s16)(0x7fffffff & 0xffff) = -1, and charman.c's very next line is
 * `if(color < 1) { particleDataP->scale = 0; }` -- so every EFFECT_LANDDUST
 * particle would die on its first update and landing dust would never render.
 *
 * BEHAVIOUR IS OTHERWISE BIT-IDENTICAL: frandmod's result is
 * (frand_seed & 0x7FFFFFFF) % arg0, always in [0, arg0) and always < 2^31, so it
 * is representable in both s32 and u32 for every modulus the game passes. Only
 * the sign-extension of the CONVERSION changes -- which is the entire point.
 */
#ifndef _GAME_FRAND_H
#define _GAME_FRAND_H

#include "dolphin.h"

u32 frandom(u32 seed);
u32 frand(void);
f32 frandf(void);
s32 frandmod(s32 arg0);

#endif
