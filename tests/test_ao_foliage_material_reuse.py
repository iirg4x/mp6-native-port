"""Bounded native regression coverage for AO foliage material-cache reuse.

The renderer keeps this record and the reuse predicate private to the HSF
draw translation unit.  The test therefore extracts the actual production
record, helper bodies, and exact predicate into a temporary include and
compiles them against the repository's real HSF/Dolphin headers.  It never
imports a recovery snapshot, a recorded command, or a private build path.
"""

from __future__ import annotations

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

try:
    from tools import build
except (ImportError, OSError):  # pragma: no cover - reported as a bounded skip
    build = None


ROOT = Path(__file__).resolve().parents[1]
PRODUCTION_HEADER = ROOT / "include" / "mp6_ao_foliage_draw.h"
SELFTEST = ROOT / "tests" / "native" / "ao_foliage_material_reuse_selftest.c"
HSF_SOURCE_CANDIDATES = (
    ROOT / "build" / "patched-src" / "src" / "game" / "hsfdraw.c",
    ROOT / "build" / "verify-patched-src" / "src" / "game" / "hsfdraw.c",
    ROOT / "build" / "deps" / "marioparty6" / "src" / "game" / "hsfdraw.c",
    ROOT.parent / "decomp-build" / "src" / "game" / "hsfdraw.c",
    ROOT / "build" / "board-qa" / "game_hsfdraw_probe.c",
)
HSF_ADAPTATION_PATCH = ROOT / "compat" / "decomp" / "src" / "game" / "hsfdraw.c.patch"


def _balanced_block(text: str, start: int) -> str:
    """Return a C function/block beginning at *start*, including its braces."""

    brace = text.index("{", start)
    depth = 0
    quote: str | None = None
    escaped = False
    line_comment = False
    block_comment = False
    i = brace
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if line_comment:
            if c == "\n":
                line_comment = False
        elif block_comment:
            if c == "*" and n == "/":
                block_comment = False
                i += 1
        elif quote:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == quote:
                quote = None
        elif c == "/" and n == "/":
            line_comment = True
            i += 1
        elif c == "/" and n == "*":
            block_comment = True
            i += 1
        elif c in "\"'":
            quote = c
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
        i += 1
    raise ValueError("unterminated production block")


def _statement_end(text: str, start: int) -> int:
    """Find the semicolon ending a C statement, ignoring comments/strings."""

    quote: str | None = None
    escaped = False
    line_comment = False
    block_comment = False
    i = start
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if line_comment:
            if c == "\n":
                line_comment = False
        elif block_comment:
            if c == "*" and n == "/":
                block_comment = False
                i += 1
        elif quote:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == quote:
                quote = None
        elif c == "/" and n == "/":
            line_comment = True
            i += 1
        elif c == "/" and n == "*":
            block_comment = True
            i += 1
        elif c in "\"'":
            quote = c
        elif c == ";":
            return i + 1
        i += 1
    raise ValueError("unterminated production statement")


