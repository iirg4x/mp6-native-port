"""Private on-device ABBA diagnostic, not a release or adoption of the memo."""
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
BASE = ROOT/'build/ready-pipeline-memo-20260912/android'
OUT = ROOT/'build/ready-pipeline-diagnostic-20260912'
sys.path.insert(0, str(HERE.parent/'fifo_packets'))
from build_renderer import compile_flags, sha, run, write_json

def replace(text, old, new):
    if text.count(old) != 1: raise RuntimeError('source anchor changed: '+old[:100])
    return text.replace(old, new)

def patches(source):
    pipeline = (source/'lib/gfx/pipeline_cache.cpp').read_text()
    pipeline = '#include "diagnostic.hpp"\n#include <chrono>\n'+pipeline
    anchor = 'static ReadyPipelineMemo<gx::ShaderInfo> g_readyGxMemo;'
    pipeline = replace(pipeline, anchor, anchor+'''
static ReadyDiagFrame g_readyDiag; // GX recording thread only
ReadyDiagFrame ready_diag_snapshot() { return g_readyDiag; }
''')
    pipeline = replace(pipeline, 'if (blocking && shaderInfo != nullptr &&\n',
                       'if (blocking && shaderInfo != nullptr && ready_diag_enabled(g_readyDiag.index) &&\n')
    pipeline = replace(pipeline, '*shaderInfo)) {\n      return hash;',
                       '*shaderInfo)) {\n      ++g_readyDiag.hits;\n      return hash;')
    pipeline = replace(pipeline, 'if (blocking && pipelineReady && shaderInfo != nullptr) {',
                       'if (blocking && pipelineReady && shaderInfo != nullptr && ready_diag_enabled(g_readyDiag.index)) {')
    old = '''  return find_pipeline_impl(ShaderType::GX, config, gx::create_pipeline,
                            PipelinePriority::Blocking, std::nullopt, &info);'''
    pipeline = replace(pipeline, old, '''  const auto start = std::chrono::steady_clock::now();
  const auto result = find_pipeline_impl(ShaderType::GX, config, gx::create_pipeline,
                            PipelinePriority::Blocking, std::nullopt, &info);
  const auto end = std::chrono::steady_clock::now();
  ++g_readyDiag.calls;
  g_readyDiag.lookupNs += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  return result;''')
    pipeline = replace(pipeline, 'void begin_pipeline_frame() {',
                       'void begin_pipeline_frame() {\n  g_readyDiag = {};\n  g_readyDiag.index = current_frame();')
    common = (source/'lib/gfx/common.cpp').read_text()
    common = '#include "diagnostic.hpp"\n#include <cstdio>\n'+common
    common = replace(common, '  end_pipeline_frame();\n  ++g_frameIndex;',
                     '  end_pipeline_frame();\n  const auto readyDiag = ready_diag_snapshot();\n  ++g_frameIndex;')
    common = replace(common, '[frameSlot, stagingSlot, callback = std::move(callback)]()',
                     '[frameSlot, stagingSlot, readyDiag, callback = std::move(callback)]()')
    common = replace(common, '''    if (callback) {
      callback(encoder);
    }
    g_frameSlots.release(frameSlot);''', '''    const auto beforePresent = g_lastPresentNs.load(std::memory_order_acquire);
    if (callback) {
      callback(encoder);
    }
    const auto afterPresent = g_lastPresentNs.load(std::memory_order_acquire);
    static ReadyDiagWindow readyWindow; // render worker only
    if (readyWindow.observe(readyDiag, afterPresent != beforePresent ? afterPresent : 0)) {
      std::fprintf(stderr, "[MP6-READYMEMO] phase=%u mode=%u valid=%u frames=%u calls=%llu hits=%llu lookup_ns=%llu first_ns=%lld last_ns=%lld\\n",
        readyWindow.phase, unsigned(ready_diag_enabled(readyDiag.index)), unsigned(readyWindow.valid),
        readyWindow.frames, (unsigned long long)readyWindow.calls, (unsigned long long)readyWindow.hits,
        (unsigned long long)readyWindow.lookupNs, (long long)readyWindow.firstNs, (long long)readyWindow.lastNs);
    }
    g_frameSlots.release(frameSlot);''')
    return {'lib/gfx/pipeline_cache.cpp': pipeline, 'lib/gfx/common.cpp': common,
            'lib/gfx/diagnostic.hpp': (HERE/'diagnostic.hpp').read_text()}

