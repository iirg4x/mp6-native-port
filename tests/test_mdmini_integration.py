"""Upstream-owned entrance data, native reloads, and widescreen adapters."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
import unittest

from tools import apply_patches, build, native_mdmini as mini, native_namespace
from tests.test_inst_integration import function_definition_body, recovered_bridge_inventory

ROOT = Path(__file__).resolve().parents[1]


class MdminiIntegration(unittest.TestCase):
    def patched(self, name):
        decomp = Path(apply_patches.require_pinned_decomp())
        rel = f'src/REL/mdminidll/{name}.c'
        return apply_patches.generated_source(decomp, rel)

    def execute(self, source, flags=()):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            path = Path(temporary) / 'subject.c'
            path.write_text(source)
            exe = path.with_suffix('.exe')
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS, '-UNDEBUG',
                                     *flags, str(path), '-o', str(exe)],
                                    capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_import_without_decomp_or_disc(self):
        with tempfile.TemporaryDirectory() as temporary:
            env = os.environ.copy()
            env['MP6_DECOMP_DIR'] = str(Path(temporary) / 'missing')
            env['MP6_DISC_ROOT'] = str(Path(temporary) / 'missing-disc')
            result = subprocess.run([sys.executable, '-c',
                'from tools import build, native_mdmini; '
                'assert native_mdmini.UNITS == ("menu_setup", "particle_effects")'],
                cwd=ROOT, env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_widescreen_camera_and_set_are_registered_for_both_entry_paths(self):
        source = self.patched('menu_setup')
        self.assertIn('#include "mp6_widescreen.h"', source)
        self.assertNotIn('Hu3DCameraPerspectiveSet(', source)
        self.assertNotIn('Hu3DCameraViewportSet(', source)
        camera = function_definition_body(source, 'fn_1_30A4')
        self.assertIn('mp6_widescreen_camera_widen(1, 30.0f, 10.0f, 10000.0f, 1.2f, 480.0f);', camera)
        self.assertIn('memset(cameraState, 0, sizeof(*cameraState))', camera)
        self.assertIn('fn_1_30A4(', function_definition_body(source, 'fn_1_16400'))
        self.assertIn('mp6_widescreen_floor_border_fill_dup(DATANUM(DATA_mdmini, 0), "yuka");', source)
        self.assertIn('mp6_widescreen_extend_pillar_edge(menuObject->mdlId[modelIndex], "pillar");', source)
        self.assertNotIn('mp6_widescreen_extrude_model(', source)
        ring = function_definition_body(source, 'fn_1_7C4C')
        self.assertLess(ring.index('mp6_widescreen_extend_ring('), ring.index('Hu3DModelLink('))
        for function in ('fn_1_71E8', 'fn_1_157B4_inlined'):
            self.assertIn('mp6_widescreen_floor_border_fill_dup_kill_all();',
                          function_definition_body(source, function))

    def test_widescreen_transition_and_native_capture(self):
        source = self.patched('particle_effects')
        self.assertIn('C_MTXLightPerspective(projectionMatrix, camera->fov, camera->aspect,', source)
        effect = function_definition_body(source, 'fn_1_20068')
        self.assertIn('0.0f, mp6_widescreen_render_width(), 0.0f, 10.0f);', effect)
        self.assertEqual(effect.count('(quadIndex + mp6_widescreen_render_width())'), 2)
        self.assertIn('GXSetTexCopyDst(320U, 240U, GX_TF_RGB565, GX_TRUE);', effect)
        self.assertNotIn('GXWGFifo', source)
        for call in ('GXPosition3f32(', 'GXColor4u8(', 'GXTexCoord2f32('):
            self.assertEqual(effect.count(call), 4)
        self.assertLess(effect.index('GXEnd();'), effect.index('GXSetTexCopySrc('))
        self.assertIn('model->hookData', source)
        self.assertNotIn('(u8 *)arg0->model + 288', source)
        capture = function_definition_body(source, 'fn_1_1F508')
        self.execute('''#include <assert.h>
#include <stddef.h>
typedef short s16;
#define GX_TF_RGB565 4
#define GX_TRUE 1
#define GX_FALSE 0
static int width, copies;
static char buffer[320*240*2];
void *lbl_1_bss_888 = buffer;
static int mp6_widescreen_render_width(void) { return width; }
static void GXSetTexCopySrc(int x,int y,int w,int h) { assert(!x && !y && w==width && h==480); }
static void GXSetTexCopyDst(int w,int h,int f,int m) { assert(w==320 && h==240 && f==4 && m==1); }
static void GXCopyTex(void *p,int clear) { assert(p==buffer && !clear); ++copies; }
''' + capture + '''
int main(void) {
    int widths[] = {640, 848, 1120, 640};
    for (int i=0;i<4;++i) { width=widths[i]; fn_1_1F508(0); }
    assert(copies==4);
    lbl_1_bss_888=0; fn_1_1F508(0); assert(copies==4);
    return 0;
}
''')

    def test_floor_fill_uses_bounded_native_density_plank_tile(self):
        source = (ROOT / 'src/hsf/mp6_widescreen_extrude.c').read_text()
        subject = function_definition_body(source, 'mp6_ws_minigame_floor_tile')
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            directory = Path(temporary)
            (directory / 'board_subject.inc').write_text(subject)
            self.execute((ROOT / 'tests/native/mdmini_floor_selftest.c').read_text(),
                         ['-I', str(directory)])
        self.assertIn('strcmp(o->mesh.attribute->bitmap->name, "ys77_floor") != 0', source)

    def test_selected_sources_and_namespace(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        selected = set(re.findall(r'Object\(\s*Matching,\s*"REL/mdminidll/([^"/]+)\.c"',
                                  (decomp / 'configure.py').read_text())) - {'runtime'}
        self.assertEqual(set(mini.UNITS), selected)
        self.assertEqual(len(mini.UNITS), 2)
        units = list(mini.sources(decomp))
        files = [decomp / rel for rel, _ in units]
        files += sorted((decomp / 'include/REL/mdminidll').glob('*.h'))
        expected = native_namespace.namespace_flags(files, 'mdmini',
            extra_names={'_prolog', '_epilog', '_ctors', '_dtors'})
        self.assertEqual(mini.namespace_flags(decomp), expected)
        for rel, flags in units:
            for label in set(re.findall(r'\b(?:fn_1_|lbl_1_)\w+', (decomp / rel).read_text())):
                self.assertIn(f'-D{label}=mdmini_{label}', flags)
        from tools import native_minigames
        objects = build.rel_object_names(build.REL_SOURCES + list(native_minigames.sources(decomp)) + units)
        self.assertEqual(len(objects), len(set(objects)))

    def test_bridge_calls_both_data_owners_before_entry(self):
        bridge = (ROOT / 'src/os/dll_bridge.c').read_text()
        self.assertEqual(recovered_bridge_inventory(bridge)['mdmini'], (
            'dll/mdminidll.rel', 'mdmini_prolog', 'mdmini_epilog',
            'mdminidll', 'MDMINI', 'MP6_MD_MINI_CTORS'))
        self.assertIn('#define MP6_MD_MINI_CTORS { mp6_mdmini_reset_data, 0 }', bridge)
        reset = function_definition_body(self.patched('menu_setup'), 'mp6_mdmini_reset_data')
        self.assertIn('mp6_mdmini_menu_reset();', reset)
        self.assertIn('mp6_mdmini_particles_reset();', reset)

    def test_real_data_owners_reload_and_host_endian_neighbors(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        for unit, prefix, reset in [('menu_setup', 'MP6_MDMINI_MENU', 'mp6_mdmini_menu_reset'),
                                    ('particle_effects', 'MP6_MDMINI_PARTICLES', 'mp6_mdmini_particles_reset')]:
            source = self.patched(unit)
            owners = set(re.findall(r'^(?!extern\b|static\b|const\b)[A-Za-z_]\w*(?:\s+|\s*\*\s*)'
                                   r'(lbl_1_(?:bss|data)_\w+)\s*(?:\[[^;\n]*\])?\s*[;=]', source, re.M))
            declared = set(re.findall(r'X\((lbl_1_\w+)\)', source))
            self.assertEqual(owners, declared)
            # Keep real types, initializers, and reset macros. Remove unrelated
            # game functions, not data: this probes every native-owned byte.
            definitions = list(re.finditer(r'(?m)^(?:static\s+)?(?:inline\s+)?[A-Za-z_]\w*[ \t*]+'
                                           r'(\w+)\s*\([^;{}]*\)\s*\{', source))
            for match in reversed(definitions):
                definition = function_definition_body(source, match[1])
                source = source.replace(definition, definition.split('{', 1)[0] + ';', 1)
            probe = '''
#include <assert.h>
#define SAVE(n) unsigned char saved_##n[sizeof(n)]; memcpy(saved_##n, &n, sizeof(n));
#define DIRTY(n) memset(&n, 0xAB, sizeof(n));
#define CHECK_DATA(n) assert(memcmp(&n, saved_##n, sizeof(n)) == 0);
#define CHECK_BSS(n) for (size_t i=0;i<sizeof(n);++i) assert(((unsigned char *)&n)[i] == 0);
int main(void) {
    PREFIX_DATA(SAVE)
    PREFIX_BSS(DIRTY)
    RESET();
    PREFIX_BSS(CHECK_BSS)
    PREFIX_DATA(CHECK_DATA)
    for (int visit=0;visit<3;++visit) {
        PREFIX_DATA(DIRTY)
        PREFIX_BSS(DIRTY)
        RESET();
        PREFIX_DATA(CHECK_DATA)
        PREFIX_BSS(CHECK_BSS)
    }
    EXTRA_CHECKS
    return 0;
}
'''.replace('PREFIX', prefix).replace('RESET', reset)
            checks = ''
            if unit == 'menu_setup':
                original = (decomp / 'src/REL/mdminidll/menu_setup.c').read_text()
                table = re.search(r'MDMinidllSelectionView lbl_1_data_89A\[11\] = \{(.*?)\n\};', original, re.S)[1]
                rows = re.findall(r'\{\{([^}]+)\},\s*(-?\d+)\}', table)
                self.assertEqual(len(rows), 11)
                for i, (values, claimed) in enumerate(rows):
                    raw = bytes(int(v.strip(), 0) for v in values.split(','))
                    for j in range(8):
                        value = int.from_bytes(raw[j*2:j*2+2], 'big', signed=True)
                        checks += f'assert(lbl_1_data_89A[{i}].neighborCharacter[{j}] == {value});\n'
                checks += '_Static_assert(sizeof(lbl_1_bss_808) > 80, "native camera width");'
            self.execute(source + probe.replace('EXTRA_CHECKS', checks), list(mini.sources(decomp))[0][1])


if __name__ == '__main__':
    unittest.main()
