#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

/* Minimal WebGPU/resource stubs. The production cache is included below by
 * the Python harness after it has replayed the pinned Aurora patch queue. */
struct TextureHandle {
    std::shared_ptr<uint32_t> value;

    TextureHandle() = default;
    explicit TextureHandle(uint32_t id) : value(std::make_shared<uint32_t>(id)) {}
    explicit operator bool() const { return value != nullptr; }
    TextureHandle CreateView() const { return *this; }
};

namespace wgpu {
enum class TextureDimension { e2D };
enum class TextureUsage : uint32_t {
    RenderAttachment = 1u << 0,
    TextureBinding = 1u << 1,
    CopySrc = 1u << 2,
    CopyDst = 1u << 3,
};

constexpr TextureUsage operator|(TextureUsage lhs, TextureUsage rhs)
{
    return static_cast<TextureUsage>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

struct Extent3D {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depthOrArrayLayers = 1;
};

struct TextureDescriptor {
    const char *label = nullptr;
    TextureUsage usage = TextureUsage::RenderAttachment;
    TextureDimension dimension = TextureDimension::e2D;
    Extent3D size{};
    int format = 0;
    uint32_t mipLevelCount = 1;
    uint32_t sampleCount = 1;
};
} // namespace wgpu

struct DeviceStub {
    uint32_t textureCreations = 0;

    TextureHandle CreateTexture(const wgpu::TextureDescriptor *)
    {
        return TextureHandle(++textureCreations);
    }
} g_device;

namespace webgpu {
struct TextureWithSampler {
    TextureHandle texture;
    TextureHandle view;
    wgpu::Extent3D size{};
    int format = 0;
};

struct GraphicsConfigStub {
    struct {
        int format = 1;
    } surfaceConfiguration;
    int depthFormat = 2;
} g_graphicsConfig;
} // namespace webgpu

/* The real source uses absl::flat_hash_map. This deterministic test adapter
 * keeps the extracted production declarations/function body unchanged while
 * supplying the key hash needed by std::unordered_map. */
namespace absl {
template <typename Key>
struct FlatHash {
    size_t operator()(const Key &key) const noexcept
    {
        return (static_cast<size_t>(key.width) << 32) ^ static_cast<size_t>(key.height);
    }
};

template <typename Key, typename Value>
using flat_hash_map = std::unordered_map<Key, Value, FlatHash<Key>>;
} // namespace absl

#include "offscreen_shadow_cache.inc"

int main()
{
    constexpr uint32_t A = 7680;
    constexpr uint32_t B = 6144;
    constexpr uint32_t C = 4096;

    assert(MP6_OFFSCREEN_CACHE_MAX == 2);
    assert(g_offscreenCache.empty());
    assert(g_device.textureCreations == 0);

    /* m632 alternates two native shadow sizes every tick. A hundred requests
     * must retain exactly two color/depth pairs, not allocate per tick. */
    for (int tick = 0; tick < 100; ++tick) {
        (void)get_offscreen_textures((tick & 1) == 0 ? A : B,
                                     (tick & 1) == 0 ? A : B);
    }
    assert(g_offscreenCache.size() == 2);
    assert(g_device.textureCreations == 4);

    /* Make A the LRU, then insert C. The bounded cache evicts only A, and B
     * remains a hit. */
    (void)get_offscreen_textures(A, A);
    (void)get_offscreen_textures(B, B);
    (void)get_offscreen_textures(C, C);
    assert(g_offscreenCache.size() == 2);
    assert(g_device.textureCreations == 6);
    (void)get_offscreen_textures(B, B);
    assert(g_device.textureCreations == 6);
    (void)get_offscreen_textures(A, A);
    assert(g_device.textureCreations == 8);

    /* Explicit shadow teardown retires every cached pair. The next request
     * recreates the pair instead of retaining a stale full-resolution target. */
    clear_offscreen_cache();
    assert(g_offscreenCache.empty());
    (void)get_offscreen_textures(A, A);
    assert(g_offscreenCache.size() == 1);
    assert(g_device.textureCreations == 10);

    return 0;
}
