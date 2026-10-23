"""The title QA runner exposes AO conditions without changing isolation."""

import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tests" / "integration" / "run_title_qa.py"


class TitleQaRunnerTests(unittest.TestCase):
    def test_cli_exposes_ao_condition_controls(self):
        result = subprocess.run(
            [sys.executable, str(RUNNER), "--help"],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--ao {0,1,2}", result.stdout)
        self.assertIn("--implementation {legacy,xegtao}", result.stdout)
        self.assertIn("--gpu-validation", result.stdout)

    def test_runner_forwards_and_records_requested_condition(self):
        source = RUNNER.read_text(encoding="utf-8")
        self.assertIn("MP6_ENH_AMBIENT_OCCLUSION=str(args.ao)", source)
        self.assertIn("MP6_AO_XEGTAO=str(int(args.implementation == 'xegtao'))", source)
        self.assertIn("MP6_AO_DIAG='1'", source)
        self.assertIn("MP6_GPU_VALIDATION=str(int(args.gpu_validation))", source)
        self.assertIn("ao=args.ao", source)
        self.assertIn("implementation=args.implementation", source)
        self.assertIn("gpu_validation=args.gpu_validation", source)


if __name__ == "__main__":
    unittest.main()
