import json
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "integration"))

import ao_resource_probe as probe
from tests.test_xegtao_parameters import run_cpp


class AOResourceProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source_path = ROOT / "src" / "gx" / "ambient_occlusion.cpp"
        cls.source = cls.source_path.read_text(encoding="utf-8")

    def test_estimate_uses_requested_mip_extents_and_formats(self):
        self.assertEqual(probe.requested_texture_bytes(8, 4, 3, 4), 168)
        self.assertEqual(probe.requested_texture_bytes(1, 1, 5, 2), 10)
        self.assertEqual(probe.requested_texture_bytes(0, 0, 2, 8), 16)
        with self.assertRaises(ValueError):
            probe.requested_texture_bytes(8, 4, 0, 4)
        with self.assertRaises(TypeError):
            probe.requested_texture_bytes(8.0, 4, 1, 4)

    def test_compiled_census_math_and_interleaved_frame_peaks(self):
        run_cpp(probe.RESOURCE_HEADER + r'''
#include <cassert>
#include <cstdint>

using namespace mp6_ao_resource_probe;

static void add_job(Census &census, const void *scratchIdentity,
                    uint32_t targetWidth, uint32_t targetHeight,
                    uint32_t workingWidth, uint32_t workingHeight, bool depthHalf) {
    observe_job(census,
        false, false, 0, 0,
        true, targetWidth, targetHeight,
        true, 80, true, 96, true, 48, true);
    observe_scratch(census, scratchIdentity, workingWidth, workingHeight, depthHalf,
                    true, true, true, true);
}

int main() {
    int scratchA = 1;
    int scratchB = 2;
    Census fullResolution{};
    // Pipeline-retained Scratch plus two Job leases must count one bundle.
    observe_scratch(fullResolution, &scratchA, 1280, 720, false, true, true, true, true);
    add_job(fullResolution, &scratchA, 1280, 720, 1280, 720, false);
    add_job(fullResolution, &scratchA, 1280, 720, 1280, 720, false);
    fullResolution.retainedTextureBytes = fullResolution.legacyTextureBytes +
        fullResolution.cleanDepthBytes + fullResolution.upstreamTextureBytes;
    assert(fullResolution.upstreamRawDepth == 1);
    assert(fullResolution.upstreamDepthMips == 1);
    assert(fullResolution.upstreamNormals == 1);
    assert(fullResolution.upstreamAo0 == 1);
    assert(fullResolution.cleanDepth == 2);
    assert(fullResolution.upstreamBundles == 1);
    assert(fullResolution.upstreamBundleRefs == 3);
    assert(fullResolution.upstreamLeaseSlots == 2);
    assert(fullResolution.retainedSlots == 2);
    assert(fullResolution.uniformBytes == 448);
    assert(fullResolution.retainedTextureBytes == 23342400);

    Census halfResolution{};
    observe_scratch(halfResolution, &scratchA, 641, 361, true, true, true, true, true);
    add_job(halfResolution, &scratchA, 1281, 721, 641, 361, true);
    add_job(halfResolution, &scratchA, 1281, 721, 641, 361, true);
    halfResolution.retainedTextureBytes = halfResolution.legacyTextureBytes +
        halfResolution.cleanDepthBytes + halfResolution.upstreamTextureBytes;
    assert(halfResolution.upstreamDepthMips == 1);
    assert(halfResolution.cleanDepthBytes == 2 * 1281u * 721u * 4u);
    assert(halfResolution.upstreamBundles == 1);
    assert(halfResolution.upstreamBundleRefs == 3);
    assert(halfResolution.uniformBytes == 448);
    assert(halfResolution.retainedTextureBytes == 10781382);

    Census legacy{};
    observe_job(legacy,
        true, true, 1281, 721,
        true, 1281, 721,
        true, 80, false, 0, false, 0, false);
    observe_job(legacy,
        true, true, 1281, 721,
        true, 1281, 721,
        true, 80, false, 0, false, 0, false);
    legacy.retainedTextureBytes = legacy.legacyTextureBytes + legacy.cleanDepthBytes +
        legacy.upstreamTextureBytes;
    assert(legacy.legacyTextures == 4);
    assert(legacy.legacySlots == 2);
    assert(legacy.cleanSlots == 2);
    assert(legacy.uniformBytes == 160);
    assert(legacy.retainedTextureBytes == 36944040);

    Census overlap{};
    observe_scratch(overlap, &scratchA, 1280, 720, false, true, true, true, true);
    observe_scratch(overlap, &scratchA, 1280, 720, false, true, true, true, true);
    observe_scratch(overlap, &scratchB, 641, 361, true, true, true, true, true);
    overlap.retainedTextureBytes = overlap.legacyTextureBytes + overlap.cleanDepthBytes +
        overlap.upstreamTextureBytes;
    assert(overlap.upstreamBundles == 2);
    assert(overlap.upstreamBundleRefs == 3);
    assert(overlap.upstreamTextureBytes == 15969600 + 3392574);

    Census fallback{};
    observe_job(fallback,
        true, true, 1281, 721, true, 1281, 721,
        true, 80, false, 0, false, 0, false);
    fallback.retainedTextureBytes = fallback.legacyTextureBytes + fallback.cleanDepthBytes +
        fallback.upstreamTextureBytes;
    assert(fallback.upstreamBundles == 0);
    assert(fallback.upstreamBundleRefs == 0);
    assert(fallback.upstreamTextureBytes == 0);
    assert(fallback.uniformBytes == 80);
    assert(fallback.retainedTextureBytes == 3694404 * 5);

    state = State{};
    note_frame(7);
    note_frame(8);
    note_frame(7);
    note_frame(8);
    note_frame(7);
    assert(state.frameCounts[7].frame == 7);
    assert(state.frameCounts[7].count == 3);
    assert(state.frameCounts[8].frame == 8);
    assert(state.frameCounts[8].count == 2);
    assert(state.currentFrame == 7);
    assert(state.encodesInFrame == 3);
    assert(state.maxEncodesPerFrame == 3);
}
''')

    def test_transform_covers_current_callback_and_preserves_draws(self):
        transformed = probe.instrument_ao_resources(self.source)
        self.assertIn("MP6_QA_AO_RESOURCES", transformed)
        self.assertIn("static const bool latched", transformed)
        self.assertIn("g_xegtao.retained_scratch()", transformed)
        self.assertIn("bool(scratch->ao[0])", transformed)
        self.assertIn("upstream_bundles", transformed)
        body_start = transformed.index("namespace mp6_ao_resource_probe")
        self.assertNotIn("#include <", transformed[body_start:])
        self.assertIn("ctx.targetWidth", transformed)
        self.assertIn("census.jobWidth = job.width", transformed)
        self.assertIn("census.fullWidth = job.fullWidth", transformed)
        self.assertIn("census.upstreamWidth = job.upstream.scratch->width", transformed)
        self.assertIn("kEstimateScope", transformed)
        self.assertIn("kMaxChangeLogs = 64", transformed)
        self.assertEqual(transformed.count("[AO-RESOURCES]"), 1)
        self.assertEqual(transformed.count("pass.Draw(3);"), self.source.count("pass.Draw(3);"))
        self.assertLess(
            transformed.index("++g_encoded;"),
            transformed.index("mp6_ao_resource_probe::observe(job.qaResourceFrame, census);"),
        )
        self.assertLess(
            transformed.index("gfx::synchronize();"),
            transformed.index("mp6_ao_resource_probe::shutdown();"),
        )
        self.assertNotIn("physical VRAM", transformed.split("namespace mp6_ao_resource_probe", 1)[1])

    def test_transform_fails_closed_for_missing_or_duplicate_exact_anchors(self):
        anchors = (
            probe.JOB_ARRAY_ANCHOR,
            probe.FRAME_MEMBER_ANCHOR,
            probe.FRAME_ASSIGN_ANCHOR,
            probe.ENCODE_END_ANCHOR,
            probe.SHUTDOWN_SYNC_ANCHOR,
            probe.PIPELINE_DECL_ANCHOR,
        )
        for anchor in anchors:
            with self.subTest(anchor=anchor, state="missing"):
                missing = self.source.replace(anchor, "", 1)
                with self.assertRaisesRegex(ValueError, "exactly one"):
                    probe.instrument_ao_resources(missing)
            with self.subTest(anchor=anchor, state="duplicate"):
                duplicate = self.source.replace(anchor, anchor + "\n" + anchor, 1)
                with self.assertRaisesRegex(ValueError, "exactly one"):
                    probe.instrument_ao_resources(duplicate)
        with self.assertRaises(ValueError):
            probe.instrument_ao_resources(probe.instrument_ao_resources(self.source))

    @staticmethod
    def _record(phase, change_logs=1):
        return {
            "version": 1,
            "phase": phase,
            "job_slots": 64,
            "frame": 7,
            "legacy_textures": 2,
            "clean_depth": 1,
            "upstream_raw_depth": 1,
            "upstream_depth_mips": 1,
            "upstream_normals": 1,
            "upstream_ao0": 1,
            "legacy_slots": 1,
            "upstream_slots": 1,
            "clean_slots": 1,
            "retained_slots": 3,
            "legacy_texture_bytes": 512,
            "clean_depth_bytes": 512,
            "upstream_texture_bytes": 106,
            "retained_texture_bytes": 1130,
            "uniform_bytes": 256,
            "target_width": 16,
            "target_height": 8,
            "job_width": 4,
            "job_height": 2,
            "full_width": 16,
            "full_height": 8,
            "upstream_width": 4,
            "upstream_height": 2,
            "encodes_in_frame": 3,
            "peak_slots": 3,
            "peak_retained_texture_bytes": 1130,
            "max_encodes_per_frame": 3,
            "estimate_scope": probe.V1_ESTIMATE_SCOPE,
            "change_logs": change_logs,
        }

    @classmethod
    def _record_v2(cls, phase, change_logs=1):
        record = cls._record(phase, change_logs)
        record.pop("upstream_slots")
        record.update({
            "version": 2,
            "provenance": probe.V2_PROVENANCE,
            "upstream_bundles": 1,
            "upstream_bundle_refs": 3,
            "upstream_lease_slots": 2,
            "estimate_scope": probe.ESTIMATE_SCOPE,
        })
        return record

    def test_parser_requires_bounded_encode_rows_and_final_post_sync_row(self):
        change = self._record("encode", 1)
        final = self._record("shutdown_after_synchronize", 1)
        log = (
            "noise before rows\n"
            + "[AO-RESOURCES-CHANGE] " + json.dumps(change, separators=(",", ":")) + "\n"
            + "[AO-RESOURCES] " + json.dumps(final, separators=(",", ":")) + "\n"
            + "release follows\n"
        )
        parsed = probe.parse_resource_log(log)
        self.assertEqual(parsed["record"], final)
        self.assertEqual(parsed["changes"], [change])
        self.assertEqual(parsed["schema_version"], 1)
        self.assertEqual(parsed["provenance"], "legacy_job_owned_draw_snapshot")
        self.assertEqual(parsed["record"]["target_width"], 16)
        self.assertEqual(parsed["record"]["job_width"], 4)
        self.assertEqual(parsed["record"]["full_width"], 16)
        self.assertEqual(parsed["record"]["upstream_width"], 4)
        json.dumps(parsed)

        with self.assertRaises(ValueError):
            probe.parse_resource_log(log + "[AO-RESOURCES] " + json.dumps(final) + "\n")
        with self.assertRaises(ValueError):
            probe.parse_resource_log("[AO-RESOURCES] " + json.dumps(final) + "\n")
        no_change_final = self._record("shutdown_after_synchronize", 0)
        with self.assertRaises(ValueError):
            probe.parse_resource_log("[AO-RESOURCES] " + json.dumps(no_change_final) + "\n")
        zero_encode = self._record("shutdown_after_synchronize", 0)
        zero_encode["encodes_in_frame"] = 0
        zero_encode["max_encodes_per_frame"] = 0
        with self.assertRaises(ValueError):
            probe.parse_resource_log("[AO-RESOURCES] " + json.dumps(zero_encode) + "\n")

        v2_change = self._record_v2("encode", 1)
        v2_final = self._record_v2("shutdown_after_synchronize", 1)
        v2_log = (
            "[AO-RESOURCES-CHANGE] " + json.dumps(v2_change, separators=(",", ":")) + "\n"
            + "[AO-RESOURCES] " + json.dumps(v2_final, separators=(",", ":")) + "\n"
        )
        parsed_v2 = probe.parse_resource_log(v2_log)
        self.assertEqual(parsed_v2["schema_version"], 2)
        self.assertEqual(parsed_v2["provenance"], probe.V2_PROVENANCE)
        self.assertEqual(parsed_v2["record"]["upstream_bundles"], 1)
        self.assertNotIn("upstream_slots", parsed_v2["record"])
        with self.assertRaises(ValueError):
            probe.parse_resource_log(
                "[AO-RESOURCES-CHANGE] " + json.dumps(change) + "\n"
                + "[AO-RESOURCES] " + json.dumps(v2_final) + "\n"
            )
        with self.assertRaises(ValueError):
            probe.parse_resource_log(
                "[AO-RESOURCES-CHANGE] " + json.dumps(v2_change) + "\n"
                + "[AO-RESOURCES] " + json.dumps(final) + "\n"
            )

    def test_parser_rejects_malformed_duplicate_and_unbounded_rows(self):
        change = self._record("encode", 1)
        final = self._record("shutdown_after_synchronize", 1)
        final_line = "[AO-RESOURCES] " + json.dumps(final, separators=(",", ":")) + "\n"
        change_line = "[AO-RESOURCES-CHANGE] " + json.dumps(change, separators=(",", ":")) + "\n"
        duplicate_field = '[AO-RESOURCES] {"version":1,"version":1}\n'
        with self.assertRaises(ValueError):
            probe.parse_resource_log(duplicate_field)

        unknown = dict(final)
        unknown["unexpected"] = 1
        with self.assertRaises(ValueError):
            probe.parse_resource_log("[AO-RESOURCES] " + json.dumps(unknown) + "\n")

        wrong_sum = dict(final)
        wrong_sum["retained_texture_bytes"] += 1
        with self.assertRaises(ValueError):
            probe.parse_resource_log("[AO-RESOURCES] " + json.dumps(wrong_sum) + "\n")

        too_many = change_line * (probe.MAX_CHANGE_LOGS + 1) + final_line
        with self.assertRaises(ValueError):
            probe.parse_resource_log(too_many)

    def test_board_qa_wires_probe_only_for_opt_in_qa_unit_and_hashes_helper(self):
        builder = (ROOT / "tests" / "integration" / "build_board_qa.py").read_text(encoding="utf-8")
        runner = (ROOT / "tests" / "integration" / "run_board_qa.py").read_text(encoding="utf-8")
        normal_build = (ROOT / "tools" / "build.py").read_text(encoding="utf-8")

        self.assertIn("from tests.integration.ao_resource_probe import instrument_ao_resources", builder)
        self.assertIn("contents = instrument_ao_resources(contents)", builder)
        self.assertIn("if name == 'plat_ambient_occlusion_aurora.o':", builder)
        self.assertIn("ao_resource_probe_sha256", builder)
        self.assertIn("tests/integration/ao_resource_probe.py", builder)
        self.assertIn("from tests.integration.ao_resource_probe import parse_resource_log", runner)
        self.assertIn("env['MP6_QA_AO_RESOURCES'] = '1'", runner)
        self.assertIn("report['ao_resources'] = parse_resource_log(log)", runner)
        self.assertIn("ready.get('ao_resource_probe_sha256')", runner)
        self.assertIn("tests/integration/ao_resource_probe.py", runner)
        self.assertNotIn("ao_resource_probe", normal_build)
        self.assertNotIn(probe.MARKER, self.source)


if __name__ == "__main__":
    unittest.main()
