/* Native behavior fixture for the private AO foliage material-cache guard.
 *
 * The Python test extracts the production private record, its three helper
 * bodies, and the exact adjacent-draw reuse expression into subject.inc.
 * This file supplies only the real HSF/Dolphin layout and exercises the
 * extracted code against live records; it does not duplicate that private
 * implementation.  No renderer or GPU work is required.
 */
#include "dolphin_compat.h"
#include "game/hu3d.h"
#include "game/hsfformat.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* These are the hsfdraw-private values read by the extracted helper. */
int Hu3DShadowF;
s16 shadowNum;
HSF_MATERIAL *materialBak;

static void math_hook_stub(HU3D_DRAW_OBJ *draw, HSF_MATERIAL *material)
{
    (void)draw;
    (void)material;
}

/* Keep this type and helper text tied to production by extraction rather than
 * reproducing a potentially stale private layout in this fixture. */
#include "subject.inc"

typedef struct GuardFixture_s {
    HU3D_MODEL model;
    HSF_DATA hsf;
    HSF_OBJECT object;
    HSF_CONSTDATA constData;
    HSF_MATERIAL materials[2];
    HSF_ATTRIBUTE attributes[3];
    HSF_BITMAP bitmap;
    s32 attrIndex[3];
    HSF_FACE face;
    HSF_DRAWDATA drawData[2];
    HU3D_DRAW_OBJ draw;
    Mp6AoFoliageDraw saved;
} GuardFixture;

static int g_cases;
static int g_failed;

static void check_case(const char *name, int condition)
{
    ++g_cases;
    if (!condition) {
        ++g_failed;
        printf("FAIL %s\n", name);
    }
}

static void fixture_init(GuardFixture *f)
{
    int i;
    memset(f, 0, sizeof(*f));
    f->model.hsf = &f->hsf;
    f->model.ambR = f->model.ambG = f->model.ambB = 1.0f;
    f->object.constData = &f->constData;
    f->object.mesh.material = f->materials;
    f->object.mesh.attribute = f->attributes;
    f->materials[0].attr = f->attrIndex;
    f->materials[0].attrNum = 0;
    f->attrIndex[0] = 0;
    f->attrIndex[1] = 1;
    f->attrIndex[2] = 2;
    f->face.mat = 0;
    f->face.type = 3;
    f->constData.drawData = f->drawData;
    f->drawData[0].flags = 0;
    f->drawData[1].flags = 2;
    f->draw.model = &f->model;
    f->draw.object = &f->object;
    f->draw.z = 10.0f;
    for (i = 0; i < 3; ++i) {
        f->draw.matrix[i][i] = 1.0f;
    }
    f->draw.scale.x = f->draw.scale.y = f->draw.scale.z = 1.0f;
    f->saved.draw = f->draw;
    f->saved.face = &f->face;
    f->saved.batch = 0;
    f->saved.camera = 0;
    f->saved.water = 0;
    f->saved.background = 0;
    f->saved.unshaded = 0;
    Hu3DShadowF = 0;
    shadowNum = 0;
    materialBak = &f->materials[0];
}

static int eligible(GuardFixture *f)
{
    HSF_MATERIAL *material = mp6AoFoliageReuseMaterial(&f->saved);
    return mp6AoFoliageReuseEligible(&f->saved, material);
}

static int exact_reuse(GuardFixture *previous, GuardFixture *current,
                       int previousEligible, int currentEligible)
{
    const Mp6AoFoliageDraw *previousSaved = previous ? &previous->saved : NULL;
    HSF_MATERIAL *previousMaterial = previousSaved
        ? mp6AoFoliageReuseMaterial(previousSaved) : NULL;
    HSF_MATERIAL *currentMaterial = mp6AoFoliageReuseMaterial(&current->saved);
    unsigned previousBatchFlags = previousSaved
        ? mp6AoFoliageReuseBatchFlags(previousSaved) : 0;
    unsigned currentBatchFlags = mp6AoFoliageReuseBatchFlags(&current->saved);
    return extracted_reuse_expression(previousSaved, &current->saved,
                                      previousEligible,
                                      currentEligible, &current->draw,
                                      currentMaterial, previousMaterial,
                                      currentBatchFlags, previousBatchFlags,
                                      materialBak);
}

