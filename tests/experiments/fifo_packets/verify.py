"""Isolated packet-writer byte tests; no production source, APK or staging edits."""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[3]
FIXTURES = Path(__file__).resolve().parent
OUT = ROOT / 'build/fifo-packets-20260912'
SOURCE = ROOT / 'build/aurora-release-source'
ANDROID_SOURCE = ROOT / 'build/android-aurora-source'
ADB = ROOT / 'build/android-sdk/platform-tools/adb.exe'
SERIAL = 'RZCTB00SF3W'
REMOTE = '/data/local/tmp/mp6-fifo-packets-20260912'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], capture_output=True, text=True,
                          check=True, timeout=120, **kwargs)


def adb(*args):
    return run([ADB, '-s', SERIAL, *args])


def replace_macro(source, name, parameters, body):
    pattern = r'^#define ' + re.escape(name + '(' + parameters + ')') + r'[^\n]*\n(?:[^\n]*\\\n)*[^\n]*while \(0\)'
    result, count = re.subn(pattern, '#define ' + name + '(' + parameters + ') ' + body,
                            source, flags=re.MULTILINE)
    if count != 1:
        raise RuntimeError('macro source drift: ' + name)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--phone', action='store_true', help='Run standalone binary on the S22+, without changing its APK')
    args = parser.parse_args()
    OUT.mkdir(exist_ok=True)
    files = ['lib/gx/fifo.hpp', 'lib/dolphin/gx/__gx.h']
    original = {name: (SOURCE / name).read_text() for name in files}
    if any((ANDROID_SOURCE / name).read_text() != text for name, text in original.items()):
        raise RuntimeError('the source profiles differ; audit them separately')
    helper = (FIXTURES / 'packet_write.inc').read_text()
    anchor = '// Overwrites a u32 previously written to the FIFO buffer'
    if original[files[0]].count(anchor) != 1:
        raise RuntimeError('FIFO header insertion point changed')
    candidate = dict(original)
    candidate[files[0]] = original[files[0]].replace(anchor, helper + '\n' + anchor)
    gx = candidate[files[1]]
    gx = replace_macro(gx, 'GX_WRITE_RAS_REG', 'value',
        'aurora::gx::fifo::write_bp_packet(static_cast<uint32_t>(value))')
    gx = replace_macro(gx, 'GX_WRITE_CP_REG', 'addr, value',
        'aurora::gx::fifo::write_cp_packet(GX_LOAD_CP_REG, static_cast<uint8_t>(addr), static_cast<uint32_t>(value))')
    gx = replace_macro(gx, 'GX_WRITE_XF_REG', 'addr, value',
        'aurora::gx::fifo::write_xf_packet(static_cast<uint32_t>(addr), static_cast<uint32_t>(value))')
    candidate[files[1]] = gx
    # Mechanical source exports for compilation/patch inspection only.
    for label, source in [('original', original), ('candidate', candidate)]:
        text = source[files[0]]
        (OUT / ('fifo_' + label + '.inc')).write_text(text[text.index('namespace aurora::gx::fifo {'):])
        gx_prefix = source[files[1]].split('// Shadow register struct')[0]
        gx_prefix = '\n'.join(line for line in gx_prefix.splitlines()
                              if not line.startswith(('#pragma', '#include')))
        macro_names = re.findall(r'^#define (\w+)\(', gx_prefix, re.MULTILINE)
        # Compile the changed macros themselves, in addition to direct helper
        # tests. Private macro cleanup lets both real versions coexist in one TU.
        wrappers = '''
static void emit_bp(uint32_t value) { GX_WRITE_RAS_REG(value); }
static void emit_cp(uint32_t addr, uint32_t value) { GX_WRITE_CP_REG(addr,value); }
static void emit_xf(uint32_t addr, uint32_t value) { GX_WRITE_XF_REG(addr,value); }
'''
        (OUT / ('macros_' + label + '.inc')).write_text(
            'constexpr uint8_t GX_LOAD_CP_REG=0x08;\n' + gx_prefix + wrappers +
            '\n'.join('#undef ' + name for name in macro_names) + '\n')
    patch = ''.join(''.join(difflib.unified_diff(original[name].splitlines(True), candidate[name].splitlines(True),
        fromfile='a/' + name, tofile='b/' + name)) for name in files)
    (OUT / 'renderer.patch').write_text(patch)
    zig = Path(json.loads((ROOT / 'build/fifo-nop-20260912/provenance.json').read_text())['compile'][0])
    ndk = ROOT / 'build/android-sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
    native_source = FIXTURES / 'packet_selftest.cpp'
    exe = OUT / 'packet_selftest.exe'
    android = OUT / 'packet_selftest'
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT / 'build/release-toolchain/zig-cache'),
               ZIG_LOCAL_CACHE_DIR=str(ROOT / 'build/release-toolchain/zig-cache'))
    common = ['-std=c++20', '-O2', '-I' + str(OUT), str(native_source)]
    commands = [[zig, 'c++', '-target', 'x86_64-windows-gnu', *common, '-o', exe],
                [ndk, '-target', 'aarch64-linux-android28', '-static-libstdc++', *common, '-o', android]]
    inputs = [native_source, FIXTURES / 'packet_write.inc', *OUT.glob('*.inc'),
              *(SOURCE / name for name in files), *(ANDROID_SOURCE / name for name in files)]
    input_hashes = {str(path): sha(path) for path in inputs}
    for command in commands:
        try:
            run(command, env=env)
        except subprocess.CalledProcessError as error:
            print(error.stdout, error.stderr)
            raise
    local = run([exe, '--bench']).stdout
    if not local.startswith('PASS:'):
        raise RuntimeError(local)
    (OUT / 'windows-native.txt').write_text(local)
    if any(sha(Path(path)) != digest for path, digest in input_hashes.items()):
        raise RuntimeError('test input changed during compilation')
    result = dict(production_modified=False, apk_installed=False, android_fps_evidence=False,
        inputs_sha256=input_hashes,
        helper_sha256=sha(FIXTURES / 'packet_write.inc'), test_sha256=sha(native_source),
        source_sha256={name: sha(SOURCE / name) for name in files},
        patch_sha256=sha(OUT / 'renderer.patch'),
        commands=[[str(item) for item in command] for command in commands],
        windows=dict(sha256=sha(exe), output=local), android=dict(sha256=sha(android), executed=False))
    if args.phone:
        if adb('shell', 'getprop', 'ro.product.model').stdout.strip() != 'SM-S906E':
            raise RuntimeError('wrong device')
        pid = subprocess.run([str(ADB), '-s', SERIAL, 'shell', 'pidof', 'com.mp6.game'], capture_output=True, text=True)
        if pid.stdout.strip():
            raise RuntimeError('do not compete with a running game')
        package_path = adb('shell', 'pm', 'path', 'com.mp6.game').stdout.strip().removeprefix('package:')
        if not package_path.startswith('/data/app/') or '\n' in package_path:
            raise RuntimeError('unknown MP6 installation')
        before_apk = adb('shell', 'sha256sum', package_path).stdout.split()[0]
        thermal_before = adb('shell', 'dumpsys', 'thermalservice').stdout
        adb('shell', 'mkdir', REMOTE) # Existing directory is not ours to overwrite.
        remote = REMOTE + '/packet_selftest'
        try:
            adb('push', android, remote)
            if adb('shell', 'sha256sum', remote).stdout.split()[0] != sha(android):
                raise RuntimeError('standalone binary hash mismatch')
            adb('shell', 'chmod', '700', remote)
            output = adb('shell', shlex.join([remote, '--bench'])).stdout
            if output.splitlines()[0] != local.splitlines()[0]:
                raise RuntimeError('phone oracle differs')
        finally:
            adb('shell', 'rm', '-f', remote)
            adb('shell', 'rmdir', REMOTE)
        after_apk = adb('shell', 'sha256sum', package_path).stdout.split()[0]
        if before_apk != after_apk:
            raise RuntimeError('installed APK changed during the standalone test')
        result['android'].update(executed=True, output=output, installed_apk_unchanged=before_apk,
            thermal_before=thermal_before, thermal_after=adb('shell', 'dumpsys', 'thermalservice').stdout,
            temporary_binary_removed=True)
        (OUT / 'android-native.txt').write_text(output)
    (OUT / 'native-tests.json').write_text(json.dumps(result, indent=2))
    print(local)
    if args.phone:
        print(result['android']['output'])
    print('Only isolated fixtures were built; kernel timings are not game FPS evidence.')


if __name__ == '__main__':
    main()
