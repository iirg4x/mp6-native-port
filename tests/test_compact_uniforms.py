"""Exercise production matrix selection/upload code against the full GX palette."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import build

ROOT = Path(__file__).resolve().parents[1]


class CompactUniforms(unittest.TestCase):
    def test_matrix_addressing_and_uploads(self):
        base = ROOT / 'build/aurora-release-source/lib/gx'
        source = (base/'shader_info.cpp').read_text()
        shader = (base/'shader.cpp').read_text()
        header = (base/'gx.hpp').read_text()
        for filename in ('shader_info.cpp', 'shader.cpp', 'gx.hpp', 'pipeline.hpp'):
            self.assertEqual((base/filename).read_text(),
                (ROOT/'build/android-aurora-source/lib/gx'/filename).read_text())
        select = source[source.index('  info.compactMatrices ='):source.index('  for (int i = 0; i < config.tevStageCount;', source.index('  info.compactMatrices ='))]
        textures = source[source.index('    if ((tcg.type =='):source.index('  if (config.fogType !=', source.index('    if ((tcg.type =='))]
        # Include the original sampled-coordinate loop, with the full selection,
        # static texture and post-matrix accounting bodies.
        textures = '  for (int i=0; i<8; ++i) {\n    if (!info.sampledTexCoords.test(i)) continue;\n    const auto& tcg=config.tcgs[i];\n' + textures
        upload = source[source.index('  if (info.compactMatrices) {',source.index('gfx::Range build_uniform')):source.index('  for (int i = 0; i < info.loadsTevReg.size();',source.index('gfx::Range build_uniform'))]
        post = source[source.index('  if (info.usesPTTexMtx.any()) {',source.index('gfx::Range build_uniform')):source.index('  if (info.usesFog) {',source.index('gfx::Range build_uniform'))]
        rank = re.search(r'  template <size_t N>\n  static u32 matrix_slot.*?\n  }',header,re.S).group()
        tex_index = re.search(r'        u32 texMtxIdx = .*?\n        }',shader,re.S).group()
        post_index = re.search(r'      u32 postMtxIdx = .*?\n      postMtxIdx = .*?;',shader,re.S).group()
        subject = ('struct ShaderInfo {\n'+rank+'\n};\n'
            'static void plan(const Config& config, Info& info) {\n'+select+textures+'}\n'
            'static Buffer upload(const Info& info) { Buffer buf;\n'+upload+post+'return buf; }\n'
            'static u32 texture_slot(const Tcg& tcg, const Info& info) {\n'+tex_index+'\nreturn texMtxIdx; }\n'
            'static u32 post_slot(const Tcg& tcg, const Info& info) {\n'+post_index+'\nreturn postMtxIdx; }\n')
        with tempfile.TemporaryDirectory(prefix='compact-uniforms-',dir=ROOT/'build') as temporary:
            directory = Path(temporary)
            (directory/'matrix_subject.inc').write_text(subject)
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG,'c++','-std=c++20','-O2','-UNDEBUG',
                '-I'+str(directory),str(ROOT/'tests/native/compact_uniforms_selftest.cpp'),
                '-o',str(exe)],capture_output=True,text=True,timeout=90)
            self.assertEqual(result.returncode,0,result.stderr)
            result = subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__ == '__main__': unittest.main()
