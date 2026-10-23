"""Checks for the optional RmlUi Context::Update timing diagnostic patch."""

from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from setup.lib import step_aurora  # noqa: E402
from tools import apply_patches, build  # noqa: E402


PATCH = ROOT / "compat/aurora/deps/rmlui-update-diagnostics.patch"
RELATIVE = "Source/Core/Context.cpp"
CURRENT = ROOT / "build/aurora-release/_deps/rmlui-src" / RELATIVE
BUCKETS = (
    "prelude_ms",
    "hover_ms",
    "data_ms",
    "root_update_ms",
    "layout_ms",
    "release_ms",
)


def _pinned_dependency_copy():
    return Path(build.AURORA_ROOT) / "build-rmlui/_deps/rmlui-src" / RELATIVE


class RmlUpdateDiagnosticsTests(unittest.TestCase):
    def test_patch_hunks_replay_exactly(self):
        sections = step_aurora.split_patch_sections(PATCH.read_text(encoding="utf-8"))
        self.assertEqual({section["new"] for section in sections}, {RELATIVE})
        for section in sections:
            for hunk in apply_patches._parse_hunks(section["body"]):
                self.assertEqual(hunk["old_count"], len(hunk["old_lines"]))
                self.assertEqual(hunk["new_count"], len(hunk["new_lines"]))

        baseline_path = _pinned_dependency_copy()
        if not baseline_path.exists():
            self.skipTest(f"pristine fetched RmlUi source is unavailable: {baseline_path}")
        if not CURRENT.exists():
            self.skipTest(f"generated RmlUi source is unavailable: {CURRENT}")

        replayed = baseline_path.read_text(encoding="utf-8", errors="surrogateescape")
        for section in sections:
            replayed = apply_patches.apply_unified_diff(replayed, section["body"], label=str(PATCH))
        self.assertEqual(replayed, CURRENT.read_text(encoding="utf-8", errors="surrogateescape"))

    def test_diagnostic_contract_is_opt_in_and_bucketed(self):
        source = CURRENT.read_text(encoding="utf-8", errors="surrogateescape")
        self.assertIn('std::getenv("MP6_RML_UPDATE_TIMINGS")', source)
        self.assertIn("static constexpr unsigned REPORT_PERIOD = 600;", source)
        self.assertIn("[MP6-RML-UPDATE-TIMINGS]", source)
        self.assertIn("std::array<Clock::time_point, 7>", source)
        for bucket in BUCKETS:
            self.assertIn(bucket, source)
        self.assertIn("if (enabled)\n\t\t\tmarks[index] = Clock::now();", source)
        self.assertIn("if (!enabled)\n\t\t\treturn;", source)


if __name__ == "__main__":
    unittest.main()