def _extract_subject(source: str) -> tuple[str, str]:
    """Extract the actual private type/helpers and exact reuse expression."""

    type_match = re.search(
        r"typedef\s+struct\s*\{\s*HU3D_DRAW_OBJ\s+draw;.*?"
        r"\}\s*Mp6AoFoliageDraw\s*;",
        source,
        re.DOTALL,
    )
    if not type_match:
        raise AssertionError("production private Mp6AoFoliageDraw layout is missing")
    private_type = type_match.group(0)

    helper_markers = (
        "static HSF_MATERIAL *mp6AoFoliageReuseMaterial",
        "static int mp6AoFoliageReuseEligible",
        "static unsigned mp6AoFoliageReuseBatchFlags",
    )
    helpers = []
    for marker in helper_markers:
        try:
            start = source.index(marker)
            helpers.append(_balanced_block(source, start))
        except ValueError as exc:
            raise AssertionError(f"production helper is incomplete: {marker}") from exc

    # These checks bind the fixture to the intended fail-closed guard rather
    # than allowing a coincidentally compilable but weaker helper to pass.
    required_guard_terms = (
        "if (!saved->draw.object || !saved->face) return NULL;",
        "if (!model || !object || !material) return 0;",
        "if (model->attr & HU3D_ATTR_HOOKFUNC) return 0;",
        "if ((flags & HSF_MATERIAL_MATHOOK) && model->matHook) return 0;",
        "if (material->refAlpha != 0.0f) return 0;",
        "(model->attr & HU3D_ATTR_HILITE)",
        "constData && (constData->attr & HU3D_CONST_HILITE)",
        "flags & HSF_MATERIAL_HILITE",
        "model->projBit || (model->attr & HU3D_ATTR_TOON_MAP)",
        "HU3D_CONST_SHADOW_MAP",
        "if (attribute->animWorkP) return 0;",
        "HSF_BMPFMT_CI_IA8",
    )
    for anchor in required_guard_terms:
        if anchor not in source:
            raise AssertionError(f"production guard lost required term: {anchor}")

    reuse_match = re.search(r"\bint\s+reuse\s*=", source)
    if not reuse_match:
        raise AssertionError("production exact reuse expression is missing")
    reuse_end = _statement_end(source, reuse_match.end())
    reuse_statement = source[reuse_match.start() : reuse_end]
    expected_terms = (
        "previousSaved && previousEligible && currentEligible",
        "draw->model==previousSaved->draw.model",
        "draw->object==previousSaved->draw.object",
        "currentMaterial==previousMaterial",
        "memcmp(&draw[0],&previousSaved->draw,sizeof(HU3D_DRAW_OBJ))==0",
        "saved->water==previousSaved->water",
        "saved->background==previousSaved->background",
        "saved->unshaded==previousSaved->unshaded",
        "currentBatchFlags==previousBatchFlags",
        "materialBak==currentMaterial",
    )
    for term in expected_terms:
        if term not in reuse_statement:
            raise AssertionError(f"exact reuse expression lost required term: {term}")
    expression = source[reuse_match.end() : reuse_end - 1].strip()

    subject = private_type + "\n\n" + "\n\n".join(helpers)
    subject += "\n\nstatic int extracted_reuse_expression("
    subject += "const Mp6AoFoliageDraw *previousSaved, "
    subject += "const Mp6AoFoliageDraw *currentSaved, int previousEligible, "
    subject += "int currentEligible, HU3D_DRAW_OBJ *draw, "
    subject += "HSF_MATERIAL *currentMaterial, HSF_MATERIAL *previousMaterial, "
    subject += "unsigned currentBatchFlags, unsigned previousBatchFlags, "
    subject += "HSF_MATERIAL *materialBak)\n{\n"
    subject += "    const Mp6AoFoliageDraw *saved = currentSaved;\n"
    subject += "    return " + expression + ";\n}\n"
    return subject, reuse_statement


def _check_render_loop_contract(source: str) -> None:
    """Check header-side per-entry state and the reuse/miss mask handoff."""

    render_start = source.index("void mp6_ao_foliage_render")
    loop_match = re.search(
        r"for\s*\(\s*int\s+i\s*=\s*0\s*;\s*i\s*<\s*count\s*;\s*i\+\+\s*\)"
        r"\s*\{\s*Mp6AoFoliageDraw\s*\*saved",
        source[render_start:],
        re.DOTALL,
    )
    if not loop_match:
        raise AssertionError("AO foliage render per-entry loop is missing")
    loop_start = render_start + loop_match.start()
    loop = _balanced_block(source, loop_start)

    face = "FaceDraw(draw,saved->face);"
    face_at = loop.find(face)
    if face_at < 0:
        raise AssertionError("per-entry FaceDraw call is missing")
    before_face = loop[:face_at]
    after_face = loop[face_at + len(face) :]

    # These operations are intentionally retained for every saved entry.
    # Matrix/FI/offset work must not be hidden behind the material predicate.
    required_before = (
        "mp6AoFoliageMatrices(draw);",
        "mp6_fi_capture_auxiliary_begin(",
        "GX2SetPolygonOffset(-2.0f,-0.5f,-2.0f,-0.5f,0.0f);",
    )
    for term in required_before:
        if term not in before_face:
            raise AssertionError(f"per-entry operation moved/removed: {term}")
    if not re.search(
        r"if\s*\(\s*reuse\s*\)\s*mp6AoFoliageMaskState\s*\(\s*\);",
        before_face,
    ):
        raise AssertionError("reuse-hit mask setup is missing from the outer loop")
    for term in (
        "GX2SetPolygonOffset(0.0f,0.0f,0.0f,0.0f,0.0f);",
        "previousSaved=saved;",
        "previousEligible=currentEligible;",
    ):
        if term not in after_face:
            raise AssertionError(f"per-entry operation moved/removed: {term}")

    reset_match = re.search(r"if\s*\(\s*!reuse\s*\)\s*\{", before_face)
    if not reset_match:
        raise AssertionError("cache reset fallback for a non-reuse entry is missing")
    reset_block = _balanced_block(before_face, reset_match.start())
    for term in (
        "materialBak=PTR_INVALID;",
        "BmpPtrBak[t]=PTR_INVALID;",
        "shadingBak=vtxModeBak=-1;",
        "matHookCallF=FALSE;",
    ):
        if term not in reset_block:
            raise AssertionError(f"non-reuse cache reset lost: {term}")


