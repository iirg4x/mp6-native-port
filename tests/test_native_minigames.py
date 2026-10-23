"""Integration boundaries and native layouts for the recovered owners."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build, native_minigames, native_namespace

ROOT = Path(__file__).resolve().parents[1]


def recovered_bridge_inventory(source):
    """Read the single recovered-DLL X-macro inventory as a structured table."""
    body = source.split('#define MP6_RECOVERED_DLLS(X)', 1)[1]
    body = body.split('#define MP6_ENUM_RECOVERED_DLL', 1)[0]
    rows = {}
    for raw in re.findall(r'^\s+X\(([^)]*)\)', body, re.M):
        fields = [field.strip() for field in raw.split(',')]
        if len(fields) != 7:
            raise AssertionError(f'invalid recovered DLL inventory row: {raw}')
        fields[1] = fields[1].strip('"')
        fields[4] = fields[4].strip('"')
        rows[fields[0]] = tuple(fields[1:])
    return rows


class NativeMinigames(unittest.TestCase):
    def test_m664_ai_rejects_sentinel_targets_and_preserves_input_fields(self):
        from tests.test_inst_integration import function_definition_body
        decomp = Path(apply_patches.require_pinned_decomp())
        rel = 'src/REL/m664dll/game.c'
        source = apply_patches.generated_source(decomp, rel)
        # Run the actual AI/input functions, with only external actor services
        # replaced. Invalid targets must not chase or dereference any opponent.
        harness = '''
#include <assert.h>
#include <string.h>
#include "REL/m664dll/m664dll.h"
#include "game/pad.h"
M664PlayerRecord lbl_1_bss_C8[4];
M664PlayerSettings lbl_1_bss_18[4];
static int padCalls;
void MgPlayerPadSet(MGPLAYER *p, int x, int y, int down, int held) {
    assert(x == 0 && y == 0 && down == 0 && held == 0); ++padCalls;
}
BOOL MgPlayerComStkOn(MGPLAYER *p) { __builtin_trap(); }
void MgPlayerComStkOff(MGPLAYER *p) { __builtin_trap(); }
BOOL MgPlayerVecChase(MGPLAYER *p, HuVecF *v, float speed, float radius) { __builtin_trap(); }
s32 frandmod(s32 n) { __builtin_trap(); }
void PSVECSubtract(const Vec *a, const Vec *b, Vec *out) { *out = (Vec){0, 0, 0}; }
f32 PSVECMag(const Vec *v) { return 0; }
void PSVECNormalize(const Vec *v, Vec *out) { __builtin_trap(); }
void PSVECScale(const Vec *v, Vec *out, f32 scale) { __builtin_trap(); }
'''
        harness += function_definition_body(source, 'fn_1_B8FC')
        harness += function_definition_body(source, 'fn_1_AC58')
        harness += '''
int main(void) {
    M664InputSettings input, expected;
    memset(&input, 0x2A, sizeof(input)); expected = input;
    expected.sessionState[4] = -60; expected.sessionState[5] = 40;
    expected.sessionState[6] = PAD_BUTTON_A; expected.sessionState[7] = PAD_BUTTON_B;
    fn_1_B8FC(&input, -60, 40, PAD_BUTTON_A, PAD_BUTTON_B);
    assert(memcmp(&input, &expected, sizeof(input)) == 0);
    MGACTOR actor = {0}; MGPLAYER player = {0}; player.actor = &actor;
    lbl_1_bss_C8[0].player = &player;
    const int invalid[] = {-3, -2, -1, 4, 100000};
    for (unsigned grounded=0; grounded<2; ++grounded) {
        actor.colGroundAttr = grounded;
        for (unsigned i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) {
            memset(lbl_1_bss_18, 0, sizeof(lbl_1_bss_18));
            lbl_1_bss_18[0].sessionState[0] = 1;
            lbl_1_bss_18[0].sessionState[2] = invalid[i];
            fn_1_AC58(0);
            assert(lbl_1_bss_18[0].sessionState[2] == -1);
            assert(lbl_1_bss_18[0].sessionState[0] == 0);
        }
    }
    assert(padCalls == 10);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temp:
            subject, executable = Path(temp) / 'ai.c', Path(temp) / 'ai.exe'
            subject.write_text(harness)
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS,
                '-O1', '-UNDEBUG', '-fsanitize=undefined', '-fno-sanitize-recover=all',
                str(subject), '-o', str(executable)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_new_module_state_owners_restore_real_native_data(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        owners = {
            'src/REL/m627dll/startup.c': ('MP6_M627_STARTUP', 'mp6_m627_startup_reset'),
            'src/REL/m627dll/gameplay.c': ('MP6_M627_GAMEPLAY', 'mp6_m627_gameplay_reset'),
            'src/REL/m629Dll/m629.c': ('MP6_M629', 'mp6_m629_reset'),
            'src/REL/m664dll/setup.c': ('MP6_M664_SETUP', 'mp6_m664_setup_reset'),
            'src/REL/m664dll/game.c': ('MP6_M664_GAME', 'mp6_m664_game_reset'),
            'src/REL/m658dll/main.c': ('MP6_M658_MAIN', 'mp6_m658_main_reset'),
            'src/REL/m658dll/gameplay.c': ('MP6_M658_GAMEPLAY', 'mp6_m658_gameplay_reset'),
            'src/REL/m658dll/math_camera.c': ('MP6_M658_MATH_CAMERA', 'mp6_m658_math_camera_reset'),
            'src/REL/m636dll/main.c': ('MP6_M636_MAIN', 'mp6_m636_main_reset'),
            'src/REL/m636dll/pattern.c': ('MP6_M636_PATTERN', 'mp6_m636_pattern_reset'),
            'src/REL/m636dll/score.c': ('MP6_M636_SCORE', 'mp6_m636_score_reset'),
            'src/REL/m636dll/sequence.c': ('MP6_M636_SEQUENCE', 'mp6_m636_sequence_reset'),
            'src/REL/m643DLL/startup.c': ('MP6_M643_STARTUP', 'mp6_m643_startup_reset'),
            'src/REL/m643DLL/game.c': ('MP6_M643_GAME', 'mp6_m643_game_reset'),
            'src/REL/m649Dll/main.c': ('MP6_M649_MAIN', 'mp6_m649_main_reset'),
            'src/REL/m649Dll/effect.c': ('MP6_M649_EFFECT', 'mp6_m649_effect_reset'),
            'src/REL/m649Dll/prop.c': ('MP6_M649_PROP', 'mp6_m649_prop_reset'),
            'src/REL/m649Dll/score.c': ('MP6_M649_SCORE', 'mp6_m649_score_reset'),
            'src/REL/m667dll/startup.c': ('MP6_M667_STARTUP', 'mp6_m667_startup_reset'),
            'src/REL/m667dll/map_players_effects.c': ('MP6_M667_GAME', 'mp6_m667_game_reset'),
            'src/REL/m662dll/main.c': ('MP6_M662_MAIN', 'mp6_m662_main_reset'),
            'src/REL/m662dll/game.c': ('MP6_M662_GAME', 'mp6_m662_game_reset'),
            'src/REL/m662dll/puzzle.c': ('MP6_M662_PUZZLE', 'mp6_m662_puzzle_reset'),
            'src/REL/m631Dll/actor.c': ('MP6_M631_ACTOR', 'mp6_m631_actor_reset'),
            'src/REL/m631Dll/game.c': ('MP6_M631_GAME', 'mp6_m631_game_reset'),
            'src/REL/m631Dll/utilities.c': ('MP6_M631_UTILITIES', 'mp6_m631_utilities_reset'),
        }
        for unit in ('actors', 'character_motion', 'collision', 'model_attachments',
                     'model_shadows', 'score_display', 'sequence', 'target_selection'):
            owners[f'src/REL/m646Dll/{unit}.c'] = ('MP6_M646_' + unit.upper(), 'mp6_m646_' + unit + '_reset')
        for unit in native_minigames.MODULES['m675Dll']:
            owners[f'src/REL/m675Dll/{unit}.c'] = ('MP6_M675_' + unit.upper(), 'mp6_m675_' + unit + '_reset')
        # Prolog and entry units without mutable storage own no reset list.
        stateless = {('m625Dll', 'text_13318'), ('m644dll', 'prolog'), ('m680Dll', 'prolog'),
                     ('m613Dll', 'easing'), ('m626Dll', 'text_0052C'), ('m626Dll', 'text_04284'),
                     ('m609dll', 'intro'), ('m619Dll', 'particle_setup'), ('m660dll', 'text_0D29C'),
                     ('m617dll', 'prolog'), ('m617dll', 'text_058DC'), ('m617dll', 'text_0D3CC'),
                     ('m617dll', 'text_0E1F0'), ('m617dll', 'text_187C8'), ('m620Dll', 'text_49F8'),
                     ('m620Dll', 'text_F180')}
        for module in ('m625Dll', 'm644dll', 'm680Dll', 'm613Dll', 'm626Dll', 'm674Dll', 'm676Dll',
                       'm609dll', 'm619Dll', 'm647dll', 'm660dll',
                       'm617dll', 'm620Dll', 'm642dll', 'm665dll', 'm666dll'):
            prefix = module[:4]
            for unit in native_minigames.MODULES[module]:
                if (module, unit) not in stateless:
                    owners[f'src/REL/{module}/{unit}.c'] = (f'MP6_{prefix.upper()}_' + unit.upper(),
                                                            f'mp6_{prefix}_{unit}_reset')
        for rel, flags in native_minigames.sources(decomp):
            if rel not in owners:
                continue
            with self.subTest(source=rel), tempfile.TemporaryDirectory(dir=ROOT / 'build') as temp:
                source = apply_patches.generated_source(decomp, rel)
                storage_exclusions = (r'extern\b|const\b|static const\b'
                                      if '/m649Dll/' in rel or '/m644dll/' in rel or '/m609dll/' in rel else r'extern\b|static\b|const\b')
                declared = set(re.findall(r'^(?!' + storage_exclusions + r')(?:[A-Za-z_]\w*\s+)+\**\s*'
                    r'(lbl_1_(?:bss|data)_\w+)\s*(?:\[[^;\n]*\])?\s*'
                    r'(?:ATTRIBUTE_ALIGN\(\d+\)\s*)?[;=]', source, re.M))
                # Exact coverage, including declarators this regex cannot read
                # (function-pointer tables), is tests/test_rel_state_lists.py.
                self.assertLessEqual(declared, set(re.findall(r'X\((lbl_1_\w+)\)', source)))
                # Keep real native types and initializers. Unrelated callbacks
                # must never execute during a reset; trap if one is invoked.
                definitions = list(re.finditer(r'(?m)^(?:static\s+)?(?:inline\s+)?[A-Za-z_]\w*[ \t*]+'
                    r'(\w+)\s*\([^;{}]*\)\s*\{', source))
                for match in reversed(definitions):
                    # Start at this definition, not a same-name mention in a
                    # preceding comment or a forward declaration.
                    depth = 1
                    end = match.end()
                    while depth:
                        depth += (source[end] == '{') - (source[end] == '}')
                        end += 1
                    source = source[:match.end()] + ' __builtin_trap(); }' + source[end:]
                if rel == 'src/REL/m664dll/setup.c':
                    # Sequence data references callbacks owned by game.c.
                    # None may execute during storage reset.
                    for callback in ('2F8', '318', '34C', '36C', '38C'):
                        source += f'\nvoid fn_1_{callback}(s16 mode, s16 frameNo) {{ __builtin_trap(); }}\n'
                if rel == 'src/REL/m649Dll/main.c':
                    for callback in ('728C', '7288', '7300', '7C94', '7DD8', '7FD4', '80B4', '8278'):
                        source += f'\nvoid fn_1_{callback}(OMOBJ *object) {{ __builtin_trap(); }}\n'
                if rel == 'src/REL/m646Dll/sequence.c':
                    for callback in ('AF4', 'AF8'):
                        source += f'\nvoid fn_1_{callback}(s16 mode, s16 frameNo) {{ __builtin_trap(); }}\n'
                if rel == 'src/REL/m675Dll/effects.c':
                    # The effect lookup table points at gameplay-owned object slots.
                    source += '\nOMOBJ *lbl_1_bss_32C, *lbl_1_bss_328, *lbl_1_bss_324;\n'
                if rel == 'src/REL/m675Dll/gameplay.c':
                    # Preserve the external player's initial formation position.
                    source += '\nVec lbl_1_data_43C = {0.0f, 180.0f, 0.0f};\n'
                prefix, reset = owners[rel]
                probe = '''
#include <assert.h>
#define SAVE(n) unsigned char initial_##n[sizeof(n)]; memcpy(initial_##n, &n, sizeof(n));
#define DIRTY(n) memset(&n, 0xAB, sizeof(n));
#define DATA_CHECK(n) assert(memcmp(&n, initial_##n, sizeof(n)) == 0);
#define BSS_CHECK(n) for (size_t i=0; i<sizeof(n); ++i) assert(((unsigned char *)&n)[i] == 0);
int main(void) {
    PREFIX_DATA(SAVE)
    PREFIX_BSS(DIRTY)
    RESET();
    PREFIX_DATA(DATA_CHECK)
    PREFIX_BSS(BSS_CHECK)
    for (int visit=0; visit<3; ++visit) {
        PREFIX_DATA(DIRTY)
        PREFIX_BSS(DIRTY)
        RESET();
        PREFIX_DATA(DATA_CHECK)
        PREFIX_BSS(BSS_CHECK)
    }
    return 0;
}
'''.replace('PREFIX', prefix).replace('RESET', reset)
                subject, executable = Path(temp) / 'state.c', Path(temp) / 'state.exe'
                subject.write_text(source + probe)
                patched = Path(temp) / 'patched'
                apply_patches.apply_all(out_dir=str(patched))
                command = [build.ZIG, 'cc', '-I', str(patched / 'include'), *build.COMMON_FLAGS, *flags,
                    '-O2', '-UNDEBUG', str(subject), '-o', str(executable)]
                result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                missing = re.findall(r'undefined symbol: (\w+)', result.stderr)
                if missing and Path(rel).parent.name in ('m626Dll', 'm676Dll', 'm660dll', 'm642dll', 'm665dll'):
                    # Newer owners' initializers point at callbacks and objects
                    # owned by sibling units. Those addresses are captured and
                    # restored, never called or dereferenced by the reset.
                    siblings = Path(temp) / 'siblings.c'
                    siblings.write_text(''.join(
                        f'void {name}(void) {{ __builtin_trap(); }}\n' if '_fn_1_' in name
                        else f'char {name}[256];\n' for name in sorted(set(missing))))
                    command.insert(-2, str(siblings))
                    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_new_owners_compile_with_strict_native_prototypes(self):
        from tools import native_mdmini
        decomp = Path(apply_patches.require_pinned_decomp())
        units = [(rel, flags) for rel, flags in native_minigames.sources(decomp)
                 if Path(rel).parent.name.lower() in ('m617dll', 'm620dll', 'm642dll', 'm665dll', 'm666dll', 'm609dll', 'm619dll', 'm647dll', 'm660dll', 'm613dll', 'm626dll', 'm674dll', 'm676dll', 'm625dll', 'm627dll', 'm629dll', 'm631dll', 'm636dll', 'm643dll', 'm644dll', 'm646dll', 'm649dll', 'm658dll', 'm662dll', 'm664dll', 'm667dll', 'm675dll', 'm680dll')
                 or rel == 'src/REL/executor.c']
        units += list(native_mdmini.sources(decomp))
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            patched = Path(temporary) / 'patched'
            apply_patches.apply_all(out_dir=str(patched))
            for index, (rel, flags) in enumerate(units):
                with self.subTest(source=rel):
                    source = patched / rel
                    if not source.exists():
                        source = decomp / rel
                    pointer_checks = (['-Werror=pointer-to-int-cast', '-Werror=int-to-pointer-cast']
                                      if Path(rel).parent.name.lower() in ('m617dll', 'm620dll', 'm642dll', 'm665dll', 'm666dll', 'm609dll', 'm619dll', 'm647dll', 'm660dll', 'm613dll', 'm626dll', 'm674dll', 'm676dll', 'm625dll', 'm631dll', 'm644dll', 'm646dll', 'm662dll', 'm667dll', 'm675dll', 'm680dll') else [])
                    result = subprocess.run([build.ZIG, 'cc', '-I', str(patched / 'include'),
                        *build.COMMON_FLAGS, *flags, '-Werror=implicit-function-declaration',
                        '-Werror=incompatible-pointer-types', '-Werror=int-conversion',
                        *pointer_checks,
                        '-c', str(source), '-o', str(Path(temporary) / f'{index}.o')],
                        capture_output=True, text=True, timeout=120)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_namespace_scanner_is_deterministic_across_aliasing_units_and_headers(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            temp = Path(temporary)
            first = temp / 'first.c'
            second = temp / 'second.c'
            header = temp / 'shared.h'
            first.write_text('void fn_1_100(void); int lbl_1_data_4;\n')
            second.write_text('void fn_1_100(void) { } void fn_1_200(void) { }\n')
            header.write_text('extern int lbl_1_data_4; extern void fn_1_200(void);\n')
            expected_names = {
                '_prolog', '_epilog', '_ctors', '_dtors', 'ObjectSetup',
                'fn_1_100', 'fn_1_200', 'lbl_1_data_4',
            }
            expected = [
                f'-D{name}=demo_{name.lstrip("_")}'
                for name in sorted(expected_names)
            ]
            forward = native_namespace.namespace_flags(
                [first, second, header], 'demo',
                extra_names={'_prolog', '_epilog', '_ctors', '_dtors', 'ObjectSetup'},
            )
            reverse = native_namespace.namespace_flags(
                [header, second, first], 'demo',
                extra_names={'_prolog', '_epilog', '_ctors', '_dtors', 'ObjectSetup'},
            )
            self.assertEqual(native_namespace.symbols_from_files([first, second, header]), {
                'fn_1_100', 'fn_1_200', 'lbl_1_data_4',
            })
            self.assertEqual(forward, expected)
            self.assertEqual(reverse, expected)
            self.assertEqual(len(forward), len(set(forward)))

    def test_explicit_complete_sources_and_lifecycles(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        units = list(native_minigames.sources(decomp))
        # Check actual directory entries even on case-insensitive Windows.
        # A successful local open must not hide a broken Linux source path.
        directory_entries = {}
        for rel, _ in units:
            current = decomp
            for part in Path(rel).parts:
                if current not in directory_entries:
                    directory_entries[current] = {entry.name for entry in current.iterdir()}
                self.assertIn(part, directory_entries[current], rel)
                current /= part
        self.assertEqual(len(units), 228)
        self.assertEqual(len(native_minigames.MODULES), 46)
        bridge = (ROOT / 'src/os/dll_bridge.c').read_text()
        inventory = recovered_bridge_inventory(bridge)
        for module, names in native_minigames.MODULES.items():
            text = '\n'.join((decomp / native_minigames.source_path(module, name)).read_text(encoding='utf-8') for name in names)
            for hook in ('_prolog', '_epilog'):
                self.assertRegex(text, r'\b' + hook + r'\(void\)\s*\{')
            prefix = module[:4]
            self.assertEqual(inventory[prefix], (
                f'dll/{module.lower()}.rel', prefix + '_prolog', prefix + '_epilog',
                module.lower(), module[:4].upper(),
                f'MP6_{prefix.upper()}_CTORS'))
        self.assertEqual(len(inventory), 56)
        self.assertNotIn('mdbank', inventory)  # Safe fallback is not a recovered module.
        self.assertIn('MP6_RECOVERED_DLLS(MP6_RECOVERED_DLL_ROW)', bridge)
        self.assertIn('const Mp6RecoveredDll *recovered = mp6_recovered_dll_by_path(path);', bridge)
        self.assertIn('const Mp6RecoveredDll *recovered = mp6_recovered_dll_by_id(dll);', bridge)
        self.assertLess(bridge.index('const Mp6RecoveredDll *recovered = mp6_recovered_dll_by_path(path);'),
                        bridge.index('is_minigame_stub_path(path, g_minigameStubName'))
        self.assertIn('hdr->prolog = (u32)(uintptr_t)recovered->prolog;', bridge)
        self.assertIn('hdr->epilog = (u32)(uintptr_t)recovered->epilog;', bridge)
        self.assertIn('mp6_dll_bridge_report_bound(recovered->name);', bridge)
        self.assertNotIn('"dll/m601dll.rel"', bridge)
        # Boards pick minigames with the recovered roulette (board/mgcall.c).
        self.assertIn('src/board/mgcall.c', dict(build.board_sources()))

    def test_mdbankdll_uses_unavailable_overlay_return(self):
        bridge = (ROOT / 'src/os/dll_bridge.c').read_text()
        route = 'if (strcmp(path, "dll/mdbankdll.rel") == 0)'
        self.assertIn(route, bridge)
        self.assertLess(bridge.index(route),
                        bridge.index('is_minigame_stub_path(path, g_minigameStubName'))
        self.assertIn('snprintf(g_minigameStubName, sizeof(g_minigameStubName), "%s", path);', bridge)
        self.assertIn('return MP6_DLL_MINIGAME_STUB;', bridge)
        self.assertIn('mp6_unavailable_overlay_return();', bridge)

    def test_each_minigame_group_keeps_shared_namespace_prefix_and_flag_order(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        units = list(native_minigames.sources(decomp))
        for module in native_minigames.MODULES:
            expected = tuple(native_minigames.namespace_flags(decomp, module))
            paths = {native_minigames.source_path(module, name) for name in native_minigames.MODULES[module]}
            actual = {
                tuple(flags[:len(expected)])
                for rel, flags in units
                if rel in paths
            }
            self.assertEqual(actual, {expected}, module)

    def test_m659_lifecycle_is_in_camera_not_a_named_prolog_file(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        self.assertFalse((decomp / 'src/REL/m659Dll/prolog.c').exists())
        source = (decomp / 'src/REL/m659Dll/camera.c').read_text()
        self.assertRegex(source, r'int _prolog\(void\)\s*\{')
        self.assertRegex(source, r'void _epilog\(void\)\s*\{')
        self.assertIn('camera', native_minigames.MODULES['m659Dll'])

    def test_window_callback_preserves_signed_16_bit_character(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            temp = Path(temporary)
            source = temp / 'callback.c'
            source.write_text('''
#include "game/window.h"
static s16 received;
static void callback(HUWINID id, u32 message, s16 character) { received = character; }
_Static_assert(__builtin_types_compatible_p(HUWIN_CALLBACK, __typeof__(&callback)), "callback width mismatch");
int main(void) { HUWIN_CALLBACK cb = callback; cb(0, 0, 511); return received != 511; }
''')
            exe = temp / 'callback.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS,
                str(source), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(exe)]).returncode, 0)

    def test_m668_player_allocation_uses_native_pointer_width(self):
        rel = 'src/REL/m668DLL/callbacks.c'
        decomp = Path(apply_patches.require_pinned_decomp())
        patched = apply_patches.generated_source(decomp, rel)
        self.assertIn('fn_1_178(30, sizeof(M668PlayerWork), fn_1_494C)', patched)
        self.assertNotIn('fn_1_178(30, 84U, fn_1_494C)', patched)
        for name, helper in [('callbacks', 'M668Abs'), ('geometry', 'm668_fabs32')]:
            rel = f'src/REL/m668DLL/{name}.c'
            patched = apply_patches.generated_source(decomp, rel)
            self.assertIn('static inline f32 ' + helper, patched)
            self.assertNotIn('extern inline f32 ' + helper, patched)

    def test_namespaces_cover_every_local_label_and_object_is_unique(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        units = list(native_minigames.sources(decomp))
        for rel, flags in units:
            prefix = next(flag.split('=', 1)[1].removesuffix('_prolog') for flag in flags if flag.startswith('-D_prolog='))
            labels = set(re.findall(r'\b(?:fn_1_|lbl_1_)[A-Za-z0-9_]+', (decomp / rel).read_text(encoding='utf-8')))
            for label in labels:
                self.assertIn(f'-D{label}={prefix}_{label}', flags)
            self.assertNotIn('runtime.c', rel)
        objects = build.rel_object_names(build.REL_SOURCES + units)
        self.assertEqual(len(objects), len({name.casefold() for name in objects}))

    def test_headless_vector_math_and_aliases(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            temp = Path(temporary)
            source = temp / 'vectors.c'
            source.write_text('''
#include <math.h>
#include <dolphin/mtx.h>
void mp6_shim_log(const char *category, const char *symbol) { }
static int close_to(const Vec *v, float x, float y, float z) {
    return fabsf(v->x-x) < 1e-6f && fabsf(v->y-y) < 1e-6f && fabsf(v->z-z) < 1e-6f;
}
int main(void) {
    Vec a = {3, -4, 0}, b = {0, 7, 0}, out;
    Vec p = {1, 2, 3}, q = {-2, 6, 3};
    if (PSVECSquareDistance(&p, &q) != 25 || PSVECSquareDistance(&p, &p) != 0) return 1;
    C_VECReflect(&a, &b, &out);
    if (!close_to(&out, .6f, .8f, 0)) return 2;
    C_VECReflect(&a, &b, &a);
    if (!close_to(&a, .6f, .8f, 0)) return 3;
    a = (Vec){3, -4, 0};
    C_VECReflect(&a, &b, &b);
    return !close_to(&b, .6f, .8f, 0);
}
''')
            exe = temp / 'vectors.exe'
            result = subprocess.run([build.ZIG, 'cc', *build.COMMON_FLAGS,
                '-DMP6_HEADLESS_BUILD', str(source), str(ROOT / 'src/os/sdk_native.c'),
                '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(exe)], timeout=5).returncode, 0)

    def test_pointer_payload_and_scene_aliases_use_native_width(self):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            temp = Path(temporary)
            apply_patches.apply_all(out_dir=str(temp / 'patched'))
            source = temp / 'layout.c'
            source.write_text('''
#include <stdint.h>
#include <stddef.h>
#include "REL/m638Dll.h"
#include "game/mg/actman.h"
#define SAME(a,b) _Static_assert(__builtin_types_compatible_p(a,b), "view must share canonical layout")
SAME(M638LightingView, M638SceneView);
SAME(M638SceneCameraView, M638SceneView);
SAME(M638SceneSplitView, M638SceneView);
SAME(M638SceneCourseView, M638SceneView);
SAME(M638ModelCacheView, M638SceneView);
SAME(M638TeamInputView, M638TeamView);
SAME(M638TeamFinishView, M638TeamView);
_Static_assert(sizeof(((MGACTOR_PARAM *)0)->correctHookParam) == sizeof(void *), "pointer truncated");
_Static_assert(sizeof(((MGACTOR *)0)->correctHookParam) == sizeof(void *), "stored pointer truncated");
static void callback(MGACTOR *actor, intptr_t param) { (void)actor; (void)param; }
static MGACTOR_CORRECT_HOOK check = callback;
int main(void) {
    M638SceneView scene = {0};
    M638SceneCameraView *camera = &scene;
    M638TeamInputView *input = &scene.teams[1];
    input->active = 1;
    camera->teams[1].course.model = 123;
    return scene.teams[1].active != 1 || scene.teams[1].course.model != 123 || !check;
}
''')
            exe = temp / 'layout.exe'
            result = subprocess.run([build.ZIG, 'cc', '-I', str(Path(temp) / 'patched/include'),
                *build.COMMON_FLAGS, '-D_FDLIBM_H', '-include', 'math.h',
                '-Werror=incompatible-function-pointer-types', str(source), '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(exe)]).returncode, 0)

    def test_m638_progress_body_uses_scene_alias_slots_and_native_marker_positions(self):
        from tests.test_inst_integration import function_definition_body
        decomp = Path(apply_patches.require_pinned_decomp())
        rel = 'src/REL/m638Dll/game.c'
        source = apply_patches.generated_source(decomp, rel)
        body = function_definition_body(source, 'fn_1_5378')
        self.assertIn('scene->teams[0].course.model', body)
        self.assertIn('scene->teams[1].course.model', body)
        self.assertIn('scene->sprites[1]', body)
        self.assertIn('scene->sprites[2]', body)
        self.assertNotIn('scene->sprite0', body)
        self.assertNotIn('scene->sprite1', body)
        harness = r'''
#include <assert.h>
#include <stddef.h>
#include "REL/m638Dll.h"

static float motionTimes[16];
static s16 observedIds[8];
static float observedX[16];
static float observedY[16];
static int calls;
/* Outside-edge prompts have a separate real-body widescreen fixture. */
static void mp6_m638_prompt_positions(M638SceneView *scene) { (void)scene; }

float Hu3DMotionTimeGet(HU3D_MODELID modelId) {
    assert(modelId >= 0 && modelId < 16);
    return motionTimes[modelId];
}

void espPosSet(s16 espId, float posX, float posY) {
    assert(espId >= 0 && espId < 16);
    assert(calls < (int)(sizeof(observedIds) / sizeof(observedIds[0])));
    observedIds[calls++] = espId;
    observedX[espId] = posX;
    observedY[espId] = posY;
}

/* Native pointers change the host offsets. Both consumers must use the same
 * typed owner, never the PPC byte offsets or another hand-written view. */
_Static_assert(__builtin_types_compatible_p(M638SceneCameraView, M638SceneView),
    "camera update must share the scene owner's native layout");

static void reset_observations(void) {
    calls = 0;
    observedX[7] = 123.0f;
    observedY[7] = 456.0f;
}

static void assert_near(float actual, float expected) {
    assert(actual > expected - 0.01f && actual < expected + 0.01f);
}
'''
        harness += body
        harness += r'''

int main(void) {
    M638SceneView scene = {0};
    scene.teams[0].course.model = 1;
    scene.teams[1].course.model = 2;
    scene.sprites[0] = 7; /* file 49 rail: production body must never move it. */
    scene.sprites[1] = 8; /* file 50 marker/rail alias: team 0 status slot. */
    scene.sprites[2] = 9; /* file 51 marker alias: team 1 status slot. */

    motionTimes[1] = 337.5f;  /* progress 0.50 -> y 260 */
    motionTimes[2] = 228.75f; /* progress 0.25 -> y 330 */
    reset_observations();
    fn_1_5378((M638SceneCameraView *)&scene);
    assert(calls == 2);
    assert(observedIds[0] == 8 && observedIds[1] == 9);
    assert_near(observedX[8], 276.0f);
    assert_near(observedX[9], 300.0f);
    assert_near(observedY[8], 260.0f);
    assert_near(observedY[9], 330.0f);
    assert(observedX[7] == 123.0f && observedY[7] == 456.0f);

    motionTimes[1] = 0.0f;     /* clamp below native motion range */
    motionTimes[2] = 1000.0f;  /* clamp above native motion range */
    reset_observations();
    fn_1_5378((M638SceneCameraView *)&scene);
    assert(calls == 2);
    assert_near(observedY[8], 400.0f);
    assert_near(observedY[9], 120.0f);
    assert(observedX[7] == 123.0f && observedY[7] == 456.0f);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temp:
            apply_patches.apply_all(out_dir=str(Path(temp) / 'patched'))
            subject, executable = Path(temp) / 'm638_progress.c', Path(temp) / 'm638_progress.exe'
            subject.write_text(harness)
            result = subprocess.run([build.ZIG, 'cc', '-I', str(Path(temp) / 'patched/include'),
                *build.COMMON_FLAGS, '-D_FDLIBM_H', '-include', 'math.h',
                '-Werror=incompatible-function-pointer-types', str(subject), '-o', str(executable)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(executable)], timeout=5).returncode, 0)

    def test_removed_header_patch_cannot_leave_stale_shadow(self):
        with tempfile.TemporaryDirectory() as temporary:
            temp = Path(temporary)
            stale = temp / 'output/include/game/stale.h'
            stale.parent.mkdir(parents=True)
            stale.write_text('old layout')
            apply_patches.apply_all(decomp_root=str(temp / 'input'),
                                    patches_dir=str(temp / 'patches'), out_dir=str(temp / 'output'))
            self.assertFalse(stale.exists())
            with self.assertRaises(ValueError):
                apply_patches.apply_all(decomp_root=str(temp / 'input'),
                                        patches_dir=str(temp / 'patches'), out_dir=str(temp / 'input'))

    def test_returning_process_reenters_like_original_lr_bootstrap(self):
        original = Path(apply_patches.require_pinned_decomp())
        process = (original / 'src/game/process.c').read_text()
        jumps = (original / 'src/game/jmp.c').read_text()
        self.assertIn('process->jump.lr = (u32)entryFunc;', process)
        self.assertRegex(jumps, r'lwz r5, jump->lr\s+lwz r6, jump->cr\s+mtlr r5')
        self.assertIn('bnelr', jumps)
        source = (ROOT / 'src/os/process_native.c').read_text()
        trampoline = re.search(r'^static void ProcessTrampoline\(void \*param\)\n\{.*?^\}',
                               source, re.M | re.S).group()
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            temp = Path(temporary)
            c = temp / 'process.c'
            c.write_text('''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
typedef struct { struct { uint32_t lr; } jump; } HUPROCESS;
static jmp_buf done;
static int calls;
static void returning_process(void) {
    volatile int local = 7;
    if (++calls == 3) longjmp(done, local);
}
''' + trampoline + '''
int main(void) {
    HUPROCESS process = {{(uint32_t)(uintptr_t)returning_process}};
    int result = setjmp(done);
    if (result == 0) ProcessTrampoline(&process);
    return result != 7 || calls != 3;
}
''')
            exe = temp / 'process.exe'
            result = subprocess.run([build.ZIG, 'cc', '-target', build.TARGET, '-O2',
                '-Wl,--image-base=0x10000000', '-Wl,--no-dynamicbase', str(c), '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(subprocess.run([str(exe)], timeout=5).returncode, 0)

    def test_m650_four_row_initializer_has_four_row_storage(self):
        rel = 'src/REL/m650dll/obstacle.c'
        original = (Path(apply_patches.require_pinned_decomp()) / rel).read_text()
        source = apply_patches.apply_unified_diff(original, (ROOT / 'compat/decomp' / (rel + '.patch')).read_text())
        declaration = re.search(r'    Mtx\w* transform;', source).group()
        self.assertIn('while (row < 4)', source)
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            temp = Path(temporary)
            for label, decl, succeeds in [('native', declaration, True), ('original', 'Mtx transform;', False)]:
                c = temp / (label + '.c')
                c.write_text('typedef float Mtx[3][4], Mtx44[4][4];\n' + decl +
                    '\n_Static_assert(sizeof(transform) == 16 * sizeof(float), "initializer writes four rows");\n')
                result = subprocess.run([build.ZIG, 'cc', '-c', str(c), '-o', str(temp / (label + '.o'))],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode == 0, succeeds, result.stderr)


if __name__ == '__main__':
    unittest.main()
