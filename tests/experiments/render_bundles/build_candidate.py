"""Build an isolated optimized GX render-bundle experiment, never stage it."""
import argparse
import difflib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
OUT = ROOT / 'build/render-bundles-20260912'
sys.path.insert(0, str(HERE.parent / 'fifo_packets'))
from build_renderer import compile_flags, sha, run, write_json, inputs_for_link

def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise RuntimeError('source anchor changed: ' + old[:100])
    return text.replace(old, new)

def candidate(text):
    text = replace(text, '#include "uniform_writer.hpp"',
                   '#include "uniform_writer.hpp"\n#include "bundle_cache.hpp"')
    text = replace(text, 'static PipelineRef g_currentPipeline;',
                   'static PipelineRef g_currentPipeline;\nstatic void clear_gx_bundles();')
    text = replace(text, '  wgpu::TextureView copySourceDepthView;', '''  wgpu::TextureView copySourceDepthView;
  wgpu::TextureFormat bundleColorFormat = wgpu::TextureFormat::Undefined;
  wgpu::TextureFormat bundleDepthFormat = wgpu::TextureFormat::Undefined;''')
    text = replace(text, '  pass.copySourceDepthView = webgpu::g_depthBuffer.view;', '''  pass.copySourceDepthView = webgpu::g_depthBuffer.view;
  pass.bundleColorFormat = pass.copySourceTexture.GetFormat();
  pass.bundleDepthFormat = webgpu::g_depthBuffer.texture.GetFormat();''')
    anchor = '      .copySourceDepthView = prevPass.copySourceDepthView,'
    text = replace(text, anchor, anchor + '''
      .bundleColorFormat = prevPass.bundleColorFormat,
      .bundleDepthFormat = prevPass.bundleDepthFormat,''', count=3)
    text = replace(text, '  shutdown_pipeline_cache();',
                   '  clear_gx_bundles();\n  shutdown_pipeline_cache();')
    anchor = 'static void render_pass(const wgpu::RenderPassEncoder& pass, FramePacket& frame, const RenderPass& passInfo) {\n'
    text = replace(text, anchor, (HERE/'bundle_renderer.inc').read_text()+'\n'+anchor)
    text = replace(text, '  g_currentPipeline = UINTPTR_MAX;\n#ifdef AURORA_GFX_DEBUG_GROUPS',
                   '  g_currentPipeline = UINTPTR_MAX;\n  bundle_frame(frame.frameIndex);\n#ifdef AURORA_GFX_DEBUG_GROUPS')
    text = replace(text, '  for (const auto& cmd : passInfo.commands) {', '''  for (size_t commandIndex = 0; commandIndex < passInfo.commands.size(); ++commandIndex) {
    const auto& cmd = passInfo.commands[commandIndex];
    const auto bundled = try_gx_bundle(pass, frame, passInfo, commandIndex);
    if (bundled.count) {
      g_bundleAllDraws += bundled.count;
      if (bundled.executed) {
        // ExecuteBundles clears these WebGPU states, but not viewport/scissor
        // or blend constant. Keep Aurora's shadows in agreement with Dawn.
        g_currentPipeline = UINTPTR_MAX;
        gxState = {};
        pass.SetBindGroup(0, g_staticBindGroup);
        pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);
      } else {
        for (size_t i = 0; i < bundled.count; ++i)
          gx::render(passInfo.commands[commandIndex+i].data.draw.gx, pass, gxState);
      }
      commandIndex += bundled.count - 1;
      continue;
    }''')
    text = replace(text, '      case ShaderType::GX:\n        gx::render(draw.gx, pass, gxState);',
                   '      case ShaderType::GX:\n        ++g_bundleAllDraws;\n        gx::render(draw.gx, pass, gxState);')
    return text

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('platform', choices=['windows', 'android'])
    parser.add_argument('--revision', default='r1')
    args = parser.parse_args()
    if not re.fullmatch(r'r[0-9]+', args.revision): raise RuntimeError('invalid revision')
    windows = args.platform == 'windows'
    source = ROOT/'build'/('aurora-release-source' if windows else 'android-aurora-source')
    tree = ROOT/'build'/('aurora-release' if windows else 'android-aurora')
    prior = ROOT/'build/fifo-packets-20260912'/args.platform
    proof = json.loads((prior/'provenance.json').read_text())
    for path, digest in {**proof['production_unchanged'], **proof['artifacts']}.items():
        if sha(path) != digest: raise RuntimeError('authenticated input changed: '+path)
    dest = OUT/(args.platform+'-'+args.revision)
    if dest.exists(): raise RuntimeError('use a new revision, preserve existing experiment evidence')
    relative = 'lib/gfx/common.cpp'
    original = (source/relative).read_text()
    modified = candidate(original)
    dest.mkdir(parents=True)
    copy = dest/'source'
    watched = dict(proof['production_unchanged'])
    for path in inputs_for_link(proof['link']): watched[str(path)] = sha(path)
    authored = {str(p): sha(p) for p in HERE.iterdir() if p.is_file()}
    write_json(dest/'owner.json', dict(platform=args.platform, revision=args.revision,
        original_sha256=sha(source/relative), authored=authored,
        parent_provenance_sha256=sha(prior/'provenance.json')))
    for top in ('lib', 'include'): shutil.copytree(source/top, copy/top)
    (copy/relative).write_text(modified)
    shutil.copy2(HERE/'bundle_cache.hpp', copy/'lib/gfx/bundle_cache.hpp')
    (dest/'renderer.patch').write_text(''.join(difflib.unified_diff(
        original.splitlines(True), modified.splitlines(True), fromfile='a/'+relative, tofile='b/'+relative)))
    for top in ('lib', 'include'):
        for path in (source/top).rglob('*'):
            if path.is_file() and path.relative_to(source).as_posix() != relative:
                if sha(path) != sha(copy/path.relative_to(source)): raise RuntimeError('private source drift')
    flags = [a.replace(source.as_posix(), copy.as_posix())
             for a in compile_flags((tree/'build.ninja').read_text(), source/relative)]
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT/'build/release-toolchain/zig-cache'),
               ZIG_LOCAL_CACHE_DIR=str(ROOT/'build/release-toolchain/zig-cache'))
    if windows:
        driver = proof['compile'][0][:4]
        archiver = [driver[0], 'ar']
        obj = dest/'common.cpp.obj'
        output = dest/'release/mp6native.exe'
        link_driver = [driver[0], 'c++']
    else:
        driver = proof['compile'][0][:2]
        ndk = Path(driver[0]).parent
        archiver = [ndk/'llvm-ar.exe']
        obj = dest/'common.cpp.o'
        output = dest/'libmp6game.so'
        link_driver = [driver[0]]
    test = dest/('cache-test.exe' if windows else 'cache-test')
    test_command = [*driver, '-std=c++20', '-O2', '-DNDEBUG',
                    str(HERE/'cache_selftest.cpp'), '-o', str(test)]
    if not windows: test_command.insert(len(driver), '-static-libstdc++')
    run(test_command, cwd=ROOT, env=env)
    test_result = run([test]).stdout if windows else 'Cross-compiled only; not executed on device.'
    (dest/'cache-test.log').write_text(test_result)
    command = [*driver, *flags, '-c', str(copy/relative), '-o', str(obj)]
    try: result = run(command, cwd=tree, env=env)
    except subprocess.CalledProcessError as error:
        (dest/'compile.log').write_text(error.stdout+error.stderr); raise
    (dest/'compile.log').write_text(result.stdout+result.stderr)
    archive = dest/'libaurora_gx.a'
    shutil.copy2(tree/'libaurora_gx.a', archive) # Stock 0053; no other experiments.
    if run([*archiver, 't', archive]).stdout.splitlines().count(obj.name) != 1:
        raise RuntimeError('archive member not unique')
    run([*archiver, 'r', archive, obj])
    link = [str(archive) if Path(a) == prior/'libaurora_gx.a' else a for a in proof['link']]
    if link.count(str(archive)) < 1: raise RuntimeError('archive substitution failed')
    link[link.index('-o')+1] = str(output)
    output.parent.mkdir(exist_ok=True)
    response = dest/'link.rsp'
    response.write_text('\n'.join('"'+a.replace('\\','\\\\').replace('"','\\"')+'"'
                                 for a in (link if windows else link[1:]))+'\n')
    try: result = run([*link_driver, '@'+str(response)], cwd=ROOT, env=env)
    except subprocess.CalledProcessError as error:
        (dest/'link.log').write_text(error.stdout+error.stderr); raise
    (dest/'link.log').write_text(result.stdout+result.stderr)
    artifacts = {str(p): sha(p) for p in (archive, output, test)}
    if windows:
        for path in (prior/'release').glob('*.dll'): shutil.copy2(path, output.parent/path.name)
    else:
        stripped = dest/'stripped/libmp6game.so'
        stripped.parent.mkdir(exist_ok=True)
        run([ndk/'llvm-strip.exe', '--strip-unneeded', '-o', stripped, output])
        artifacts[str(stripped)] = sha(stripped)
    for path, digest in watched.items():
        if sha(path) != digest: raise RuntimeError('production input changed: '+path)
    for path, digest in authored.items():
        if sha(path) != digest: raise RuntimeError('experiment input changed during build: '+path)
    write_json(dest/'provenance.json', dict(adopted=False, experimental_only=True,
        artifacts=artifacts, production_unchanged=watched, authored=authored,
        compile=[str(a) for a in command], link=link,
        cache_test_command=[str(a) for a in test_command], cache_test_result=test_result,
        patch_sha256=sha(dest/'renderer.patch'), android_fps_gain_proven=False))
    print(args.platform+': private render-bundle Release experiment linked; production unchanged.')

if __name__ == '__main__': main()
