#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <dolphin/gx.h>
#include "selftest_assert.h"
namespace wgpu { struct SamplerDescriptor {}; }
namespace aurora::gfx {
constexpr u32 InvalidTextureFormat=UINT32_MAX;
struct Image { u32 format=GX_TF_RGBA8_PC; };
using TextureHandle=std::shared_ptr<Image>;
}
#include "sampler_types.inc"
#define ZoneScoped
namespace aurora::gx {
constexpr unsigned MaxTextures=8;
struct GXState {
    struct CopyTextureRef { gfx::TextureHandle handle; unsigned revision=0; };
    std::array<GXTexObj_,8> loadedTextures{};
    std::array<gfx::TextureBind,8> textures{};
    std::array<GXTlutObj_,16> loadedTluts{};
    std::unordered_map<const void*,CopyTextureRef> copyTextures;
    bool stateDirty=false;
} g_gxState;
struct ShaderInfo { std::bitset<8> sampledTextures; };
struct TexBpRegMapping {
    enum class Kind { Mode0,Mode1,Image0,Image3,Tlut,Image1,Image2 };
    Kind kind; unsigned texMapId;
};
static void bp(TexBpRegMapping::Kind kind,unsigned mapId,u32 value) {
    TexBpRegMapping data{kind,mapId}; const auto* mapping=&data;
#include "sampler_bp.inc"
}
static unsigned resolves=0;
static std::atomic_bool s_staticTextureCacheClearPending=false;
static void do_clear_static_texture_cache() {}
static bool is_palette_format(u32 format) { return format==GX_TF_C4 || format==GX_TF_C8 || format==GX_TF_C14X2; }
static gfx::TextureHandle resolve_static_texture(const GXTexObj_&) { ++resolves; return std::make_shared<gfx::Image>(); }
static gfx::TextureHandle resolve_static_palette_texture(const GXTexObj_& obj,const GXTlutObj_&) { return resolve_static_texture(obj); }
static gfx::TextureHandle resolve_dynamic_palette_texture(const GXTexObj_& obj,const GXState::CopyTextureRef&,const GXTlutObj_&) { return resolve_static_texture(obj); }
static u32 resolved_format_for_handle(const gfx::TextureHandle& handle) { return handle ? handle->format : GX_TF_RGBA8; }
#include "sampler_resolve.inc"
}
int main() {
    using namespace aurora::gx;
    ShaderInfo info;
    for(unsigned slot=0;slot<8;++slot) {
        auto& loaded=g_gxState.loadedTextures[slot];
        loaded.data=reinterpret_cast<void*>(uintptr_t(0x10000+slot*4096));
        loaded.texObjId=slot+1; loaded.texDataVersion=1; loaded.mFormat=GX_TF_I8;
        loaded.mWidth=32; loaded.mHeight=64;
        info.sampledTextures.reset(); info.sampledTextures.set(slot);
        resolve_sampled_textures(info);
        const auto original=g_gxState.textures[slot]; const auto uploads=resolves;
        assert(original.texObj.mFormat==GX_TF_RGBA8_PC);
        for(unsigned i=0;i<1000;++i) {
            // Wrap/filter/bias/aniso in mode0 and LOD range in mode1 are separate
            // register owners. Same image ID/version must not freeze either.
            const auto mode0=(i*0x9173u)&0xFFFFFFu, mode1=(i*0x7163u)&0xFFFFu;
            bp(TexBpRegMapping::Kind::Mode0,slot,mode0);
            bp(TexBpRegMapping::Kind::Mode1,slot,mode1);
            resolve_sampled_textures(info);
            const auto& bound=g_gxState.textures[slot];
            assert(bound.ref==original.ref && resolves==uploads);
            assert(bound.texObj.mode0==mode0 && bound.texObj.mode1==mode1);
            assert(bound.texObj.mFormat==GX_TF_RGBA8_PC);
            assert(loaded.mFormat==GX_TF_I8 && loaded.texObjId==slot+1 && loaded.texDataVersion==1);
            assert(g_gxState.stateDirty);
        }
        // A prior recorded draw's copied state/immutable resource survives.
        assert(original.texObj.mode0==0 && original.texObj.mode1==0);
        ++loaded.texDataVersion; resolve_sampled_textures(info);
        assert(resolves==uploads+1 && g_gxState.textures[slot].ref!=original.ref);
    }
    assert(resolves==16);
    puts("PASS: BP sampler changes, unchanged-image fast path, 8 slots, converted format retained, no extra texture resolve/upload, prior draws unchanged");
}
