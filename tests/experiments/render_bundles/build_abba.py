"""Instrument the verified bundle prototype for same-process A/B/B/A testing."""
import argparse
import difflib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
OUT = ROOT/'build/render-bundle-abba-20260912'
sys.path.insert(0,str(HERE.parent/'fifo_packets'))
from build_renderer import compile_flags,sha,run,write_json

def replace(text,old,new):
    if text.count(old) != 1: raise RuntimeError('source anchor drift: '+old[:80])
    return text.replace(old,new)

def candidate(text):
    text = '#include "abba.hpp"\n#include <cstdio>\n'+text
    text = replace(text,'static void clear_gx_bundles();', '''static void clear_gx_bundles();
static void bundle_abba_present(uint32_t index, int64_t presentNs);''')
    anchor = 'static uint64_t g_bundleMisses = 0, g_bundleRejected = 0, g_bundleAllDraws = 0;'
    text = replace(text,anchor,anchor+'''
static uint64_t g_bundleEncodeNs = 0;
static mp6_bundle_abba::Sample g_bundlePrevious;
static mp6_bundle_abba::Window g_bundleWindow;
struct BundleEncodeTimer {
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  ~BundleEncodeTimer() {
    g_bundleEncodeNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
  }
};
static void bundle_abba_present(uint32_t index, int64_t presentNs) {
  const mp6_bundle_abba::Sample total{index, g_bundleAllDraws, g_bundleHits, g_bundleBuilds, g_bundleEncodeNs};
  const mp6_bundle_abba::Sample sample{index,
      total.draws-g_bundlePrevious.draws, total.hits-g_bundlePrevious.hits,
      total.builds-g_bundlePrevious.builds, total.encodeNs-g_bundlePrevious.encodeNs};
  g_bundlePrevious = total;
  if (g_bundleWindow.observe(sample, presentNs)) {
    std::fprintf(stderr, "[MP6-BUNDLE-ABBA] phase=%u mode=%u valid=%u frames=%u draws=%llu hits=%llu builds=%llu encode_ns=%llu first_ns=%lld last_ns=%lld\\n",
        g_bundleWindow.phase, unsigned(mp6_bundle_abba::enabled(index)), unsigned(g_bundleWindow.valid),
        g_bundleWindow.frames, (unsigned long long)g_bundleWindow.draws,
        (unsigned long long)g_bundleWindow.hits, (unsigned long long)g_bundleWindow.builds,
        (unsigned long long)g_bundleWindow.encodeNs, (long long)g_bundleWindow.firstNs,
        (long long)g_bundleWindow.lastNs);
  }
}
''')
    text = replace(text,'  g_gxBundles.clear();', '''  g_gxBundles.clear();
  g_bundleEncodeNs = 0;
  g_bundlePrevious = {};
  g_bundleWindow = {};''')
    text = replace(text,'  if (passInfo.bundleColorFormat == wgpu::TextureFormat::Undefined ||',
                   '  if (!mp6_bundle_abba::enabled(frame.frameIndex)) return {};\n  if (passInfo.bundleColorFormat == wgpu::TextureFormat::Undefined ||')
    text = replace(text,'  bundle_frame(frame.frameIndex);','  BundleEncodeTimer bundleTimer;\n  bundle_frame(frame.frameIndex);')
    text = replace(text,'  const uint64_t frameId = frame.frameId;\n  g_currentRenderPass = UINT32_MAX;',
                   '  const uint64_t frameId = frame.frameId;\n  const uint32_t bundleIndex = frame.frameIndex;\n  g_currentRenderPass = UINT32_MAX;')
    text = replace(text,'[frameSlot, stagingSlot, callback = std::move(callback)]()',
                   '[frameSlot, stagingSlot, bundleIndex, callback = std::move(callback)]()')
    text = replace(text,'''    if (callback) {
      callback(encoder);
    }
    g_frameSlots.release(frameSlot);''','''    const auto beforePresent = g_lastPresentNs.load(std::memory_order_acquire);
    if (callback) {
      callback(encoder);
    }
    const auto afterPresent = g_lastPresentNs.load(std::memory_order_acquire);
    bundle_abba_present(bundleIndex, afterPresent != beforePresent ? afterPresent : 0);
    g_frameSlots.release(frameSlot);''')
    return text

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('platform',choices=['windows','android'])
    a = p.parse_args(); windows = a.platform == 'windows'
    prior = ROOT/'build/render-bundles-20260912'/(a.platform+'-r1')
    proof = json.loads((prior/'provenance.json').read_text())
    for path,digest in {**proof['production_unchanged'],**proof['artifacts']}.items():
        if sha(path) != digest: raise RuntimeError('changed input: '+path)
    source = prior/'source'; dest = OUT/a.platform
    if dest.exists(): raise RuntimeError('preserve the previous diagnostic build')
    relative = 'lib/gfx/common.cpp'
    original = (source/relative).read_text(); modified = candidate(original)
    dest.mkdir(parents=True)
    owner = dict(parent_sha256=sha(prior/'provenance.json'),
        inputs={str(p):sha(p) for p in (Path(__file__),HERE/'abba.hpp',HERE/'abba_selftest.cpp')},
        sources={str(p):sha(p) for top in ('lib','include') for p in (source/top).rglob('*') if p.is_file()})
    write_json(dest/'owner.json',owner)
    for top in ('lib','include'): shutil.copytree(source/top,dest/'source'/top)
    (dest/'source'/relative).write_text(modified)
    shutil.copy2(HERE/'abba.hpp',dest/'source/lib/gfx/abba.hpp')
    (dest/'diagnostic.patch').write_text(''.join(difflib.unified_diff(original.splitlines(True),
        modified.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative)))
    stock = ROOT/'build'/('aurora-release-source' if windows else 'android-aurora-source')
    tree = ROOT/'build'/('aurora-release' if windows else 'android-aurora')
    flags = [x.replace(stock.as_posix(),(dest/'source').as_posix())
             for x in compile_flags((tree/'build.ninja').read_text(),stock/relative)]
    driver = proof['compile'][:4 if windows else 2]
    env = dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(ROOT/'build/release-toolchain/zig-cache'),
               ZIG_LOCAL_CACHE_DIR=str(ROOT/'build/release-toolchain/zig-cache'))
    obj = dest/('common.cpp.obj' if windows else 'common.cpp.o')
    command = [*driver,*flags,'-c',str(dest/'source'/relative),'-o',str(obj)]
    try: result = run(command,cwd=tree,env=env)
    except subprocess.CalledProcessError as e:
        (dest/'compile.log').write_text(e.stdout+e.stderr); raise
    (dest/'compile.log').write_text(result.stdout+result.stderr)
    archive = dest/'libaurora_gx.a'; shutil.copy2(prior/archive.name,archive)
    archiver = [driver[0],'ar'] if windows else [Path(driver[0]).parent/'llvm-ar.exe']
    assert run([*archiver,'t',archive]).stdout.splitlines().count(obj.name) == 1
    run([*archiver,'r',archive,obj])
    link = [str(archive) if Path(x) == prior/archive.name else x for x in proof['link']]
    assert str(archive) in link
    output = dest/('release/mp6native.exe' if windows else 'libmp6game.so')
    output.parent.mkdir(exist_ok=True)
    link[link.index('-o')+1] = str(output)
    response = dest/'link.rsp'
    response.write_text('\n'.join('"'+x.replace('\\','\\\\').replace('"','\\"')+'"' for x in (link if windows else link[1:]))+'\n')
    link_driver = [driver[0],'c++'] if windows else [driver[0]]
    run([*link_driver,'@'+str(response)],cwd=ROOT,env=env)
    test = dest/('abba-test.exe' if windows else 'abba-test')
    test_command = [*driver,'-std=c++20','-O2','-DNDEBUG',str(HERE/'abba_selftest.cpp'),'-o',str(test)]
    if not windows: test_command.insert(len(driver),'-static-libstdc++')
    run(test_command,cwd=ROOT,env=env)
    test_result = run([test]).stdout if windows else 'Cross-compiled only.'
    artifacts = {str(p):sha(p) for p in (archive,output,test)}
    if windows:
        for path in (prior/'release').glob('*.dll'): shutil.copy2(path,output.parent/path.name)
    else:
        stripped = dest/'stripped/libmp6game.so'; stripped.parent.mkdir()
        run([Path(driver[0]).parent/'llvm-strip.exe','--strip-unneeded','-o',stripped,output])
        artifacts[str(stripped)] = sha(stripped)
    for path,digest in {**proof['production_unchanged'],**owner['sources'],**owner['inputs']}.items():
        if sha(path) != digest: raise RuntimeError('input drift during build: '+path)
    write_json(dest/'provenance.json',dict(adopted=False,diagnostic_only=True,artifacts=artifacts,
        compile=command,link=link,owner_sha256=sha(dest/'owner.json'),
        patch_sha256=sha(dest/'diagnostic.patch'),test_result=test_result,
        production_unchanged=proof['production_unchanged'],android_fps_gain_proven=False))
    print(a.platform+': private bundle ABBA Release diagnostic linked. '+test_result)
if __name__ == '__main__': main()
