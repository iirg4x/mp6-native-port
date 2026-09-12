"""Native invariant tests for an unadopted, recording-thread pipeline memo."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
OUT=ROOT/'build/ready-pipeline-memo-20260912'

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    OUT.mkdir(exist_ok=True)
    zig=Path(json.loads((ROOT/'build/fifo-nop-20260912/provenance.json').read_text())['compile'][0])
    ndk=ROOT/'build/android-sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
    env=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(ROOT/'build/release-toolchain/zig-cache'),
             ZIG_LOCAL_CACHE_DIR=str(ROOT/'build/release-toolchain/zig-cache'))
    inputs={str(p):sha(p) for p in (HERE/'selftest.cpp',HERE/'ready_memo.hpp')}
    common=['-std=c++20','-O2','-DNDEBUG',str(HERE/'selftest.cpp')]
    commands=[[str(zig),'c++','-target','x86_64-windows-gnu',*common,'-o',str(OUT/'selftest.exe')],
              [str(ndk),'-target','aarch64-linux-android28','-static-libstdc++',*common,'-o',str(OUT/'selftest')]]
    for command in commands:
        result=subprocess.run(command,env=env,capture_output=True,text=True)
        if result.returncode: raise RuntimeError(result.stdout+result.stderr)
    result=subprocess.run([OUT/'selftest.exe'],capture_output=True,text=True,check=True).stdout
    assert result.startswith('PASS:')
    assert all(sha(Path(p))==value for p,value in inputs.items())
    proof=dict(production_modified=False,integrated=False,android_executed=False,fps_evidence=False,
               inputs=inputs,commands=commands,windows_output=result,
               binaries={str(p):sha(p) for p in (OUT/'selftest.exe',OUT/'selftest')})
    (OUT/'native-tests.json').write_text(json.dumps(proof,indent=2))
    print(result,end='')
    print('Model tests only; actual renderer integration and performance remain unverified.')

if __name__=='__main__': main()