def _load_generated_hsf_source() -> str:
    """Load the generated HSF translation unit used by the native build."""

    for path in HSF_SOURCE_CANDIDATES:
        if path.is_file():
            source = path.read_text(encoding="utf-8")
            if '#include "mp6_ao_foliage_draw.h"' not in source:
                raise AssertionError(
                    f"generated HSF source lacks the foliage adaptation include: {path}"
                )
            if "mp6AoFoliageMaskState();" not in source:
                raise AssertionError(
                    f"generated HSF source lacks the foliage mask adaptation: {path}"
                )
            return source
    raise AssertionError(
        "generated HSF source unavailable; cannot verify the FaceDraw miss path"
    )


def _check_face_draw_mask_paths(source: str) -> None:
    """Prove miss and hit paths each configure the private foliage mask once."""

    marker = re.search(
        r"static\s+void\s+FaceDraw\s*\(\s*HU3D_DRAW_OBJ\s*\*\s*drawObj\s*,"
        r"\s*HSF_FACE\s*\*\s*face\s*\)",
        source,
    )
    if not marker:
        raise AssertionError("generated HSF FaceDraw definition is missing")
    face = _balanced_block(source, marker.start())
    mask = "mp6AoFoliageMaskState();"
    if face.count(mask) != 1:
        raise AssertionError("FaceDraw must configure the foliage mask exactly once on a miss")
    miss_path = re.search(
        r"if\s*\(\s*matP\s*!=\s*materialBak\s*\)\s*\{.*?"
        r"mp6AoFoliageMaskState\s*\(\s*\);\s*"
        r"GXCallDisplayList\s*\(.*?\}\s*else\s*\{.*?"
        r"GXFastCallDisplayList\s*\(",
        face,
        re.DOTALL,
    )
    if not miss_path:
        raise AssertionError(
            "FaceDraw miss path must mask before GXCallDisplayList and retain GXFastCallDisplayList"
        )

    patch = HSF_ADAPTATION_PATCH.read_text(encoding="utf-8")
    if patch.count("+        mp6AoFoliageMaskState();") != 1:
        raise AssertionError("hsfdraw adaptation patch lost its FaceDraw mask anchor")


def _known_header_dependency_missing(stderr: str) -> bool:
    """Recognize only missing pinned compiler/HSF headers as skippable."""

    lowered = stderr.lower()
    if "file not found" not in lowered and "no such file or directory" not in lowered:
        return False
    return any(
        token in lowered
        for token in (
            "dolphin_compat.h",
            "game/hu3d.h",
            "game/hsfformat.h",
            "dolphin/types.h",
            "dolphin/",
        )
    )


class AoFoliageMaterialReuse(unittest.TestCase):
    def test_production_reuse_guard_and_native_behavior(self):
        source = PRODUCTION_HEADER.read_text(encoding="utf-8")
        subject, _reuse_statement = _extract_subject(source)
        _check_render_loop_contract(source)
        _check_face_draw_mask_paths(_load_generated_hsf_source())

        if build is None or not Path(build.ZIG).is_file():
            self.skipTest("repository Zig compiler is unavailable")
        if not SELFTEST.is_file():
            self.fail(f"owned native fixture is missing: {SELFTEST}")

        try:
            with tempfile.TemporaryDirectory(
                prefix="ao-foliage-reuse-", dir=ROOT / "build"
            ) as temporary:
                folder = Path(temporary)
                (folder / "subject.inc").write_text(subject, encoding="utf-8")
                executable = folder / "ao_foliage_material_reuse.exe"
                command = [
                    str(build.ZIG),
                    "cc",
                    *build.COMMON_FLAGS,
                    "-O2",
                    "-UNDEBUG",
                    "-std=c11",
                    "-I",
                    str(folder),
                    str(SELFTEST),
                    "-o",
                    str(executable),
                ]
                result = subprocess.run(
                    command,
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    timeout=90,
                )
                if result.returncode != 0:
                    if _known_header_dependency_missing(result.stderr):
                        self.skipTest(
                            "repository HSF/Dolphin header dependency is unavailable: "
                            + result.stderr.strip()
                        )
                    self.fail(
                        "native AO foliage material-reuse fixture failed to compile:\n"
                        + result.stderr
                    )

                result = subprocess.run(
                    [str(executable)],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
        except FileNotFoundError as exc:
            self.skipTest(f"repository compiler dependency is unavailable: {exc}")
        except subprocess.TimeoutExpired as exc:
            self.fail(f"bounded native AO foliage material-reuse test timed out: {exc}")

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        match = re.search(r"CASES\s+(\d+)\s+FAILED\s+(\d+)", result.stdout)
        self.assertIsNotNone(match, result.stdout + result.stderr)
        self.assertEqual(int(match.group(1)), 47)
        self.assertEqual(int(match.group(2)), 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
