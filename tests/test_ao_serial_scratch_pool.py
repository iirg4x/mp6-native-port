"""CPU and source-contract checks for serial AO scratch leases."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import build


ROOT = Path(__file__).resolve().parents[1]


CPP_TEST = r"""
#include "ao_serial_scratch_pool.hpp"

#include <cassert>
#include <memory>

struct FakeScratch {
    static int constructed;
    static int destroyed;

    int width;
    int height;
    int format;
    int marker;

    FakeScratch(int w, int h, int f, int m) : width(w), height(h), format(f), marker(m) {
        ++constructed;
    }
    ~FakeScratch() { ++destroyed; }
};

int FakeScratch::constructed = 0;
int FakeScratch::destroyed = 0;

using Pool = mp6::ao::SerialScratchPool<FakeScratch>;

int main() {
    int creates = 0;
    auto make = [&](int width, int height, int format, int marker) {
        ++creates;
        return std::make_shared<FakeScratch>(width, height, format, marker);
    };

    Pool pool;
    auto matches640 = [](const FakeScratch &value) {
        return value.width == 640 && value.height == 360 && value.format == 1;
    };

    // A released lease leaves the pool's sole cached owner, so a serial
    // acquire reuses identity and does not allocate in the hot loop.
    auto first = pool.acquire(matches640, [&] { return make(640, 360, 1, 11); });
    FakeScratch *firstIdentity = first.get();
    first.reset();
    for (int i = 0; i < 1000; ++i) {
        auto lease = pool.acquire(matches640, [&] { return make(640, 360, 1, 11); });
        assert(lease.get() == firstIdentity);
    }
    assert(creates == 1);

    // An outstanding producer/consumer lease forces a distinct object. The
    // old object retains its data while the new object is modified.
    auto oldLease = pool.acquire(matches640, [&] { return make(640, 360, 1, 11); });
    oldLease->marker = 77;
    auto overlapLease = pool.acquire(matches640, [&] { return make(640, 360, 1, 22); });
    assert(overlapLease.get() != oldLease.get());
    overlapLease->marker = 88;
    assert(oldLease->marker == 77);
    assert(overlapLease->marker == 88);

    // A dimensions/format mismatch replaces the idle cached object.
    overlapLease.reset();
    auto replacement = pool.acquire(
        [](const FakeScratch &value) {
            return value.width == 1280 && value.height == 720 && value.format == 2;
        },
        [&] { return make(1280, 720, 2, 33); });
    assert(replacement.get() != firstIdentity);
    assert(replacement->width == 1280 && replacement->height == 720 && replacement->format == 2);

    // Reset drops only the pool owner; a live lease remains valid and keeps
    // the object alive until that lease is released.
    int destroyedBeforeReset = FakeScratch::destroyed;
    FakeScratch *resetIdentity = replacement.get();
    pool.reset();
    assert(!pool.retained());
    assert(replacement.get() == resetIdentity && replacement->marker == 33);
    assert(FakeScratch::destroyed == destroyedBeforeReset);

    // A failed/null replacement must not invalidate an outstanding old lease
    // or mutate its data, even though the pool's cached owner is cleared.
    auto outstanding = replacement;
    outstanding->marker = 44;
    auto failed = pool.acquire(
        [](const FakeScratch &) { return true; },
        []() -> std::shared_ptr<FakeScratch> { return {}; });
    assert(!failed);
    assert(!pool.retained());
    assert(outstanding->marker == 44);
    outstanding.reset();
    replacement.reset();
    assert(FakeScratch::destroyed == destroyedBeforeReset + 1);

    // Destruction occurs once the last lease and the pool owner disappear.
    int destroyedBeforeScope = FakeScratch::destroyed;
    {
        Pool scoped;
        auto lease = scoped.acquire(
            [](const FakeScratch &value) { return value.width == 32 && value.height == 32; },
            [&] { return make(32, 32, 7, 55); });
        scoped.reset();
        assert(FakeScratch::destroyed == destroyedBeforeScope);
        lease.reset();
        assert(FakeScratch::destroyed == destroyedBeforeScope + 1);
    }
    assert(FakeScratch::constructed == creates);
    return 0;
}
"""


class AoSerialScratchPoolTests(unittest.TestCase):
    def test_executable_serial_lease_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix="ao-serial-scratch-", dir=ROOT / "build") as folder:
            directory = Path(folder)
            source = directory / "serial_scratch_selftest.cpp"
            source.write_text(CPP_TEST, encoding="utf-8")
            exe = directory / "serial-scratch-selftest.exe"
            result = subprocess.run(
                [build.ZIG, "c++", "-std=c++20", "-O2", "-UNDEBUG",
                 "-I" + str(ROOT / "src/gx"), str(source), "-o", str(exe)],
                capture_output=True, text=True, timeout=90,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_pool_header_contract_is_serial_and_non_destructive(self):
        header = (ROOT / "src/gx/ao_serial_scratch_pool.hpp").read_text(encoding="utf-8")
        self.assertIn("std::shared_ptr<T> cached;", header)
        self.assertIn("cached.use_count() != 1", header)
        self.assertIn("cached = std::forward<Create>(create)();", header)
        self.assertIn("void reset() { cached.reset(); }", header)
        self.assertNotIn("std::mutex", header)
        self.assertNotIn("std::vector", header)

    def test_ambient_source_contract_keeps_serial_lease_until_draw_end(self):
        source = (ROOT / "src/gx/ambient_occlusion.cpp").read_text(encoding="utf-8")
        composite = source.split("void composite(", 1)[1].split("void foliage_seed(", 1)[0]
        # The composite draws through draw_composite before releasing its lease.
        draw = composite.index("draw_composite(ctx, pass, job.composite")
        lease_reset = composite.index("job.upstream.scratch.reset();")
        available = composite.index("job.available.store(true, std::memory_order_release);")
        self.assertLess(draw, lease_reset)
        self.assertLess(lease_reset, available)
        shutdown = source.split('extern "C" void mp6_ao_shutdown', 1)[1]
        self.assertIn("gfx::synchronize();", shutdown)
        self.assertLess(shutdown.index("gfx::synchronize();"), shutdown.index("job.upstream = {};"))

        # Scratch is a worker-side upstream resource. The game-thread apply
        # path may select a Job, but must not acquire/reset the shared pool.
        apply = source.split("extern \"C\" void mp6_ao_apply", 1)[1].split(
            "extern \"C\" void mp6_ao_shutdown", 1
        )[0]
        self.assertNotIn("scratch", apply)
        self.assertNotIn("release_idle_scratch", apply)

    def test_native_source_contract_uses_per_job_uniforms_and_shared_scratch(self):
        source = (ROOT / "src/gx/xegtao/native.hpp").read_text(encoding="utf-8")
        resources = source.split("struct Resources {", 1)[1].split("};", 1)[0]
        self.assertIn("wgpu::Buffer constants, adapter;", resources)
        self.assertIn("std::shared_ptr", resources)
        self.assertIn("std::shared_ptr<Scratch> scratch;", resources)
        self.assertNotIn("SerialScratchPool", resources)
        self.assertNotIn("wgpu::Buffer uniform", resources)

        pipeline = source.split("class Pipeline {", 1)[1]
        self.assertIn("SerialScratchPool", pipeline)
        self.assertIn("release_idle_scratch", pipeline)

        # Uniform buffers remain per-Resources/per-Job; the serial pool is only
        # for the shared texture bundle and must not replace those bindings.
        self.assertIn("r.constants", pipeline)
        self.assertIn("r.adapter", pipeline)

    def test_game_thread_enqueue_order_seals_encoder_before_composite(self):
        source = (ROOT / "src/gx/ambient_occlusion.cpp").read_text(encoding="utf-8")
        apply = source.split('extern "C" void mp6_ao_apply', 1)[1].split(
            'extern "C" void mp6_ao_shutdown', 1
        )[0]
        task = apply.index("gfx::push_encoder_task")
        note = apply.index("mp6_fi_note_ao(view)")
        composite = apply.index("gfx::push_custom_draw(g_drawType")
        self.assertLess(task, note)
        self.assertLess(note, composite)
        self.assertIn("push_encoder_task drains GX before sealing", apply)


if __name__ == "__main__":
    unittest.main()
