"""Resume the owned warm board only for a screenshot, then close it (not FPS)."""
from pathlib import Path
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[3]
script = ROOT/'tests/integration/android_device_profile.py'
args = ['--serial','RZCTB00SF3W','--model','SM-S906E',
        '--session','s22-render-bundles-20260912','--name','ff-candidate-smoke']
def run(action,*extra):
    return subprocess.run([sys.executable,str(script),action,*args,*extra],check=True)
run('resume','--max-start-status','1','--max-start-ap-c','40.5','--max-start-skin-c','39')
try:
    run('capture')
    run('sample')
finally:
    run('stop')
