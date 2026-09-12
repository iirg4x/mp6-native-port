"""Run the exact ABBA accounting fixture on S22, then remove that owned binary."""
import hashlib
import json
from pathlib import Path
import sys
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tests/integration'))
from android_device_profile import adb,shell
SERIAL='RZCTB00SF3W'
REMOTE='/data/local/tmp/mp6-bundle-abba-test-20260912'
OUT=ROOT/'build/s22-bundle-abba-20260912'
local=ROOT/'build/render-bundle-abba-20260912/android/abba-test'
digest=hashlib.sha256(local.read_bytes()).hexdigest()
proof=json.loads((local.parent/'provenance.json').read_text())
assert proof['artifacts'][str(local)]==digest
assert shell(SERIAL,'getprop','ro.product.model')=='SM-S906E'
assert adb(SERIAL,'shell','test -e '+REMOTE,check=False).returncode!=0
adb(SERIAL,'push',str(local),REMOTE)
try:
    assert shell(SERIAL,'sha256sum',REMOTE).split()[0]==digest
    shell(SERIAL,'chmod','755',REMOTE)
    result=shell(SERIAL,REMOTE)
    assert result=='PASS: 307486 bundle ABBA accounting checks'
finally:
    shell(SERIAL,'rm',REMOTE)
    assert adb(SERIAL,'shell','test -e '+REMOTE,check=False).returncode!=0
(OUT/'native-abba-test.json').write_text(json.dumps(dict(serial=SERIAL,sha256=digest,result=result,
    device_file_removed=True),indent=2)+'\n')
print(result)
