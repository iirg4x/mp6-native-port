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
struct ShaderInfo { std::bitset<MaxTextures> sampledTextures; };
bool is_palette_format(u32 f) { return f==GX_TF_C4 || f==GX_TF_C8 || f==GX_TF_C14X2; }
namespace {
#include "texture_cache_private.inc"
}
#include "texture_cache_public.inc"

GXTexObj_& load(bool palette=true) {
    auto& obj=g_gxState.loadedTextures[0];
    obj.data=reinterpret_cast<void*>(0x10000);
    obj.mWidth=32; obj.mHeight=32;
    obj.mFormat=palette ? GX_TF_C8 : GX_TF_I8;
    obj.texObjId=1; obj.texDataVersion=1;
    auto& tlut=g_gxState.loadedTluts[0];
    tlut.data=reinterpret_cast<void*>(0x20000);
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
            if(is("dynamic_pointer")) tlut.data=reinterpret_cast<void*>(0x30000);
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
        } else if(is("palette_version")) ++tlut.tlutDataVersion;
        else if(is("palette_selection")) {
            g_gxState.loadedTluts[1]=tlut;
            g_gxState.loadedTluts[1].tlutObjId=3;
            obj.tlut=GX_TLUT1;
        } else if(is("palette_pointer")) tlut.data=reinterpret_cast<void*>(0x30000);
        else if(is("palette_format")) tlut.format=GX_TL_RGB5A3;
        else if(is("palette_count")) tlut.numEntries=128;
        else if(is("palette_missing")) { assert(!first); tlut.data=reinterpret_cast<void*>(0x20000); }
        else if(is("image_pointer")) obj.data=reinterpret_cast<void*>(0x40000);
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
            ++tlut.tlutDataVersion;
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
