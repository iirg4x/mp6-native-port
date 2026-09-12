#include "bundle_cache.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace mp6_bundle_test;
static unsigned checks = 0;
static void require(bool value) { ++checks; if (!value) std::abort(); }
int main() {
  Cache<std::shared_ptr<int>, 1, 4> cache;
  Key key{}; key.count = 1; key.draws[0].count = 3;
  auto [entry, seen] = cache.observe(key, 0, 7);
  require(!seen); entry->payload = std::make_shared<int>(42);
  auto weak = std::weak_ptr<int>(entry->payload);
  require(cache.observe(key, 1, 7).second);
  require(*cache.observe(key, 1, 7).first->payload == 42);
  // A hash collision must never replay a different command or attachment.
  auto changed = key; changed.draws[0].count++;
  require(!cache.observe(changed, 2, 7).second);
  changed = key; changed.samples++;
  require(!cache.observe(changed, 3, 7).second);
  changed = key; changed.uniformGroup++;
  require(!cache.observe(changed, 4, 7).second);
  changed = key; changed.draws[0].uniformOffset++;
  require(!cache.observe(changed, 5, 7).second);
  // Oldest slot was evicted, releasing its resource ownership.
  require(weak.expired());
  for (uint32_t n = 0; n < 10000; ++n) {
    key.draws[0].firstIndex = n;
    auto [e, hit] = cache.observe(key, n + 10, key.hash());
    require(!hit);
    e->payload = std::make_shared<int>(int(n));
    require(cache.observe(key, n + 11, key.hash()).second);
    require(*cache.observe(key, n + 11, key.hash()).first->payload == int(n));
  }
  cache.clear();
  auto [wrap, hit] = cache.observe(key, UINT32_MAX - 2, key.hash());
  require(!hit); wrap->payload = std::make_shared<int>(1); weak = wrap->payload;
  require(cache.observe(key, 2, key.hash()).second);
  cache.expire(18); require(!weak.expired()); // Exactly MaxAge remains live.
  cache.expire(19); require(weak.expired());
  require(!cache.observe(key, 19, key.hash()).second);
  cache.clear();
  require(!cache.observe(key, 20, key.hash()).second);
  std::printf("PASS: %u bounded bundle cache checks\n", checks);
}
