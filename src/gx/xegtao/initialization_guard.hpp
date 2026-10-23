#pragma once

#include <webgpu/webgpu_cpp.h>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>

namespace mp6::xegtao {

// Initialization only, on the render worker. WebGPU error objects can have
// non-null handles: checking handles without draining error scopes is unsafe.
class InitializationGuard {
#if defined(WEBGPU_DAWN) && !defined(__EMSCRIPTEN__) // emdawnwebgpu has no Device::GetAdapter
    wgpu::Device device;
    wgpu::Instance instance;
    bool active = false, accepted = false;

public:
    explicit InitializationGuard(const wgpu::Device &selected) : device(selected) {
        if (!device) return;
        const auto adapter = device.GetAdapter();
        if (adapter) instance = adapter.GetInstance();
        if (!instance) return;
        device.PushErrorScope(wgpu::ErrorFilter::Internal);
        device.PushErrorScope(wgpu::ErrorFilter::OutOfMemory);
        device.PushErrorScope(wgpu::ErrorFilter::Validation);
        active = true;
    }

    bool ready() const { return active; }

    bool finish() {
        if (!active) return accepted;
        active = false;
        using Clock = std::chrono::steady_clock;
        const auto deadline = Clock::now() + std::chrono::seconds(1);
        std::array<wgpu::Future, 3> futures;
        std::array<std::shared_ptr<std::atomic<unsigned>>, 3> outcomes;
        // Pop every scope before waiting, even if one reports an error. The
        // callback owns its state if a timed-out future completes later through
        // Aurora's ProcessEvents; it must never reference this guard or locals.
        for (size_t i = 0; i < futures.size(); ++i) {
            auto outcome = std::make_shared<std::atomic<unsigned>>(0);
            outcomes[i] = outcome;
            futures[i] = device.PopErrorScope(wgpu::CallbackMode::AllowProcessEvents,
                [outcome](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView) {
                    outcome->store(status == wgpu::PopErrorScopeStatus::Success &&
                                   type == wgpu::ErrorType::NoError ? 1u : 2u,
                                   std::memory_order_release);
                });
        }
        accepted = true;
        for (size_t i = 0; i < futures.size(); ++i) {
            const auto now = Clock::now();
            const auto remaining = now < deadline ?
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count()) : 0;
            const auto status = instance.WaitAny(futures[i], remaining);
            if (status != wgpu::WaitStatus::Success ||
                outcomes[i]->load(std::memory_order_acquire) != 1u) accepted = false;
        }
        return accepted;
    }

    ~InitializationGuard() { finish(); }
    std::shared_ptr<std::atomic<unsigned>> deferred() const { return nullptr; }
#elif defined(__EMSCRIPTEN__)
    // Browsers resolve error scopes on the event loop, and the frame cannot
    // block on them. finish() pops the scopes and hands the verdict to the
    // caller (0 pending, 1 accepted, 2 rejected); the caller keeps its
    // fallback until the verdict arrives.
    struct Verdict { std::atomic<unsigned> state{0}, passed{0}; };
    wgpu::Device device;
    std::shared_ptr<Verdict> verdict;
    bool active = false;

public:
    explicit InitializationGuard(const wgpu::Device &selected) : device(selected) {
        if (!device) return;
        device.PushErrorScope(wgpu::ErrorFilter::Internal);
        device.PushErrorScope(wgpu::ErrorFilter::OutOfMemory);
        device.PushErrorScope(wgpu::ErrorFilter::Validation);
        verdict = std::make_shared<Verdict>();
        active = true;
    }

    bool ready() const { return active; }

    bool finish() {
        if (!active) return verdict != nullptr;
        active = false;
        for (int i = 0; i < 3; ++i)
            device.PopErrorScope(wgpu::CallbackMode::AllowSpontaneous,
                [v = verdict](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView) {
                    if (status != wgpu::PopErrorScopeStatus::Success || type != wgpu::ErrorType::NoError)
                        v->state.store(2, std::memory_order_release);
                    else if (v->passed.fetch_add(1, std::memory_order_acq_rel) == 2) {
                        unsigned pending = 0;
                        v->state.compare_exchange_strong(pending, 1, std::memory_order_acq_rel);
                    }
                });
        return true;
    }

    ~InitializationGuard() { finish(); }
    std::shared_ptr<std::atomic<unsigned>> deferred() const {
        return verdict ? std::shared_ptr<std::atomic<unsigned>>(verdict, &verdict->state) : nullptr;
    }
#else
public:
    explicit InitializationGuard(const wgpu::Device &) {}
    // Other providers need an explicit instance/callback integration first.
    bool ready() const { return false; }
    bool finish() { return false; }
    std::shared_ptr<std::atomic<unsigned>> deferred() const { return nullptr; }
#endif
    InitializationGuard(const InitializationGuard &) = delete;
    InitializationGuard &operator=(const InitializationGuard &) = delete;
};

} // namespace mp6::xegtao
