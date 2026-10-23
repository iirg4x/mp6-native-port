"""CPU scope/lifetime checks; actual driver failure is tested by board QA."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from test_xegtao_parameters import run_cpp

GUARD = ROOT / 'src/gx/xegtao/initialization_guard.hpp'
NATIVE = ROOT / 'src/gx/xegtao/native.hpp'


class InitializationGuardTests(unittest.TestCase):
    def test_success_errors_timeout_and_late_callback_lifetime(self):
        # Execute the production guard with a tiny deterministic WebGPU facade.
        # Only the API include is replaced; the guard's implementation is not.
        guard = GUARD.read_text().replace('#include <webgpu/webgpu_cpp.h>', '')
        run_cpp(r'''
#define WEBGPU_DAWN 1
#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>
namespace wgpu {
enum class ErrorFilter { Internal, OutOfMemory, Validation };
enum class ErrorType { NoError, Validation, OutOfMemory, Internal };
enum class PopErrorScopeStatus { Success, Error };
enum class CallbackMode { AllowProcessEvents };
enum class WaitStatus { Success, TimedOut };
struct StringView {};
struct Future { size_t id; };
inline std::vector<ErrorFilter> pushed;
inline std::vector<std::function<void()>> pending;
inline ErrorType error = ErrorType::NoError;
inline PopErrorScopeStatus popStatus = PopErrorScopeStatus::Success;
inline bool timeout = false;
struct Instance {
    explicit operator bool() const { return true; }
    WaitStatus WaitAny(Future f, uint64_t duration) const {
        assert(duration <= 1000000000);
        if (timeout) return WaitStatus::TimedOut;
        auto callback = std::move(pending[f.id]);
        callback();
        return WaitStatus::Success;
    }
};
struct Adapter {
    explicit operator bool() const { return true; }
    Instance GetInstance() const { return {}; }
};
struct Device {
    bool valid = true;
    explicit operator bool() const { return valid; }
    Adapter GetAdapter() const { return {}; }
    void PushErrorScope(ErrorFilter filter) const { pushed.push_back(filter); }
    template<class Callback> Future PopErrorScope(CallbackMode, Callback cb) const {
        size_t id = pending.size();
        auto type = id == 0 ? error : ErrorType::NoError;
        auto status = popStatus;
        pending.push_back([cb, type, status] { cb(status, type, {}); });
        return {id};
    }
};
void reset() { pushed.clear(); pending.clear(); error=ErrorType::NoError;
               popStatus=PopErrorScopeStatus::Success; timeout=false; }
}
''' + guard + r'''
int main() {
    using namespace wgpu;
    using mp6::xegtao::InitializationGuard;
    for (auto type : {ErrorType::NoError, ErrorType::Validation,
                      ErrorType::OutOfMemory, ErrorType::Internal}) {
        reset(); error = type;
        InitializationGuard guard(Device{});
        assert(guard.ready());
        assert(pushed == std::vector<ErrorFilter>({ErrorFilter::Internal,
            ErrorFilter::OutOfMemory, ErrorFilter::Validation}));
        assert(guard.finish() == (type == ErrorType::NoError));
        assert(guard.finish() == (type == ErrorType::NoError));
        assert(pending.size() == 3);
    }
    reset(); popStatus = PopErrorScopeStatus::Error;
    { InitializationGuard guard(Device{}); assert(!guard.finish()); }
    reset(); timeout = true;
    { InitializationGuard guard(Device{}); assert(!guard.finish()); }
    assert(pending.size() == 3);
    // The production guard is gone. Pending callbacks must own their data.
    for (auto &cb : pending) { cb(); cb = {}; }
    reset();
    { InitializationGuard guard(Device{}); } // Early-return destructor drains all scopes.
    assert(pending.size() == 3);
    reset();
    { InitializationGuard guard(Device{false}); assert(!guard.ready()); assert(!guard.finish()); }
    assert(pushed.empty() && pending.empty());
}
''')

    def test_failure_is_cached_and_partial_handles_are_released(self):
        source = NATIVE.read_text()
        failure = source.split('bool initialization_failed(', 1)[1].split('\npublic:', 1)[0]
        self.assertIn('*this = Pipeline{};', failure)
        self.assertIn('checked = true;', failure)
        self.assertIn('using legacy', failure)
        init = source.split('bool initialize(', 1)[1]
        self.assertLess(init.index('if (checked) return supported && accept_deferred();'), init.index('InitializationGuard validation'))
        self.assertLess(init.index('validation.finish() || !handlesValid'), init.index('supported = true;'))
        self.assertIn('(policy.denoisePasses == 1 || stages[3])', init)

    def test_malformed_shader_seam_exists_only_in_qa_build(self):
        source = NATIVE.read_text()
        seam = source.split('#ifdef MP6_XEGTAO_QA_INVALID_SHADER', 1)[1].split('#endif', 1)[0]
        self.assertIn('MP6_QA_XEGTAO_INVALID_SHADER', seam)
        self.assertIn('[MP6-AO-QA] injecting invalid XeGTAO main shader', seam)
        self.assertIn('deliberately invalid WGSL', seam)
        self.assertEqual(source.count('MP6_QA_XEGTAO_INVALID_SHADER'), 1)


if __name__ == '__main__':
    unittest.main()
