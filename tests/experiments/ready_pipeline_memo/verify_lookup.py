"""Compile actual before/after lookup bodies with deterministic cache/worker stand-ins."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
OUT=ROOT/'build/ready-pipeline-memo-20260912'
spec=importlib.util.spec_from_file_location('memo_builder',HERE/'build_renderer.py')
builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    paths=[ROOT/'build'/name/'lib/gfx/pipeline_cache.cpp' for name in ('aurora-release-source','android-aurora-source')]
    original=paths[0].read_text();assert paths[1].read_text()==original
    changed=builder.candidate(original)
    start='template <typename PipelineConfig>\nstatic void publish_shader_info'
    stop='static void pipeline_cache_abort()'
    for label,text in (('original',original),('candidate',changed)):
        assert text.count(start)==text.count(stop)==1
        (OUT/('lookup_'+label+'.inc')).write_text(text[text.index(start):text.index(stop)])
    # Check that the real generated lifecycle clears are present, not just the fixture reset.
    assert 'void initialize_pipeline_cache() {\n  g_readyGxMemo.clear();' in changed
    assert '  g_readyGxMemo.clear();\n  g_pipelines.clear();' in changed
    inputs={str(p):sha(p) for p in [*paths,HERE/'ready_memo.hpp',HERE/'cache_fixture.inc',HERE/'lookup_selftest.cpp']}
    zig=Path(json.loads((ROOT/'build/fifo-nop-20260912/provenance.json').read_text())['compile'][0])
    ndk=ROOT/'build/android-sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
    env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(ROOT/'build/release-toolchain/zig-cache'),ZIG_LOCAL_CACHE_DIR=str(ROOT/'build/release-toolchain/zig-cache'))
    common=['-std=c++20','-O2','-DNDEBUG','-I'+str(OUT),str(HERE/'lookup_selftest.cpp')]
    commands=[[str(zig),'c++','-target','x86_64-windows-gnu',*common,'-o',str(OUT/'lookup_selftest.exe')],
              [str(ndk),'-target','aarch64-linux-android28','-static-libstdc++',*common,'-o',str(OUT/'lookup_selftest')]]
    for command in commands:
        result=subprocess.run(command,env=env,capture_output=True,text=True)
        if result.returncode:raise RuntimeError(result.stdout+result.stderr)
    result=subprocess.run([OUT/'lookup_selftest.exe'],capture_output=True,text=True,check=True).stdout
    assert result.startswith('PASS:')
    assert all(sha(Path(p))==digest for p,digest in inputs.items())
    (OUT/'lookup-tests.json').write_text(json.dumps(dict(inputs=inputs,commands=commands,output=result,
        binaries={str(p):sha(p) for p in (OUT/'lookup_selftest.exe',OUT/'lookup_selftest')},
        production_modified=False,android_executed=False,thread_race_proof=False,fps_evidence=False),indent=2))
    print(result,end='')
if __name__=='__main__':main()
