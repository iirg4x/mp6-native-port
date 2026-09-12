"""Verify player data and remove only this profiling session's device files."""
import argparse
import hashlib
import json
import shlex
from android_device_profile import adb, shell, PACKAGE, USER_ROOT, session_paths


def canonical_session_path(canonical, device_root):
    return canonical in (device_root, device_root.replace('/data/user/0/', '/data/data/', 1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--model', choices=('SM-X920', 'SM-S906E'), default='SM-X920')
    parser.add_argument('--session', default='tablet-performance-20260910')
    parser.add_argument('--cleanup', action='store_true')
    args = parser.parse_args()
    if shell(args.serial, 'getprop', 'ro.product.model') != args.model:
        raise RuntimeError('connected device does not match --model')
    output, device_root, _ = session_paths(args.serial, args.model, args.session)
    if args.session != 'tablet-performance-20260910' and not (output/'session-owner.json').exists():
        raise RuntimeError('missing session owner; refusing cleanup')
    records = json.loads((output/'original-manifest.json').read_text())
    verified = {}
    for relative, digest in records.items():
        if relative == 'installed.apk':
            continue
        parts = relative.split('/')
        if not (relative == 'mp6_config.json' or relative.startswith('saves/')) or any(
                part in ('', '.', '..') or '\\' in part for part in parts):
            raise RuntimeError('invalid backup-relative path')
        data = adb(args.serial, 'exec-out', shlex.join(['cat', USER_ROOT+'/'+relative])).stdout
        actual = hashlib.sha256(data).hexdigest()
        if actual != digest:
            raise RuntimeError('Player file changed; never overwrite it: '+relative)
        verified[relative] = actual
    actual = shell(args.serial, 'find', USER_ROOT+'/saves', '-type', 'f').splitlines()
    expected = [USER_ROOT+'/'+p for p in verified if p.startswith('saves/')]
    if sorted(actual) != sorted(expected):
        raise RuntimeError('Player save inventory changed; inspect rather than restoring')
    (output/'player-data-verification.json').write_text(json.dumps(verified, indent=2))
    print('Player settings and memory-card files are byte-identical to the original backup')
    if args.cleanup:
        if shell(args.serial, 'pidof', PACKAGE, check=False):
            raise RuntimeError('Game must be closed before cleanup')
        canonical = shell(args.serial, 'run-as', PACKAGE, 'readlink', '-f', device_root)
        if not canonical_session_path(canonical, device_root):
            raise RuntimeError('isolated test directory resolves outside the expected session')
        if shell(args.serial, 'run-as', PACKAGE, 'readlink', device_root+'/GP6E01') != USER_ROOT+'/GP6E01':
            raise RuntimeError('unexpected imported-content symlink')
        # rm removes the symlink itself, never the imported content it points to.
        inventory = shell(args.serial, 'run-as', PACKAGE, 'find', device_root, '-maxdepth', '2', '-type', 'd')
        (output/'removed-device-test-directories.txt').write_text(inventory+'\n')
        shell(args.serial, 'run-as', PACKAGE, 'rm', '-r', device_root)
        if args.session == 'tablet-performance-20260910':
            for path in ('/data/local/tmp/mp6-simpleperf-20260910',
                         '/data/local/tmp/mp6-baseline-off-20260910.data',
                         '/data/local/tmp/mp6-replay-benchmark-20260911'):
                shell(args.serial, 'rm', '-f', path)
        elif args.session == 's22-performance-20260911' and args.serial == 'RZCTB00SF3W':
            shell(args.serial, 'rm', '-f', '/data/local/tmp/mp6-s22-simpleperf-20260911')
        print('Removed isolated profiling files; original game data was untouched')


if __name__ == '__main__':
    main()
