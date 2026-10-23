"""Native ABI contracts for the minigames recovered after the 72-module baseline."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import apply_patches, build, native_minigames, native_support

ROOT = Path(__file__).resolve().parents[1]
MODULES = {'m625Dll': native_minigames, 'm644dll': native_minigames,
           'm680Dll': native_minigames, 'micquizdll': native_support,
           'm613Dll': native_minigames, 'm626Dll': native_minigames,
           'm674Dll': native_minigames, 'm676Dll': native_minigames,
           'm609dll': native_minigames, 'm619Dll': native_minigames,
           'm647dll': native_minigames, 'm660dll': native_minigames,
           'm617dll': native_minigames, 'm620Dll': native_minigames,
           'm642dll': native_minigames, 'm665dll': native_minigames,
           'm666dll': native_minigames}

DEFINITION = re.compile(r'^(?P<ret>(?:static\s+|inline\s+)*[A-Za-z_][\w \t*]*?[\s*])'
                        r'(?P<name>fn_1_[0-9A-Fa-f]+)\s*\((?P<params>[^;{}]*?)\)\s*\{', re.M)
DECLARATION = re.compile(r'^[ \t]*(?!return\b|else\b|case\b|do\b|goto\b)(?:extern\s+)?(?P<ret>[A-Za-z_][\w \t*]*?[\s*])(?P<name>fn_1_[0-9A-Fa-f]+)'
                         r'\s*\((?P<params>[^;{}()]*)\)\s*;', re.M)
# Spellings of the same native type are one ABI.
ALIASES = {'short': 's16', 'int': 's32', 'float': 'f32', 'double': 'f64', 'unsigned int': 'u32',
           'HuVecF': 'Vec', 'Point3d': 'Vec', 'HUPROCESS': 'OMOBJMAN'}
TYPE_WORDS = {'int', 'char', 'short', 'long', 'float', 'double', 'unsigned', 'signed', 'void',
              's8', 's16', 's32', 's64', 'u8', 'u16', 'u32', 'u64', 'f32', 'f64', 'Vec', 'Mtx', 'BOOL'}


def _normal_type(text):
    text = re.sub(r'\s+', ' ', re.sub(r'\b(static|inline|extern|const)\b', '', text.replace('*', ' * '))).strip()
    for alias, name in ALIASES.items():
        text = re.sub(r'\b' + alias + r'\b', name, text)
    return text


def _normal_params(params):
    params = re.sub(r'/\*.*?\*/', '', params, flags=re.S).strip()
    if not params:
        return None  # An unprototyped declaration.
    if params == 'void':
        return 'void'
    result = []
    for item in params.split(','):
        words = _normal_type(item).split(' ')
        if len(words) > 1 and words[-1] not in TYPE_WORDS and words[-1] != '*':
            words = words[:-1] + (['*'] if '[' in item else [])
        result.append(' '.join(words))
    return ', '.join(result)


def _opaque_pointers(declared, defined):
    """A `void *` parameter stands for a single-level pointer to a type that
    unit cannot see (another unit's local struct); the ABI is identical."""
    if declared[1] is None or defined[1] is None or declared[0] != defined[0]:
        return declared
    ours, theirs = declared[1].split(', '), defined[1].split(', ')
    if len(ours) != len(theirs):
        return declared
    return declared[0], ', '.join(
        b if a == 'void *' and re.fullmatch(r'[A-Za-z_]\w* \*', b) else a for a, b in zip(ours, theirs))


def patched_module_sources(decomp, patched, module):
    rels = [rel for rel, _ in MODULES[module].sources(decomp) if Path(rel).parent.name == module]
    rels += [str(path.relative_to(decomp)).replace('\\', '/')
             for path in sorted((decomp / 'include/REL' / module).rglob('*.h'))]
    for rel in rels:
        path = patched / rel
        yield rel, (path if path.is_file() else decomp / rel).read_text(encoding='utf-8', errors='replace')


class NewMinigameAbis(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.decomp = Path(apply_patches.require_pinned_decomp())
        cls.temporary = tempfile.TemporaryDirectory(dir=ROOT / 'build')
        cls.patched = Path(cls.temporary.name) / 'patched'
        apply_patches.apply_all(out_dir=str(cls.patched))

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_cross_unit_declarations_match_recovered_definitions(self):
        # Native calls rely on exact prototypes: a narrowed or K&R declaration
        # reads undefined high bits or promotes floats to double on x86-64/ARM64.
        for module in MODULES:
            sources = dict(patched_module_sources(self.decomp, self.patched, module))
            definitions = {}
            for text in sources.values():
                for match in DEFINITION.finditer(text):
                    definitions.setdefault(match['name'], (_normal_type(match['ret']),
                                                           _normal_params(match['params'])))
            for rel, text in sources.items():
                for match in DECLARATION.finditer(text):
                    name = match['name']
                    if name not in definitions:
                        continue
                    with self.subTest(source=rel, function=name):
                        declared = (_normal_type(match['ret']), _normal_params(match['params']))
                        self.assertEqual(_opaque_pointers(declared, definitions[name]), definitions[name])

    def test_units_compile_with_native_pointer_and_prototype_checks(self):
        for module, inventory in MODULES.items():
            for rel, flags in inventory.sources(self.decomp):
                if Path(rel).parent.name != module:
                    continue
                with self.subTest(source=rel):
                    source = self.patched / rel
                    if not source.is_file():
                        source = self.decomp / rel
                    output = Path(self.temporary.name) / (module + '_' + Path(rel).stem + '.o')
                    command = [build.ZIG, 'cc', '-I', str(self.patched / 'include'),
                               *[f for f in build.COMMON_FLAGS if f != '-w'], *flags,
                               '-Werror=implicit-function-declaration', '-Werror=int-conversion',
                               '-Werror=incompatible-pointer-types',
                               '-Werror=incompatible-function-pointer-types',
                               '-Werror=pointer-to-int-cast', '-Werror=int-to-pointer-cast',
                               '-Werror=void-pointer-to-int-cast', '-Werror=int-to-void-pointer-cast',
                               '-Werror=deprecated-non-prototype',
                               '-c', str(source), '-o', str(output)]
                    command = build.transport_long_defines(command, str(output))
                    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=300)
                    self.assertEqual(result.returncode, 0, result.stderr[-4000:])

    def test_speak_up_window_text_is_owned_and_never_packed_into_message_ids(self):
        quiz = (self.patched / 'src/REL/micquizdll/quiz.c').read_text(encoding='utf-8', errors='replace')
        functions = (self.patched / 'include/REL/micquizdll/quiz_functions.h').read_text(encoding='utf-8')
        self.assertNotRegex(quiz, r'fn_1_20D08\([^;]*\(u32\)\s*\w+(Text|Number|Score|number)\b')
        self.assertEqual(quiz.count('mp6_micquiz_insert_text('), 24)
        self.assertIn('strncpy(slot, text, sizeof(mp6_micquiz_insert_text_storage[0][0]) - 1);', functions)
        self.assertIn('mp6_window_insert_text(fn_1_206FC(windowNo), slot, insertNo);', functions)
        for text in (quiz, functions):
            self.assertNotRegex(text, r'\(u32\)\s*(?:process|currentProcess)->property')
            self.assertNotRegex(text, r'\(s(?:16|32)\)\s*(?:process|currentProcess)->property')

    def test_sink_or_swim_crest_list_uses_native_pointer_width(self):
        source = (self.patched / 'src/REL/m625Dll/text_15C8.c').read_text(encoding='utf-8')
        self.assertIn('peakCount * sizeof(*peakVertices)', source)
        self.assertNotIn('peakCount * 4', source)

    def test_widescreen_screen_copies_and_backdrops_follow_live_width(self):
        stage = (self.patched / 'src/REL/m625Dll/text_15C8.c').read_text(encoding='utf-8')
        # The refraction texture samples the whole widened view; 4:3 stays 640.
        self.assertNotIn('Hu3DFbCopyExec(0, 0, 640, 480', stage)
        self.assertIn('GXSetTexCopySrc(0, 0, mp6_widescreen_render_width(), 480);', stage)
        self.assertIn('GXSetTexCopyDst(640, 480, GX_TF_RGB565, GX_FALSE);', stage)
        self.assertIn('mp6_widescreen_extend_wall_edges(lbl_1_bss_194.baseModel, "bg", 0.0f, 2.0f, 1);', stage)
        self.assertIn('mp6_widescreen_extrude_model_selective_horizontal(lbl_1_bss_194.baseModel, '
                      'mp6_m625_backstop);', stage)
        self.assertIn('return strcmp(name, "skyBG") == 0;', stage)
        # The sheet is extended before the wave process captures crest vertices.
        self.assertLess(stage.index('mp6_widescreen_extend_sides(lbl_1_bss_0.model, "wave2", 0);'),
                        stage.index('HuPrcChildCreate(fn_1_20AC'))
        quiz = (self.patched / 'src/REL/micquizdll/quiz.c').read_text(encoding='utf-8', errors='replace')
        functions = (self.patched / 'include/REL/micquizdll/quiz_functions.h').read_text(encoding='utf-8')
        self.assertEqual(quiz.count('fn_1_407C((s16)((mp6_widescreen_render_width() - 640) / 2), 0, 640, 480, 5,'), 2)
        self.assertIn('right = (f32)mp6_widescreen_render_width();', quiz)
        self.assertIn('-margin, 576.0f + margin, 0.0f, 10.0f);', functions)
        self.assertIn('GXSetScissor(0, 0, mp6_widescreen_render_width(), 480);', functions)
        self.assertIn('GXTexCoord2f32(36.0f + margin / 16.0f, 30.0f);', functions)

    def test_speak_up_mode_work_matches_selector_layout(self):
        # Speak Up reads the record its selector writes through MgModeWork.
        fields = [('playerActive', 'isHumanPlayer'), ('playerCount', 'humanPlayerCount'),
                  ('currentPlayer', 'currentPlayerIndex'), ('roundResult', 'answerSucceeded'),
                  ('playerScore', 'playerScores'), ('playerCharacter', 'playerCharacterNumbers'),
                  ('questionEvent', 'quizSubmode'), ('presenterLayout', 'useAlternatePrompt'),
                  ('pauseAllowed', 'startPromptEnabled'), ('setupValue', 'randomSelectionEnabled')]
        offsets = {}
        for module, struct, index in (('micquizdll', 'MicquizModeWorkView', 0),
                                      ('micquizseldll', 'MicquizWork', 1)):
            probe = Path(self.temporary.name) / f'{module}_layout.c'
            probe.write_text(f'#include <stddef.h>\n#include <stdio.h>\n'
                             f'#include "REL/{module}/module_types.h"\n'
                             f'_Static_assert(sizeof({struct}) <= 256, "MgModeWork capacity");\n'
                             'int main(void) {\n' + ''.join(
                                 f'    printf("%zu\\n", offsetof({struct}, {pair[index]}));\n'
                                 for pair in fields) + '    return 0;\n}\n')
            executable = probe.with_suffix('.exe')
            result = subprocess.run([build.ZIG, 'cc', '-I', str(self.patched / 'include'), *build.COMMON_FLAGS,
                                     str(probe), '-o', str(executable)],
                                    cwd=ROOT, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr[-4000:])
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            offsets[module] = run.stdout.split()
        self.assertEqual(offsets['micquizdll'], offsets['micquizseldll'])


if __name__ == '__main__':
    unittest.main()
