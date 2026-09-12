"""Synthetic decoder-only cost; census-weighted, not a captured FIFO or FPS claim."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT),str(ROOT/'tests')]
from test_texture_metadata_reuse import compile_subject,SOURCE

parser=argparse.ArgumentParser()
parser.add_argument('--baseline-audit',type=Path,required=True)
parser.add_argument('--out',type=Path,required=True)
parser.add_argument('--candidate-source',type=Path)
args=parser.parse_args()
out=(ROOT/args.out).resolve()
if not out.is_relative_to(ROOT/'build'): raise ValueError('output must stay under build')
out.mkdir(parents=True,exist_ok=False)
cp=json.loads(args.baseline_audit.read_text())['source']['lib/gx/command_processor.cpp']
start=cp.index('    CHECK(pos + 34 <= size, "GX_AURORA_LOAD_TEXOBJ read overrun");')
end=cp.index('  } else if (subCmd == GX_AURORA_LOAD_TLUT)',start)
candidate=(args.candidate_source or SOURCE/'lib/gx/command_processor.cpp').read_text()
exe=compile_subject(out,cp[start:end],candidate)
run=subprocess.run([str(exe),'--bench'],capture_output=True,text=True,check=True,timeout=60)
(out/'run.log').write_text(run.stdout+run.stderr)
rows=[dict(trial=int(i),reference_ns=float(a),candidate_ns=float(b)) for i,a,b in
      re.findall(r'trial=(\d+) reference_ns=([\d.]+) candidate_ns=([\d.]+)',run.stdout)]
assert len(rows)==12
a=statistics.median(r['reference_ns'] for r in rows)
b=statistics.median(r['candidate_ns'] for r in rows)
result=dict(scope='Windows CPU metadata decoding only; synthetic 195-load batches with 28 exact repeats. No downstream drawing or GPU cost; not Android FPS.',
    trials=rows,reference_ns=a,candidate_ns=b,reduction_percent=100*(1-b/a),
    baseline_source_sha256=hashlib.sha256(cp.encode()).hexdigest(),
    candidate_source_sha256=hashlib.sha256(candidate.encode()).hexdigest())
(out/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