def main():
    proof = json.loads((BASE/'provenance.json').read_text())
    for path, digest in {**proof['production_unchanged'], **proof['artifacts']}.items():
        if sha(path) != digest: raise RuntimeError('authenticated input changed: '+path)
    source = BASE/'source'
    modified = patches(source)
    owner = dict(parent=sha(BASE/'provenance.json'), inputs={str(p):sha(p) for p in
                  (Path(__file__),HERE/'diagnostic.hpp',HERE/'diagnostic_selftest.cpp')},
                 sources={p.relative_to(source).as_posix():sha(p) for top in ('lib','include')
                          for p in (source/top).rglob('*') if p.is_file()})
    if (OUT/'owner.json').exists():
        if json.loads((OUT/'owner.json').read_text()) != owner: raise RuntimeError('diagnostic input drift')
    else:
        OUT.mkdir(exist_ok=False)
        write_json(OUT/'owner.json',owner)
        for top in ('lib','include'): shutil.copytree(source/top,OUT/'source'/top)
        for rel, text in modified.items(): (OUT/'source'/rel).write_text(text)
    for rel,digest in owner['sources'].items():
        if rel in modified:
            if (OUT/'source'/rel).read_text()!=modified[rel]: raise RuntimeError('diagnostic drift: '+rel)
        elif sha(OUT/'source'/rel)!=digest: raise RuntimeError('source drift: '+rel)
    changes=''.join(''.join(difflib.unified_diff(
        (source/rel).read_text().splitlines(True) if (source/rel).exists() else [],text.splitlines(True),
        fromfile='a/'+rel,tofile='b/'+rel)) for rel,text in modified.items())
    (OUT/'renderer.patch').write_text(changes)
    driver=proof['compile'][:2]
    ndk=Path(driver[0]).parent
    stock=ROOT/'build/android-aurora-source'
    ninja=(ROOT/'build/android-aurora/build.ninja').read_text()
    commands=[]
    archive=OUT/'libaurora_gx.a'
    shutil.copy2(BASE/'libaurora_gx.a',archive)
    for rel in modified:
        if not rel.endswith('.cpp'): continue
        flags=[x.replace(stock.as_posix(),(OUT/'source').as_posix()) for x in compile_flags(ninja,stock/rel)]
        obj=OUT/(Path(rel).name+'.o')
        cmd=[*driver,*flags,'-c',str(OUT/'source'/rel),'-o',str(obj)]
        try: result=run(cmd,cwd=ROOT/'build/android-aurora')
        except subprocess.CalledProcessError as e:
            (OUT/(obj.name+'.log')).write_text(e.stdout+e.stderr); raise
        (OUT/(obj.name+'.log')).write_text(result.stdout+result.stderr)
        commands.append(cmd)
        if run([ndk/'llvm-ar.exe','t',archive]).stdout.splitlines().count(obj.name)!=1:
            raise RuntimeError('archive member not unique')
        run([ndk/'llvm-ar.exe','r',archive,obj])
    link=[str(archive) if Path(x)==BASE/'libaurora_gx.a' else x for x in proof['link']]
    assert str(archive) in link
    output=OUT/'libmp6game.so'
    link[link.index('-o')+1]=str(output)
    response=OUT/'link.rsp'
    response.write_text('\n'.join('"'+x.replace('\\','\\\\').replace('"','\\"')+'"' for x in link[1:])+'\n')
    result=run([driver[0],'@'+str(response)],cwd=ROOT)
    (OUT/'link.log').write_text(result.stdout+result.stderr)
    stripped=OUT/'stripped/libmp6game.so'
    stripped.parent.mkdir(exist_ok=True)
    run([ndk/'llvm-strip.exe','--strip-unneeded','-o',stripped,output])
    windows=json.loads((BASE.parent/'windows/provenance.json').read_text())
    zig=windows['compile'][0]
    env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(ROOT/'build/release-toolchain/zig-cache'),
             ZIG_LOCAL_CACHE_DIR=str(ROOT/'build/release-toolchain/zig-cache'))
    test=OUT/'diagnostic_selftest.exe'
    run([zig,'c++','-target','x86_64-windows-gnu','-std=c++20','-O2',HERE/'diagnostic_selftest.cpp','-o',test],env=env)
    test_result=run([test]).stdout
    for path,digest in proof['production_unchanged'].items():
        if sha(path)!=digest: raise RuntimeError('production changed: '+path)
    write_json(OUT/'provenance.json',dict(adopted=False,diagnostic_only=True,
       artifacts={str(p):sha(p) for p in (output,stripped,archive,test)}, compile=commands,link=link,
       owner_sha256=sha(OUT/'owner.json'),patch_sha256=sha(OUT/'renderer.patch'),
       production_unchanged=proof['production_unchanged'],selftest=test_result))
    print(test_result+'Private Release diagnostic linked; production unchanged.')

if __name__=='__main__': main()
