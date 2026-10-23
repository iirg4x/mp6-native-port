"""Complete recovered m670 closure, with unavailable minigames still stubbed."""
from pathlib import Path
import re
import unittest

from tools import apply_patches, build, native_namespace

ROOT = Path(__file__).resolve().parents[1]


def collected_m670_units():
    """Return the m670 rows from the lazy build collection."""
    decomp = Path(build.DECOMP)
    patched = Path(build.PATCHED_SRC_DIR)
    units = []
    for path, flags, obj_name, flavor in build.collect_units(headless=True):
        rel = None
        for root in (decomp, patched):
            try:
                rel = Path(path).relative_to(root).as_posix()
                break
            except ValueError:
                pass
        if rel is None:
            continue
        if rel.startswith('src/REL/m670dll/'):
            units.append((rel, list(flags), obj_name, flavor))
    return units


def recovered_bridge_inventory(source):
    body = source.split('#define MP6_RECOVERED_DLLS(X)', 1)[1]
    body = body.split('#define MP6_ENUM_RECOVERED_DLL', 1)[0]
    rows = {}
    for raw in re.findall(r'^\s+X\(([^)]*)\)', body, re.M):
        fields = [field.strip() for field in raw.split(',')]
        fields[1] = fields[1].strip('"')
        fields[4] = fields[4].strip('"')
        rows[fields[0]] = tuple(fields[1:])
    return rows


