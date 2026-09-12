"""Exercise actual cache lookup code: no hit allocations, owned deferred inputs."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools import build

ROOT = Path(__file__).resolve().parents[1]


class LazyPipelineFactory(unittest.TestCase):
    def test_cache_hits_and_deferred_ownership(self):
        base = ROOT/'build/aurora-release-source/lib/gfx'
        for name in ('pipeline_cache.hpp', 'pipeline_cache.cpp', 'common.cpp'):
            self.assertEqual((base/name).read_text(),
                (ROOT/'build/android-aurora-source/lib/gfx'/name).read_text())
        source = (base/'pipeline_cache.cpp').read_text()
        start = source.index('template <typename PipelineConfig>\nstatic void publish_shader_info(')
        subject = source[start:source.index('static void pipeline_cache_abort()', start)]
        cached = source[source.index('struct CachedPipeline {'):source.index('struct PendingPipeline {')]
        entry = re.search(r'PipelineRef find_gx_pipeline\(.*?^}', source, re.S | re.M).group()
        self.assertIn('PipelinePriority::Blocking', entry)
        subject += entry
        command = (base.parent/'gx/command_processor.cpp').read_text()
        self.assertIn('gfx::find_gx_pipeline(config, info)', command)
        self.assertNotIn('build_shader_info(config.shaderConfig)', command)
        self.assertEqual(command, (ROOT/'build/android-aurora-source/lib/gx/command_processor.cpp').read_text())
        self.assertIn('g_pipelines.clear();', source[source.index('void shutdown_pipeline_cache()'):])
        for kind in ('clear', 'gx', 'rmlui'):
            definition = re.search(r'PipelineRef find_pipeline\(ShaderType type, const '+kind+
                r'::PipelineConfig& config,.*?^}', source, re.S | re.M).group()
            self.assertIn('PipelinePriority::Blocking', definition)
            self.assertNotIn('std::move', definition)
        common = (base/'common.cpp').read_text()
        for kind in ('Clear', 'GX', 'Rml'):
            self.assertNotIn('find_pipeline(ShaderType::'+kind+', config, [', common)
        with tempfile.TemporaryDirectory(prefix='lazy-pipeline-', dir=ROOT/'build') as temporary:
            directory = Path(temporary)
            (directory/'lazy_pipeline_subject.inc').write_text(subject)
            (directory/'lazy_pipeline_cached.inc').write_text(cached)
            exe = directory/'test.exe'
            result = subprocess.run([build.ZIG, 'c++', '-std=c++20', '-O2', '-UNDEBUG',
                '-I'+str(directory), str(ROOT/'tests/native/lazy_pipeline_factory_selftest.cpp'),
                '-o', str(exe)], capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
