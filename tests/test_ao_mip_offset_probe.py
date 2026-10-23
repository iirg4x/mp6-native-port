"""CPU and host-compiled contracts for the QA AO mip-offset transform."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from tools import build


ROOT = Path(__file__).resolve().parents[1]
INTEGRATION = ROOT / "tests" / "integration"
if str(INTEGRATION) not in sys.path:
    sys.path.insert(0, str(INTEGRATION))

from ao_mip_offset_probe import (  # noqa: E402
    ERROR_MARKER,
    ENV_NAME,
    LOG_MARKER,
    MARKER,
    MIP_OFFSET_HELPER,
    instrument_ao_mip_offset,
)


class AoMipOffsetTransformTests(unittest.TestCase):
    def setUp(self):
        self.source = (ROOT / "src" / "gx" / "ambient_occlusion.cpp").read_text(
            encoding="utf-8"
        )

    def test_production_source_transform_is_bounded_and_ordered(self):
        transformed = instrument_ao_mip_offset(self.source)
        begin = transformed.index("// AO_UPSTREAM_PREPARATION_BEGIN")
        initialize = transformed.index("g_xegtao.initialize(ctx.device, g_xegtaoPolicy);")
        assignment = transformed.index("DepthMIPSamplingOffset = qaAoDepthMipOffset.value;")
        end = transformed.index("// AO_UPSTREAM_PREPARATION_END")
        self.assertIn(MARKER, transformed)
        self.assertIn("static const Mp6QaAoDepthMipOffset config", transformed)
        self.assertEqual(transformed.count("DepthMIPSamplingOffset ="), 1)
        self.assertEqual(transformed.count("qaAoDepthMipOffset = mp6_qa_ao_mip_offset();"), 1)
        self.assertLess(begin, initialize)
        self.assertLess(initialize, assignment)
        self.assertLess(assignment, end)
        self.assertEqual(transformed.count(LOG_MARKER), 1)  # applied path

    def test_transform_rejects_missing_duplicate_and_second_instrumentation(self):
        with self.assertRaises(ValueError):
            instrument_ao_mip_offset(self.source.replace(
                "// AO_UPSTREAM_PREPARATION_END",
                "// AO_UPSTREAM_PREPARATION_END\n    // AO_UPSTREAM_PREPARATION_END",
                1,
            ))
        with self.assertRaises(ValueError):
            instrument_ao_mip_offset(self.source.replace(
                "                upstream = g_xegtao.initialize(ctx.device, g_xegtaoPolicy);\n",
                "",
                1,
            ))
        transformed = instrument_ao_mip_offset(self.source)
        with self.assertRaises(ValueError):
            instrument_ao_mip_offset(transformed)

    def test_helper_is_self_contained_and_fail_closed(self):
        self.assertIn("std::strtof", MIP_OFFSET_HELPER)
        self.assertIn("std::isfinite", MIP_OFFSET_HELPER)
        self.assertIn("value < 2.0f || value > 6.0f", MIP_OFFSET_HELPER)
        self.assertIn("std::abort", MIP_OFFSET_HELPER)
        self.assertIn(ERROR_MARKER, MIP_OFFSET_HELPER)


class AoMipOffsetHostParserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tempdir = tempfile.TemporaryDirectory(prefix="ao-mip-offset-", dir=ROOT / "build")
        folder = Path(cls.tempdir.name)
        cls.executable = folder / "probe.exe"
        source = folder / "probe.cpp"
        source.write_text(
            "#include <cmath>\n"
            "#include <cstdio>\n"
            "#include <cstdlib>\n"
            # Windows may display a crash dialog for std::abort(), which
            # would make a CPU contract hang.  Replace it only in this host
            # fixture with a deterministic nonzero process exit.
            "namespace std { [[noreturn]] inline void qa_test_abort() { std::_Exit(77); } }\n"
            "#define abort qa_test_abort\n"
            + MIP_OFFSET_HELPER
            + "#undef abort\n"
            + r'''
int main() {
    const Mp6QaAoDepthMipOffset config = mp6_qa_ao_mip_offset();
    float upstream = 3.3f;
    if (config.enabled) upstream = config.value;
    std::printf("enabled=%d value=%.9g upstream=%.9g\n",
                config.enabled ? 1 : 0, config.value, upstream);
}
''',
            encoding="utf-8",
        )
        result = subprocess.run(
            [build.ZIG, "c++", "-std=c++20", "-O2", str(source), "-o", str(cls.executable)],
            capture_output=True,
            text=True,
            timeout=90,
        )
        if result.returncode:
            cls.tempdir.cleanup()
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tempdir.cleanup()

    def _run(self, requested: str | None):
        env = os.environ.copy()
        env.pop(ENV_NAME, None)
        if requested is not None:
            env[ENV_NAME] = requested
        return subprocess.run(
            [str(self.executable)], capture_output=True, text=True, env=env, timeout=10
        )

    def test_absent_environment_keeps_default_constant(self):
        result = self._run(None)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("enabled=0", result.stdout)
        self.assertAlmostEqual(float(result.stdout.split("upstream=")[1]), 3.3, places=6)

    def test_valid_decimal_range_overrides_constant(self):
        for requested in ("2", "2.5", "6"):
            with self.subTest(requested=requested):
                result = self._run(requested)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("enabled=1", result.stdout)
                self.assertIn(f"upstream={float(requested):g}", result.stdout)

    def test_invalid_values_abort_with_unmistakable_marker(self):
        for requested in ("1.999", "6.001", "nan", "inf", "0x1p+2", "3.3f", "3.3junk", ""):
            with self.subTest(requested=requested):
                result = self._run(requested)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(ERROR_MARKER + " invalid", result.stderr)


if __name__ == "__main__":  # pragma: no cover
    unittest.main(verbosity=2)
