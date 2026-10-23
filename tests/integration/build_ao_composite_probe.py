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

ROOT = Path(__file__).resolve().parents[2]


ENV_VAR = 'MP6_PROFILE_SKIP_AO_COMPOSITE'
PROBE_MARKER = '[MP6-COMPOSITE-PROBE]'
# The draw setup lives in draw_composite, shared by the composite callback
# and the retained (interpolated-frame) composite; the probe body spans both
# up to the foliage callback, so the composite's job release still follows.
COMPOSITE_ENTRY_ANCHOR = '''void draw_composite(const gfx::DrawContext &ctx, const wgpu::RenderPassEncoder &pass,
                    const wgpu::BindGroup &bindings, bool preparedDepth, bool upstreamSignal,
                    bool canonicalDepth) {'''
COMPOSITE_SETUP_ANCHOR = '''    pass.SetViewport(0, 0, float(ctx.targetWidth), float(ctx.targetHeight), 0, 1);
    pass.SetScissorRect(0, 0, ctx.targetWidth, ctx.targetHeight);
    pass.SetPipeline(found->pipeline);'''
COMPOSITE_DRAW_ANCHOR = '''    pass.SetBindGroup(0, bindings);
    pass.Draw(3);'''
COMPOSITE_RELEASE_ANCHOR = '''    job.composite = nullptr;
    job.upstream.scratch.reset();
    job.available.store(true, std::memory_order_release);'''
FOLIAGE_ENTRY_ANCHOR = '''void foliage_seed(const gfx::DrawContext &ctx, const wgpu::RenderPassEncoder &pass,
                  const void *payload, size_t, void *) {'''
COMPOSITE_POLICY = '''
    static const bool skipComposite = [] {
        const char *value = std::getenv("MP6_PROFILE_SKIP_AO_COMPOSITE");
        const bool skip = value && std::strcmp(value, "1") == 0;
        if (skip)
            std::fprintf(stderr, "[MP6-COMPOSITE-PROBE] skip=1 local-diagnostic-only\\n");
        return skip;
    }();'''
COMPOSITE_DRAW_REPLACEMENT = '''    pass.SetBindGroup(0, bindings);
    if (!skipComposite) {
        pass.Draw(3);
    }'''


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class LinkCaptured(Exception):
    pass


def _replace_once(source, anchor, replacement):
    count = source.count(anchor)
    if count != 1:
        raise ValueError(f'expected exactly one AO composite source anchor, found {count}: {anchor!r}')
    return source.replace(anchor, replacement, 1)


def _composite_body(source):
    if source.count(COMPOSITE_ENTRY_ANCHOR) != 1:
        raise ValueError('AO composite entry anchor drifted')
    if source.count(FOLIAGE_ENTRY_ANCHOR) != 1:
        raise ValueError('AO foliage callback boundary drifted')
    start = source.index(COMPOSITE_ENTRY_ANCHOR)
    end = source.index(FOLIAGE_ENTRY_ANCHOR, start)
    if end <= start:
        raise ValueError('AO composite callback boundary is invalid')
    return source[start:end]


def instrument_ao_composite(source):
    """Guard only the final AO composite draw in a source-bound probe.

    The transform is pure and fail-closed: callback setup, bind-group state,
    counters, and the shared scratch/job release tail must all match the
    current production source before a diagnostic variant is generated.
    """
    if not isinstance(source, str):
        raise TypeError('source must be text')
    if ENV_VAR in source or PROBE_MARKER in source:
        raise ValueError('source is already AO composite probe instrumented')

    body = _composite_body(source)
    for anchor, label in (
        (COMPOSITE_SETUP_ANCHOR, 'draw setup'),
        (COMPOSITE_DRAW_ANCHOR, 'final draw'),
        (COMPOSITE_RELEASE_ANCHOR, 'shared cleanup'),
    ):
        if body.count(anchor) != 1:
            raise ValueError(f'AO composite {label} anchor drifted')
    draw_offset = body.index(COMPOSITE_DRAW_ANCHOR)
    release_offset = body.index(COMPOSITE_RELEASE_ANCHOR)
    if draw_offset >= release_offset:
        raise ValueError('AO composite draw must precede shared cleanup')

    transformed = _replace_once(
        source,
        COMPOSITE_ENTRY_ANCHOR,
        COMPOSITE_ENTRY_ANCHOR + COMPOSITE_POLICY,
    )
    return _replace_once(transformed, COMPOSITE_DRAW_ANCHOR, COMPOSITE_DRAW_REPLACEMENT)


# Keep the shorter name available to source-test callers following the other
# AO probe modules' convention.
instrument_ao = instrument_ao_composite


def main():
    # Android build helpers are optional for CPU/source tests and belong only
    # to the executable path.
    import sys

    sys.path.insert(0, str(ROOT / 'build'))
    from android_local import build, configure, step_android

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
    text = instrument_ao_composite(source.read_text(encoding='utf-8'))
    generated = out / source.name
    generated.write_text(text, encoding='utf-8')
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
