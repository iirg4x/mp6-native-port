"""CPU checks for the stable HSF object identity capture seam.

The retained GX stream only sees a position-matrix load.  The production HSF
walker therefore supplies a one-shot authored-object hint immediately before
each visible/deferred load.  These tests compile the actual model-context
implementation against a small ABI-shaped Hu3D fixture and exercise the
one-shot, invalid, reset, generation, immediate, deferred, auxiliary, and
layer-hook paths without creating a GPU/device or running native code.
"""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import build


ROOT = Path(__file__).resolve().parents[1]
MODEL_SOURCE = ROOT / "src" / "hsf" / "mp6_fi_model.c"
MODEL_HEADER = ROOT / "include" / "mp6_fi_model.h"
HSF_PATCH = ROOT / "compat" / "decomp" / "src" / "game" / "hsfdraw.c.patch"
FOLIAGE_HEADER = ROOT / "include" / "mp6_ao_foliage_draw.h"


# Keep this header deliberately small.  It is enough of the production Hu3D
# ABI for mp6_fi_model.c's context, generation, and camera code, while making
# the test independent of a generated/decomp build tree.
STUB_HU3D_H = r"""
#ifndef TEST_GAME_HU3D_H
#define TEST_GAME_HU3D_H
#include <stdint.h>

#define HU3D_MODEL_MAX 8
#define HU3D_CAM_MAX 4
#define HU3D_LAYER_HOOK_MAX 2
#define HU3D_ATTR_HOOKFUNC (1u << 4)

typedef struct { float x, y, z; } Vec;
typedef struct { float time, speed, start, end; } Hu3dMotWork;
typedef struct {
    float fov;
    float nearPlane, farPlane;
    float aspect;
    Vec pos, up, target;
} HU3D_CAMERA;
typedef struct HSF_OBJECT_s {
    uint32_t marker[4];
} HSF_OBJECT;
typedef struct HSF_DATA_s {
    HSF_OBJECT *object;
    int objectNum;
} HSF_DATA;
typedef struct HU3D_MODEL_s {
    uint32_t tick;
    uint32_t attr;
    Hu3dMotWork motWork, motOvlWork, motShiftWork, motShapeWork;
    HSF_DATA *hsf;
    Vec pos, rot, scale;
} HU3D_MODEL;

extern HU3D_MODEL *Hu3DData;
extern HU3D_CAMERA Hu3DCamera[HU3D_CAM_MAX];
#endif
"""


FIXTURE_C = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game/hu3d.h"
#include "mp6_fi_model.h"

static HU3D_MODEL g_models[HU3D_MODEL_MAX];
static HU3D_CAMERA g_cameras[HU3D_CAM_MAX];
HU3D_MODEL *Hu3DData = g_models;
HU3D_CAMERA Hu3DCamera[HU3D_CAM_MAX];

static HSF_DATA g_hsf;
static HSF_OBJECT g_objects[3];
static HSF_OBJECT g_other_object;

static void setup_model(void) {
    memset(g_models, 0, sizeof(g_models));
    memset(g_cameras, 0, sizeof(g_cameras));
    memset(Hu3DCamera, 0, sizeof(Hu3DCamera));
    memset(&g_hsf, 0, sizeof(g_hsf));
    memset(g_objects, 0, sizeof(g_objects));
    memset(&g_other_object, 0, sizeof(g_other_object));
    g_hsf.object = g_objects;
    g_hsf.objectNum = 3;
    g_models[3].hsf = &g_hsf;
    g_models[3].attr = 0;
    Hu3DCamera[2].fov = 45.0f;
    Hu3DData = g_models;
}

static void next_expect(int expected_ok, int expected_camera, int expected_model,
                        uint16_t expected_sub, uint16_t expected_ordinal,
                        uint32_t *generation_out) {
    int camera = -99;
    int model = -99;
    uint32_t generation = 0;
    uint16_t sub = 0;
    uint16_t ordinal = 0;
    int ok = mp6_fi_capture_context_next(&camera, &model, &generation,
                                         &sub, &ordinal);
    assert(ok == expected_ok);
    if (!expected_ok) return;
    assert(camera == expected_camera);
    assert(model == expected_model);
    assert(generation != 0);
    assert(sub == expected_sub);
    assert(ordinal == expected_ordinal);
    if (generation_out) *generation_out = generation;
}

static void begin_visible(void) {
    mp6_fi_capture_camera(2);
    mp6_fi_capture_model_begin(3);
}

