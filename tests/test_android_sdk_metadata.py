"""Only the two authenticated SDK Manager metadata serializations are accepted."""
import os
import unittest
from unittest import mock

from setup.lib import common, step_android


class AndroidSdkMetadataTests(unittest.TestCase):
    def verify(self, current=False, corrupt=None, explicit=False):
        records = dict(step_android._ANDROID_PLATFORM_FILES)
        tree = dict(step_android._ANDROID_BUILD_TOOLS_TREE)
        if current:
            records['package.xml'] = dict(step_android._ANDROID_PLATFORM_XML_CURRENT)
            tree = dict(step_android._ANDROID_BUILD_TOOLS_TREE_CURRENT)
        if corrupt == 'tree':
            tree['sha256'] = '0' * 64
        elif corrupt:
            records[corrupt] = dict(records[corrupt], sha256='0' * 64)
        def record(path):
            return records[os.fspath(path).replace('\\', '/').split('/android-36/', 1)[1]]
        kwargs = {}
        if explicit:
            kwargs = {'platform_files': step_android._ANDROID_PLATFORM_FILES,
                      'build_tools_tree': step_android._ANDROID_BUILD_TOOLS_TREE}
        with mock.patch.object(step_android.os.path, 'getsize', side_effect=lambda p: record(p)['bytes']), \
             mock.patch.object(step_android, '_sha256_file', side_effect=lambda p: record(p)['sha256']), \
             mock.patch.object(step_android, '_bounded_tree_fingerprint', return_value=tree):
            return step_android.verify_android_sdk('fixture-sdk', **kwargs)

    def test_both_exact_install_layouts_are_accepted(self):
        self.assertTrue(self.verify())
        self.assertTrue(self.verify(current=True))

    def test_new_metadata_does_not_hide_payload_or_metadata_mutations(self):
        for corrupt in ('android.jar', 'package.xml', 'tree'):
            with self.subTest(corrupt=corrupt), self.assertRaises(common.SetupError):
                self.verify(current=True, corrupt=corrupt)

    def test_explicit_caller_pins_do_not_gain_alternatives(self):
        with self.assertRaises(common.SetupError):
            self.verify(current=True, explicit=True)


if __name__ == '__main__':
    unittest.main()
