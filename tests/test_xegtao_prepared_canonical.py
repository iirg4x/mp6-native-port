"""CPU-only contracts for the prepared/canonical depth QA experiment."""

from __future__ import annotations

import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


INTEGRATION = Path(__file__).resolve().parent / "integration"
if str(INTEGRATION) not in sys.path:
    sys.path.insert(0, str(INTEGRATION))

import importlib.util

# Load the integration harness by path: it shares this module's name, so a
# plain import resolves to whichever one an earlier test put on sys.path first.
_spec = importlib.util.spec_from_file_location(
    "xegtao_prepared_canonical_harness", INTEGRATION / "test_xegtao_prepared_canonical.py")
harness = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = harness
_spec.loader.exec_module(harness)


class PreparedCanonicalSourceContract(unittest.TestCase):
    def _canonical(self, *, compatibility=False, depth_samples=1, decals=True):
        try:
            return harness.production_source(
                compatibility=compatibility,
                depth_samples=depth_samples,
                decals=decals,
                canonical_depth=True,
            )
        except ValueError as exc:
            # Only the pre-feature five-argument header is a capability skip.
            # Once the factory advertises canonicalDepth, any ValueError is a
            # real source/build defect and must remain visible to the test.
            if "canonical_depth=True requires the six-argument" not in str(exc):
                raise
            self.skipTest(f"canonical production factory not present in this source checkpoint: {exc}")

    def test_exact_compiled_source_factory_selection_is_single_target(self):
        candidate = harness.candidate_source(self._canonical())
        self.assertEqual(len(re.findall(r"@fragment\s+fn\s+fs_canonical_depth\s*\(", candidate)), 1)
        self.assertIn("@group(0) @binding(7)", candidate)
        self.assertIn("AdapterParams", candidate)
        self.assertNotIn("@fragment fn fs_prepared_canonical", candidate)
        self.assertNotIn("@location(1)", harness._function(candidate, "fs_canonical_depth"))

    def test_source_validation_fails_closed_on_drift(self):
        production = self._canonical(compatibility=False, depth_samples=1, decals=False)
        with self.assertRaises(harness.ShaderTransformError):
            harness.candidate_source(production.replace("struct Params", "struct ParamsBroken", 1))
        with self.assertRaises(harness.ShaderTransformError):
            harness.candidate_source(
                production.replace("@binding(7)", "@binding(6)", 1)
            )

    def test_old_source_support_and_explicit_canonical_opt_in_error(self):
        for compatibility in (False, True):
            with self.subTest(compatibility=compatibility):
                legacy = harness.production_source(
                    compatibility=compatibility, depth_samples=1, decals=True
                )
                self.assertIn("@fragment fn fs_decal_depth", legacy)
                try:
                    canonical = harness.production_source(
                        compatibility=compatibility,
                        depth_samples=1,
                        decals=True,
                        canonical_depth=True,
                    )
                except ValueError as exc:
                    # Old headers are a supported fallback for legacy source;
                    # explicit canonical opt-in must never silently downgrade.
                    if "canonical_depth=True requires the six-argument" not in str(exc):
                        raise
                    self.assertIn("six-argument", str(exc))
                else:
                    self.assertNotEqual(canonical, legacy)
                    self.assertRegex(canonical, r"@fragment\s+fn\s+fs_canonical_depth\s*\(")
                with self.assertRaises(harness.ShaderTransformError):
                    harness.candidate_source(legacy)

    def test_manual_adapter_injection_is_rejected(self):
        with self.assertRaises(harness.ShaderTransformError):
            harness.candidate_source(adapter=harness._adapter_source())

    def test_adapter_contract_is_copied_and_not_the_fused_normals_candidate(self):
        adapter = harness._adapter_source()
        candidate = harness.candidate_source(
            self._canonical(compatibility=True, depth_samples=4, decals=True)
        )
        params = re.search(r"struct\s+AdapterParams\s*\{.*?\};", adapter, re.DOTALL)
        self.assertIsNotNone(params)
        for field in ("depth: vec4f", "viewport: vec4f", "scissor: vec4f"):
            self.assertIn(field, params.group(0))
            self.assertIn(field, candidate)
        self.assertIn("@binding(7)", candidate)
        canonical_function = harness._function(candidate, "fs_canonical_depth")
        self.assertNotIn("textureGather(", canonical_function)
        self.assertNotIn("textureStore(", canonical_function)

    def test_uniform_layout_and_exact_source_hashes(self):
        production = harness.production_source(compatibility=False, depth_samples=1, decals=False)
        candidate = harness.candidate_source(self._canonical(compatibility=False, depth_samples=1, decals=False))
        self.assertEqual(
            len(harness.pack_production_params(129, 65, (0, 0, 1, 1), (0, 0, 1, 1), (0, 1), True)),
            80,
        )
        self.assertEqual(
            len(harness.pack_adapter_params(harness.adapter_depth_constants(), (0, 0, 1, 1), (0, 0, 1, 1))),
            48,
        )
        self.assertRegex(harness.source_hash(production), r"^[0-9a-f]{64}$")
        self.assertRegex(harness.source_hash(harness._adapter_source()), r"^[0-9a-f]{64}$")
        self.assertRegex(harness.source_hash(candidate), r"^[0-9a-f]{64}$")
        self.assertRegex(harness._test_ao_gpu_fixture_hash(), r"^[0-9a-f]{64}$")

    def test_bit_diagnostics_is_exact_and_actionable(self):
        if harness.np is None:
            self.skipTest("NumPy is optional for this assertion")
        expected = harness.np.asarray([[1.0, 0.5]], dtype=harness.np.float32)
        actual = expected.copy()
        actual[0, 1] = harness.np.nextafter(actual[0, 1], harness.np.float32(1.0))
        self.assertEqual(harness.bit_diagnostics(expected, expected), {"mismatch_count": 0, "max_ulp": 0})
        diagnostics = harness.bit_diagnostics(expected, actual)
        self.assertEqual(diagnostics["mismatch_count"], 1)
        self.assertGreaterEqual(diagnostics["max_ulp"], 1)


