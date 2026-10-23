"""Compile real index generation, draw callers and display-list parser."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools import build
from tests.test_frame_resource_lifetimes import function
from tests.test_direct_generated_indices import replay

ROOT = Path(__file__).resolve().parents[1]
RENDERER = ROOT/'third_party/aurora'


def _sources(cp, hdr, pipe, internal):
    checks = internal[internal.index('#define FATAL('):internal.index('#define DEFAULT_FATAL(')]
    buffer = hdr[hdr.index('class ByteBuffer {'):hdr.index('} // namespace aurora')]
    index_names = ('drawable_vertex_count', 'index_count', 'write_idx_buffer') \
        if 'static u32 write_idx_buffer' in cp else ('drawable_vertex_count', 'prepare_idx_buffer')
    indices = '\n'.join(function(cp, name) for name in index_names)
    if 'static u32 write_idx_buffer' in cp:
        # Fixture-only adapter for the historical topology oracle. Production
        # writes directly into the recording frame and has no scratch wrapper.
        indices += '''
static u32 prepare_idx_buffer(ByteBuffer& buf, GXPrimitive prim, u16 start, u16 count) {
    const u32 n = index_count(prim, count);
    if (n == 0) return 0;
    auto* out = reinterpret_cast<u16*>(buf.append_uninitialized(n * sizeof(u16)));
    return write_idx_buffer(out, prim, start, count);
}
'''
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


def sources():
    return _sources(
        (RENDERER/'lib/gx/command_processor.cpp').read_text(),
        (RENDERER/'lib/gfx/common.hpp').read_text(),
        (RENDERER/'lib/gx/pipeline.hpp').read_text(),
        (RENDERER/'lib/internal.hpp').read_text(),
    )


def candidate_sources():
    """Extract the same fixture from the canonical pinned Aurora patch queue."""
    return _sources(
        replay('lib/gx/command_processor.cpp'),
        replay('lib/gfx/common.hpp'),
        replay('lib/gx/pipeline.hpp'),
        replay('lib/internal.hpp'),
    )


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

    def assert_canonical_draw_topology(self):
        """Prove the replayed source still carries the direct-index draw contract."""
        cp = replay('lib/gx/command_processor.cpp')
        common = replay('lib/gfx/common.hpp')
        common_impl = replay('lib/gfx/common.cpp')
        draw_start = cp.index('static void draw_prim')
        draw_end = cp.index('static void handle_draw(', draw_start)
        draw_body = cp[draw_start:draw_end]
        indexed_start = cp.index('} else if (subCmd == GX_AURORA_DRAW_INDEXED)')
        indexed_end = cp.index('} else if (subCmd == GX_AURORA_DEBUG_GROUP_PUSH)', indexed_start)
        indexed_body = cp[indexed_start:indexed_end]
        self.assertNotIn('prepare_idx_buffer', cp)
        self.assertIn('gfx::reserve_indices', draw_body)
        self.assertNotIn('gfx::push_indices', draw_body)
        self.assertIn('gfx::push_indices', indexed_body)
        self.assertIn('IndexReservation reserve_indices(size_t length, size_t alignment)', common)
        self.assertIn('append_uninitialized(length)', common_impl)

    def test_topology_batch_boundaries_and_upload_validation(self):
        parts = candidate_sources()
        parts['buffer'] = parts['buffer'].replace('    resize(m_length + size, false);',
            '    ++bufferWrites;\n    resize(m_length + size, false);')
        self.assert_canonical_draw_topology()
        self.compile_run('draw_batching_selftest.cpp', parts)

    def test_topology_batch_boundaries_against_generated_android_snapshot(self):
        parts = sources()
        parts['buffer'] = parts['buffer'].replace('    resize(m_length + size, false);',
            '    ++bufferWrites;\n    resize(m_length + size, false);')
        self.compile_run('draw_batching_selftest.cpp', parts)

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

    def test_release_draw_guards_against_post_0077_replay(self):
        parts = candidate_sources()
        parts['buffer'] = parts['buffer'].replace('    resize(m_length + size, false);',
            '    ++bufferWrites;\n    resize(m_length + size, false);')
        self.compile_run('draw_batching_selftest.cpp', parts, ('-DMP6_RENDERER_RELEASE',))


if __name__ == '__main__':
    unittest.main()