static void test_object_index_bounds_and_alignment(void) {
    char *misaligned = (char *)&g_objects[1] + 1;
    assert(mp6_fi_object_index(3, &g_objects[0]) == 0);
    assert(mp6_fi_object_index(3, &g_objects[2]) == 2);
    assert(mp6_fi_object_index(3, misaligned) == -1);
    assert(mp6_fi_object_index(3, &g_other_object) == -1);
    assert(mp6_fi_object_index(3, (const void *)((uintptr_t)g_objects -
                                                  sizeof(HSF_OBJECT))) == -1);
    assert(mp6_fi_object_index(-1, &g_objects[0]) == -1);
    assert(mp6_fi_object_index(HU3D_MODEL_MAX, &g_objects[0]) == -1);
    assert(mp6_fi_object_index(3, NULL) == -1);
    assert(mp6_fi_object_index(3, &g_objects[3]) == -1);
    Hu3DData = NULL;
    assert(mp6_fi_object_index(3, &g_objects[0]) == -1);
    begin_visible();
    mp6_fi_capture_object_matrix(3, 1);
    next_expect(0, 0, 0, 0, 0, NULL);
    Hu3DData = g_models;

    g_hsf.objectNum = 0;
    assert(mp6_fi_object_index(3, &g_objects[0]) == -1);
    g_hsf.objectNum = 3;
    g_models[3].attr = HU3D_ATTR_HOOKFUNC;
    assert(mp6_fi_object_index(3, &g_objects[0]) == -1);
    g_models[3].attr = 0;
}

static void test_valid_one_shot_does_not_consume_ordinal(void) {
    uint32_t generation = 0;
    begin_visible();
    assert(mp6_fi_object_index(3, &g_objects[1]) == 1);
    mp6_fi_capture_object_matrix(3, 1);
    next_expect(1, 2, 3, 0x4001u, 0, &generation);
    assert(generation != 0);
    /* The authored-object override labels the next load only. */
    next_expect(1, 2, 3, 0xffffu, 0, NULL);
    next_expect(1, 2, 3, 0xffffu, 1, NULL);
}

static void test_invalid_hint_is_a_consumed_invalid_load(void) {
    begin_visible();
    mp6_fi_capture_object_matrix(3, -1);
    next_expect(0, 0, 0, 0, 0, NULL);
    /* No stale object hint and no consumed normal ordinal. */
    next_expect(1, 2, 3, 0xffffu, 0, NULL);

    mp6_fi_capture_object_matrix(3, 0x4000);
    next_expect(0, 0, 0, 0, 0, NULL);
    next_expect(1, 2, 3, 0xffffu, 1, NULL);

    /* A valid index paired with the wrong model is invalid, not a fallback
     * attribution to the currently open model. */
    mp6_fi_capture_object_matrix(4, 0);
    next_expect(0, 0, 0, 0, 0, NULL);
    next_expect(1, 2, 3, 0xffffu, 2, NULL);
}

static void test_context_boundaries_and_unknown_shadow(void) {
    begin_visible();
    mp6_fi_capture_object_matrix(3, 0);
    mp6_fi_capture_model_end();
    next_expect(0, 0, 0, 0, 0, NULL);

    begin_visible();
    next_expect(1, 2, 3, 0xffffu, 0, NULL);
    mp6_fi_capture_object_matrix(3, 1);
    mp6_fi_capture_context_reset();
    next_expect(0, 0, 0, 0, 0, NULL);

    /* Camera-only shadow/reflect work has no model identity.  A hinted load
     * must be marked invalid; it must not inherit the previous model. */
    mp6_fi_capture_camera(2);
    mp6_fi_capture_object_matrix(3, 0);
    next_expect(0, 0, 0, 0, 0, NULL);
    next_expect(0, 0, 0, 0, 0, NULL);

    /* Layer hooks remain a separate, valid non-object namespace when no
     * authored-object hint is pending. */
    mp6_fi_capture_layer_hook_begin(0, (const void *)(uintptr_t)0x12340u);
    next_expect(1, 2, HU3D_MODEL_MAX, 0xffffu, 0, NULL);
    mp6_fi_capture_object_matrix(3, 0);
    next_expect(0, 0, 0, 0, 0, NULL);
}

static void test_generation_reuse_and_model_reset(void) {
    uint32_t first = 0;
    uint32_t second = 0;
    begin_visible();
    mp6_fi_capture_object_matrix(3, 2);
    next_expect(1, 2, 3, 0x4002u, 0, &first);

    mp6_fi_model_identity_new(3);
    begin_visible();
    mp6_fi_capture_object_matrix(3, 2);
    next_expect(1, 2, 3, 0x4002u, 0, &second);
    assert(first != second);

    /* A live slot is generation-stamped again across reset, and reset clears
     * an unconsumed pending object hint. */
    mp6_fi_capture_object_matrix(3, 1);
    mp6_fi_model_reset();
    begin_visible();
    next_expect(1, 2, 3, 0xffffu, 0, &second);
    assert(second != first);
}

