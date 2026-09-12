"""Run only the authenticated cache fixture on the owned S22 and remove it."""
import hashlib
import json
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tests/integration'))
from android_device_profile import adb,shell
SERIAL = 'RZCTB00SF3W'
OUT = ROOT/'build/s22-render-bundles-20260912'
REMOTE = '/data/local/tmp/mp6-bundle-cache-r1'
local = ROOT/'build/render-bundles-20260912/android-r1/cache-test'
digest = hashlib.sha256(local.read_bytes()).hexdigest()
proof = json.loads((local.parent/'provenance.json').read_text())
assert proof['artifacts'][str(local)] == digest
assert shell(SERIAL,'getprop','ro.product.model') == 'SM-S906E'
assert shell(SERIAL,'test','-e',REMOTE,check=False) == ''
assert adb(SERIAL,'shell','test -e '+REMOTE,check=False).returncode != 0
adb(SERIAL,'push',str(local),REMOTE)
try:
    assert shell(SERIAL,'sha256sum',REMOTE).split()[0] == digest
    shell(SERIAL,'chmod','755',REMOTE)
    result = shell(SERIAL,REMOTE)
    assert result == 'PASS: 30014 bounded bundle cache checks'
finally:
    shell(SERIAL,'rm',REMOTE)
    assert adb(SERIAL,'shell','test -e '+REMOTE,check=False).returncode != 0
(OUT/'native-cache-test.json').write_text(json.dumps(dict(
    serial=SERIAL, sha256=digest, result=result, device_file_removed=True),indent=2))
print(result)
