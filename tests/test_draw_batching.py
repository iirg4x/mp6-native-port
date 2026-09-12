"""Compile real index generation, draw callers and display-list parser."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build
from tests.test_frame_resource_lifetimes import function

ROOT = Path(__file__).resolve().parents[1]
RENDERER = ROOT/'build/android-aurora-source'


def sources():
    cp = (RENDERER/'lib/gx/command_processor.cpp').read_text()
    hdr = (RENDERER/'lib/gfx/common.hpp').read_text()
    pipe = (RENDERER/'lib/gx/pipeline.hpp').read_text()
    internal = (RENDERER/'lib/internal.hpp').read_text()
    checks = internal[internal.index('#define FATAL('):internal.index('#define DEFAULT_FATAL(')]
    buffer = hdr[hdr.index('class ByteBuffer {'):hdr.index('} // namespace aurora')]
    indices = '\n'.join(function(cp, name) for name in ('drawable_vertex_count', 'prepare_idx_buffer'))
    draws = '\n'.join(function(cp, name) for name in
                      ('draw_prim', 'handle_draw_unmerged', 'push_gx_draw'))
    parts = dict(buffer=buffer, indices=indices, draws=draws, ordinary=function(cp, 'handle_draw'), checks=checks,
                 drawdata=pipe[pipe.index('struct DrawData {'):pipe.index('constexpr uint32_t GXPipeline')])
    for name, next_name in [('SIZED', 'INDEXED'), ('INDEXED', 'DEBUG_GROUP_PUSH')]:
        start = cp.index('  } else if (subCmd == GX_AURORA_DRAW_'+name+') {')
        end = cp.index('  } else if (subCmd == GX_AURORA_'+
                       ('DRAW_' if next_name=='INDEXED' else '')+next_name+') {', start+1)
        body = cp[start: end].split('{', 1)[1]
        parts[name.lower()] = 'void parse_'+name.lower()+'(const u8* data, u32& pos, u32 size, bool bigEndian) {'+body+'\n}'
    return parts


class DrawBatching(unittest.TestCase):
    def compile_run(self, fixture, parts, extra=()):
        with tempfile.TemporaryDirectory(prefix='draw-batch-', dir=ROOT/'build') as tmp:
            folder = Path(tmp)
            for name, text in parts.items():
                (folder/(name+'.inc')).write_text(text)
            exe = folder/'test.exe'
            result = subprocess.run([build.ZIG, 'c++', '-std=c++20', '-O2', '-UNDEBUG',
                '-I'+str(folder), '-I'+str(RENDERER/'include'), *extra,
                str(ROOT/'tests/native'/fixture), '-o', str(exe)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_topology_batch_boundaries_and_upload_validation(self):
        parts = sources()
        parts['buffer'] = parts['buffer'].replace('    resize(m_length + size, false);',
            '    ++bufferWrites;\n    resize(m_length + size, false);')
        self.compile_run('draw_batching_selftest.cpp', parts)
        for unit in ('lib/gx/command_processor.cpp', 'lib/gfx/common.hpp',
                     'lib/gx/pipeline.hpp', 'lib/gx/dl.cpp'):
            self.assertEqual((RENDERER/unit).read_text(),
                             (ROOT/'build/aurora-release-source'/unit).read_text(), unit)

    def test_display_list_reader_overflow_and_valid_optimization(self):
        dl = (RENDERER/'lib/gx/dl.cpp').read_text()
        dl = dl.replace('#include "../internal.hpp"', '').replace('#include "gx.hpp"', '')
        attrs = (RENDERER/'lib/gx/attr_fmt.cpp').read_text()
        attrs = '\n'.join(function(attrs, name) for name in ('comp_type_size', 'comp_cnt_count'))
        self.compile_run('draw_reader_selftest.cpp', {'reader': dl, 'attrs': attrs})

    def test_release_draw_guards_with_actual_renderer_macros(self):
        parts = sources()
        parts['buffer'] = parts['buffer'].replace('    resize(m_length + size, false);',
            '    ++bufferWrites;\n    resize(m_length + size, false);')
        self.compile_run('draw_batching_selftest.cpp', parts, ('-DMP6_RENDERER_RELEASE',))


if __name__ == '__main__':
    unittest.main()