static void test_deferred_and_auxiliary_namespaces(void) {
    int i;
    begin_visible();
    mp6_fi_capture_defer_reset();
    mp6_fi_capture_defer_push(0);
    mp6_fi_capture_defer_push(1);
    mp6_fi_capture_defer_begin(0);
    next_expect(1, 2, 3, 0u, 0, NULL);
    mp6_fi_capture_defer_begin(1);
    next_expect(1, 2, 3, 1u, 0, NULL);

    /* Saturation stays below the authored-object sub-key namespace. */
    mp6_fi_capture_defer_reset();
    begin_visible();
    for (i = 0; i < 0x4000; ++i) mp6_fi_capture_defer_push(0);
    mp6_fi_capture_defer_begin(0);
    next_expect(1, 2, 3, 0x3fffu, 0, NULL);

    mp6_fi_capture_auxiliary_begin(3, 2);
    next_expect(1, 2, 3, 0x8002u, 0, NULL);
    mp6_fi_capture_auxiliary_begin(3, 0x4000);
    next_expect(0, 0, 0, 0, 0, NULL);
}

int main(void) {
    setup_model();
    test_object_index_bounds_and_alignment();
    test_valid_one_shot_does_not_consume_ordinal();
    test_invalid_hint_is_a_consumed_invalid_load();
    test_context_boundaries_and_unknown_shadow();
    test_generation_reuse_and_model_reset();
    test_deferred_and_auxiliary_namespaces();
    puts("FI object-context checks passed");
    return 0;
}
"""


class FiObjectContextTests(unittest.TestCase):
    def test_compiled_production_context_and_object_fixture(self):
        with tempfile.TemporaryDirectory(prefix="fi-object-context-",
                                          dir=ROOT / "build") as folder:
            directory = Path(folder)
            game = directory / "game"
            game.mkdir()
            (game / "hu3d.h").write_text(STUB_HU3D_H, encoding="utf-8")
            fixture = directory / "fi_object_context_selftest.c"
            source = MODEL_SOURCE.read_text(encoding="utf-8")
            # Compile the production context implementation itself, without
            # pulling in the unrelated engine/link surface.  This is the same
            # source-extraction pattern used by the camera-cache CPU test.
            start = source.index("typedef struct {\n    int valid;")
            end = source.index("static int animlog_enabled(void)")
            constants = source[source.index("#define FI_CAM_TRANSLATION_CUT"):start]
            context = source[start:end]
            # The CPU-deformed buffer registry needs the real HSF types; it
            # is not part of the object context under test.
            deformed = context.index("/* ---- CPU-deformed vertex buffers")
            context = constants + context[:deformed] + context[context.index("void mp6_fi_model_reset"):]
            fixture.write_text(
                "#include <math.h>\n"
                "#include <stdlib.h>\n"
                "#include <string.h>\n"
                "#include \"game/hu3d.h\"\n"
                "#include \"mp6_fi_model.h\"\n\n"
                + context + "\n" + FIXTURE_C,
                encoding="utf-8",
            )
            exe = directory / "fi-object-context-selftest.exe"
            result = subprocess.run(
                [build.ZIG, "cc", "-std=c11", "-O2", "-UNDEBUG",
                 "-I" + str(directory), "-I" + str(ROOT / "include"),
                 str(fixture), "-lm", "-o", str(exe)],
                cwd=ROOT, capture_output=True, text=True, timeout=90,
            )
            self.assertEqual(result.returncode, 0,
                             result.stdout + result.stderr)
            result = subprocess.run([str(exe)], cwd=ROOT,
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0,
                             result.stdout + result.stderr)
            self.assertIn("FI object-context checks passed", result.stdout)

    def test_production_wiring_reserves_domains_and_validates_both_load_sites(self):
        header = MODEL_HEADER.read_text(encoding="utf-8")
        source = MODEL_SOURCE.read_text(encoding="utf-8")
        patch = HSF_PATCH.read_text(encoding="utf-8")
        foliage = FOLIAGE_HEADER.read_text(encoding="utf-8")

        self.assertIn("int mp6_fi_object_index(int model_id, const void *object);",
                      header)
        self.assertIn("void mp6_fi_capture_object_matrix(int model_id, int object_index);",
                      header)
        self.assertIn("_Static_assert(FI_DEFER_MAX < 0x4000", source)
        self.assertRegex(source, r"s_deferRank\[model\]\s*<\s*0x3FFFu")
        self.assertIn("if (objectHint && objectIndex < 0) return 0;", source)
        self.assertIn("if (!objectHint && s_contextOrdinal != UINT16_MAX)", source)

        # Both direct HSF matrix loads get exactly one validated, one-shot
        # object hint; matrix bytes and render-state calls remain untouched.
        hints = re.findall(r"\+[^\n]*mp6_fi_capture_object_matrix\(", patch)
        self.assertEqual(len(hints), 2)
        self.assertEqual(patch.count("mp6_fi_object_index("), 2)
        self.assertNotIn("drawObj->matrix =", patch)
        self.assertIn("mp6_fi_object_index(modelId,draw->object)", foliage)
        self.assertNotIn("draw->object-draw->model->hsf->object", foliage)


if __name__ == "__main__":
    unittest.main()
