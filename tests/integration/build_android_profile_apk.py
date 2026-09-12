"""Local-device wrapper: same Release native code, optionally with run-as enabled.

Never a release asset. The original installed APK stays recoverable. Only the
generated manifest and APK signing change; every other entry is byte-verified.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'build/tablet-performance-20260910'
SDK = ROOT/'build/android-sdk'
BT = SDK/'build-tools/35.0.0'
ANDROID = '{http://schemas.android.com/apk/res/android}'
CERT = '409fe99c6d150808ed606878de5ea6e1914a201c20dac33ceb91a3ffdc537c3f'


def command(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True,
                          text=True, **kwargs).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--apk', type=Path, default=OUT/'original/installed.apk')
    parser.add_argument('--native', type=Path)
    parser.add_argument('--output-root', type=Path, default=OUT)
    parser.add_argument('--release-native', action='store_true',
                        help='Keep the original non-debuggable manifest; require verified Release native code')
    args = parser.parse_args()
    if args.release_native:
        if not args.native:
            parser.error('--release-native requires --native')
        import sys
        sys.path.insert(0,str(ROOT))
        from setup.lib import step_android
        step_android.verify_android_native_build_manifest(str(ROOT),'release')
        staged=ROOT/'packaging/android/app/src/main/jniLibs/arm64-v8a/libmp6game.so'
        assert hashlib.sha256(args.native.read_bytes()).digest()==hashlib.sha256(staged.read_bytes()).digest()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
        parser.error('simple local artifact name required')
    output_root = args.output_root.resolve()
    if not output_root.is_relative_to((ROOT/'build').resolve()):
        parser.error('local profiling output must remain under build/')
    output = output_root/('apk-'+args.name)
    output.mkdir(exist_ok=False)
    manifest_path = ROOT/'packaging/android/app/build/intermediates/packaged_manifests/release/processReleaseManifestForPackage/arm64-v8a/AndroidManifest.xml'
    tree = ET.parse(manifest_path)
    assert tree.getroot().get('package') == 'com.mp6.game'
    # Preserve the installed version so the original APK can be restored with
    # a normal replacement install; never require a downgrade or data wipe.
    badging = command([BT/'aapt2.exe', 'dump', 'badging', args.apk])
    metadata = re.search(r"^package: name='com.mp6.game' versionCode='(\d+)' versionName='([^']+)'", badging, re.M)
    assert metadata, 'expected the backed-up MP6 base APK'
    tree.getroot().set(ANDROID+'versionCode', metadata[1])
    tree.getroot().set(ANDROID+'versionName', metadata[2])
    tree.getroot().find('application').set(ANDROID+'debuggable', 'true')
    ET.register_namespace('android', ANDROID[1:-1])
    xml = output/'AndroidManifest.xml'
    tree.write(xml, encoding='utf-8', xml_declaration=True)
    linked = output/'manifest.apk'
    command([BT/'aapt2.exe', 'link', '-I', SDK/'platforms/android-36/android.jar',
             '--manifest', xml, '-o', linked])
    with zipfile.ZipFile(linked) as z:
        binary_manifest = z.read('AndroidManifest.xml')
    if args.release_native:
        with zipfile.ZipFile(args.apk) as z:
            binary_manifest=z.read('AndroidManifest.xml')
            assert z.read('lib/arm64-v8a/libmain.so')==(ROOT/'packaging/android/app/src/main/jniLibs/arm64-v8a/libmain.so').read_bytes()
    unsigned = output/'unsigned.apk'
    hashes = {}
    with zipfile.ZipFile(args.apk) as src, zipfile.ZipFile(unsigned, 'w') as dst:
        for info in src.infolist():
            if info.filename.startswith('META-INF/'):
                continue
            data = src.read(info)
            if info.filename == 'AndroidManifest.xml':
                data = binary_manifest
            elif info.filename == 'lib/arm64-v8a/libmp6game.so' and args.native:
                data = args.native.read_bytes()
            else:
                hashes[info.filename] = hashlib.sha256(data).hexdigest()
            dst.writestr(info, data)
    aligned, signed = output/'aligned.apk', output/('mp6-tablet-optimized-release.apk' if args.release_native else 'profile.apk')
    command([BT/'zipalign.exe', '-P', '16', '4', unsigned, aligned])
    env = os.environ.copy()
    env['JAVA_HOME'] = r'C:\Program Files\Java\jdk-22'
    command([BT/'apksigner.bat', 'sign', '--ks', r'C:\Users\Anony\.android\debug.keystore',
             '--ks-key-alias', 'androiddebugkey', '--ks-pass', 'pass:android',
             '--key-pass', 'pass:android', '--v4-signing-enabled', 'false',
             '--out', signed, aligned], env=env)
    verify = command([BT/'apksigner.bat', 'verify', '--verbose', '--print-certs', signed], env=env)
    assert CERT in verify
    command([BT/'zipalign.exe', '-c', '-P', '16', '4', signed])
    with zipfile.ZipFile(signed) as z:
        for name, digest in hashes.items():
            assert hashlib.sha256(z.read(name)).hexdigest() == digest, name
    tree_dump = command([BT/'aapt2.exe', 'dump', 'xmltree', signed, '--file', 'AndroidManifest.xml'])
    original_dump = command([BT/'aapt2.exe', 'dump', 'xmltree', args.apk, '--file', 'AndroidManifest.xml'])
    (output/'manifest.txt').write_text(tree_dump)
    (output/'original-manifest.txt').write_text(original_dump)
    assert 'android:debuggable' not in original_dump
    assert ('android:debuggable' in tree_dump) != args.release_native
    (output/'provenance.json').write_text(json.dumps(dict(
        release_asset=False, local_nondebuggable_release=args.release_native, original_apk=str(args.apk),
        original_sha256=hashlib.sha256(args.apk.read_bytes()).hexdigest(),
        native_override=str(args.native) if args.native else None,
        unchanged_entries=hashes, sha256=hashlib.sha256(signed.read_bytes()).hexdigest()), indent=2))
    print(signed)
    print('Verified', len(hashes), 'unchanged entries;',
          'local non-debuggable Release build' if args.release_native else 'local profiling wrapper only')


if __name__ == '__main__':
    main()
