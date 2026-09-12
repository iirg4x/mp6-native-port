"""Build only the ready-pipeline memo in private Release renderer copies."""
import argparse
import difflib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
OUT=ROOT/'build/ready-pipeline-memo-20260912'
sys.path.insert(0,str(HERE.parent/'fifo_packets'))
from build_renderer import compile_flags,sha,run,write_json

def replace(text,old,new):
    if text.count(old)!=1: raise RuntimeError('source anchor changed: '+old[:80])
    return text.replace(old,new)

def candidate(text):
    helper=(HERE/'ready_memo.hpp').read_text().replace('#pragma once\n','')
    text=replace(text,'namespace aurora::gfx {',helper+'\nnamespace aurora::gfx {')
    anchor='static PipelineRef g_lastPipelineRef = std::numeric_limits<PipelineRef>::max();'
    text=replace(text,anchor,anchor+'\nstatic ReadyPipelineMemo<gx::ShaderInfo> g_readyGxMemo;')
    anchor='  const uint32_t firstFrameUsed = firstFrameUsedOverride.value_or(current_frame());'
    text=replace(text,anchor,anchor+'''
  if constexpr (std::is_same_v<PipelineConfig, gx::PipelineConfig>) {
    if (blocking && shaderInfo != nullptr &&
        g_readyGxMemo.get(hash, align_uniform(1), firstFrameUsed, *shaderInfo)) {
      return hash;
    }
  }''')
    anchor='''    if (pipelineIt != g_pipelines.end()) {
      publish_shader_info(config, pipelineIt->second, shaderInfo);'''
    text=replace(text,anchor,'''    if (pipelineIt != g_pipelines.end()) {
      pipelineReady = true;
      publish_shader_info(config, pipelineIt->second, shaderInfo);''')
    anchor='''  return hash;
}

static void pipeline_cache_abort()'''
    text=replace(text,anchor,'''  if constexpr (std::is_same_v<PipelineConfig, gx::PipelineConfig>) {
    // A shutdown wake without a ready pipeline is deliberately not memoized.
    if (blocking && pipelineReady && shaderInfo != nullptr) {
      g_readyGxMemo.remember(hash, align_uniform(1), firstFrameUsed, *shaderInfo);
    }
  }
  return hash;
}

static void pipeline_cache_abort()''')
    text=replace(text,'void initialize_pipeline_cache() {','void initialize_pipeline_cache() {\n  g_readyGxMemo.clear();')
    text=replace(text,'  g_pipelines.clear();','  g_readyGxMemo.clear();\n  g_pipelines.clear();')
    return text

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('platform',choices=['windows','android'])
    args=parser.parse_args()
    windows=args.platform=='windows'
    source=ROOT/'build'/('aurora-release-source' if windows else 'android-aurora-source')
    tree=ROOT/'build'/('aurora-release' if windows else 'android-aurora')
    prior=ROOT/'build/fifo-packets-20260912'/args.platform
    proof=json.loads((prior/'provenance.json').read_text())
    for path,digest in {**proof['production_unchanged'],**proof['artifacts']}.items():
        if sha(path)!=digest: raise RuntimeError('authenticated input changed: '+path)
    native_proof=json.loads((OUT/'native-tests.json').read_text())
    for path,digest in native_proof['inputs'].items():
        if sha(path)!=digest: raise RuntimeError('native test input changed: '+path)
    dest=OUT/args.platform
    dest.mkdir(exist_ok=True)
    relative='lib/gfx/pipeline_cache.cpp'
    original=(source/relative).read_text()
    modified=candidate(original)
    patch=''.join(difflib.unified_diff(original.splitlines(True),modified.splitlines(True),
                                     fromfile='a/'+relative,tofile='b/'+relative))
    owner=dict(original_sha256=sha(source/relative),helper_sha256=sha(HERE/'ready_memo.hpp'),
               parent_provenance_sha256=sha(prior/'provenance.json'),platform=args.platform)
    copy=dest/'source'
    if (dest/'owner.json').exists():
        if json.loads((dest/'owner.json').read_text())!=owner: raise RuntimeError('owned build input drift')
        if (copy/relative).read_text()!=modified: raise RuntimeError('private source drift')
    else:
        if copy.exists(): raise RuntimeError('unowned source directory exists')
        write_json(dest/'owner.json',owner)
        for top in ('lib','include'): shutil.copytree(source/top,copy/top)
        (copy/relative).write_text(modified)
    (dest/'renderer.patch').write_text(patch)
    for top in ('lib','include'):
        for path in (source/top).rglob('*'):
            if path.is_file() and path.relative_to(source).as_posix()!=relative:
                if sha(path)!=sha(copy/path.relative_to(source)): raise RuntimeError('private input drift: '+str(path))
    flags=[a.replace(source.as_posix(),copy.as_posix())
           for a in compile_flags((tree/'build.ninja').read_text(),source/relative)]
    env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(ROOT/'build/release-toolchain/zig-cache'),
             ZIG_LOCAL_CACHE_DIR=str(ROOT/'build/release-toolchain/zig-cache'))
    if windows:
        driver=proof['compile'][0][:4]
        archiver=[driver[0],'ar']
        obj=dest/'pipeline_cache.cpp.obj'
        output=dest/'release/mp6native.exe'
        link_driver=[driver[0],'c++']
    else:
        driver=proof['compile'][0][:2]
        ndk=Path(driver[0]).parent
        archiver=[ndk/'llvm-ar.exe']
        obj=dest/'pipeline_cache.cpp.o'
        output=dest/'libmp6game.so'
        link_driver=[driver[0]]
    command=[*driver,*flags,'-c',str(copy/relative),'-o',str(obj)]
    try: result=run(command,cwd=tree,env=env)
    except subprocess.CalledProcessError as error:
        (dest/'compile.log').write_text(error.stdout+error.stderr); raise
    (dest/'compile.log').write_text(result.stdout+result.stderr)
    archive=dest/'libaurora_gx.a'
    shutil.copy2(tree/'libaurora_gx.a',archive) # Stock 0053, NOT the packet writer.
    if run([*archiver,'t',archive]).stdout.splitlines().count(obj.name)!=1:
        raise RuntimeError('archive member not unique')
    run([*archiver,'r',archive,obj])
    link=[str(archive) if Path(a)==prior/'libaurora_gx.a' else a for a in proof['link']]
    if link.count(str(archive))<1: raise RuntimeError('renderer archive substitution failed')
    link[link.index('-o')+1]=str(output)
    output.parent.mkdir(exist_ok=True)
    response=dest/'link.rsp'
    response.write_text('\n'.join('"'+a.replace('\\','\\\\').replace('"','\\"')+'"'
                                  for a in (link if windows else link[1:]))+'\n')
    try: result=run([*link_driver,'@'+str(response)],cwd=ROOT,env=env)
    except subprocess.CalledProcessError as error:
        (dest/'link.log').write_text(error.stdout+error.stderr); raise
    (dest/'link.log').write_text(result.stdout+result.stderr)
    artifacts={str(output):sha(output),str(archive):sha(archive)}
    if windows:
        for path in (prior/'release').glob('*.dll'): shutil.copy2(path,output.parent/path.name)
    else:
        stripped=dest/'stripped/libmp6game.so'
        stripped.parent.mkdir(exist_ok=True)
        run([ndk/'llvm-strip.exe','--strip-unneeded','-o',stripped,output])
        artifacts[str(stripped)]=sha(stripped)
    for path,digest in proof['production_unchanged'].items():
        if sha(path)!=digest: raise RuntimeError('production changed during build: '+path)
    write_json(dest/'provenance.json',dict(adopted=False,experimental_only=True,artifacts=artifacts,
        production_unchanged=proof['production_unchanged'],patch_sha256=sha(dest/'renderer.patch'),
        native_test_proof_sha256=sha(OUT/'native-tests.json'),compile=[str(a) for a in command],link=link,
        windows_frame_equivalence_proven=False,android_fps_gain_proven=False))
    print(args.platform+': private ready-pipeline-memo Release renderer linked; stock production unchanged.')

if __name__=='__main__': main()
