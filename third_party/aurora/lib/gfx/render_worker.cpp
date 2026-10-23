#include "render_worker.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __ANDROID__
#include <unistd.h> // gettid
#endif

#include <algorithm>
#include <atomic>
#include <chrono>

#include <tracy/Tracy.hpp>

namespace aurora::gfx::render_worker {
namespace {
constexpr size_t QueueCapacity = 256;
constexpr auto IdlePumpInterval = std::chrono::milliseconds{1};

BoundedQueue g_queue{QueueCapacity};
std::thread g_thread;
std::atomic_bool g_running = false;
std::atomic_size_t g_pendingItems = 0;
std::thread::id g_workerThreadId;
#ifdef __ANDROID__
std::atomic<int> g_workerTid{0};
#endif
std::mutex g_progressMutex;
std::condition_variable g_progressCv;
std::atomic<uint64_t> g_progressVersion{0};

void complete_sync(const std::shared_ptr<SyncState>& sync) {
  if (!sync) {
    return;
  }
  ZoneScoped;
  {
    std::lock_guard lock{sync->mutex};
    sync->complete = true;
  }
  sync->cv.notify_all();
}

void worker_main() {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Aurora render worker");
#endif
  g_workerThreadId = std::this_thread::get_id();
#ifdef __ANDROID__
  g_workerTid.store(static_cast<int>(gettid()), std::memory_order_release);
#endif

  while (true) {
    bool closed = false;
    auto item = g_queue.pop_for(IdlePumpInterval, closed);
    if (!item) {
      if (closed) {
        break;
      }
      continue;
    }

    if (item->work) {
      ZoneScopedN("QueueItem work");
      item->work();
    }
    complete_sync(item->sync);
    g_pendingItems.fetch_sub(1, std::memory_order_acq_rel);
    if (item->type == ItemType::Shutdown) {
      break;
    }
  }

  g_workerThreadId = {};
}

void enqueue(QueueItem item) {
  ZoneScoped;
  if (is_worker_thread()) {
    if (item.work) {
      item.work();
    }
    complete_sync(item.sync);
    return;
  }

  if (!g_running.load(std::memory_order_acquire)) {
    if (item.work) {
      item.work();
    }
    complete_sync(item.sync);
    return;
  }

  g_pendingItems.fetch_add(1, std::memory_order_acq_rel);
  if (!g_queue.push(std::move(item))) {
    g_pendingItems.fetch_sub(1, std::memory_order_acq_rel);
  }
}

bool try_enqueue(QueueItem item) {
  ZoneScoped;
  if (is_worker_thread() || !g_running.load(std::memory_order_acquire)) {
    if (item.work) {
      item.work();
    }
    complete_sync(item.sync);
    return true;
  }

  g_pendingItems.fetch_add(1, std::memory_order_acq_rel);
  if (!g_queue.try_push(std::move(item))) {
    g_pendingItems.fetch_sub(1, std::memory_order_acq_rel);
    return false;
  }
  return true;
}
} // namespace

BoundedQueue::BoundedQueue(size_t capacity) : m_capacity(capacity) {}

bool BoundedQueue::push(QueueItem item) {
  ZoneScoped;
  std::unique_lock lock{m_mutex};
  m_notFull.wait(lock, [&] { return m_closed || m_items.size() < m_capacity; });
  if (m_closed) {
    return false;
  }
  m_items.emplace_back(std::move(item));
  lock.unlock();
  m_notEmpty.notify_one();
  return true;
}

bool BoundedQueue::try_push(QueueItem item) {
  ZoneScoped;
  std::unique_lock lock{m_mutex};
  if (m_closed || m_items.size() >= m_capacity) {
    return false;
  }
  m_items.emplace_back(std::move(item));
  lock.unlock();
  m_notEmpty.notify_one();
  return true;
}

std::optional<QueueItem> BoundedQueue::pop_for(std::chrono::milliseconds timeout, bool& closed) {
  std::unique_lock lock{m_mutex};
  m_notEmpty.wait_for(lock, timeout, [&] { return m_closed || !m_items.empty(); });
  closed = m_closed && m_items.empty();
  if (m_items.empty()) {
    return std::nullopt;
  }
  const bool wasFull = m_items.size() == m_capacity;
  auto item = std::move(m_items.front());
  m_items.pop_front();
  lock.unlock();
  m_notFull.notify_one();
  if (wasFull) notify_progress();
  return item;
}

void BoundedQueue::close() {
  {
    std::lock_guard lock{m_mutex};
    m_closed = true;
  }
  m_notEmpty.notify_all();
  m_notFull.notify_all();
  notify_progress();
}

void BoundedQueue::reset() {
  {
    std::lock_guard lock{m_mutex};
    m_items.clear();
    m_closed = false;
  }
  m_notFull.notify_all();
}

bool BoundedQueue::has_space() const {
  std::lock_guard lock{m_mutex};
  return !m_closed && m_items.size() < m_capacity;
}

size_t BoundedQueue::size() const {
  std::lock_guard lock{m_mutex};
  return m_items.size();
}

FrameSlotPool::FrameSlotPool(size_t slotCount) : m_freeSlots(slotCount, true) {}

size_t FrameSlotPool::acquire() {
  std::unique_lock lock{m_mutex};
  m_cv.wait(lock, [&] {
    for (const bool free : m_freeSlots) {
      if (free) {
        return true;
      }
    }
    return false;
  });
  for (size_t i = 0; i < m_freeSlots.size(); ++i) {
    if (m_freeSlots[i]) {
      m_freeSlots[i] = false;
      return i;
    }
  }
  return 0;
}

std::optional<size_t> FrameSlotPool::try_acquire() {
  std::lock_guard lock{m_mutex};
  for (size_t i = 0; i < m_freeSlots.size(); ++i) {
    if (m_freeSlots[i]) {
      m_freeSlots[i] = false;
      return i;
    }
  }
  return std::nullopt;
}

void FrameSlotPool::release(size_t slot) {
  {
    std::lock_guard lock{m_mutex};
    if (slot < m_freeSlots.size()) {
      m_freeSlots[slot] = true;
    }
  }
  m_cv.notify_one();
}

void FrameSlotPool::reset() {
  {
    std::lock_guard lock{m_mutex};
    std::fill(m_freeSlots.begin(), m_freeSlots.end(), true);
  }
  m_cv.notify_all();
}

size_t FrameSlotPool::free_count() const {
  std::lock_guard lock{m_mutex};
  return static_cast<size_t>(std::ranges::count(m_freeSlots, true));
}

std::optional<size_t> FrameSlotPool::first_free() const {
  std::lock_guard lock{m_mutex};
  for (size_t i = 0; i < m_freeSlots.size(); ++i) {
    if (m_freeSlots[i]) return i;
  }
  return std::nullopt;
}

uint64_t progress_version() { return g_progressVersion.load(std::memory_order_acquire); }

void notify_progress() {
  // Serialize predicate changes with wait's unlock-and-sleep transition.
  // The caller snapshots before probing resources to prevent a lost wakeup.
  {
    std::lock_guard lock{g_progressMutex};
    g_progressVersion.fetch_add(1, std::memory_order_release);
  }
  g_progressCv.notify_all();
}

void wait_for_progress(uint64_t observed, std::chrono::nanoseconds timeout) {
  if (timeout.count() <= 0) return;
#ifdef __EMSCRIPTEN__
  // Single-threaded: GPU callbacks (buffer maps, queue work done) are only
  // delivered from the browser event loop, so waiting must yield to it.
  (void)observed;
  emscripten_sleep(static_cast<unsigned>(timeout.count() / 1'000'000));
  return;
#endif
  std::unique_lock lock{g_progressMutex};
  g_progressCv.wait_for(lock, timeout, [&] { return progress_version() != observed; });
}

bool can_enqueue() {
  return !g_running.load(std::memory_order_acquire) || is_worker_thread() || g_queue.has_space();
}

void initialize() {
#ifdef __EMSCRIPTEN__
  // No threads on the web build: leaving g_running false makes every
  // enqueue run its work inline on the game thread.
  return;
#endif
  if (g_running.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  g_queue.reset();
  g_pendingItems.store(0, std::memory_order_release);
  g_thread = std::thread(worker_main);
}

// [MP6] The worker's OS thread handle (Windows: a HANDLE), for the host's
// sampling profiler (MP6_SAMPLE_THREAD=render); null until it has started.
// [MP6] Android: the worker's kernel thread id, for the same profiler; 0 until
// it has started.
extern "C" int aurora_render_worker_tid() {
#ifdef __ANDROID__
  return g_workerTid.load(std::memory_order_acquire);
#else
  return 0;
#endif
}

extern "C" void* aurora_render_worker_thread_handle() {
#ifdef _WIN32
  return g_running.load(std::memory_order_acquire) ? reinterpret_cast<void*>(g_thread.native_handle()) : nullptr;
#else
  return nullptr;
#endif
}

void shutdown() {
  if (!g_running.load(std::memory_order_acquire)) {
    return;
  }
  enqueue({
      .type = ItemType::Shutdown,
  });
  if (g_thread.joinable()) {
    g_thread.join();
  }
  g_running.store(false, std::memory_order_release);
  g_queue.close();
  g_queue.reset();
  g_pendingItems.store(0, std::memory_order_release);
}

void enqueue_begin_frame(uint64_t frameId, WorkCallback work) {
  enqueue({
      .type = ItemType::BeginFrame,
      .frameId = frameId,
      .work = std::move(work),
  });
}

bool try_enqueue_begin_frame(uint64_t frameId, WorkCallback work) {
  return try_enqueue({
      .type = ItemType::BeginFrame,
      .frameId = frameId,
      .work = std::move(work),
  });
}

void enqueue_encode_pass(uint64_t frameId, uint32_t passIndex, WorkCallback work) {
  enqueue({
      .type = ItemType::EncodePass,
      .frameId = frameId,
      .passIndex = passIndex,
      .work = std::move(work),
  });
}

void enqueue_end_frame(uint64_t frameId, WorkCallback work) {
  enqueue({
      .type = ItemType::EndFrame,
      .frameId = frameId,
      .work = std::move(work),
  });
}

void enqueue_work(WorkCallback work) {
  enqueue({
      .type = ItemType::Sync,
      .work = std::move(work),
  });
}

void synchronize() {
  if (is_worker_thread()) {
    return;
  }

  ZoneScoped;
  auto sync = std::make_shared<SyncState>();
  enqueue({
      .type = ItemType::Sync,
      .sync = sync,
  });
  std::unique_lock lock{sync->mutex};
  sync->cv.wait(lock, [&] { return sync->complete; });
}

bool is_worker_thread() noexcept {
#ifdef __EMSCRIPTEN__
  return true; // the one thread runs all worker items inline
#else
  return g_workerThreadId == std::this_thread::get_id();
#endif
}

bool is_idle() noexcept { return g_pendingItems.load(std::memory_order_acquire) == 0; }

} // namespace aurora::gfx::render_worker
