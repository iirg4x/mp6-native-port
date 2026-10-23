"""The shared QA builder must not silently omit requested instrumentation."""
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

from tests.integration import build_overlay_probe


class OverlayProbeTests(unittest.TestCase):
    def exercise(self, key, duplicate=False):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'build').mkdir()
            source = root / 'instDll' / 'setup.c'
            source.parent.mkdir()
            source.write_text('original')
            unit = (str(source), [], 'setup.o', 'c')
            fake = types.SimpleNamespace(
                OBJ_DIR=str(root / 'objects'),
                configure_windows=lambda configuration: None,
                collect_units=lambda: [unit] * (2 if duplicate else 1),
            )
            original_configure = fake.configure_windows
            original_collect = fake.collect_units
            original_argv = sys.argv

            def main():
                fake.configure_windows('release')
                units = fake.collect_units()
                self.assertEqual(Path(units[0][0]).read_text(), 'observed')
                return 0

            fake.main = main
            with patch.object(build_overlay_probe, 'ROOT', root), \
                    patch.dict(sys.modules, {'build': fake}), \
                    patch.dict('os.environ', {}, clear=True), \
                    patch.object(sys, 'path', list(sys.path)):
                try:
                    return build_overlay_probe.build_probe(
                        'probe', {key: lambda text: 'observed'})
                finally:
                    self.assertIs(fake.configure_windows, original_configure)
                    self.assertIs(fake.collect_units, original_collect)
                    self.assertIs(sys.argv, original_argv)

    def test_exact_source_is_instrumented(self):
        self.assertEqual(self.exercise(('instDll', 'setup.c')), 0)

    def test_case_mismatch_is_an_error_even_on_windows(self):
        with self.assertRaisesRegex(RuntimeError, 'QA sources not found'):
            self.exercise(('instdll', 'setup.c'))

    def test_duplicate_source_is_an_error(self):
        with self.assertRaisesRegex(RuntimeError, 'matched more than once'):
            self.exercise(('instDll', 'setup.c'), duplicate=True)


if __name__ == '__main__':
    unittest.main()
