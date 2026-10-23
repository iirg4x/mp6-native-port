#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <dolphin/gx.h>

#define ZoneScoped
// Content keys hash real bytes; the hash function itself is not under test.
inline unsigned long long XXH3_64bits_withSeed(const void* data, size_t len, unsigned long long seed) {
    unsigned long long h = 1469598103934665603ull ^ seed;
    for (size_t i = 0; i < len; ++i) h = (h ^ static_cast<const unsigned char*>(data)[i]) * 1099511628211ull;
    return h;
}
namespace wgpu { struct SamplerDescriptor {}; }
namespace absl {
// Hash implementation is not under test. Real cache equality and all cache
// dependency/lifetime code are extracted unchanged from the renderer.
struct Hash { template<class T> size_t operator()(const T&) const { return 0; } };
template<class K, class V> using flat_hash_map = std::unordered_map<K,V,Hash>;
template<class K> using flat_hash_set = std::unordered_set<K,Hash>;
}
namespace aurora::gfx {
constexpr u32 InvalidTextureFormat = UINT32_MAX;
struct Image {
    struct { u32 width=0, height=0; } size;
    u32 gxFormat=GX_TF_RGBA8_PC;
    bool hasArbitraryMips=false;
    unsigned generation=0;
};
using TextureHandle = std::shared_ptr<Image>;
}
#include "texture_cache_types.inc"
namespace aurora::gfx {
unsigned uploads=0, conversions=0;
u32 frame=0;
u32 current_frame() { return frame; }
bool failUpload=false;
TextureHandle make_image(u32 w=32,u32 h=32,u32 format=GX_TF_RGBA8_PC) {
    auto image=std::make_shared<Image>();
    image->size={w,h}; image->gxFormat=format;
    return image;
}
TextureHandle new_static_texture_2d(u32 w,u32 h,u32,u32 format,std::span<const u8>,bool,const char*) {
    ++uploads;
    if(failUpload) return {};
    auto image=make_image(w,h,format);
    image->generation=uploads;
    return image;
}
TextureHandle new_conv_texture(u32 w,u32 h,u32 format,const char*) { return make_image(w,h,format); }
u32 tlut_texture_format(GXTlutFmt format) { return static_cast<u32>(format); }
struct Converted { std::vector<u8> data{0,0,0,0}; bool hasArbitraryMips=false; };
Converted convert_texture_palette(u32,u32,u32,u32,std::span<const u8>,GXTlutFmt,u16,std::span<const u8>) { return {}; }
namespace texture_replacement {
std::optional<TextureHandle> find_replacement(const GXTexObj_&) { return {}; }
std::optional<TextureHandle> find_replacement(const GXTexObj_&,const GXTlutObj_&) { return {}; }
}
namespace tex_palette_conv { enum class Variant { FromFloat4,FromFloat8 }; }
struct Conversion { tex_palette_conv::Variant variant; TextureHandle src,dst,tlut; };
void queue_palette_conv(Conversion conversion) {
    ++conversions;
    conversion.dst->generation=conversions;
}
}
namespace aurora::gx {
constexpr u32 MaxTextures=8;
struct GXState {
    struct CopyTextureRef { gfx::TextureHandle handle; u32 revision=0; };
    struct CopyKey {
        const void* dest;
        bool operator==(const CopyKey&) const = default;
    };
    std::array<GXTexObj_,MaxTextures> loadedTextures{};
    std::array<gfx::TextureBind,MaxTextures> textures{};
    std::array<GXTlutObj_,256> loadedTluts{};
    absl::flat_hash_map<const void*,CopyTextureRef> copyTextures;
    absl::flat_hash_map<CopyKey,CopyTextureRef> copyTextureCache;
    bool stateDirty=false;
} g_gxState;
// Texel and palette memory: [0] is loaded, [1] is another block.
alignas(32) u8 g_image[2][64*64*4*2];
alignas(32) u16 g_tlut[2][256];
u32 GXGetTexBufferSize(u16 w, u16 h, u32, GXBool mips, u8) { return u32{w}*h*4*(mips ? 2 : 1); }
struct ShaderInfo { std::bitset<MaxTextures> sampledTextures; };
bool is_palette_format(u32 f) { return f==GX_TF_C4 || f==GX_TF_C8 || f==GX_TF_C14X2; }
namespace {
#include "texture_cache_private.inc"
}
#include "texture_cache_public.inc"

GXTexObj_& load(bool palette=true) {
    std::memset(g_image[0],1,sizeof g_image[0]); std::memset(g_image[1],2,sizeof g_image[1]);
    std::fill(std::begin(g_tlut[0]),std::end(g_tlut[0]),u16{1}); std::fill(std::begin(g_tlut[1]),std::end(g_tlut[1]),u16{2});
    auto& obj=g_gxState.loadedTextures[0];
    obj.data=g_image[0];
    obj.mWidth=32; obj.mHeight=32;
    obj.mFormat=palette ? GX_TF_C8 : GX_TF_I8;
    obj.texObjId=1; obj.texDataVersion=1;
    auto& tlut=g_gxState.loadedTluts[0];
    tlut.data=g_tlut[0];
    tlut.tlutObjId=2; tlut.tlutDataVersion=1;
    tlut.format=GX_TL_RGB565; tlut.numEntries=256;
    return obj;
}
gfx::TextureHandle resolve() {
    ShaderInfo info; info.sampledTextures.set(0);
    resolve_sampled_textures(info);
    return g_gxState.textures[0].ref;
}
void drop_binding() { g_gxState.textures[0]={}; }
}
int main(int argc,char** argv) {
    assert(argc==2);
    using namespace aurora;
    using namespace aurora::gx;
    const auto is=[&](const char* name){return std::strcmp(argv[1],name)==0;};
    auto& obj=load(!is("image_pointer") && !is("image_size") && !is("image_format") &&
                  !is("image_mips") && !is("null_retry") && !is("static_clear"));
    auto& tlut=g_gxState.loadedTluts[0];
    if(is("palette_missing")) tlut.data=nullptr;
    if(is("anonymous_palette")) tlut.tlutObjId=0;
    if(is("null_retry")) gfx::failUpload=true;
    if(is("image_mips")) { obj.flags=1; obj.mode1=16u<<8; }
    if(is("copy_released") || is("copy_after_release")) {
        // An EFB copy outranks memory at its destination. Once that memory is
        // freed the copy must go, or a texture the game later writes there
        // samples the old copy (the instruction preview after the bus menu).
        auto source=gfx::make_image(64,64,GX_TF_I8);
        g_gxState.copyTextures[obj.data]={source,1u};
        g_gxState.copyTextureCache[{obj.data}]={source,1u};
        sweep_idle_textures(); // the frame that made the copy ends
        if(is("copy_after_release")) note_copy_texture(obj.data); // copied again, same frame as the free
        aurora_texture_sources_released(g_image[0],g_image[0]+sizeof g_image[0]);
        sweep_idle_textures();
        const bool kept=is("copy_after_release");
        assert(g_gxState.copyTextures.contains(obj.data)==kept);
        assert(g_gxState.copyTextureCache.contains({obj.data})==kept);
        printf("PASS %s\n",argv[1]); return 0;
    }
    if(std::strncmp(argv[1],"dynamic_",8)==0) {
        auto source=gfx::make_image(64,64,GX_TF_I8);
        g_gxState.copyTextures[obj.data]={source,is("dynamic_zero") ? 0u : 1u};
        tlut.tlutDataVersion=is("dynamic_zero") ? 0 : 1;
        if(is("dynamic_uncached")) tlut.set_no_cache(true);
        if(is("dynamic_anonymous")) tlut.tlutObjId=0;
        const auto first=resolve();
        assert(first && gfx::conversions==1);
        if(is("dynamic_zero")) {
            assert(resolve()==first && gfx::conversions==1);
        } else if(is("dynamic_pointer") || is("dynamic_format") || is("dynamic_count") || is("dynamic_version")) {
            if(is("dynamic_pointer")) tlut.data=g_tlut[1];
            if(is("dynamic_format")) tlut.format=GX_TL_RGB5A3;
            if(is("dynamic_count")) tlut.numEntries=128;
            if(is("dynamic_version")) ++tlut.tlutDataVersion;
            auto next=resolve();
            assert(next!=first && gfx::conversions==2 && gfx::uploads==2);
            assert(first->generation==1);
        } else if(is("dynamic_uncached")) {
            assert(s_tlutObjectCaches.empty());
            assert(resolve()==first && gfx::conversions==1);
        } else {
            assert(is("dynamic_anonymous") && s_tlutObjectCaches.empty());
            assert(resolve()!=first && gfx::conversions==2);
            assert(s_tlutObjectCaches.empty());
        }
    } else {
        const auto first=resolve();
        const unsigned before=gfx::uploads;
        if(is("palette_removed") || is("palette_empty") || is("palette_invalid_slot")) {
            if(is("palette_removed")) tlut.data=nullptr;
            if(is("palette_empty")) tlut.numEntries=0;
            if(is("palette_invalid_slot")) obj.tlut=static_cast<GXTlut>(-1);
            assert(first && !resolve() && gfx::uploads==before);
            printf("PASS %s\n",argv[1]); return 0;
        } else if(is("palette_version")) { ++g_tlut[0][0]; ++tlut.tlutDataVersion; }
        else if(is("palette_selection")) {
            g_gxState.loadedTluts[1]=tlut;
            g_gxState.loadedTluts[1].tlutObjId=3;
            g_gxState.loadedTluts[1].data=g_tlut[1];
            obj.tlut=GX_TLUT1;
        } else if(is("palette_pointer")) tlut.data=g_tlut[1];
        else if(is("palette_format")) tlut.format=GX_TL_RGB5A3;
        else if(is("palette_count")) tlut.numEntries=128;
        else if(is("palette_missing")) { assert(!first); tlut.data=g_tlut[0]; }
        else if(is("image_pointer")) obj.data=g_image[1];
        else if(is("image_size")) obj.mWidth=64;
        else if(is("image_format")) obj.mFormat=GX_TF_IA8;
        else if(is("image_mips")) {
            obj.mode1=48u<<8;
            // BP owner already refreshes bound sampler words (0047). They
            // cannot double as a record of the image's allocated mip count.
            g_gxState.textures[0].texObj.mode1=obj.mode1;
        } else if(is("null_retry")) { assert(!first); gfx::failUpload=false; }
        else if(is("static_clear")) clear_static_texture_cache();
        else if(is("anonymous_palette")) {
            assert(s_textureObjectCaches.empty() && s_tlutObjectCaches.empty());
        } else if(is("shared_palette_upload")) {
            // A dynamic indexed copy needs a GPU TLUT image. This must not
            // discard a correct, already converted static user of that TLUT.
            auto copy=gfx::make_image(64,64,GX_TF_I8);
            assert(resolve_dynamic_palette_texture(obj,{copy,1},tlut));
            assert(resolve_static_palette_texture(obj,tlut)==first);
            assert(gfx::uploads==before+1 && gfx::conversions==1);
            ++g_tlut[0][0]; ++tlut.tlutDataVersion;
            assert(resolve_dynamic_palette_texture(obj,{copy,1},tlut));
            const auto next=resolve_static_palette_texture(obj,tlut);
            assert(next && next!=first);
            assert(gfx::uploads==before+3 && gfx::conversions==2);
            puts("PASS shared_palette_upload"); return 0;
        } else if(is("retained_destroy")) {
            evict_texture_object(obj.texObjId);
            evict_tlut_object(tlut.tlutObjId);
            assert(obj.no_cache() && tlut.no_cache());
            assert(resolve()==first && gfx::uploads==before);
            assert(s_textureObjectCaches.empty() && s_tlutObjectCaches.empty());
            // Destruction retires cache ownership, not an already bound GPU
            // image. The source pointer may no longer be safe to read.
            clear_static_texture_cache();
            assert(resolve()==first && gfx::uploads==before);
            puts("PASS retained_destroy"); return 0;
        } else if(is("content_reload")) {
            // The game loads the same texture into a fresh block: a new
            // object, a new address, identical bytes. No conversion.
            std::memcpy(g_image[1],g_image[0],sizeof g_image[0]);
            obj.data=g_image[1]; obj.texObjId=9;
            assert(resolve()==first && gfx::uploads==before);
            assert(s_textureObjectCaches.size()==2 && s_textureContentCache.size()==1);
            puts("PASS content_reload"); return 0;
        } else if(is("source_released")) {
            // Freeing the texels drops the object's conversion; a reload of
            // the same bytes right after still finds it by content.
            aurora_texture_sources_released(g_image[0],g_image[0]+sizeof g_image[0]);
            sweep_idle_textures();
            assert(s_textureObjectCaches.empty() && s_textureContentCache.size()==1);
            drop_binding();
            assert(resolve()==first && gfx::uploads==before);
            assert(s_textureObjectCaches.size()==1);
            aurora_texture_sources_released(g_tlut[0],g_tlut[0]+256);
            sweep_idle_textures();
            assert(s_textureObjectCaches.empty() && s_tlutObjectCaches.empty());
            puts("PASS source_released"); return 0;
        } else if(is("rewrite_in_place")) {
            // A texture rewritten at the same address: the old image has no
            // future, so it is not kept by content.
            ++g_image[0][0]; ++obj.texDataVersion;
            drop_binding();
            const auto next=resolve();
            assert(next && next!=first && gfx::uploads==before+1);
            assert(s_textureContentCache.size()==1);
            puts("PASS rewrite_in_place"); return 0;
        } else if(is("written_in_place")) {
            // The game draws into a texture and flushes it (Mowtown's lawn).
            // GXInitTexObj's sampled version misses the edit; the flush
            // does not, and the bound image is replaced at the next draw.
            g_image[0][100]^=0x5a;
            aurora_texture_sources_written(g_image[0]+100,g_image[0]+101);
            const auto next=resolve();
            assert(next && next!=first && gfx::uploads==before+1);
            assert(resolve()==next && gfx::uploads==before+1);
            assert(s_textureObjectCaches.size()==1 && s_textureContentCache.size()==1);
            puts("PASS written_in_place"); return 0;
        } else if(is("written_unchanged")) {
            // A flush over bytes that did not change finds the conversion by
            // content; a flush elsewhere touches nothing.
            aurora_texture_sources_written(g_image[0],g_image[0]+sizeof g_image[0]);
            assert(resolve()==first && gfx::uploads==before);
            aurora_texture_sources_written(g_image[1],g_image[1]+sizeof g_image[1]);
            assert(resolve()==first && gfx::uploads==before);
            assert(s_textureObjectCaches.size()==1 && s_textureContentCache.size()==1);
            puts("PASS written_unchanged"); return 0;
        } else if(is("unchanged_reuse")) {
            for(unsigned i=0;i<10000;++i) {
                assert(resolve()==first);
                if(i%11==0) drop_binding(); // Exercise secondary cache hits too.
            }
            assert(resolve()==first && gfx::uploads==before);
            assert(s_textureObjectCaches.size()==1 && s_tlutObjectCaches.size()==1);
            puts("PASS unchanged_reuse"); return 0;
        } else assert(false);
        const auto changed=resolve();
        assert(changed && changed!=first && gfx::uploads==before+1);
        if(!is("anonymous_palette")) {
            assert(resolve()==changed && gfx::uploads==before+1);
            drop_binding();
            assert(resolve()==changed && gfx::uploads==before+1);
        } else {
            assert(s_textureObjectCaches.empty() && s_tlutObjectCaches.empty());
        }
    }
    printf("PASS %s\n",argv[1]);
}
