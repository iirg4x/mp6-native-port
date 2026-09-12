"""Build an isolated O2 ThinLTO experiment from authenticated native inputs.

Production sources, object records, libraries and release manifests are never
replaced. Only game/platform translation units use ThinLTO; the existing native
renderer/dependency archives remain unchanged. This does not enable fast-math,
strict aliasing, new CPU requirements, or change the shared-library ABI policy.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'build'))
from android_local import build, configure, step_android


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class LinkCaptured(Exception):
    pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--resume-link', action='store_true', help='Resume only authenticated, completed compilation')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
        parser.error('simple local experiment name required')
    configure()
    step_android.verify_android_native_build_manifest(ROOT, 'release')
    out = ROOT/'build'/args.name
    if out.exists() and not args.resume_link:
        assert not any(p.is_file() for p in out.rglob('*')), 'Never overwrite an existing experiment'
    out.mkdir(exist_ok=True)
    (out/'obj').mkdir(exist_ok=True)
    native = ROOT/'build/android/aurora/libmp6game.so'
    staged = ROOT/'packaging/android/app/src/main/jniLibs/arm64-v8a/libmp6game.so'
    manifest_path = native.parent/'native-build-manifest.json'
    manifest = json.loads(manifest_path.read_text())
    watched = {str(p): digest(p) for p in (native, staged, manifest_path)}
    captured = []
    run = subprocess.run

    def capture(command, *a, **kw):
        if '-shared' in command and '-o' in command and Path(command[command.index('-o')+1]) == native:
            captured.extend(command)
            raise LinkCaptured()
        return run(command, *a, **kw)

    subprocess.run = capture
    sys.argv = ['tools/build.py', '--target', 'aarch64-android', '--windowed',
                '--configuration', 'release', '--link-only']
    try:
        build.main()
    except LinkCaptured:
        pass
    finally:
        subprocess.run = run
    assert captured
    assert hashlib.sha256(json.dumps(captured,separators=(',',':')).encode()).hexdigest() == manifest['link_command_sha256']
    objects = [Path(arg) for arg in captured if Path(arg).suffix == '.o']
    link_stamp = ROOT/'build/mp6_link_stamp_android_aurora.o'
    assert objects.count(link_stamp) == 1
    watched[str(link_stamp)] = digest(link_stamp)
    objects.remove(link_stamp)  # Generated provenance string, not a game/platform TU.
    assert len(objects) == manifest['compile_profile']['units'] == len({p.name for p in objects})
    commands = []
    records = {}
    for obj in objects:
        stamp = Path(str(obj)+'.cmd.json')
        record = json.loads(stamp.read_text())
        records[str(stamp)] = digest(stamp)
        cmd = list(record['command'])
        assert cmd.count('-O2') == 1 and '-O0' not in cmd and '-ffast-math' not in cmd
        assert '-fno-strict-aliasing' in cmd and not any(arg.startswith('-flto') for arg in cmd)
        source = Path(cmd[cmd.index('-c')+1])
        watched[str(source)] = digest(source)
        watched[str(obj)] = digest(obj)
        for option,suffix in (('-o',''),('-MF','.d'),('-MT','')):
            cmd[cmd.index(option)+1] = str(out/'obj'/(obj.name+suffix))
        cmd.append('-flto=thin')
        commands.append(cmd)

    def compile_one(cmd):
        result = run(cmd,cwd=ROOT,capture_output=True,text=True)
        obj = Path(cmd[cmd.index('-o')+1])
        (out/'obj'/(obj.name+'.log')).write_text(result.stdout+result.stderr)
        if result.returncode:
            raise RuntimeError('Compile failed: '+str(obj))
        assert obj.read_bytes()[:4] == b'BC\xc0\xde', 'expected ThinLTO bitcode'

    input_contract = dict(unchanged_production=watched, command_records=records, compile=commands)
    checkpoint = out/'compiled-inputs.json'
    if args.resume_link:
        saved = json.loads(checkpoint.read_text())
        assert saved['input_contract'] == input_contract
        assert all(digest(path) == value for path,value in saved['bitcode'].items())
    else:
        with ThreadPoolExecutor(max_workers=3) as pool:
            for completed,future in enumerate(as_completed([pool.submit(compile_one,cmd) for cmd in commands]),1):
                future.result()
                if completed%20 == 0 or completed == len(commands):
                    print(f'ThinLTO objects: {completed}/{len(commands)}',flush=True)
        checkpoint.write_text(json.dumps(dict(input_contract=input_contract,
            bitcode={str(out/'obj'/p.name):digest(out/'obj'/p.name) for p in objects}),indent=2))
    link = [str(out/'obj'/Path(arg).name) if Path(arg) in objects else arg for arg in captured]
    link[link.index('-o')+1] = str(out/'libmp6game.so')
    link += ['-flto=thin','-O2','-Wl,--thinlto-jobs=3',f'-Wl,--thinlto-cache-dir={out / "cache"}']
    # Clang response parsing accepts a quoted, backslash-escaped argument per
    # line. Avoid Windows' 32 KiB process-command limit without shell expansion.
    response = out/'link.rsp'
    response.write_text('\n'.join('"'+arg.replace('\\','\\\\').replace('"','\\"')+'"' for arg in link[1:])+'\n')
    result = run([link[0],'@'+str(response)],cwd=ROOT,capture_output=True,text=True)
    (out/'link.log').write_text(result.stdout+result.stderr)
    if result.returncode:
        raise RuntimeError('ThinLTO link failed; see '+str(out/'link.log'))
    ndk = Path(commands[0][0]).parent
    stripped = out/'stripped/libmp6game.so'
    stripped.parent.mkdir(exist_ok=True)
    run([str(ndk/'llvm-strip.exe'),'--strip-unneeded','-o',str(stripped),str(out/'libmp6game.so')],check=True)
    for label,path in (('baseline',native),('candidate',out/'libmp6game.so')):
        symbols = run([str(ndk/'llvm-readelf.exe'),'--dyn-syms','--wide',str(path)],capture_output=True,text=True,check=True)
        (out/(label+'-dyn-syms.txt')).write_text(symbols.stdout)
    assert all(digest(path) == value for path,value in {**watched,**records}.items())
    (out/'provenance.json').write_text(json.dumps(dict(
        release_asset=False, installed=False, scope='O2 ThinLTO game/platform only; no performance or compatibility claim yet',
        unchanged_production=watched, command_records=records, compile=commands, link=link,
        stripped_sha256=digest(stripped), bytes=stripped.stat().st_size),indent=2))
    print('Isolated ThinLTO candidate:',stripped,flush=True)


if __name__ == '__main__':
    main()
