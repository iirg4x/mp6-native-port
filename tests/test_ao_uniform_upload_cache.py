"""CPU checks for the AO per-buffer byte-exact uniform upload caches."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import build


ROOT = Path(__file__).resolve().parents[1]


CPP_TEST = r"""
#include "ao_uniform_upload_cache.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <type_traits>

struct Params80 { std::array<unsigned char, 80> bytes{}; };
struct Constants96 { std::array<unsigned char, 96> bytes{}; };
struct Adapter48 { std::array<unsigned char, 48> bytes{}; };

static_assert(sizeof(Params80) == 80);
static_assert(sizeof(Constants96) == 96);
static_assert(sizeof(Adapter48) == 48);
static_assert(std::is_trivially_copyable_v<Params80>);
static_assert(std::is_trivially_copyable_v<Constants96>);
static_assert(std::is_trivially_copyable_v<Adapter48>);

template <typename T>
struct Uploads {
    unsigned count = 0;
    std::array<unsigned char, sizeof(T)> last{};

    void operator()(const void *data, std::size_t size) {
        assert(size == sizeof(T));
        std::memcpy(last.data(), data, size);
        ++count;
    }
};

template <typename T>
void same_change_reset() {
    mp6::ao::UniformUploadCache<T> cache;
    Uploads<T> uploads;
    T zero{};

    assert(!cache.valid());
    assert(cache.upload_if_changed(zero, uploads));
    assert(uploads.count == 1); // The first all-zero block is not suppressed.
    assert(!cache.upload_if_changed(zero, uploads));
    assert(uploads.count == 1);

    T changed = zero;
    changed.bytes[sizeof(T) - 1] = 0x7f;
    assert(cache.upload_if_changed(changed, uploads));
    assert(uploads.count == 2);
    assert(uploads.last.back() == 0x7f);
    assert(!cache.upload_if_changed(changed, uploads));
    assert(uploads.count == 2);

    cache.reset();
    assert(!cache.valid());
    assert(cache.upload_if_changed(changed, uploads));
    assert(uploads.count == 3); // Reset makes the same bytes upload again.
}

int main() {
    same_change_reset<Params80>();
    same_change_reset<Constants96>();
    same_change_reset<Adapter48>();

    // Caches model distinct GPU buffers/resources; equal bytes must not share
    // a global decision, and changing one cache cannot suppress another.
    mp6::ao::UniformUploadCache<Params80> first;
    mp6::ao::UniformUploadCache<Params80> second;
    Uploads<Params80> first_uploads;
    Uploads<Params80> second_uploads;
    Params80 value{};
    assert(first.upload_if_changed(value, first_uploads));
    assert(second.upload_if_changed(value, second_uploads));
    assert(first_uploads.count == 1 && second_uploads.count == 1);
    value.bytes[0] = 1;
    assert(first.upload_if_changed(value, first_uploads));
    assert(first_uploads.count == 2 && second_uploads.count == 1);
}
"""


class AoUniformUploadCacheTests(unittest.TestCase):
    def test_executable_cache_lifecycle_and_per_resource_isolation(self):
        with tempfile.TemporaryDirectory(prefix="ao-uniform-cache-", dir=ROOT / "build") as folder:
            directory = Path(folder)
            (directory / "cache_selftest.cpp").write_text(CPP_TEST, encoding="utf-8")
            exe = directory / "cache-selftest.exe"
            result = subprocess.run(
                [build.ZIG, "c++", "-std=c++20", "-O2", "-UNDEBUG",
                 "-I" + str(ROOT / "src/gx"), str(directory / "cache_selftest.cpp"),
                 "-o", str(exe)],
                capture_output=True, text=True, timeout=90,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_production_wiring_has_typed_caches_and_explicit_resets(self):
        ambient = (ROOT / "src/gx/ambient_occlusion.cpp").read_text(encoding="utf-8")
        native = (ROOT / "src/gx/xegtao/native.hpp").read_text(encoding="utf-8")
        header = (ROOT / "src/gx/ao_uniform_upload_cache.hpp").read_text(encoding="utf-8")

        self.assertIn("std::array<std::byte, sizeof(T)> snapshot", header)
        self.assertIn("std::is_trivially_copyable_v<T>", header)
        self.assertIn("std::memcmp(snapshot.data(), &value, sizeof(value))", header)
        self.assertIn("std::memcpy(snapshot.data(), &value, sizeof(value))", header)
        self.assertIn("#include \"ao_uniform_upload_cache.hpp\"", ambient)
        self.assertIn("UniformUploadCache<Params> uniformUpload", ambient)
        self.assertIn("job.uniformUpload.upload_if_changed(job.params", ambient)
        self.assertIn("job.uniformUpload.reset();", ambient)
        self.assertIn("#include \"../ao_uniform_upload_cache.hpp\"", native)
        self.assertIn("UniformUploadCache<XeGTAO::GTAOConstants> constantsUpload", native)
        self.assertIn("UniformUploadCache<AdapterParams> adapterUpload", native)
        self.assertIn("r.constantsUpload.upload_if_changed(constants", native)
        self.assertIn("r.adapterUpload.upload_if_changed(adapter", native)
        self.assertIn("if (r.constants && r.adapter && r.common) return;", native)
        self.assertIn("if (!r.constants)", native)
        self.assertIn("if (!r.adapter)", native)
        self.assertIn("r.common = bindings(device, commonLayout", native)
        self.assertIn("std::shared_ptr<Scratch> scratch;", native)
        self.assertNotIn("r = {};", native)
        self.assertNotIn("queue.WriteBuffer(r.constants, 0, &constants", native)
        self.assertNotIn("queue.WriteBuffer(r.adapter, 0, &adapter", native)


if __name__ == "__main__":
    unittest.main()