static void eligibility_cases(void)
{
    GuardFixture f, g;
    fixture_init(&f);
    check_case("plain-eligible", eligible(&f));

    f.saved.draw.object = NULL;
    check_case("null-object-fail-closed", !mp6AoFoliageReuseMaterial(&f.saved));
    check_case("null-object-eligible-fail-closed", !eligible(&f));
    fixture_init(&f);
    f.saved.face = NULL;
    check_case("null-face-material-fail-closed", !mp6AoFoliageReuseMaterial(&f.saved));
    check_case("null-face-eligible-fail-closed", !eligible(&f));
    fixture_init(&f);
    f.saved.draw.model = NULL;
    check_case("null-model-eligible-fail-closed", !eligible(&f));
    fixture_init(&f);
    check_case("material-pointer-derived-from-face", mp6AoFoliageReuseMaterial(&f.saved) == &f.materials[0]);
    f.face.mat = 1;
    check_case("material-slot-uses-face-index", mp6AoFoliageReuseMaterial(&f.saved) == &f.materials[1]);

    fixture_init(&f);
    f.model.attr = HU3D_ATTR_HOOKFUNC;
    check_case("hookfunc-excluded", !eligible(&f));
    fixture_init(&f);
    f.materials[0].flags = HSF_MATERIAL_MATHOOK;
    f.model.matHook = math_hook_stub;
    check_case("actual-material-hook-excluded", !eligible(&f));
    fixture_init(&f);
    f.object.flags = HSF_MATERIAL_MATHOOK;
    f.model.matHook = math_hook_stub;
    check_case("actual-object-material-hook-excluded", !eligible(&f));
    fixture_init(&f);
    f.materials[0].flags = HSF_MATERIAL_MATHOOK;
    f.model.matHook = NULL;
    check_case("unused-material-hook-allowed", eligible(&f));
    fixture_init(&f);
    f.model.matHook = math_hook_stub;
    check_case("unused-model-hook-pointer-allowed", eligible(&f));

    fixture_init(&f);
    f.materials[0].refAlpha = 0.01f;
    check_case("reflection-alpha-excluded", !eligible(&f));
    fixture_init(&f);
    f.model.attr = HU3D_ATTR_HILITE;
    check_case("model-hilite-excluded", !eligible(&f));
    fixture_init(&f);
    f.constData.attr = HU3D_CONST_HILITE;
    check_case("const-hilite-excluded", !eligible(&f));
    fixture_init(&f);
    f.materials[0].flags = HSF_MATERIAL_HILITE;
    check_case("material-hilite-excluded", !eligible(&f));
    fixture_init(&f);
    f.object.flags = HSF_MATERIAL_HILITE;
    check_case("object-hilite-excluded", !eligible(&f));
    fixture_init(&f);
    f.model.projBit = 1;
    check_case("projection-excluded", !eligible(&f));
    fixture_init(&f);
    f.model.attr = HU3D_ATTR_TOON_MAP;
    check_case("toon-excluded", !eligible(&f));
    fixture_init(&f);
    Hu3DShadowF = 1;
    shadowNum = 1;
    f.constData.attr = HU3D_CONST_SHADOW_MAP;
    check_case("shadow-map-excluded", !eligible(&f));
    Hu3DShadowF = 0;
    check_case("shadow-disabled-allows", eligible(&f));
    Hu3DShadowF = 1;
    shadowNum = 0;
    check_case("shadow-slot-empty-allows", eligible(&f));
    shadowNum = 1;
    f.constData.attr = 0;
    check_case("shadow-const-bit-clear-allows", eligible(&f));

    fixture_init(&f);
    f.object.mesh.attribute = NULL;
    f.materials[0].attrNum = 2;
    check_case("no-attribute-table-allows", eligible(&f));
    fixture_init(&f);
    f.materials[0].attrNum = 1;
    f.attributes[0].animWorkP = (void *)(uintptr_t)1;
    check_case("attribute-animation-excluded", !eligible(&f));
    fixture_init(&f);
    f.materials[0].attrNum = 2;
    f.attributes[1].animWorkP = (void *)(uintptr_t)1;
    check_case("second-attribute-animation-excluded", !eligible(&f));
    fixture_init(&f);
    f.materials[0].attrNum = 1;
    f.attributes[0].bitmap = &f.bitmap;
    f.bitmap.dataFmt = HSF_BMPFMT_CI_IA8;
    check_case("ci-ia8-excluded", !eligible(&f));
    fixture_init(&f);
    f.materials[0].attrNum = 1;
    f.attributes[0].bitmap = &f.bitmap;
    f.bitmap.dataFmt = HSF_BMPFMT_RGBA8;
    f.bitmap.sizeX = 1024;
    f.bitmap.sizeY = 1024;
    f.bitmap.pixSize = 4;
    check_case("oversized-static-bitmap-allowed", eligible(&f));
    fixture_init(&f);
    f.materials[0].attrNum = 2;
    f.attributes[0].bitmap = &f.bitmap;
    f.bitmap.dataFmt = HSF_BMPFMT_RGBA8;
    check_case("second-ordinary-attribute-allows", eligible(&f));

    /* Keep one fixture clone around so the compile-time exact pair wrapper
     * is also exercised by the same native HSF layout. */
    fixture_init(&g);
    check_case("eligibility-clone-stable", eligible(&g));
}

