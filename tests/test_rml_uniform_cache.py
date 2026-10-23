"""Bounded behavioral checks for the per-frame RmlUi uniform-range cache."""

from dataclasses import dataclass, replace
from pathlib import Path
import struct
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from setup.lib import step_aurora  # noqa: E402
from tools import apply_patches, build  # noqa: E402


TARGETS = (
    "lib/rmlui/WebGPURenderInterface.hpp",
    "lib/rmlui/WebGPURenderInterface.cpp",
)


@dataclass(frozen=True)
class UniformKey:
    """The 21 float values serialized by UniformBlock and push_uniform()."""

    matrix: tuple[float, ...]
    translation: tuple[float, float, float, float]
    gamma: float

    def bytes(self) -> bytes:
        if len(self.matrix) != 16:
            raise ValueError("UniformBlock MVP must contain 16 floats")
        return struct.pack("<21f", *self.matrix, *self.translation, self.gamma)


class UniformCacheKernel:
    """Small executable model of the backend cache's lifecycle and eviction."""

    capacity = 8

    def __init__(self):
        self.entries: list[tuple[bytes, tuple[int, int]]] = []
        self.next_slot = 0
        self.pushes = 0
        self.in_frame = False

    def reset(self):
        self.entries.clear()
        self.next_slot = 0

    def begin_frame(self):
        self.reset()
        self.in_frame = True

    def end_frame(self):
        self.reset()
        self.in_frame = False

    def create_device_objects(self):
        self.reset()

    def setup(self, key: UniformKey) -> tuple[int, int]:
        if not self.in_frame:
            raise AssertionError("uniform ranges are only valid while recording")
        payload = key.bytes()
        for cached, value in self.entries:
            if cached == payload:
                return value

        value = (self.pushes * 256, len(payload))
        self.pushes += 1
        entry = (payload, value)
        if len(self.entries) < self.capacity:
            self.entries.append(entry)
        else:
            self.entries[self.next_slot] = entry
            self.next_slot = (self.next_slot + 1) % self.capacity
        return value


def _base_key() -> UniformKey:
    return UniformKey(
        tuple(float(i) for i in range(16)),
        (12.0, -4.0, 0.0, 0.0),
        2.2,
    )


class RmlUniformCacheTests(unittest.TestCase):
    def test_kernel_reuses_only_identical_bytes_within_one_frame(self):
        cache = UniformCacheKernel()
        key = _base_key()
        cache.begin_frame()

        first = cache.setup(key)
        self.assertEqual(cache.setup(key), first)
        self.assertEqual(cache.pushes, 1)

        # Each field represented in the payload must distinguish a draw. This
        # covers matrix, translation, opaque-scene (z), mask mode (w), and
        # gamma without relying on source-string assertions alone.
        mutations = (
            replace(key, matrix=tuple(99.0 if i == 0 else value
                                       for i, value in enumerate(key.matrix))),
            replace(key, translation=(13.0, *key.translation[1:])),
            replace(key, translation=(key.translation[0], key.translation[1], 1.0, key.translation[3])),
            replace(key, translation=(key.translation[0], key.translation[1], key.translation[2], 1.0)),
            replace(key, gamma=1.0),
        )
        for mutation in mutations:
            self.assertNotEqual(mutation.bytes(), key.bytes())
            isolated = UniformCacheKernel()
            isolated.begin_frame()
            original_range = isolated.setup(key)
            self.assertNotEqual(isolated.setup(mutation), original_range)
            self.assertEqual(isolated.pushes, 2)

        # Full byte identity deliberately distinguishes signed zero too.
        positive_zero = replace(key, translation=(0.0, *key.translation[1:]))
        signed_zero = replace(key, translation=(-0.0, *key.translation[1:]))
        self.assertNotEqual(signed_zero.bytes(), positive_zero.bytes())
        self.assertNotEqual(cache.setup(signed_zero), cache.setup(positive_zero))

    def test_lifecycle_does_not_leak_staging_ranges_between_frames_or_devices(self):
        cache = UniformCacheKernel()
        key = _base_key()
        cache.begin_frame()
        old_range = cache.setup(key)
        cache.end_frame()

        cache.begin_frame()
        self.assertNotEqual(cache.setup(key), old_range)
        cache.create_device_objects()
        # Device recreation invalidates the cache even if it occurs while the
        # caller is still inside the recording boundary.
        device_range = cache.setup(key)
        self.assertNotEqual(device_range, old_range)
        cache.begin_frame()
        self.assertEqual(cache.pushes, 3)
        self.assertNotEqual(cache.setup(key), old_range)

    def test_cache_is_bounded_and_replaces_oldest_slot(self):
        cache = UniformCacheKernel()
        cache.begin_frame()
        keys = [replace(_base_key(), matrix=tuple(float(i + n) for i in range(16)))
                for n in range(cache.capacity + 1)]
        ranges = [cache.setup(key) for key in keys]
        self.assertEqual(cache.pushes, cache.capacity + 1)
        self.assertNotEqual(cache.setup(keys[0]), ranges[0])
        self.assertEqual(cache.pushes, cache.capacity + 2)

    def test_patch_hunks_and_generated_lifecycle_are_exact(self):
        generated = ROOT / "third_party/aurora/lib/rmlui/WebGPURenderInterface.cpp"
        if not generated.exists():
            self.skipTest("generated Aurora release source is unavailable")
        source = generated.read_text(encoding="utf-8")
        self.assertEqual(source.count("ResetUniformCache();"), 3)
        self.assertEqual(source.count("void WebGPURenderInterface::ResetUniformCache()"), 1)
        self.assertIn("std::memcmp(&lhs, &rhs, sizeof(UniformBlock))", source)
        self.assertIn("static_assert(sizeof(UniformBlock) == sizeof(float) * 21)", source)
        setup = source.split("gfx::Range WebGPURenderInterface::SetupRenderState", 1)[1]
        setup = setup.split("void WebGPURenderInterface::ResetUniformCache", 1)[0]
        for assignment in ("ubo.MVP =", "ubo.translation =", "ubo.Gamma ="):
            self.assertIn(assignment, setup)
        self.assertIn("return entry.range;", setup)

if __name__ == "__main__":
    unittest.main()
