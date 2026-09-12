"""Single-process, alternating native replay-builder A/B on the attached tablet."""
from pathlib import Path
import json
import re
import statistics
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT),str(ROOT/'tests')]
from test_frame_replay_batch import replay_test_source
from android_device_profile import adb, shell

serial='R52XA02MKDF'
assert shell(serial,'getprop','ro.product.model')=='SM-X920'
assert not shell(serial,'pidof','com.mp6.game',check=False), 'Profile only with the game closed'
out=ROOT/'build/tablet-performance-20260910/replay-benchmark'
out.mkdir(exist_ok=False)
(out/'replay-under-test.inc').write_text(replay_test_source())
ndk=ROOT/'build/android-sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/windows-x86_64'
binary=out/'mp6-replay-benchmark'
subprocess.run([str(ndk/'bin/clang.exe'),'--target=aarch64-linux-android28','--sysroot='+str(ndk/'sysroot'),
    '-O2','-UNDEBUG','-DMP6_BENCHMARK','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
    '-I'+str(out),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'src/host'),
    '-I'+str(ROOT/'build/android-aurora-source/include'),
    str(ROOT/'tests/native/frame_replay_batch_selftest.c'),'-lm','-o',str(binary)],check=True)
remote='/data/local/tmp/mp6-replay-benchmark-20260911'
adb(serial,'push',str(binary),remote)
shell(serial,'chmod','755',remote)
result=shell(serial,remote,timeout=60)
(out/'results.txt').write_text(result)
rows=re.findall(r'REPLAY trial=(\d+) reference_us=([\d.]+) candidate_us=([\d.]+)',result)
assert len(rows)==40 and '400 exact replay byte/marker comparisons passed' in result
before=statistics.median(float(x[1]) for x in rows)
after=statistics.median(float(x[2]) for x in rows)
summary=dict(reference_median_us=before,candidate_median_us=after,
             reduction_percent=100*(1-after/before),trials=len(rows),replays_per_trial=200,
             scope='Synthetic native GX streams, same real builder; not whole-game FPS')
(out/'summary.json').write_text(json.dumps(summary,indent=2))
print(json.dumps(summary,indent=2))
