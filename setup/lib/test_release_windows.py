"""Windows optimized-profile isolation and artifact provenance regressions."""
import json
import pathlib
import subprocess
import sys
import unittest
from unittest import mock

from setup.lib import step_aurora

ROOT = pathlib.Path(__file__).resolve().parents[2]


class WindowsReleaseTests(unittest.TestCase):
    def profile(self, name):
        script = (
            'import json; from tools import build as b; '
            f'b.configure_windows({name!r}); '
            'print(json.dumps(dict(configuration=b.WINDOWS_CONFIGURATION, '
            'objects=b.OBJ_DIR, common=b.COMMON_FLAGS, aurora=b.AURORA_FLAGS, '
            'tree=b.AURORA_BUILD_RMLUI, libs=b._resolve_aurora_link_items(), '
            'dlls=b.AURORA_RUNTIME_DLLS, stamp=b.AURORA_ARTIFACT_STAMP)))'
        )
        return json.loads(subprocess.check_output([sys.executable, '-c', script], cwd=ROOT, text=True))

    def test_release_optimizes_both_game_and_host_without_fast_math(self):
        release = self.profile('release')
        for flavor in ('common', 'aurora'):
            for flag in ('-O2', '-fno-strict-aliasing', '-DNDEBUG', '-g', '-gcodeview'):
                self.assertIn(flag, release[flavor])
            self.assertNotIn('-ffast-math', release[flavor])
        self.assertIn('-fwrapv', release['common'])

    def test_release_is_local_and_cannot_replace_debug_artifacts(self):
        release, debug = self.profile('release'), self.profile('debug')
        self.assertEqual(pathlib.Path(release['objects']), ROOT / 'build' / 'obj-release')
        self.assertEqual(pathlib.Path(debug['objects']), ROOT / 'build' / 'obj')
        tree = ROOT / 'build' / 'aurora-release'
        self.assertEqual(pathlib.Path(release['tree']), tree)
        self.assertEqual(pathlib.Path(release['stamp']).parent, tree)
        self.assertIsNone(debug['stamp'])
        for library in release['libs']:
            if not library.startswith('-'):
                self.assertTrue(pathlib.Path(library).is_relative_to(tree), library)
        for old_name in ('libpng16d.', 'libfmtd.a', 'libfreetyped.a'):
            self.assertNotIn(old_name, repr(release['libs']) + repr(release['dlls']))
        self.assertNotIn('-O2', debug['common'])

    def test_debug_stamp_cannot_authorize_release_link(self):
        build = mock.Mock(AURORA_ARTIFACT_STAMP='local-stamp.json', WINDOWS_CONFIGURATION='release')
        stamp = {'artifact_stamp_version': step_aurora.ARTIFACT_STAMP_VERSION}
        with mock.patch.object(step_aurora, 'read_stamp', return_value=stamp) as reader:
            problems = step_aurora.verify_link_inputs('windows', build_module=build, check_fingerprint=False)
        reader.assert_called_once_with('local-stamp.json')
        self.assertEqual(len(problems), 1)
        self.assertIn('optimization configuration', problems[0])

    def test_changed_release_recipe_is_rejected_before_artifact_lookup(self):
        build = mock.Mock(AURORA_ARTIFACT_STAMP='local-stamp.json', WINDOWS_CONFIGURATION='release')
        stamp = {'artifact_stamp_version': step_aurora.ARTIFACT_STAMP_VERSION,
                 'windows_configuration': 'release', 'windows_release_fingerprint': 'old'}
        with mock.patch.object(step_aurora, 'read_stamp', return_value=stamp), \
                mock.patch.object(step_aurora, 'windows_release_fingerprint', return_value='new'), \
                mock.patch.object(step_aurora, '_profile_paths') as resolver:
            problems = step_aurora.verify_link_inputs('windows', build_module=build, check_fingerprint=False)
        resolver.assert_not_called()
        self.assertIn('release recipe/patch fingerprint mismatch', problems[0])


if __name__ == '__main__':
    unittest.main()
