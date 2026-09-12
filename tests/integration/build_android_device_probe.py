"""Build a local Android Release probe without replacing production artifacts."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'build'))
from android_local import build, configure, step_android
configure()


class LinkCaptured(Exception):
    pass


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--direct-integer', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
        parser.error('simple probe name required')
    out = ROOT/'build/tablet-performance-20260910'/('probe-'+args.name)
    out.mkdir(exist_ok=False)
    step_android.verify_android_native_build_manifest(ROOT, 'release')
    production = ROOT/'build/android/aurora/libmp6game.so'
    original_hash = digest(production)
    native_manifest = json.loads((production.parent/'native-build-manifest.json').read_text())
    captured = []
    run = subprocess.run
    def capture(command, *a, **kw):
        if '-shared' in command and '-o' in command and Path(command[command.index('-o')+1]) == production:
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
    assert hashlib.sha256(json.dumps(captured, separators=(',', ':')).encode()).hexdigest() == native_manifest['link_command_sha256']
    tree = ROOT/'build/android-aurora'
    source = ROOT/'build/android-aurora-source/lib/webgpu/gpu.cpp'
    text = source.read_text()
    anchor = '  const auto& surface = g_graphicsConfig.surfaceConfiguration;\n  return viewport.left == 0.f'
    assert text.count(anchor) == 1
    probe = '''  const auto& surface = g_graphicsConfig.surfaceConfiguration;
  static unsigned probeFrames=0;
  if (++probeFrames % 600 == 1) {
    const auto& src=present_source();
    std::printf("[MP6-PRESENT-PROBE] viewport=%.3f,%.3f,%.3f,%.3f surface=%ux%u/%u source=%ux%u/%u\\n",
      viewport.left,viewport.top,viewport.width,viewport.height,surface.width,surface.height,unsigned(surface.format),
      src.size.width,src.size.height,unsigned(src.format));
  }
  return viewport.left == 0.f'''
    text = text.replace(anchor, probe)
    if args.direct_integer:
        old = '''  return viewport.left == 0.f && viewport.top == 0.f &&
         viewport.width == float(surface.width) && viewport.height == float(surface.height) &&
         present_source().format == surface.format;'''
        new = '''  return (g_Resampler != SAMPLER_AREA || (viewport.left == 0.f && viewport.top == 0.f)) &&
         viewport.left >= 0.f && viewport.top >= 0.f && viewport.width > 0.f && viewport.height > 0.f &&
         viewport.left == std::floor(viewport.left) && viewport.top == std::floor(viewport.top) &&
         viewport.width == std::floor(viewport.width) && viewport.height == std::floor(viewport.height) &&
         viewport.left + viewport.width <= float(surface.width) &&
         viewport.top + viewport.height <= float(surface.height) &&
         present_source().format == surface.format;'''
        assert text.count(old) == 1
        text = text.replace(old, new)
    target = out/'gpu.cpp'
    target.write_text(text)
    ninja = (tree/'build.ninja').read_text()
    needle = source.as_posix().replace(':', '$:')
    blocks = [m.group(1) for m in re.finditer(r'^build CMakeFiles/aurora_core.dir/[^\n]* '+re.escape(needle)+r' [^\n]*\n((?:  [^\n]*\n)+)', ninja, re.M)]
    assert len(blocks) == 1
    flags = []
    for key in ('DEFINES', 'FLAGS', 'INCLUDES'):
        rows = re.findall(r'^  '+key+r' = (.*)$', blocks[0], re.M)
        assert len(rows) == 1
        flags += shlex.split(rows[0])
    assert '-O2' in flags and '-DNDEBUG' in flags
    ndk = ROOT/'build/android-sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/windows-x86_64/bin'
    obj = out/'gpu.cpp.o'
    compile_cmd = [str(ndk/'clang++.exe'), '--target=aarch64-none-linux-android28',
                   *flags, '-iquote', str(source.parent), '-c', str(target), '-o', str(obj)]
    run(compile_cmd, cwd=tree, check=True)
    archive = out/'libaurora_core.a'
    shutil.copy2(tree/archive.name, archive)
    members = run([str(ndk/'llvm-ar.exe'), 't', str(archive)], capture_output=True, text=True, check=True).stdout.splitlines()
    assert members.count(obj.name) == 1
    run([str(ndk/'llvm-ar.exe'), 'r', str(archive), str(obj)], check=True)
    link = list(captured)
    link[link.index('-o')+1] = str(out/'libmp6game.so')
    count = 0
    for i, arg in enumerate(link):
        if Path(arg).name == archive.name:
            link[i] = str(archive)
            count += 1
    assert count == 2  # CMake deliberately repeats aurora_core for archive ordering.
    run(link, check=True)
    stripped = out/'stripped/libmp6game.so'
    stripped.parent.mkdir()
    run([str(ndk/'llvm-strip.exe'), '--strip-unneeded', '-o', str(stripped), str(out/'libmp6game.so')], check=True)
    assert digest(production) == original_hash
    (out/'provenance.json').write_text(json.dumps(dict(source_sha256=digest(source), original_native=original_hash,
        compile=compile_cmd, link=link, stripped_sha256=digest(stripped), release_asset=False), indent=2))
    print('Isolated Release probe:', stripped)


if __name__ == '__main__':
    main()
