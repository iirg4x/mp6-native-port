"""Import-time safety checks for the native build driver."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class BuildImport(unittest.TestCase):
    def run_isolated(self, source):
        with tempfile.TemporaryDirectory(prefix='mp6-build-import-') as temp:
            env = os.environ.copy()
            env['MP6_DECOMP_DIR'] = str(Path(temp) / 'missing-decomp')
            pythonpath = [str(ROOT), str(ROOT / 'tools')]
            if env.get('PYTHONPATH'):
                pythonpath.append(env['PYTHONPATH'])
            env['PYTHONPATH'] = os.pathsep.join(pythonpath)
            return subprocess.run(
                [sys.executable, '-c', source],
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
                timeout=30,
            )

    def assert_isolated_import_ok(self, source):
        result = self.run_isolated(source)
        self.assertEqual(result.returncode, 0,
                         result.stdout + result.stderr)
        self.assertIn('import-ok', result.stdout)

    def test_top_level_build_import_does_not_scan_decomp(self):
        self.assert_isolated_import_ok(
            'import build; from pathlib import Path; '
            'assert not Path(build.DECOMP).exists(); print("import-ok")'
        )

    def test_package_build_import_does_not_scan_decomp(self):
        self.assert_isolated_import_ok(
            'from tools import build; '
            'from pathlib import Path; '
            'assert not Path(build.DECOMP).exists(); print("import-ok")'
        )

    def test_missing_decomp_still_fails_when_collecting(self):
        result = self.run_isolated(
            'from tools import build\n'
            'try:\n'
            '    build.collect_units(headless=True)\n'
            'except FileNotFoundError:\n'
            '    print("import-ok")\n'
            'else:\n'
            '    raise AssertionError("missing decomp was silently accepted")\n'
        )
        self.assertEqual(result.returncode, 0,
                         result.stdout + result.stderr)
        self.assertIn('import-ok', result.stdout)

    def test_setup_production_modules_import_without_running_tests(self):
        modules = [
            'setup.setup',
            'setup.lib.common',
            'setup.lib.nod_ffi',
            'setup.lib.step_android',
            'setup.lib.step_aurora',
            'setup.lib.step_build',
            'setup.lib.step_decomp',
            'setup.lib.step_disc',
            'setup.lib.step_prereqs',
            'setup.lib.step_toolchain',
            'setup.lib.test_setup_regressions',
            'tests.test_m616_integration',
            'tests.test_m670_integration',
            'tests.test_native_minigames',
            'tests.test_native_modes',
            'tools.audit_widescreen',
        ]
        source = (
            'import importlib\n'
            f'for module in {modules!r}: importlib.import_module(module)\n'
            'print("import-ok")\n'
        )
        self.assert_isolated_import_ok(source)


if __name__ == '__main__':
    unittest.main()
