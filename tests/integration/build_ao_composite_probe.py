"""Isolate AO composition cost on Android; never replace production artifacts.

The local-only probe may omit the final AO draw. All depth, coverage, GTAO and
denoise work remains scheduled. This is an ablation, not an optimization or a
release asset. Production sources, objects and native libraries remain intact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'build'))
from android_local import build, configure, step_android


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class LinkCaptured(Exception):
    pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.session):
        parser.error('simple session name required')
    configure()
    step_android.verify_android_native_build_manifest(ROOT, 'release')
    out = ROOT / 'build' / args.session / 'probe'
    out.mkdir(parents=True, exist_ok=False)
    source = ROOT / 'src/gx/ambient_occlusion.cpp'
    obj = ROOT / 'build/android/obj/plat_ambient_occlusion_aurora.o'
    native = ROOT / 'build/android/aurora/libmp6game.so'
    staged = ROOT / 'packaging/android/app/src/main/jniLibs/arm64-v8a/libmp6game.so'
    watched = {str(path): digest(path) for path in (source, obj, native, staged)}
    manifest = json.loads((native.parent / 'native-build-manifest.json').read_text())
    text = source.read_text()
    anchor = '''void composite(const gfx::DrawContext &ctx, const wgpu::RenderPassEncoder &pass,
               const void *payload, size_t, void *) {
    auto &job = payload_job(payload);
'''
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + '''    static const bool skipComposite = [] {
        const char *value = std::getenv("MP6_PROFILE_SKIP_AO_COMPOSITE");
        const bool skip = value && std::strcmp(value, "1") == 0;
        std::fprintf(stderr, "[MP6-COMPOSITE-PROBE] skip=%d local-diagnostic-only\\n", int(skip));
        return skip;
    }();
    if (skipComposite) {
        ++g_composited;
        job.available.store(true, std::memory_order_release);
        return;
    }
''')
    generated = out / source.name
    generated.write_text(text)
    record = json.loads(Path(str(obj) + '.cmd.json').read_text())
    compile_cmd = list(record['command'])
    assert compile_cmd.count('-O2') == 1 and '-O0' not in compile_cmd
    assert Path(compile_cmd[compile_cmd.index('-c') + 1]) == source
    compile_cmd[compile_cmd.index('-c') + 1] = str(generated)
    compile_cmd[compile_cmd.index('-o') + 1] = str(out / obj.name)
    if '-MF' in compile_cmd:
        compile_cmd[compile_cmd.index('-MF') + 1] = str(out / 'probe.d')
    compile_cmd += ['-iquote', str(source.parent)]
    subprocess.run(compile_cmd, cwd=ROOT, check=True)
    captured = []
    run = subprocess.run

    def capture(command, *a, **kw):
        if '-shared' in command and '-o' in command and Path(command[command.index('-o') + 1]) == native:
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
    assert captured, 'production validation failed before link'
    assert hashlib.sha256(json.dumps(captured, separators=(',', ':')).encode()).hexdigest() == manifest['link_command_sha256']
    link = [str(out / obj.name) if Path(arg) == obj else arg for arg in captured]
    assert link.count(str(out / obj.name)) == 1
    link[link.index('-o') + 1] = str(out / native.name)
    run(link, cwd=ROOT, check=True)
    ndk = Path(compile_cmd[0]).parent
    stripped = out / 'stripped/libmp6game.so'
    stripped.parent.mkdir()
    run([str(ndk / 'llvm-strip.exe'), '--strip-unneeded', '-o', str(stripped), str(out / native.name)], check=True)
    assert all(digest(path) == value for path, value in watched.items())
    (out / 'provenance.json').write_text(json.dumps(dict(
        release_asset=False, diagnostic='omit final AO composition only when explicitly enabled',
        unchanged_production=watched, compile=compile_cmd, link=link,
        probe_source_sha256=digest(generated), stripped_sha256=digest(stripped)), indent=2))
    print('Local ablation probe, NOT a release asset:', stripped)


if __name__ == '__main__':
    main()
