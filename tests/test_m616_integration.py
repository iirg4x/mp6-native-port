"""Native integration of the recovered m616 owner, not a gameplay substitute."""
from pathlib import Path
import re
import unittest

from tools import apply_patches, build, native_namespace

ROOT = Path(__file__).resolve().parents[1]


def collected_m616_units():
    """Return the m616 rows from the lazy build collection."""
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
        if rel.startswith('src/REL/m616dll/'):
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


class M616Integration(unittest.TestCase):
    def test_namespace_covers_source_header_closure_without_duplicate_aliases(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        collected = collected_m616_units()
        units = {rel: flags for rel, flags, _, _ in collected}
        self.assertEqual({Path(rel).name for rel in units},
                         {'prolog.c', 'm616.c', 'init.c', 'utility.c', 'cpu.c'})
        self.assertEqual([Path(rel).name for rel, _, _, _ in collected],
                         ['prolog.c', 'm616.c', 'init.c', 'utility.c', 'cpu.c'])
        paths = [decomp / rel for rel in units]
        paths.append(decomp / 'include/REL/m616dll.h')
        expected = native_namespace.namespace_flags(paths, 'm616')
        self.assertEqual(len(expected), len(set(expected)))
        occurrences = [name for path in paths for name in
                       native_namespace.LOCAL_SYMBOL_RE.findall(
                       path.read_text(encoding='utf-8'))]
        labels = native_namespace.symbols_from_files(paths)
        self.assertLess(len(labels), len(occurrences))
        self.assertEqual(set(expected),
                         {'-D' + name + '=m616_' + name for name in labels})
        self.assertEqual(len(expected), len(labels))
        self.assertTrue(all(path.is_file() for path in paths))

        prolog_flags = dict(build.REL_SOURCES)['src/REL/m616dll/prolog.c']
        self.assertTrue({
            '-D_prolog=m616Dll_prolog',
            '-D_epilog=m616Dll_epilog',
            '-D_ctors=m616Dll_ctors',
            '-D_dtors=m616Dll_dtors',
        }.issubset(prolog_flags))
        expected_flags = {
            'prolog.c': [
                '-D_prolog=m616Dll_prolog', '-D_epilog=m616Dll_epilog',
                '-D_ctors=m616Dll_ctors', '-D_dtors=m616Dll_dtors',
            ] + expected,
            'm616.c': expected,
            'init.c': expected,
            'utility.c': expected,
            'cpu.c': expected,
        }
        for rel, flags, _, _ in collected:
            self.assertEqual(flags, expected_flags[Path(rel).name])

    def test_all_recovered_gameplay_units_share_the_module_namespace(self):
        decomp = Path(apply_patches.require_pinned_decomp())
        collected = collected_m616_units()
        units = {rel: flags for rel, flags, _, _ in collected}
        self.assertEqual({Path(rel).name for rel in units},
                         {'prolog.c', 'm616.c', 'init.c', 'utility.c', 'cpu.c'})
        paths = [decomp / rel for rel in units]
        paths.append(decomp / 'include/REL/m616dll.h')
        expected = native_namespace.namespace_flags(paths, 'm616')
        labels = set()
        for rel, flags in units.items():
            source = (decomp / rel).read_text(encoding='utf-8')
            labels.update(re.findall(r'\b(?:fn_1_|lbl_1_)[A-Za-z0-9_]+', source))
            self.assertFalse((ROOT / 'compat/decomp' / (rel + '.patch')).exists())
            self.assertTrue(set(expected).issubset(flags))
        self.assertEqual(set(expected),
                         {'-D' + name + '=m616_' + name for name in labels})
        for shared in ('seqman.c', 'mgtimer.c'):
            self.assertIn('src/game/' + shared, dict(build.game_sources()))

    def test_repeated_collection_does_not_mutate_static_rel_flags(self):
        baseline = [(rel, list(flags)) for rel, flags in build.REL_SOURCES
                    if rel.startswith('src/REL/m616dll/')]
        first = collected_m616_units()
        second = collected_m616_units()
        self.assertEqual(first, second)
        self.assertEqual(baseline,
                         [(rel, list(flags)) for rel, flags in build.REL_SOURCES
                          if rel.startswith('src/REL/m616dll/')])
        for _, flags, _, _ in first:
            self.assertEqual(len(flags), len(set(flags)))

    def test_real_lifecycle_precedes_unavailable_minigame_fallback(self):
        source = (ROOT / 'src/os/dll_bridge.c').read_text()
        self.assertEqual(recovered_bridge_inventory(source)['m616Dll'], (
            'dll/m616dll.rel', 'm616Dll_prolog', 'm616Dll_epilog',
            'm616Dll', 'M616', 'MP6_M616_CTORS'))
        self.assertIn('mp6_recovered_dll_by_path(path)', source)
        self.assertIn('mp6_recovered_dll_by_id(dll)', source)
        self.assertIn('mp6_dll_bridge_report_bound(recovered->name);', source)
        # Boards pick through the recovered roulette, never a forced m616.
        self.assertIn('src/board/mgcall.c', dict(build.board_sources()))

    def test_duplicate_source_basenames_never_share_an_object(self):
        objects = build.rel_object_names(build.REL_SOURCES)
        self.assertEqual(len(objects), len(set(name.casefold() for name in objects)))
        paths = dict(zip((rel for rel, _ in build.REL_SOURCES), objects))
        self.assertNotEqual(paths['src/REL/m616dll/utility.c'],
                            paths['src/REL/mdpresultdll/utility.c'])
        self.assertEqual(paths['src/REL/bootDll/boot.c'], 'rel_boot.o')
        self.assertEqual(build.rel_object_names([
            ('src/REL/a/utility.c', []), ('src/REL/b/Utility.c', [])]),
            ['rel_a_utility.o', 'rel_b_Utility.o'])
        with self.assertRaises(RuntimeError):
            build.rel_object_names([('src/REL/a/utility.c', []),
                                    ('src/REL/a/utility.c', [])])


if __name__ == '__main__':
    unittest.main()
