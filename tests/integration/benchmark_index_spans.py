"""Alternate identical index writes; source baseline must be explicitly supplied."""
import argparse
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT),str(ROOT/'tests')]
from test_draw_batching import sources
from tests.test_frame_resource_lifetimes import function
from tools import build

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--checkpoint',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    args.out.mkdir(parents=True,exist_ok=False)
    audit=json.loads(args.checkpoint.read_text())
    baseline=function(audit['source']['lib/gx/command_processor.cpp'],'prepare_idx_buffer')
    parts=sources()
    parts['baseline_indices']=baseline.replace('prepare_idx_buffer(','prepare_idx_buffer_baseline(')
    for name,text in parts.items():(args.out/(name+'.inc')).write_text(text)
    exe=args.out/'benchmark.exe'
    subprocess.run([build.ZIG,'c++','-std=c++20','-O2','-UNDEBUG','-DMP6_INDEX_BENCHMARK','-DMP6_RENDERER_RELEASE',
        '-I'+str(args.out.resolve()),'-I'+str(ROOT/'build/android-aurora-source/include'),
        str(ROOT/'tests/native/draw_batching_selftest.cpp'),'-o',str(exe.resolve())],check=True)
    result=subprocess.run([str(exe.resolve())],capture_output=True,text=True,timeout=120)
    (args.out/'results.txt').write_text(result.stdout+result.stderr)
    result.check_returncode()
    rows=re.findall(r'INDEX prim=(\d+) count=(\d+) trial=(\d+) baseline_ns=([\d.]+) candidate_ns=([\d.]+)',result.stdout)
    assert len(rows)==180
    summary=[]
    for prim,count in sorted({(int(r[0]),int(r[1])) for r in rows}):
        selected=[r for r in rows if int(r[0])==prim and int(r[1])==count]
        before=statistics.median(float(r[3]) for r in selected)
        after=statistics.median(float(r[4]) for r in selected)
        summary.append(dict(primitive=prim,vertices=count,baseline_ns=before,candidate_ns=after,
                            reduction_percent=100*(1-after/before)))
    (args.out/'summary.json').write_text(json.dumps(summary,indent=2))
    print(json.dumps(summary,indent=2))
if __name__=='__main__':main()
