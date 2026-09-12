"""Read the actual C++-selected WGSL, including platform-conditional literals."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import build


def shader_source(path=None, *, android=False, compatibility=None, prepared_depth=False,
                  attachment_depth=False, multisampled_depth=False, decals=True):
    path = Path(path).resolve() if path else ROOT / 'src/gx/ambient_occlusion_shader.hpp'
    header = path.read_text()
    if 'inline std::string ambient_occlusion_shader(' in header:
        # Execute the production builder, not a regex interpretation of its
        # C++ branches. Android defaults to the conservative test variant.
        mode = android if compatibility is None else compatibility
        values = (mode, prepared_depth)
        if 'bool attachmentDepth' in header:
            values += (attachment_depth, multisampled_depth, decals)
        elif attachment_depth or multisampled_depth or not decals:
            raise ValueError('This checkpoint does not support attachment depth')
        with tempfile.TemporaryDirectory(prefix='ao-source-', dir=ROOT/'build') as tmp:
            folder = Path(tmp)
            cpp, exe = folder/'dump.cpp', folder/'dump.exe'
            cpp.write_text('#include <cstdio>\n#include "' + path.as_posix() +
                           '"\nint main() { auto s = ambient_occlusion_shader(' +
                           ', '.join('true' if value else 'false' for value in
                                     values) +
                           '); std::fputs(s.c_str(), stdout); }\n')
            result = subprocess.run([build.ZIG, 'c++', '-std=c++20', '-O0', str(cpp), '-o', str(exe)],
                                    capture_output=True, text=True, timeout=90)
            if result.returncode: raise RuntimeError(result.stderr)
            return subprocess.run([str(exe)], capture_output=True, text=True, check=True, timeout=10).stdout
    result = subprocess.run([build.ZIG, 'c++', '-E', '-P', '-x', 'c++',
                             *(['-D__ANDROID__'] if android else []), str(path)],
                            capture_output=True, text=True, check=True, timeout=30)
    parts = re.findall(r'R"wgsl\((.*?)\)wgsl"', result.stdout, re.S)
    if not parts:
        raise ValueError('No WGSL string literals in ' + str(path))
    return ''.join(parts)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--compatibility', action='store_true')
    parser.add_argument('--prepared-depth', action='store_true')
    parser.add_argument('--attachment-depth', action='store_true')
    parser.add_argument('--multisampled-depth', action='store_true')
    parser.add_argument('--no-decals', action='store_true')
    args = parser.parse_args()
    print(shader_source(compatibility=args.compatibility, prepared_depth=args.prepared_depth,
                        attachment_depth=args.attachment_depth, multisampled_depth=args.multisampled_depth,
                        decals=not args.no_decals))
