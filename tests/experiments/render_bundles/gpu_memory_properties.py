"""Read Vulkan memory properties from the owned S22; no device mutations."""
import hashlib
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
ADB = ROOT / 'build/android-sdk/platform-tools/adb.exe'
SERIAL = 'RZCTB00SF3W'


def query(*args):
    return subprocess.run([str(ADB), '-s', SERIAL, 'shell', *args],
                          check=True, capture_output=True, timeout=30).stdout


def main():
    assert query('getprop', 'ro.product.model').strip() == b'SM-S906E'
    raw = query('cmd', 'gpu', 'vkjson')
    data = json.loads(raw)
    selected = []

    def visit(value, path='root'):
        if isinstance(value, dict):
            for key, child in value.items():
                name = path + '.' + key
                if key in ('memory', 'memoryProperties', 'memory_properties'):
                    selected.append(dict(path=name, value=child))
                elif key in ('deviceName', 'deviceID', 'vendorID', 'driverVersion', 'apiVersion'):
                    selected.append(dict(path=name, value=child))
                else:
                    visit(child, name)
        elif isinstance(value, list):
            for index, child in enumerate(value):
                visit(child, f'{path}[{index}]')

    visit(data)
    assert selected, 'Vulkan JSON schema did not expose expected properties'
    output = ROOT / 'build/s22-bundle-abba-20260912' / f'vk-memory-{time.time_ns()}.json'
    report = dict(serial=SERIAL, model='SM-S906E', raw_sha256=hashlib.sha256(raw).hexdigest(),
                  properties=selected)
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
