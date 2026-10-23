"""CPU/source contracts for the opt-in XeGTAO batched submission path."""

from pathlib import Path
import re
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "gx" / "xegtao" / "native.hpp"
sys.path.insert(0, str(ROOT / "tests"))
from test_xegtao_parameters import run_cpp


def _section(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class XeGTAOBatchCompute(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text()

    def test_exact_env_is_latched_once_and_default_stays_silent(self):
        source = self.source
        self.assertIn("bool batchedCompute = false;", source)
        self.assertEqual(source.count('std::getenv("MP6_AO_XEGTAO_BATCH_COMPUTE")'), 1)
        initialize = _section(source, "    bool initialize(", "    void reset()")
        self.assertLess(initialize.index("if (checked) return supported && accept_deferred();"),
                        initialize.index("MP6_AO_XEGTAO_BATCH_COMPUTE"))
        self.assertIn('std::strcmp(batchComputeEnv, "1") == 0', initialize)
        self.assertEqual(source.count("[MP6-AO] XeGTAO compute submission: batched"), 1)
        self.assertIn(
            'if (batchedCompute)\n            std::fprintf(stderr, "[MP6-AO] XeGTAO compute submission: batched\\n");',
            initialize,
        )

    def test_batched_pass_has_one_timestamp_and_rebinds_each_stage(self):
        batch = _section(self.source, "    void dispatch_batched(", "    void dispatch(")
        self.assertEqual(batch.count("BeginComputePass"), 1)
        self.assertEqual(batch.count("pass.End();"), 1)
        self.assertEqual(batch.count("pass.DispatchWorkgroups"), 1)
        self.assertEqual(batch.count("aurora::gfx::gpu_pass_timestamps("), 1)
        self.assertIn('.label = "XeGTAO compute"', batch)
        self.assertIn('gpu_pass_timestamps("XeGTAO compute")', batch)
        self.assertIn("pass.SetBindGroup(0, r.common);", batch)
        self.assertIn(
            "for (uint32_t gap = 1; gap < groupIndex; ++gap) pass.SetBindGroup(gap, emptyGroup);",
            batch,
        )
        self.assertIn("pass.SetBindGroup(groupIndex, group);", batch)
        self.assertNotIn("XeGTAO depth MIPs", batch)
        self.assertNotIn("XeGTAO normals", batch)
        self.assertNotIn("XeGTAO main", batch)
        self.assertNotIn("XeGTAO denoise", batch)
        self.assertEqual(
            re.findall(r"dispatch_stage\((\d+),", batch),
            ["0", "1", "2", "3", "4"],
        )

    def test_separate_path_keeps_stage_labels_and_resource_contract(self):
        source = self.source
        dispatch = _section(source, "    void dispatch(", "    bool initialization_failed")
        self.assertEqual(dispatch.count("pass.End();"), 1)
        self.assertIn("pass.SetBindGroup(0, r.common);", dispatch)
        self.assertIn("pass.SetBindGroup(groupIndex, group);", dispatch)
        encode = source.split("    void encode(", 1)[1]
        self.assertIn("if (batchedCompute)", encode)
        separate = encode.split("if (batchedCompute)", 1)[1].split("    }", 1)[1]
        self.assertEqual(
            re.findall(r"dispatch\(encoder,\s*([^,]+)", separate),
            ["0", "1", "2", "3", "4"],
        )
        for label in (
            "XeGTAO depth MIPs", "XeGTAO normals", "XeGTAO main",
            "XeGTAO denoise", "XeGTAO final denoise",
        ):
            self.assertIn(label, separate)

    def _fake_batch_source(self, timestamps: bool) -> str:
        """Compile the production batch method against a recording API."""
        batch = "void dispatch_batched(" + _section(
            self.source, "    void dispatch_batched(", "    void dispatch("
        )
        batch = (batch
                 .replace("wgpu::CommandEncoder", "FakeEncoder")
                 .replace("wgpu::ComputePassDescriptor", "FakeComputePassDescriptor")
                 .replace("wgpu::BindGroup", "FakeBindGroup"))
        feature = "#define AURORA_GPU_STAT_ROWS 1\n" if timestamps else ""
        return feature + r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct Event {
    unsigned kind;
    unsigned first;
    unsigned second;
};

struct FakeLog {
    std::vector<Event> events;
    std::vector<std::string> timestamps;
    std::string passLabel;
    unsigned beginCount = 0;
    unsigned endCount = 0;
};

struct FakeBindGroup { unsigned id = 0; };
struct FakePipeline { unsigned id = 0; };
struct FakeTimestampWrites { const char *label = nullptr; };

struct FakeComputePassDescriptor {
    const char *label = nullptr;
#ifdef AURORA_GPU_STAT_ROWS
    const FakeTimestampWrites *timestampWrites = nullptr;
#endif
};

struct FakePass {
    FakeLog *log;
    void SetPipeline(FakePipeline pipeline) {
        log->events.push_back({1, pipeline.id, 0});
    }
    void SetBindGroup(unsigned slot, FakeBindGroup group) {
        log->events.push_back({2, slot, group.id});
    }
    void DispatchWorkgroups(unsigned width, unsigned height) {
        log->events.push_back({3, width, height});
    }
    void End() { ++log->endCount; }
};

struct FakeEncoder {
    FakeLog *log;
    FakePass BeginComputePass(const FakeComputePassDescriptor *desc) const {
        ++log->beginCount;
        log->passLabel = desc->label ? desc->label : "";
#ifdef AURORA_GPU_STAT_ROWS
        if (desc->timestampWrites && desc->timestampWrites->label)
            log->timestamps.emplace_back(desc->timestampWrites->label);
#endif
        return {log};
    }
};

#ifdef AURORA_GPU_STAT_ROWS
namespace aurora::gfx {
const FakeTimestampWrites *gpu_pass_timestamps(const char *label) {
    static FakeTimestampWrites writes;
    writes.label = label;
    return &writes;
}
}
#endif

struct Resources {
    FakeBindGroup common;
    unsigned finalAoIndex = 0;
};
struct Scratch { unsigned width = 0, height = 0; };
struct FakePolicy { unsigned denoisePasses = 2; };

struct TestPipeline {
    std::array<FakePipeline, 5> stages{{{0}, {1}, {2}, {3}, {4}}};
    FakeBindGroup emptyGroup{99};
    FakePolicy policy;
''' + batch + r'''
};

static void expect_event(const Event &actual, unsigned kind,
                         unsigned first, unsigned second) {
    assert(actual.kind == kind);
    assert(actual.first == first);
    assert(actual.second == second);
}

static void check_policy(unsigned denoisePasses) {
    FakeLog log;
    FakeEncoder encoder{&log};
    TestPipeline pipeline;
    pipeline.policy.denoisePasses = denoisePasses;
    Resources resources{{7}, denoisePasses % 2};
    Scratch scratch{37, 19};
    const FakeBindGroup prefilter{10};
    const FakeBindGroup normal{30};
    const FakeBindGroup main{20};
    const std::array<FakeBindGroup, 2> denoise{{{40}, {41}}};

    pipeline.dispatch_batched(encoder, resources, scratch,
                              prefilter, normal, main, denoise);
    assert(log.beginCount == 1);
    assert(log.endCount == 1);
    assert(log.passLabel == "XeGTAO compute");

    std::vector<Event> expected;
    const auto expected_stage = [&](unsigned stage, unsigned groupIndex,
                                    unsigned groupId, unsigned width,
                                    unsigned height) {
        expected.push_back({1, stage, 0});
        expected.push_back({2, 0, 7});
        for (unsigned gap = 1; gap < groupIndex; ++gap)
            expected.push_back({2, gap, 99});
        expected.push_back({2, groupIndex, groupId});
        expected.push_back({3, width, height});
    };
    expected_stage(0, 1, 10, 3, 2);
    expected_stage(1, 3, 30, 5, 3);
    expected_stage(2, 2, 20, 5, 3);
    for (unsigned i = 0; i + 1 < denoisePasses; ++i)
        expected_stage(3, 3, 40 + i % 2, 3, 3);
    expected_stage(4, 3, 40 + (1 - resources.finalAoIndex), 3, 3);

    assert(log.events.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        expect_event(log.events[i], expected[i].kind,
                     expected[i].first, expected[i].second);
}

int main() {
    for (unsigned denoisePasses = 1; denoisePasses <= 3; ++denoisePasses)
        check_policy(denoisePasses);
#ifdef AURORA_GPU_STAT_ROWS
    // The descriptor's one shared timestamp is the only timestamp write.
    // Each policy call has its own encoder/log, so check this in one call.
    FakeLog log;
    FakeEncoder encoder{&log};
    TestPipeline pipeline;
    Resources resources{{7}, 0};
    Scratch scratch{16, 16};
    const FakeBindGroup group{10};
    const std::array<FakeBindGroup, 2> denoise{{{40}, {41}}};
    pipeline.dispatch_batched(encoder, resources, scratch,
                              group, group, group, denoise);
    assert(log.timestamps.size() == 1);
    assert(log.timestamps[0] == "XeGTAO compute");
#else
    // Without AURORA_GPU_STAT_ROWS the same production method must submit no
    // timestamp writes while retaining the one-pass submission.
    FakeLog log;
    FakeEncoder encoder{&log};
    TestPipeline pipeline;
    Resources resources{{7}, 0};
    Scratch scratch{16, 16};
    const FakeBindGroup group{10};
    const std::array<FakeBindGroup, 2> denoise{{{40}, {41}}};
    pipeline.dispatch_batched(encoder, resources, scratch,
                              group, group, group, denoise);
    assert(log.timestamps.empty());
#endif
}
'''

    def test_fake_pass_executes_production_batch_method_without_timestamps(self):
        run_cpp(self._fake_batch_source(False))

    def test_fake_pass_executes_production_batch_method_with_one_timestamp(self):
        run_cpp(self._fake_batch_source(True))

    def test_fake_environment_latch_uses_default_false_and_exact_one(self):
        run_cpp(r'''
#include <cassert>
#include <cstdlib>
#include <cstring>

struct Latch {
    bool checked = false;
    bool enabled = false;
    bool initialize() {
        if (checked) return enabled;
        checked = true;
        const char *value = std::getenv("MP6_AO_XEGTAO_BATCH_COMPUTE");
        enabled = value && std::strcmp(value, "1") == 0;
        return enabled;
    }
};

int main() {
    _putenv_s("MP6_AO_XEGTAO_BATCH_COMPUTE", "");
    Latch baseline;
    assert(!baseline.initialize());
    _putenv_s("MP6_AO_XEGTAO_BATCH_COMPUTE", "1");
    assert(!baseline.initialize());

    Latch enabled;
    assert(enabled.initialize());
    _putenv_s("MP6_AO_XEGTAO_BATCH_COMPUTE", "0");
    assert(enabled.initialize());
}
''')


if __name__ == "__main__":
    unittest.main()
