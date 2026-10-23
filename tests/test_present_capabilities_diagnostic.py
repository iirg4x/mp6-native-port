"""Guard the present-capability diagnostic in Aurora's current source."""
from pathlib import Path
import subprocess
import unittest

from setup.lib import step_aurora
from tools import apply_patches, build


ROOT = Path(__file__).resolve().parents[1]
TARGET = 'lib/webgpu/gpu.cpp'


def replay():
    """Aurora's current gpu.cpp (third_party/aurora)."""
    return (ROOT / 'third_party/aurora' / TARGET).read_text(encoding='utf-8')


class PresentCapabilitiesDiagnostic(unittest.TestCase):
    def test_pinned_queue_logs_all_modes_without_changing_selection(self):
        source = replay()
        self.assertEqual(
            source.count('Log.info("Surface capabilities: supported-present-modes=[{}]"'),
            1,
        )
        self.assertEqual(
            source.count('Log.info("Using surface format {}, present mode {}"'),
            1,
        )
        self.assertIn(
            'supportedPresentModes += magic_enum::enum_name('
            'g_surfaceCapabilities.presentModes[i]);',
            source,
        )
        best_mode = source[source.index('wgpu::PresentMode best_present_mode(bool vsync) {'):
                           source.index('wgpu::TextureFormat to_linear(')]
        self.assertIn('return wgpu::PresentMode::Mailbox;', best_mode)
        self.assertIn('return wgpu::PresentMode::Immediate;', best_mode)
        self.assertIn('return wgpu::PresentMode::Fifo;', best_mode)


if __name__ == '__main__':
    unittest.main()
