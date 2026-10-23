"""Compile the real Aurora offscreen-target cache against deterministic stubs."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools import build

try:
    from tests.aurora_test_support import replay as replay_pinned_source
except ModuleNotFoundError:
    from aurora_test_support import replay as replay_pinned_source  # type: ignore[no-redef]


ROOT = Path(__file__).resolve().parents[1]
TARGET = "lib/gfx/common.cpp"
SELFTEST = ROOT / "tests/native/offscreen_shadow_cache_selftest.cpp"


def extract_braced_function(source: str, name: str) -> str:
    """Extract one real C++ function without reducing its production body."""

    match = re.search(
        r"^[^\n;{}]*\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{",
        source,
        re.MULTILINE,
    )
    if match is None:
        raise AssertionError(f"Missing function: {name}")
    start = match.start()
    brace = source.find("{", match.start(), match.end())
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"Unclosed function: {name}")


class OffscreenShadowCache(unittest.TestCase):
    def test_replayed_production_cache_has_bounded_lru(self):
        self.assertTrue(SELFTEST.is_file())
        source = replay_pinned_source(TARGET)

        capacity = re.search(
            r"static constexpr size_t MP6_OFFSCREEN_CACHE_MAX\s*=\s*(\d+)\s*;",
            source,
        )
        self.assertIsNotNone(capacity, "resolved common.cpp lost the cache bound")
        self.assertEqual(capacity.group(1), "2")
        self.assertIn("clear_offscreen_cache", source)

        key_start = source.index("struct OffscreenCacheKey {")
        key = source[key_start : source.index("struct OffscreenCacheEntry {", key_start)]
        cache_start = source.index("struct OffscreenCacheEntry {")
        cache = source[cache_start : source.index("struct PassSnapshotEntry {", cache_start)]
        subject = "\n".join(
            [
                key,
                cache,
                extract_braced_function(source, "get_offscreen_textures"),
                extract_braced_function(source, "clear_offscreen_cache"),
            ]
        )

        with tempfile.TemporaryDirectory(prefix="offscreen-shadow-cache-", dir=ROOT / "build") as temporary:
            directory = Path(temporary)
            (directory / "offscreen_shadow_cache.inc").write_text(subject, encoding="utf-8")
            executable = directory / "offscreen-shadow-cache.exe"
            compile_result = subprocess.run(
                [
                    build.ZIG,
                    "c++",
                    "-std=c++20",
                    "-O2",
                    "-UNDEBUG",
                    "-I" + str(directory),
                    str(SELFTEST),
                    "-o",
                    str(executable),
                ],
                capture_output=True,
                text=True,
                timeout=90,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            run_result = subprocess.run(
                [str(executable)], capture_output=True, text=True, timeout=60
            )
        self.assertEqual(run_result.returncode, 0, run_result.stdout + run_result.stderr)


if __name__ == "__main__":
    unittest.main()
