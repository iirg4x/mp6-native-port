"""Resolve this diagnostic's app-only samples against its exact ELF build ID."""
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[3]
RUN=ROOT/'build/s22-ready-memo-20260912/normal-off'
NDK=ROOT/'build/android-sdk/ndk/27.3.13750724'
sys.path.insert(0,str(NDK/'simpleperf'))
from simpleperf_report_lib import ReportLib

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    proof=json.loads((RUN/'cpu-provenance.json').read_text())
    record=RUN/'perf.data'
    assert sha(record)==proof['perf_sha256']
    native=Path(proof['unstripped_elf'])
    build=ROOT/'build/ready-pipeline-diagnostic-20260912/provenance.json'
    assert sha(build)==proof['native_proof_sha256']
    assert sha(native)==json.loads(build.read_text())['artifacts'][str(native)]
    reader=ReportLib(); reader.SetRecordFile(str(record)); reader.ShowIpForUnknownSymbol()
    samples=[]; addresses=set(); paths=set(); total=0
    while sample:=reader.GetNextSample():
        leaf=reader.GetSymbolOfCurrentSample()
        chain=reader.GetCallChainOfCurrentSample()
        frames=[]
        for item in [leaf]+[chain.entries[i].symbol for i in range(chain.nr)]:
            if item.dso_name.endswith('/libmp6game.so'):
                paths.add(item.dso_name)
                key=hex(item.vaddr_in_file); addresses.add(key)
            else:
                key=item.symbol_name+' ['+Path(item.dso_name).name+']'
            frames.append(key)
        samples.append((sample.period,sample.tid,sample.thread_comm,frames))
        total+=sample.period
    assert len(paths)==1 and total>0
    recorded=reader.GetBuildIdForPath(paths.pop()).removeprefix('0x').lower()
    reader.Close()
    llvm=NDK/'toolchains/llvm/prebuilt/windows-x86_64/bin'
    notes=subprocess.check_output([str(llvm/'llvm-readelf.exe'),'-n',str(native)],text=True)
    actual=re.search(r'Build ID: ([0-9a-f]+)',notes)[1]
    assert recorded.startswith(actual) and not recorded[len(actual):].strip('0')
    ordered=sorted(addresses)
    result=subprocess.run([str(llvm/'llvm-addr2line.exe'),'-f','-C','-e',str(native)],
                          input='\n'.join(ordered)+'\n',capture_output=True,text=True,check=True)
    lines=result.stdout.splitlines()
    assert len(lines)==2*len(ordered)
    names={address:lines[i*2] for i,address in enumerate(ordered)}
    own=Counter(); inclusive=Counter(); threads=Counter(); clocks=Counter(); copies=Counter()
    for weight,tid,comm,frames in samples:
        resolved=[names.get(key,key) for key in frames]
        own[resolved[0]]+=weight
        for name in set(resolved): inclusive[name]+=weight
        threads[(tid,comm)]+=weight
        if 'clock_gettime' in resolved[0]: clocks[tuple(resolved[:9])]+=weight
        if 'memcpy' in resolved[0] or 'memmove' in resolved[0]: copies[tuple(resolved[:9])]+=weight
    report=dict(samples=len(samples),sampled_user_cycles=total,native_build_id=actual,
                native_sha256=sha(native),perf_sha256=sha(record),fps_evidence=False,
                diagnostic_overhead_present=True,
                scope='Sampled active user-space CPU cycles; inclusive percentages overlap and must not be summed.',
                self=[dict(symbol=name,percent=100*weight/total) for name,weight in own.most_common(40)],
                inclusive=[dict(symbol=name,percent=100*weight/total) for name,weight in inclusive.most_common(70)],
                clock_callers=[dict(frames=list(frames),percent=100*weight/total) for frames,weight in clocks.most_common(15)],
                copy_callers=[dict(frames=list(frames),percent=100*weight/total) for frames,weight in copies.most_common(15)],
                threads=[dict(tid=tid,name=name,percent=100*weight/total) for (tid,name),weight in threads.most_common()])
    (RUN/'cpu-report.json').write_text(json.dumps(report,indent=2))
    print(json.dumps({**report,'self':report['self'][:8],'inclusive':report['inclusive'][:8],
                      'clock_callers':report['clock_callers'][:6],'copy_callers':report['copy_callers'][:3]},indent=2))

if __name__=='__main__': main()
