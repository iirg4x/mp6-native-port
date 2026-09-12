"""Alternating optimized CPU decoder benchmark; not a game/Android FPS measurement."""
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
from test_vertex_format_registers import compile_subject,SOURCE

parser=argparse.ArgumentParser()
parser.add_argument('--out',type=Path,required=True)
args=parser.parse_args()
output=(ROOT/args.out).resolve()
assert output.is_relative_to((ROOT/'build').resolve())
output.mkdir(parents=True,exist_ok=False)
exe,command=compile_subject(output,benchmark=True)
run=subprocess.run([str(exe),'--bench'],capture_output=True,text=True,check=True,timeout=90)
(output/'run.log').write_text(run.stdout+run.stderr)
rows=[dict(trial=int(t),writes=int(w),reference_ns=float(a),candidate_ns=float(b))
      for t,w,a,b in re.findall(r'trial=(\d+) writes=(\d+) reference_ns=([\d.]+) candidate_ns=([\d.]+)',run.stdout)]
assert len(rows)==12 and all(r['writes']==915 for r in rows)
old=statistics.median(r['reference_ns'] for r in rows)
new=statistics.median(r['candidate_ns'] for r in rows)
result=dict(scope='Windows CPU, synthetic 915-command format-setup batch with frequent repeats; not Android or whole-frame FPS',
            trials=rows,reference_median_ns=old,candidate_median_ns=new,
            reduction_percent=100*(1-new/old),compile_command=command,
            source_sha256={name:hashlib.sha256((SOURCE/name).read_bytes()).hexdigest()
                           for name in ('lib/gx/command_processor.cpp','lib/gx/gx.hpp')})
(output/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