static void pair_init(GuardFixture *previous, GuardFixture *current)
{
    fixture_init(previous);
    fixture_init(current);
    /* The optimized path is intentionally adjacent draws of the same live
     * model/object/material.  Keep current's draw and saved record paired to
     * the previous HSF instance; individual tests then perturb one identity. */
    current->draw.model = previous->draw.model;
    current->draw.object = previous->draw.object;
    current->saved.draw = current->draw;
    current->saved.face = previous->saved.face;
    materialBak = &previous->materials[0];
}

static void reuse_expression_cases(void)
{
    GuardFixture previous, current;
    fixture_init(&current);
    check_case("reuse-no-previous", !exact_reuse(NULL, &current, 0, 1));
    pair_init(&previous, &current);
    check_case("reuse-exact-pair", exact_reuse(&previous, &current, 1, 1));
    check_case("reuse-previous-ineligible", !exact_reuse(&previous, &current, 0, 1));
    check_case("reuse-current-ineligible", !exact_reuse(&previous, &current, 1, 0));

    pair_init(&previous, &current);
    current.draw.model = &current.model;
    check_case("reuse-model-difference", !exact_reuse(&previous, &current, 1, 1));
    pair_init(&previous, &current);
    current.draw.object = &previous.object;
    current.saved.draw.object = current.draw.object;
    current.draw.object = &current.object;
    check_case("reuse-object-difference", !exact_reuse(&previous, &current, 1, 1));
    pair_init(&previous, &current);
    current.face.mat = 1;
    current.saved.face = &current.face;
    check_case("reuse-material-difference", !exact_reuse(&previous, &current, 1, 1));
    pair_init(&previous, &current);
    current.draw.matrix[1][2] = 0.25f;
    check_case("reuse-matrix-difference", !exact_reuse(&previous, &current, 1, 1));
    pair_init(&previous, &current);
    current.draw.scale.x = 1.25f;
    check_case("reuse-scale-difference", !exact_reuse(&previous, &current, 1, 1));
    pair_init(&previous, &current);
    current.draw.z = 11.0f;
    check_case("reuse-z-difference", !exact_reuse(&previous, &current, 1, 1));
    pair_init(&previous, &current);
    current.saved.water = 1;
    check_case("reuse-water-mask-difference", !exact_reuse(&previous, &current, 1, 1));
    pair_init(&previous, &current);
    current.saved.background = 1;
    check_case("reuse-background-mask-difference", !exact_reuse(&previous, &current, 1, 1));
    pair_init(&previous, &current);
    current.saved.unshaded = 1;
    check_case("reuse-unshaded-mask-difference", !exact_reuse(&previous, &current, 1, 1));
    pair_init(&previous, &current);
    current.saved.batch = 1;
    check_case("reuse-layout-bit2-difference", !exact_reuse(&previous, &current, 1, 1));
    pair_init(&previous, &current);
    materialBak = &current.materials[1];
    check_case("reuse-materialBak-difference", !exact_reuse(&previous, &current, 1, 1));
    pair_init(&previous, &current);
    check_case("reuse-adjacent-face-same-draw", exact_reuse(&previous, &current, 1, 1));
}

int main(void)
{
    eligibility_cases();
    reuse_expression_cases();
    printf("CASES %d FAILED %d\n", g_cases, g_failed);
    return g_failed == 0 ? 0 : 1;
}
