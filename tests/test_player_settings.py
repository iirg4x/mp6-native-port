"""Compile-time menu visibility and player-copy contracts, not Android device tests."""
from pathlib import Path
import re
import subprocess
import unittest
from tools import build

ROOT = Path(__file__).resolve().parents[1]


class PlayerSettings(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'src/gx/ui/settings.cpp').read_text(encoding='utf-8')
        # Test the actual platform branches with a real C++ preprocessor.
        # Header declarations are irrelevant to row visibility.
        source = re.sub(r'^#include .*$', '', source, flags=re.M)
        cls.variants = {}
        for platform, defines in [('desktop', ['-DMP6_AURORA_LIVE_AA']),
                                  ('android', ['-D__ANDROID__'])]:
            result = subprocess.run([build.ZIG, 'c++', '-E', '-P', '-x', 'c++',
                                     *defines, '-'], input=source, text=True,
                                    capture_output=True, cwd=ROOT, check=True)
            cls.variants[platform] = result.stdout

    def test_desktop_actions_never_appear_on_android(self):
        for text in ['Window Mode', 'Window Size', '.key = "Display"',
                     'Open Save Folder', 'Quick Save (F5)', 'Quick Load (F8)',
                     'Ultra (SSAA']:
            self.assertIn(text, self.variants['desktop'])
            self.assertNotIn(text, self.variants['android'])
        self.assertIn('Drag with one finger', self.variants['android'])
        for text in ['Touch Controls', 'Control Opacity', 'Floating Sticks', 'Edit Layout']:
            self.assertIn(text, self.variants['android'])
            self.assertNotIn(text, self.variants['desktop'])

    def test_options_have_one_logical_home(self):
        expected = {'Video': ['Preset', 'Widescreen', 'Unlocked FPS', 'Anti-Aliasing',
                              'Shadow Quality', 'Ambient Occlusion', 'VSync'],
                    'Audio': ['Master Volume', 'More Sound Effects'],
                    'Mods': ['Fast Forward', 'Free Camera'],
                    'Advanced': ['Extra Memory', 'Graphics Driver']}
        for source in self.variants.values():
            tabs = dict(re.findall(r'add_tab\("([^"]+)"(.*?)(?=add_tab\("|if \(mInGame\)|$)',
                                   source, re.S))
            for tab, keys in expected.items():
                for key in keys:
                    self.assertIn(f'.key = "{key}"', tabs[tab])
                    self.assertEqual(source.count(f'.key = "{key}"'), 1)

    def test_no_raw_paths_or_developer_help(self):
        for source in self.variants.values():
            literals = ' '.join(re.findall(r'"(?:[^"\\]|\\.)*"', source))
            for obsolete in ['Free-Run Tick', 'Content Root', 'Clear Path',
                             'frame boundary', 'texel', 'environment lever',
                             'live stacks', 'retail heaps', 'byte-identical']:
                self.assertNotIn(obsolete, literals)


if __name__ == '__main__':
    unittest.main()