class PreparedCanonicalImportContract(unittest.TestCase):
    def test_case_release_destroys_only_owned_allocations_once(self):
        class Allocation:
            def __init__(self):
                self.destroy_calls = 0

            def destroy(self):
                self.destroy_calls += 1

        owned = [Allocation() for _ in range(10)]
        resources = harness._CaseResources(
            width=1, height=1, depth_attachment=owned[0],
            baseline_prepared=owned[1], baseline_canonical=owned[2],
            candidate_canonical=owned[3], baseline_prepare_group=object(),
            baseline_adapter_group=object(), candidate_group=object(),
            keep_alive=(*owned[4:], object()),
        )
        resources.release()
        resources.release()
        self.assertEqual([item.destroy_calls for item in owned], [1] * 10)
        self.assertEqual(resources.keep_alive, ())
        self.assertIsNone(resources.candidate_group)

    def test_import_does_not_request_gpu(self):
        code = (
            "import sys, types; "
            "stub=types.ModuleType('wgpu'); "
            "stub.gpu=types.SimpleNamespace(request_adapter_sync=lambda **kw: (_ for _ in ()).throw(AssertionError('GPU requested during import'))); "
            "sys.modules['wgpu']=stub; "
            f"sys.path.insert(0, {str(INTEGRATION)!r}); "
            "import test_xegtao_prepared_canonical as m; "
            "print(m.HAS_GPU_DEPS is not None)"
        )
        result = subprocess.run(
            [sys.executable, "-c", code],
            cwd=str(Path(__file__).resolve().parents[1]),
            capture_output=True,
            text=True,
            check=True,
            timeout=30,
        )
        self.assertIn("True", result.stdout)

    def test_timing_report_refuses_existing_path(self):
        # Keep the collision fixture isolated from a developer's build tree;
        # the contract is about an existing path, not a fixed repository file.
        with tempfile.TemporaryDirectory(
            prefix="prepared-canonical-path-", dir=str(harness.ROOT / "build")
        ) as temp:
            target = Path(temp) / "existing.json"
            target.write_text("sentinel\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                harness._validate_output_path(target)


if __name__ == "__main__":  # pragma: no cover
    unittest.main(verbosity=2)
