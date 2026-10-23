"""Checks that zero-opacity text skips paint only, without losing dirty state."""

import hashlib
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

from setup.lib import step_aurora
from tools import apply_patches, build


ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "compat/aurora/base/0067-rml-transparent-text-paint.patch"
SELFTEST = ROOT / "tests/native/rml_transparent_text_selftest.cpp"
RELATIVE = "Source/Core/ElementText.cpp"
CURRENT = ROOT / "build/aurora-release/_deps/rmlui-src" / RELATIVE


def pristine_rml_source() -> str | None:
    """Read an unpatched pinned RmlUi source from the local build inputs.

    CMake's generated dependency stamp records both the exact archive URL and
    its SHA-256.  Prefer an unpatched checkout when available, otherwise read
    the verified local archive without extracting it.  No network access is
    attempted and a patched generated checkout is never treated as pristine.
    """

    aurora_root = Path(build.AURORA_ROOT)
    checkout = aurora_root / "build-rmlui/_deps/rmlui-src" / RELATIVE
    if checkout.is_file():
        source = checkout.read_text(encoding="utf-8", errors="surrogateescape")
        if "if (opacity == 0.f)" not in source:
            return source

    stamp = aurora_root / (
        "build-rmlui/_deps/rmlui-subbuild/rmlui-populate-prefix/src/"
        "rmlui-populate-stamp"
    )
    urlinfo = stamp / "rmlui-populate-urlinfo.txt"
    if not urlinfo.is_file():
        return None
    metadata = urlinfo.read_text(encoding="utf-8", errors="surrogateescape").splitlines()
    url = next(
        (
            line.removeprefix("url(s)=")
            for line in metadata
            if line.startswith("url(s)=")
        ),
        None,
    )
    hash_value = next(
        (line.split("=", 2)[2] for line in metadata if line.startswith("hash=SHA256=")),
        None,
    )
    if not url or not hash_value:
        return None
    archive = stamp.parent / url.rsplit("/", 1)[-1]
    if not archive.is_file():
        return None
    actual_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
    if actual_hash.lower() != hash_value.lower():
        raise AssertionError(
            f"local RmlUi archive hash mismatch; refusing unverified pristine source: {archive}"
        )

    member_name = RELATIVE.replace("\\", "/")
    with tarfile.open(archive, mode="r:*") as bundle:
        matches = [
            member
            for member in bundle.getmembers()
            if member.name.endswith("/" + member_name)
        ]
        if len(matches) != 1:
            return None
        extracted = bundle.extractfile(matches[0])
        if extracted is None:
            return None
        return extracted.read().decode("utf-8", "surrogateescape")


def element_text_on_render_body(source: str) -> str:
    """Extract the actual patched OnRender body for native fixture compilation."""

    marker = "void ElementText::OnRender()"
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 0
    state: str | None = None
    index = opening
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "line":
            if char == "\n":
                state = None
            index += 1
            continue
        if state == "block":
            if char == "*" and next_char == "/":
                state = None
                index += 2
                continue
            index += 1
            continue
        if state in ('"', "'"):
            if char == "\\":
                index += 2
                continue
            if char == state:
                state = None
            index += 1
            continue
        if char == "/" and next_char == "/":
            state = "line"
            index += 2
            continue
        if char == "/" and next_char == "*":
            state = "block"
            index += 2
            continue
        if char in ('"', "'"):
            state = char
            index += 1
            continue
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
        index += 1
    raise AssertionError("unterminated ElementText::OnRender body")


def inject_on_render(template: str, body: str) -> str:
    begin_marker = "// BEGIN ACTUAL_ELEMENT_TEXT_ON_RENDER"
    end_marker = "// END ACTUAL_ELEMENT_TEXT_ON_RENDER"
    begin = template.index(begin_marker)
    content_start = template.index("\n", begin) + 1
    end = template.index(end_marker, content_start)
    return template[:content_start] + body + "\n" + template[end:]


class TransparentTextTests(unittest.TestCase):
    def test_native_actual_on_render_preserves_dirty_state_and_paints(self):
        if not CURRENT.exists() or not SELFTEST.exists():
            self.skipTest("patched RmlUi source or native fixture is unavailable")
        source = CURRENT.read_text(encoding="utf-8", errors="surrogateescape")
        template = SELFTEST.read_text(encoding="utf-8")
        generated_source = inject_on_render(template, element_text_on_render_body(source))
        with tempfile.TemporaryDirectory(
            prefix="rml-transparent-text-", dir=ROOT / "build"
        ) as temporary:
            directory = Path(temporary)
            generated = directory / "rml_transparent_text_generated.cpp"
            executable = directory / "rml-transparent-text.exe"
            generated.write_text(generated_source, encoding="utf-8", newline="\n")
            compile_result = subprocess.run(
                [
                    build.ZIG,
                    "c++",
                    "-std=c++20",
                    "-O2",
                    "-UNDEBUG",
                    str(generated),
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=90,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            run_result = subprocess.run(
                [str(executable)], cwd=ROOT, capture_output=True, text=True, timeout=60
            )
        self.assertEqual(run_result.returncode, 0, run_result.stdout + run_result.stderr)
        self.assertIn("PASS RmlUi transparent text actual OnRender", run_result.stdout)

if __name__ == "__main__":
    unittest.main()
