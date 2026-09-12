"""Explicit experimental native checks; not a production feature toggle."""
import os
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
OUT=ROOT/'build/compact-instance-20260912/durable-oracles'
os.environ['MP6_DISC_ROOT']=str(ROOT/'build/disc-cache/orig/GP6E01')
os.environ['MP6_DECOMP_INC_DATA']=str(ROOT/'build/disc-cache/split/include')
sys.path.insert(0,str(ROOT/'tools'))
import build
OUT.mkdir(parents=True,exist_ok=True)
header=ROOT/'build/aurora-release-source/lib/gfx'
assert (header/'uniform_writer.hpp').is_file(), 'build the production renderer through patch 0053 first'
for name in ('compact_instance_selftest','compact_instance_upload_selftest'):
    exe=OUT/(name+'.exe')
    subprocess.run([build.ZIG,'c++','-std=c++20','-O2','-Wall','-Wextra','-I',str(header),
                    str(HERE/(name+'.cpp')),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
