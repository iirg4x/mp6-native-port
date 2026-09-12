"""Enable Dawn API validation in a private PC copy of the bundle prototype."""
import difflib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'fifo_packets'))
from build_renderer import compile_flags,sha,run,write_json

def main():
    out = ROOT/'build/render-bundles-20260912'
    prior = out/'windows-r1'
    proof = json.loads((prior/'provenance.json').read_text())
    for path,digest in {**proof['production_unchanged'], **proof['artifacts']}.items():
        if sha(path) != digest: raise RuntimeError('changed input: '+path)
    dest = out/'windows-validation'
    if dest.exists(): raise RuntimeError('preserve prior validation evidence')
    source = ROOT/'build/aurora-release-source'
    tree = ROOT/'build/aurora-release'
    relative = 'lib/webgpu/gpu.cpp'
    original = (source/relative).read_text()
    assert original.count('        "skip_validation",\n') == 1
    modified = original.replace('        "skip_validation",\n', '')
    dest.mkdir()
    copy = dest/'source'
    for top in ('lib','include'): shutil.copytree(prior/'source'/top,copy/top)
    (copy/relative).write_text(modified)
    (dest/'validation.patch').write_text(''.join(difflib.unified_diff(
        original.splitlines(True),modified.splitlines(True),fromfile='a/'+relative,tofile='b/'+relative)))
    ninja = (tree/'build.ninja').read_text().replace('CMakeFiles/aurora_core.dir/', 'CMakeFiles/aurora_gx.dir/')
    flags = [arg.replace(source.as_posix(), copy.as_posix()) for arg in compile_flags(ninja, source/relative)]
    driver = proof['compile'][:4]
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT/'build/release-toolchain/zig-cache'),
               ZIG_LOCAL_CACHE_DIR=str(ROOT/'build/release-toolchain/zig-cache'))
    obj = dest/'gpu.cpp.obj'
    command = [*driver,*flags,'-c',str(copy/relative),'-o',str(obj)]
    try: result = run(command,cwd=tree,env=env)
    except subprocess.CalledProcessError as error:
        (dest/'compile.log').write_text(error.stdout+error.stderr); raise
    (dest/'compile.log').write_text(result.stdout+result.stderr)
    archive = dest/'libaurora_core.a'
    shutil.copy2(tree/archive.name,archive)
    assert run([driver[0],'ar','t',archive]).stdout.splitlines().count(obj.name) == 1
    run([driver[0],'ar','r',archive,obj])
    link = [str(archive) if Path(a) == tree/archive.name else a for a in proof['link']]
    assert link.count(str(archive)) > 0
    exe = dest/'release/mp6native.exe'
    exe.parent.mkdir()
    link[link.index('-o')+1] = str(exe)
    response = dest/'link.rsp'
    response.write_text('\n'.join('"'+a.replace('\\','\\\\').replace('"','\\"')+'"' for a in link)+'\n')
    run([driver[0],'c++','@'+str(response)],cwd=ROOT,env=env)
    for path in (prior/'release').glob('*.dll'): shutil.copy2(path,exe.parent/path.name)
    for path,digest in proof['production_unchanged'].items():
        assert sha(path) == digest,path
    write_json(dest/'provenance.json',dict(validation_only=True,not_for_performance=True,
        experiment=sha(prior/'provenance.json'),compile=[str(a) for a in command],link=link,
        artifacts={str(p):sha(p) for p in (exe,archive,dest/'validation.patch')},
        production_unchanged=proof['production_unchanged']))
    print('Private Release prototype with Dawn API validation enabled; not a timing build.')

if __name__ == '__main__': main()