class M670Integration(unittest.TestCase):
    def test_namespace_covers_source_header_closure_without_duplicate_aliases(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        collected = collected_m670_units()
        units = {rel: flags for rel, flags, _, _ in collected}
        self.assertEqual({Path(rel).name for rel in units},
                         {'prolog.c', 'sequence.c', 'seqparam.c', 'init.c',
                          'pattern.c', 'actor.c', 'pillar.c', 'cpu.c'})
        self.assertEqual([Path(rel).name for rel, _, _, _ in collected],
                         ['prolog.c', 'sequence.c', 'seqparam.c', 'init.c',
                          'pattern.c', 'actor.c', 'pillar.c', 'cpu.c'])
        paths = [decomp / rel for rel in units]
        paths.append(decomp / 'include/REL/m670dll.h')
        expected = native_namespace.namespace_flags(paths, 'm670')
        self.assertEqual(len(expected), len(set(expected)))
        occurrences = [name for path in paths for name in
                       native_namespace.LOCAL_SYMBOL_RE.findall(
                       path.read_text(encoding='utf-8'))]
        labels = native_namespace.symbols_from_files(paths)
        self.assertLess(len(labels), len(occurrences))
        self.assertEqual(set(expected),
                         {'-D' + name + '=m670_' + name for name in labels})
        self.assertEqual(len(expected), len(labels))
        self.assertTrue(all(path.is_file() for path in paths))

        prolog_flags = dict(build.REL_SOURCES)['src/REL/m670dll/prolog.c']
        self.assertTrue({
            '-D_prolog=m670Dll_prolog',
            '-D_epilog=m670Dll_epilog',
            '-D_ctors=m670Dll_ctors',
            '-D_dtors=m670Dll_dtors',
        }.issubset(prolog_flags))

    def test_complete_units_share_module_namespace(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        collected = collected_m670_units()
        units = {rel: flags for rel, flags, _, _ in collected}
        self.assertEqual({Path(rel).name for rel in units},
                         {'prolog.c', 'sequence.c', 'seqparam.c', 'init.c',
                          'pattern.c', 'actor.c', 'pillar.c', 'cpu.c'})
        paths = [decomp / rel for rel in units]
        paths.append(decomp / 'include/REL/m670dll.h')
        expected = native_namespace.namespace_flags(paths, 'm670')
        labels = set()
        for rel, flags in units.items():
            labels.update(re.findall(r'\b(?:fn_1_|lbl_1_)[A-Za-z0-9_]+',
                                     (decomp / rel).read_text()))
            self.assertTrue(set(expected).issubset(flags))
        labels.update(re.findall(r'\b(?:fn_1_|lbl_1_)[A-Za-z0-9_]+',
                                 (decomp / 'include/REL/m670dll.h').read_text()))
        self.assertEqual(set(expected),
                         {'-D' + name + '=m670_' + name for name in labels})
        for shared in ('actman.c', 'colman.c', 'seqman.c', 'mgtimer.c', 'mic.c'):
            self.assertIn('src/game/' + shared, dict(build.game_sources()))
        for name in ('init.c', 'sequence.c'):
            flags = units['src/REL/m670dll/' + name]
            self.assertIn(('-include', 'game/hsfex.h'), list(zip(flags, flags[1:])))

    def test_repeated_collection_preserves_exact_flag_order(self):
        baseline = [(rel, list(flags)) for rel, flags in build.REL_SOURCES
                    if rel.startswith('src/REL/m670dll/')]
        first = collected_m670_units()
        second = collected_m670_units()
        self.assertEqual(first, second)
        self.assertEqual(baseline,
                         [(rel, list(flags)) for rel, flags in build.REL_SOURCES
                          if rel.startswith('src/REL/m670dll/')])

        expected_names = ['prolog.c', 'sequence.c', 'seqparam.c', 'init.c',
                          'pattern.c', 'actor.c', 'pillar.c', 'cpu.c']
        self.assertEqual([Path(rel).name for rel, _, _, _ in first], expected_names)
        decomp = Path(build.DECOMP)
        paths = [decomp / 'src/REL/m670dll' / name for name in
                 ('prolog.c', 'sequence.c', 'seqparam.c', 'init.c',
                  'pattern.c', 'actor.c', 'pillar.c', 'cpu.c')]
        paths.append(decomp / 'include/REL/m670dll.h')
        namespace = native_namespace.namespace_flags(paths, 'm670')
        expected_flags = {
            'prolog.c': [
                '-D_prolog=m670Dll_prolog', '-D_epilog=m670Dll_epilog',
                '-D_ctors=m670Dll_ctors', '-D_dtors=m670Dll_dtors',
            ] + namespace,
            'sequence.c': namespace + ['-include', 'game/hsfex.h'],
            'seqparam.c': namespace,
            'init.c': namespace + ['-include', 'game/hsfex.h'],
            'pattern.c': namespace,
            'actor.c': namespace,
            'pillar.c': namespace,
            'cpu.c': namespace,
        }
        for rel, flags, _, _ in first:
            self.assertEqual(flags, expected_flags[Path(rel).name])

    def test_cpu_pattern_tables_have_four_slots_per_word(self):
        source = (Path(build.DECOMP) / 'src/REL/m670dll/pattern.c').read_text()
        patterns = re.findall(r's8 lbl_1_data_\w+\[24\] = \{([^}]+)\}', source)
        self.assertEqual(len(patterns), 5)
        for pattern in patterns:
            values = [int(value) for value in re.findall(r'\d+', pattern)]
            self.assertEqual(len(values), 24)
            for word in range(6):
                self.assertEqual(values.count(word), 4)

    def test_lifecycle_and_incomplete_overlay_gate(self):
        source = (ROOT / 'src/os/dll_bridge.c').read_text()
        self.assertEqual(recovered_bridge_inventory(source)['m670Dll'], (
            'dll/m670dll.rel', 'm670Dll_prolog', 'm670Dll_epilog',
            'm670Dll', 'M670', 'MP6_M670_CTORS'))
        self.assertIn('mp6_recovered_dll_by_path(path)', source)
        self.assertIn('mp6_recovered_dll_by_id(dll)', source)
        self.assertIn('mp6_dll_bridge_report_bound(recovered->name);', source)
        self.assertFalse(any(rel.startswith('src/REL/m601dll/')
                             for rel, _ in build.REL_SOURCES))
        self.assertNotIn('"dll/m601dll.rel"', source)
        # Boards pick minigames with the recovered roulette (board/mgcall.c).
        self.assertIn('src/board/mgcall.c', dict(build.board_sources()))


if __name__ == '__main__':
    unittest.main()
