#include "ubershader.hpp"

#include "../gfx/common.hpp"
#include "../gfx/uniform_writer.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "pipeline.hpp"
#include "shader_info.hpp"

#include <dolphin/gx/GXEnum.h>

#include <array>
#include <cmath>
#include <mutex>
#include <string>

#include <fmt/format.h>

namespace aurora::gx {
std::string_view shader_helpers() noexcept; // shader.cpp

namespace {
// Packed config layout (u32 words). Kept in step with UberConfigWords and the
// WGSL accessors below.
enum : u32 {
  W_STAGE_COUNT = 0,
  W_NUM_IND = 1,
  W_FOG = 2,
  W_AC_EN = 3,
  W_AC_COMP0 = 4,
  W_AC_REF0 = 5,
  W_AC_OP = 6,
  W_AC_COMP1 = 7,
  W_AC_REF1 = 8,
  W_VTX_STRIDE = 9,
  W_LOADS = 10,
  W_TEXCOORDS = 11,
  W_IND_USED = 12,
  W_COMPACT = 13,
  W_CHANNELS = 14,
  W_ANY_IND = 15,
  W_CC = 16,    // 4 x 5: matSrc, ambSrc, diffFn, attnFn, lightingEnabled
  W_SWAP = 36,  // 4 x 4: r, g, b, a
  W_ATTR = 52,  // 21 x 4: type, compType | cnt << 8 | frac << 16, offset | stride << 8, le | nbt3 << 1
  W_TCG = 136,  // 8 x 4: type, src, mtx, postMtx | normalize << 16 | embossSrc << 24
  W_IND = 168,  // 4 x 4: texCoordId, texMapId, scaleS, scaleT
  W_STAGE = 184, // 16 x 10
  STAGE_WORDS = 10,
};
static_assert(W_STAGE + MaxTevStages * STAGE_WORDS == UberConfigWords);

bool is_alpha_bump(GXChannelID id) { return id == GX_ALPHA_BUMP || id == GX_ALPHA_BUMPN; }

bool uses_texture_sample(const TevStage& stage) {
  const auto& c = stage.colorPass;
  const auto& a = stage.alphaPass;
  return c.a == GX_CC_TEXC || c.a == GX_CC_TEXA || c.b == GX_CC_TEXC || c.b == GX_CC_TEXA || c.c == GX_CC_TEXC ||
         c.c == GX_CC_TEXA || c.d == GX_CC_TEXC || c.d == GX_CC_TEXA || a.a == GX_CA_TEXA || a.b == GX_CA_TEXA ||
         a.c == GX_CA_TEXA || a.d == GX_CA_TEXA;
}

std::array<u32, UberConfigWords> pack_config(const ShaderConfig& config, const ShaderInfo& info) {
  std::array<u32, UberConfigWords> w{};
  w[W_STAGE_COUNT] = config.tevStageCount;
  w[W_NUM_IND] = config.numIndStages;
  w[W_FOG] = config.fogType;
  w[W_AC_EN] = static_cast<bool>(config.alphaCompare) ? 1u : 0u;
  w[W_AC_COMP0] = config.alphaCompare.comp0;
  w[W_AC_REF0] = config.alphaCompare.ref0;
  w[W_AC_OP] = config.alphaCompare.op;
  w[W_AC_COMP1] = config.alphaCompare.comp1;
  w[W_AC_REF1] = config.alphaCompare.ref1;
  w[W_VTX_STRIDE] = config.vtxStride;
  w[W_LOADS] = static_cast<u32>(info.loadsTevReg.to_ulong());
  w[W_TEXCOORDS] = static_cast<u32>(info.sampledTexCoords.to_ulong());
  w[W_IND_USED] = static_cast<u32>(info.usedIndStages.to_ulong());
  w[W_COMPACT] = info.compactMatrices ? 1u : 0u;
  w[W_CHANNELS] = static_cast<u32>(info.sampledColorChannels.to_ulong());
  w[W_ANY_IND] = info.usedIndStages.any() ? 1u : 0u;
  for (u32 i = 0; i < MaxColorChannels; ++i) {
    const auto& cc = config.colorChannels[i];
    w[W_CC + i * 5 + 0] = cc.matSrc;
    w[W_CC + i * 5 + 1] = cc.ambSrc;
    w[W_CC + i * 5 + 2] = cc.diffFn;
    w[W_CC + i * 5 + 3] = cc.attnFn;
    w[W_CC + i * 5 + 4] = cc.lightingEnabled ? 1u : 0u;
  }
  for (u32 i = 0; i < MaxTevSwap; ++i) {
    const auto& s = config.tevSwapTable[i];
    w[W_SWAP + i * 4 + 0] = s.red;
    w[W_SWAP + i * 4 + 1] = s.green;
    w[W_SWAP + i * 4 + 2] = s.blue;
    w[W_SWAP + i * 4 + 3] = s.alpha;
  }
  for (u32 i = 0; i < MaxVtxAttr; ++i) {
    const auto& a = config.attrs[i];
    w[W_ATTR + i * 4 + 0] = a.attrType;
    w[W_ATTR + i * 4 + 1] = u32(a.compType) | (u32(a.cnt) << 8) | (u32(a.frac) << 16);
    w[W_ATTR + i * 4 + 2] = u32(a.offset) | (u32(a.stride) << 8);
    w[W_ATTR + i * 4 + 3] = (a.le ? 1u : 0u) | (a.nbt3 ? 2u : 0u);
  }
  for (u32 i = 0; i < MaxTexCoord; ++i) {
    const auto& t = config.tcgs[i];
    w[W_TCG + i * 4 + 0] = t.type;
    w[W_TCG + i * 4 + 1] = t.src;
    w[W_TCG + i * 4 + 2] = t.mtx;
    w[W_TCG + i * 4 + 3] = u32(t.postMtx) | (t.normalize ? 1u << 16 : 0u) | (u32(t.embossSrc) << 24);
  }
  for (u32 i = 0; i < MaxIndStages; ++i) {
    const auto& s = config.indStages[i];
    w[W_IND + i * 4 + 0] = s.texCoordId;
    w[W_IND + i * 4 + 1] = s.texMapId;
    w[W_IND + i * 4 + 2] = s.scaleS;
    w[W_IND + i * 4 + 3] = s.scaleT;
  }
  for (u32 i = 0; i < config.tevStageCount && i < MaxTevStages; ++i) {
    const auto& s = config.tevStages[i];
    const u32 b = W_STAGE + i * STAGE_WORDS;
    w[b + 0] = u32(s.colorPass.a) | (u32(s.colorPass.b) << 8) | (u32(s.colorPass.c) << 16) | (u32(s.colorPass.d) << 24);
    w[b + 1] = u32(s.alphaPass.a) | (u32(s.alphaPass.b) << 8) | (u32(s.alphaPass.c) << 16) | (u32(s.alphaPass.d) << 24);
    w[b + 2] = u32(s.colorOp.op) | (u32(s.colorOp.bias) << 8) | (u32(s.colorOp.scale) << 16) |
               (u32(s.colorOp.outReg) << 24);
    w[b + 3] = u32(s.alphaOp.op) | (u32(s.alphaOp.bias) << 8) | (u32(s.alphaOp.scale) << 16) |
               (u32(s.alphaOp.outReg) << 24);
    w[b + 4] = u32(s.kcSel) | (u32(s.kaSel) << 8) | (u32(s.texCoordId) << 16) | (u32(s.texMapId) << 24);
    w[b + 5] = u32(s.channelId) | (u32(s.tevSwapRas) << 8) | (u32(s.tevSwapTex) << 16) | (u32(s.indTexStage) << 24);
    w[b + 6] = u32(s.indTexFormat) | (u32(s.indTexBiasSel) << 8) | (u32(s.indTexAlphaSel) << 16) |
               (u32(s.indTexMtxId) << 24);
    w[b + 7] = u32(s.indTexWrapS) | (u32(s.indTexWrapT) << 8) | (u32(s.indTexUseOrigLOD) << 16) |
               (u32(s.indTexAddPrev) << 24);
    w[b + 8] = (s.colorOp.clamp ? 1u : 0u) | (s.alphaOp.clamp ? 2u : 0u) | (uses_texture_sample(s) ? 4u : 0u);
    w[b + 9] = 0;
  }
  return w;
}

// WGSL constants for the GX enum values the shader switches on, taken from
// the C enums so they can't drift.
std::string wgsl_constants() {
  std::string out;
  const auto c = [&out](std::string_view name, u32 value) { out += fmt::format("const {} = {}u;\n", name, value); };
  c("W_STAGE_COUNT", W_STAGE_COUNT); c("W_NUM_IND", W_NUM_IND); c("W_FOG", W_FOG); c("W_AC_EN", W_AC_EN);
  c("W_AC_COMP0", W_AC_COMP0); c("W_AC_REF0", W_AC_REF0); c("W_AC_OP", W_AC_OP); c("W_AC_COMP1", W_AC_COMP1);
  c("W_AC_REF1", W_AC_REF1); c("W_VTX_STRIDE", W_VTX_STRIDE); c("W_LOADS", W_LOADS); c("W_TEXCOORDS", W_TEXCOORDS);
  c("W_IND_USED", W_IND_USED); c("W_COMPACT", W_COMPACT); c("W_CHANNELS", W_CHANNELS); c("W_ANY_IND", W_ANY_IND);
  c("W_CC", W_CC); c("W_SWAP", W_SWAP); c("W_ATTR", W_ATTR); c("W_TCG", W_TCG); c("W_IND", W_IND);
  c("W_STAGE", W_STAGE); c("STAGE_WORDS", STAGE_WORDS);
  c("A_NONE", GX_NONE); c("A_DIRECT", GX_DIRECT); c("A_INDEX8", GX_INDEX8); c("A_INDEX16", GX_INDEX16);
  c("VA_PNMTXIDX", GX_VA_PNMTXIDX); c("VA_TEX0MTXIDX", GX_VA_TEX0MTXIDX); c("VA_POS", GX_VA_POS);
  c("VA_NRM", GX_VA_NRM); c("VA_CLR0", GX_VA_CLR0); c("VA_TEX0", GX_VA_TEX0);
  c("CT_U8", GX_U8); c("CT_S8", GX_S8); c("CT_U16", GX_U16); c("CT_S16", GX_S16); c("CT_F32", GX_F32);
  c("CT_RGBA8_RAW", GX_RGBA8);
  c("CL_RGB565", GX_RGB565); c("CL_RGB8", GX_RGB8); c("CL_RGBX8", GX_RGBX8); c("CL_RGBA4", GX_RGBA4);
  c("CL_RGBA6", GX_RGBA6); c("CL_RGBA8", GX_RGBA8);
  c("SRC_REG", GX_SRC_REG); c("SRC_VTX", GX_SRC_VTX);
  c("DF_NONE", GX_DF_NONE); c("DF_SIGN", GX_DF_SIGN); c("DF_CLAMP", GX_DF_CLAMP);
  c("AF_SPEC", GX_AF_SPEC); c("AF_SPOT", GX_AF_SPOT); c("AF_NONE", GX_AF_NONE);
  c("TG_MTX3x4", GX_TG_MTX3x4); c("TG_MTX2x4", GX_TG_MTX2x4); c("TG_BUMP0", GX_TG_BUMP0); c("TG_BUMP7", GX_TG_BUMP7);
  c("TG_SRTG", GX_TG_SRTG);
  c("TG_POS", GX_TG_POS); c("TG_NRM", GX_TG_NRM); c("TG_BINRM", GX_TG_BINRM); c("TG_TANGENT", GX_TG_TANGENT);
  c("TG_TEX0", GX_TG_TEX0); c("TG_TEX7", GX_TG_TEX7); c("TG_COLOR0", GX_TG_COLOR0); c("TG_COLOR1", GX_TG_COLOR1);
  c("TM_IDENTITY", GX_IDENTITY); c("PT_IDENTITY", GX_PTIDENTITY); c("PT_TEXMTX0", GX_PTTEXMTX0);
  c("CC_CPREV", GX_CC_CPREV); c("CC_APREV", GX_CC_APREV); c("CC_C0", GX_CC_C0); c("CC_A0", GX_CC_A0);
  c("CC_C1", GX_CC_C1); c("CC_A1", GX_CC_A1); c("CC_C2", GX_CC_C2); c("CC_A2", GX_CC_A2);
  c("CC_TEXC", GX_CC_TEXC); c("CC_TEXA", GX_CC_TEXA); c("CC_RASC", GX_CC_RASC); c("CC_RASA", GX_CC_RASA);
  c("CC_ONE", GX_CC_ONE); c("CC_HALF", GX_CC_HALF); c("CC_KONST", GX_CC_KONST); c("CC_ZERO", GX_CC_ZERO);
  c("CA_APREV", GX_CA_APREV); c("CA_A0", GX_CA_A0); c("CA_A1", GX_CA_A1); c("CA_A2", GX_CA_A2);
  c("CA_TEXA", GX_CA_TEXA); c("CA_RASA", GX_CA_RASA); c("CA_KONST", GX_CA_KONST); c("CA_ZERO", GX_CA_ZERO);
  c("OP_ADD", GX_TEV_ADD); c("OP_SUB", GX_TEV_SUB); c("OP_R8_GT", GX_TEV_COMP_R8_GT);
  c("OP_R8_EQ", GX_TEV_COMP_R8_EQ); c("OP_GR16_GT", GX_TEV_COMP_GR16_GT); c("OP_GR16_EQ", GX_TEV_COMP_GR16_EQ);
  c("OP_BGR24_GT", GX_TEV_COMP_BGR24_GT); c("OP_BGR24_EQ", GX_TEV_COMP_BGR24_EQ);
  c("OP_RGB8_GT", GX_TEV_COMP_RGB8_GT); c("OP_RGB8_EQ", GX_TEV_COMP_RGB8_EQ);
  c("TB_ZERO", GX_TB_ZERO); c("TB_ADDHALF", GX_TB_ADDHALF); c("TB_SUBHALF", GX_TB_SUBHALF);
  c("CS_SCALE_1", GX_CS_SCALE_1); c("CS_SCALE_2", GX_CS_SCALE_2); c("CS_SCALE_4", GX_CS_SCALE_4);
  c("CS_DIVIDE_2", GX_CS_DIVIDE_2);
  c("KC_K0", GX_TEV_KCSEL_K0); c("KC_K3", GX_TEV_KCSEL_K3); c("KC_K0_R", GX_TEV_KCSEL_K0_R);
  c("KC_K0_G", GX_TEV_KCSEL_K0_G); c("KC_K0_B", GX_TEV_KCSEL_K0_B); c("KC_K0_A", GX_TEV_KCSEL_K0_A);
  c("KC_K3_A", GX_TEV_KCSEL_K3_A); c("KC_1_8", GX_TEV_KCSEL_1_8);
  c("KA_K0_R", GX_TEV_KASEL_K0_R); c("KA_K0_G", GX_TEV_KASEL_K0_G); c("KA_K0_B", GX_TEV_KASEL_K0_B);
  c("KA_K0_A", GX_TEV_KASEL_K0_A); c("KA_K3_A", GX_TEV_KASEL_K3_A); c("KA_1_8", GX_TEV_KASEL_1_8);
  c("CH_COLOR0", GX_COLOR0); c("CH_COLOR1", GX_COLOR1); c("CH_ALPHA0", GX_ALPHA0); c("CH_ALPHA1", GX_ALPHA1);
  c("CH_COLOR0A0", GX_COLOR0A0); c("CH_COLOR1A1", GX_COLOR1A1); c("CH_ZERO", GX_COLOR_ZERO);
  c("CH_BUMP", GX_ALPHA_BUMP); c("CH_BUMPN", GX_ALPHA_BUMPN); c("CH_NULL", GX_COLOR_NULL);
  c("TEXCOORD_NULL", GX_TEXCOORD_NULL); c("TEXMAP_NULL", GX_TEXMAP_NULL);
  c("ITF_8", GX_ITF_8); c("ITF_5", GX_ITF_5); c("ITF_4", GX_ITF_4); c("ITF_3", GX_ITF_3);
  c("ITB_NONE", GX_ITB_NONE); c("ITB_S", GX_ITB_S); c("ITB_T", GX_ITB_T); c("ITB_ST", GX_ITB_ST);
  c("ITB_U", GX_ITB_U); c("ITB_SU", GX_ITB_SU); c("ITB_TU", GX_ITB_TU); c("ITB_STU", GX_ITB_STU);
  c("ITBA_OFF", GX_ITBA_OFF); c("ITBA_S", GX_ITBA_S); c("ITBA_T", GX_ITBA_T); c("ITBA_U", GX_ITBA_U);
  c("ITM_OFF", GX_ITM_OFF); c("ITM_0", GX_ITM_0); c("ITM_2", GX_ITM_2); c("ITM_S0", GX_ITM_S0);
  c("ITM_S2", GX_ITM_S2); c("ITM_T0", GX_ITM_T0); c("ITM_T2", GX_ITM_T2);
  c("ITW_OFF", GX_ITW_OFF); c("ITW_256", GX_ITW_256); c("ITW_128", GX_ITW_128); c("ITW_64", GX_ITW_64);
  c("ITW_32", GX_ITW_32); c("ITW_16", GX_ITW_16); c("ITW_0", GX_ITW_0);
  c("FOG_NONE", GX_FOG_NONE);
  c("FOG_P_LIN", GX_FOG_PERSP_LIN); c("FOG_O_LIN", GX_FOG_ORTHO_LIN); c("FOG_P_EXP", GX_FOG_PERSP_EXP);
  c("FOG_O_EXP", GX_FOG_ORTHO_EXP); c("FOG_P_EXP2", GX_FOG_PERSP_EXP2); c("FOG_O_EXP2", GX_FOG_ORTHO_EXP2);
  c("FOG_P_REVEXP", GX_FOG_PERSP_REVEXP); c("FOG_O_REVEXP", GX_FOG_ORTHO_REVEXP);
  c("FOG_P_REVEXP2", GX_FOG_PERSP_REVEXP2); c("FOG_O_REVEXP2", GX_FOG_ORTHO_REVEXP2);
  c("CMP_NEVER", GX_NEVER); c("CMP_LESS", GX_LESS); c("CMP_EQUAL", GX_EQUAL); c("CMP_LEQUAL", GX_LEQUAL);
  c("CMP_GREATER", GX_GREATER); c("CMP_NEQUAL", GX_NEQUAL); c("CMP_GEQUAL", GX_GEQUAL); c("CMP_ALWAYS", GX_ALWAYS);
  c("AOP_AND", GX_AOP_AND); c("AOP_OR", GX_AOP_OR); c("AOP_XOR", GX_AOP_XOR); c("AOP_XNOR", GX_AOP_XNOR);
  c("MAX_LIGHTS", GX::MaxLights);
  return out;
}

// One fetch dispatcher per storage buffer: WGSL pointers can't be chosen at
// run time. Each case calls the exact helper the specialized generator emits
// for that format (shader.cpp fetch_attr / fetch_color_attr).
constexpr std::string_view FetchTemplate = R"(
fn fetch_attr_BUF(off: u32, ct: u32, cnt: u32, frac: u32, le: bool) -> vec4f {
  switch ct {
    case CT_U8: {
      switch cnt {
        case 1u: { return vec4f(fetch_u8_1(&BUF, off, frac, le), 0.0, 0.0, 0.0); }
        case 2u: { return vec4f(fetch_u8_2(&BUF, off, frac, le), 0.0, 0.0); }
        case 3u: { return vec4f(fetch_u8_3(&BUF, off, frac, le), 0.0); }
        default: { return fetch_u8_4(&BUF, off, frac, le); }
      }
    }
    case CT_S8: {
      switch cnt {
        case 1u: { return vec4f(fetch_s8_1(&BUF, off, frac, le), 0.0, 0.0, 0.0); }
        case 2u: { return vec4f(fetch_s8_2(&BUF, off, frac, le), 0.0, 0.0); }
        case 3u: { return vec4f(fetch_s8_3(&BUF, off, frac, le), 0.0); }
        default: { return fetch_s8_4(&BUF, off, frac, le); }
      }
    }
    case CT_U16: {
      switch cnt {
        case 1u: { return vec4f(fetch_u16_1(&BUF, off, frac, le), 0.0, 0.0, 0.0); }
        case 2u: { return vec4f(fetch_u16_1(&BUF, off + 0u, frac, le), fetch_u16_1(&BUF, off + 2u, frac, le), 0.0, 0.0); }
        case 3u: { return vec4f(fetch_u16_1(&BUF, off + 0u, frac, le), fetch_u16_1(&BUF, off + 2u, frac, le),
                                fetch_u16_1(&BUF, off + 4u, frac, le), 0.0); }
        default: { return fetch_u16_4(&BUF, off, frac, le); }
      }
    }
    case CT_S16: {
      switch cnt {
        case 1u: { return vec4f(fetch_s16_1(&BUF, off, frac, le), 0.0, 0.0, 0.0); }
        case 2u: { return vec4f(fetch_s16_1(&BUF, off + 0u, frac, le), fetch_s16_1(&BUF, off + 2u, frac, le), 0.0, 0.0); }
        case 3u: { return vec4f(fetch_s16_1(&BUF, off + 0u, frac, le), fetch_s16_1(&BUF, off + 2u, frac, le),
                                fetch_s16_1(&BUF, off + 4u, frac, le), 0.0); }
        default: { return fetch_s16_4(&BUF, off, frac, le); }
      }
    }
    case CT_F32: {
      switch cnt {
        case 1u: { return vec4f(fetch_f32_1(&BUF, off, le), 0.0, 0.0, 0.0); }
        case 2u: { return vec4f(fetch_f32_2(&BUF, off, le), 0.0, 0.0); }
        case 3u: { return vec4f(fetch_f32_3(&BUF, off, le), 0.0); }
        default: { return fetch_f32_4(&BUF, off, le); }
      }
    }
    case CT_RGBA8_RAW: { return unpack4x8unorm(load_u32_raw(&BUF, off)); }
    default: { return vec4f(0.0); }
  }
}

fn fetch_color_BUF(off: u32, ct: u32, le: bool) -> vec4f {
  switch ct {
    case CL_RGB565: { return fetch_rgb565(&BUF, off, le); }
    case CL_RGB8: { return fetch_rgb8(&BUF, off, le); }
    case CL_RGBX8: { return fetch_rgbx8(&BUF, off, le); }
    case CL_RGBA4: { return fetch_rgba4(&BUF, off, le); }
    case CL_RGBA6: { return fetch_rgba6(&BUF, off, le); }
    default: { return fetch_rgba8(&BUF, off, le); }
  }
}
)";

constexpr std::string_view UberBody = R"(
struct Light {
    pos: vec3f,
    dir: vec3f,
    color: vec4f,
    cos_att: vec3f,
    dist_att: vec3f,
};
struct Fog {
    color: vec4f,
    a: f32,
    b: f32,
    c: f32,
    pad: f32,
};
struct Uniform {
    vtx_start: u32,
    current_pnmtx: u32,
    render_viewport_size: vec2f,
    logical_viewport_size: vec2f,
    pad: vec2u,
    array_start: array<u32, 12>,
    line_width: f32,
    line_aspect_y: f32,
    line_tex_offset: f32,
    line_texcoord_mask: u32,
    proj: mat4x4f,
    postex_mtx: array<mat3x4f, 20>,
    nrm_mtx: array<mat3x4f, 10>,
    tevreg: array<vec4f, 4>,
    lights: array<Light, 8>,
    light_state: vec4u,
    cc_amb: array<vec4f, 4>,
    cc_mat: array<vec4f, 4>,
    kcolor: array<vec4f, 4>,
    postmtx: array<mat3x4f, 20>,
    fog: Fog,
    texcoord_scale: array<vec4f, 8>,
    ind_mtx: array<mat2x4f, 3>,
    tex_size_bias: array<vec4f, 8>,
    cfg: array<vec4u, CFG_VEC4S>,
};
@group(0) @binding(0)
var<storage, read> vbuf: array<u32>;
@group(0) @binding(1)
var<storage, read> abuf: array<u32>;
@group(1) @binding(0)
var<uniform> ubuf: Uniform;
@group(2) @binding(0) var tex0: texture_2d<f32>;
@group(2) @binding(1) var tex0_samp: sampler;
@group(2) @binding(2) var tex1: texture_2d<f32>;
@group(2) @binding(3) var tex1_samp: sampler;
@group(2) @binding(4) var tex2: texture_2d<f32>;
@group(2) @binding(5) var tex2_samp: sampler;
@group(2) @binding(6) var tex3: texture_2d<f32>;
@group(2) @binding(7) var tex3_samp: sampler;
@group(2) @binding(8) var tex4: texture_2d<f32>;
@group(2) @binding(9) var tex4_samp: sampler;
@group(2) @binding(10) var tex5: texture_2d<f32>;
@group(2) @binding(11) var tex5_samp: sampler;
@group(2) @binding(12) var tex6: texture_2d<f32>;
@group(2) @binding(13) var tex6_samp: sampler;
@group(2) @binding(14) var tex7: texture_2d<f32>;
@group(2) @binding(15) var tex7_samp: sampler;

fn cw(i: u32) -> u32 { return ubuf.cfg[i >> 2u][i & 3u]; }
fn byte_at(v: u32, b: u32) -> u32 { return (v >> (b * 8u)) & 0xFFu; }

fn sample_tex(map: u32, uv: vec2f, bias: f32) -> vec4f {
  switch map {
    case 0u: { return textureSampleBias(tex0, tex0_samp, uv, bias); }
    case 1u: { return textureSampleBias(tex1, tex1_samp, uv, bias); }
    case 2u: { return textureSampleBias(tex2, tex2_samp, uv, bias); }
    case 3u: { return textureSampleBias(tex3, tex3_samp, uv, bias); }
    case 4u: { return textureSampleBias(tex4, tex4_samp, uv, bias); }
    case 5u: { return textureSampleBias(tex5, tex5_samp, uv, bias); }
    case 6u: { return textureSampleBias(tex6, tex6_samp, uv, bias); }
    default: { return textureSampleBias(tex7, tex7_samp, uv, bias); }
  }
}

// ---- vertex attributes (shader.cpp attr_address / attr_load) --------------
fn attr_type(attr: u32) -> u32 { return cw(W_ATTR + attr * 4u); }
fn attr_ct(attr: u32) -> u32 { return byte_at(cw(W_ATTR + attr * 4u + 1u), 0u); }
fn attr_cnt(attr: u32) -> u32 { return byte_at(cw(W_ATTR + attr * 4u + 1u), 1u); }
fn attr_frac(attr: u32) -> u32 { return byte_at(cw(W_ATTR + attr * 4u + 1u), 2u); }
fn attr_offset(attr: u32) -> u32 { return byte_at(cw(W_ATTR + attr * 4u + 2u), 0u); }
fn attr_stride(attr: u32) -> u32 { return byte_at(cw(W_ATTR + attr * 4u + 2u), 1u); }
fn attr_le(attr: u32) -> bool { return (cw(W_ATTR + attr * 4u + 3u) & 1u) != 0u; }
fn attr_nbt3(attr: u32) -> bool { return (cw(W_ATTR + attr * 4u + 3u) & 2u) != 0u; }

struct AttrAddr {
  off: u32,
  arr: bool,
  le: bool,
};

fn attr_address(attr: u32, vidx: u32, dl_extra: u32, within: u32) -> AttrAddr {
  let ty = attr_type(attr);
  let dl_offset = attr_offset(attr) + dl_extra;
  let stride = cw(W_VTX_STRIDE);
  if (ty == A_INDEX8) {
    let idx = raw_fetch_u8_1(&vbuf, ubuf.vtx_start + vidx * stride + dl_offset);
    return AttrAddr(ubuf.array_start[attr - VA_POS] + idx * attr_stride(attr) + within, true, attr_le(attr));
  }
  if (ty == A_INDEX16) {
    let idx = raw_fetch_u16_1(&vbuf, ubuf.vtx_start + vidx * stride + dl_offset, false);
    return AttrAddr(ubuf.array_start[attr - VA_POS] + idx * attr_stride(attr) + within, true, attr_le(attr));
  }
  return AttrAddr(ubuf.vtx_start + vidx * stride + (dl_offset + within), false, false);
}

fn fetch_attr_cnt(attr: u32, vidx: u32, dl_extra: u32, within: u32, cnt: u32) -> vec4f {
  let a = attr_address(attr, vidx, dl_extra, within);
  if (a.arr) {
    return fetch_attr_abuf(a.off, attr_ct(attr), cnt, attr_frac(attr), a.le);
  }
  return fetch_attr_vbuf(a.off, attr_ct(attr), cnt, attr_frac(attr), a.le);
}

fn load_u8_attr(attr: u32, vidx: u32) -> u32 {
  let a = attr_address(attr, vidx, 0u, 0u);
  if (a.arr) {
    return raw_fetch_u8_1(&abuf, a.off);
  }
  return raw_fetch_u8_1(&vbuf, a.off);
}

fn load_pnmtxidx(vidx: u32) -> u32 {
  if (attr_type(VA_PNMTXIDX) == A_NONE) {
    if (cw(W_COMPACT) != 0u) {
      // Specialized compact matrices put min(current, 9) in slot 0.
      return min(ubuf.current_pnmtx, 9u);
    }
    return ubuf.current_pnmtx;
  }
  return load_u8_attr(VA_PNMTXIDX, vidx) / 3u;
}

fn load_pos(vidx: u32) -> vec3f {
  let cnt = attr_cnt(VA_POS);
  let v = fetch_attr_cnt(VA_POS, vidx, 0u, 0u, cnt);
  if (cnt == 2u) {
    return vec3f(v.xy, 0.0);
  }
  return v.xyz;
}

fn load_nrm(vidx: u32) -> vec3f {
  if (attr_type(VA_NRM) == A_NONE) {
    return vec3f(1.0, 0.0, 0.0);
  }
  return fetch_attr_cnt(VA_NRM, vidx, 0u, 0u, min(attr_cnt(VA_NRM), 3u)).xyz;
}

fn comp_size(ct: u32) -> u32 {
  switch ct {
    case CT_U8, CT_S8: { return 1u; }
    case CT_U16, CT_S16: { return 2u; }
    default: { return 4u; }
  }
}

fn load_nbt_slice(vidx: u32, slice: u32) -> vec3f {
  var dl_extra = 0u;
  if (attr_nbt3(VA_NRM)) {
    let ty = attr_type(VA_NRM);
    if (ty == A_INDEX8) {
      dl_extra = slice;
    } else if (ty == A_INDEX16) {
      dl_extra = slice * 2u;
    }
  }
  let within = slice * 3u * comp_size(attr_ct(VA_NRM));
  return fetch_attr_cnt(VA_NRM, vidx, dl_extra, within, 3u).xyz;
}

fn load_color(i: u32, vidx: u32) -> vec4f {
  let attr = VA_CLR0 + i;
  if (attr_type(attr) == A_NONE) {
    return vec4f(0.0, 0.0, 0.0, 0.0);
  }
  let a = attr_address(attr, vidx, 0u, 0u);
  if (a.arr) {
    return fetch_color_abuf(a.off, attr_ct(attr), a.le);
  }
  return fetch_color_vbuf(a.off, attr_ct(attr), a.le);
}

fn load_tex(i: u32, vidx: u32) -> vec2f {
  let attr = VA_TEX0 + i;
  let cnt = attr_cnt(attr);
  let v = fetch_attr_cnt(attr, vidx, 0u, 0u, cnt);
  if (cnt == 1u) {
    return vec2f(v.x, 0.0);
  }
  return v.xy;
}

// ---- lighting (shader.cpp lighting_func, per-vertex) ------------------------
fn light_channel(chan: u32, amb_in: vec4f, mat_in: vec4f, mv_pos: vec3f, mv_nrm: vec3f) -> vec4f {
  let base = W_CC + chan * 5u;
  let diff_fn = cw(base + 2u);
  let attn_fn = cw(base + 3u);
  var lighting = amb_in;
  for (var i = 0u; i < MAX_LIGHTS; i++) {
      if ((ubuf.light_state[chan] & (1u << i)) == 0u) { continue; }
      var light = ubuf.lights[i];
      var ldir = light.pos - mv_pos;
      var dist2 = dot(ldir, ldir);
      var dist = sqrt(dist2);
      ldir = ldir / dist;
      var attn: f32;
      if (attn_fn == AF_NONE) {
        attn = 1.0;
      } else if (attn_fn == AF_SPOT) {
          var cosine = max(0.0, dot(ldir, light.dir));
          var cos_attn = dot(light.cos_att, vec3f(1.0, cosine, cosine * cosine));
          var dist_attn = dot(light.dist_att, vec3f(1.0, dist, dist2));
          attn = max(0.0, cos_attn / dist_attn);
      } else if (attn_fn == AF_SPEC) {
          attn = select(0.0, max(0.0, dot(mv_nrm, light.dir)), dot(mv_nrm, ldir) >= 0.0);
          var cos_attn = dot(light.cos_att, vec3f(1.0, attn, attn * attn));
          var dist_attn: f32;
          if (diff_fn != DF_NONE) {
            dist_attn = max(0.0, dot(normalize(light.dist_att), vec3f(1.0, attn, attn * attn)));
          } else {
            dist_attn = max(0.0, dot(light.dist_att, vec3f(1.0, attn, attn * attn)));
          }
          attn = max(0.0, cos_attn / dist_attn);
      }
      var diff: f32;
      if (diff_fn == DF_SIGN) {
        diff = dot(ldir, mv_nrm);
      } else if (diff_fn == DF_CLAMP) {
        diff = max(0.0, dot(ldir, mv_nrm));
      } else {
        diff = 1.0;
      }
      lighting = lighting + (attn * diff * light.color);
  }
  return (mat_in * clamp(lighting, vec4f(0.0), vec4f(1.0)));
}

fn channel_src(chan: u32, amb: bool, vtx_color: vec4f) -> vec4f {
  let src = cw(W_CC + chan * 5u + select(0u, 1u, amb));
  if (src == SRC_VTX) {
    return vtx_color;
  }
  return select(ubuf.cc_mat[chan], ubuf.cc_amb[chan], amb);
}

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) cc0: vec4f,
    @location(1) cc1: vec4f,
    @location(2) tex0: vec3f,
    @location(3) tex1: vec3f,
    @location(4) tex2: vec3f,
    @location(5) tex3: vec3f,
    @location(6) tex4: vec3f,
    @location(7) tex5: vec3f,
    @location(8) tex6: vec3f,
    @location(9) tex7: vec3f,
};

@vertex
fn vs_main(
    @builtin(vertex_index) vidx: u32,
    @builtin(instance_index) iidx: u32
) -> VertexOutput {
    var out: VertexOutput;
    var in_vidx = vidx;
    var in_pos: vec3f;
    var in_pnmtxidx: u32;
    var mv_pos: vec3f;
    var mv_pos_a: vec3f;
    var mv_pos_b: vec3f;
    var use_b = false;
    if (LINE_MODE == 3u) {
      in_vidx = iidx;
      in_pos = load_pos(in_vidx);
      in_pnmtxidx = load_pnmtxidx(in_vidx);
      mv_pos = vec4f(in_pos, 1.0) * ubuf.postex_mtx[in_pnmtxidx];
    } else if (LINE_MODE != 0u) {
      use_b = vidx >= 2u;
      let vidx_a = iidx * LINE_STEP;
      let vidx_b = vidx_a + 1u;
      in_vidx = select(vidx_a, vidx_b, use_b);
      let pos_a = load_pos(vidx_a);
      let pos_b = load_pos(vidx_b);
      in_pos = select(pos_a, pos_b, use_b);
      let pnmtxidx_a = load_pnmtxidx(vidx_a);
      let pnmtxidx_b = load_pnmtxidx(vidx_b);
      in_pnmtxidx = select(pnmtxidx_a, pnmtxidx_b, use_b);
      mv_pos_a = vec4f(pos_a, 1.0) * ubuf.postex_mtx[pnmtxidx_a];
      mv_pos_b = vec4f(pos_b, 1.0) * ubuf.postex_mtx[pnmtxidx_b];
      mv_pos = select(mv_pos_a, mv_pos_b, use_b);
    } else {
      in_pnmtxidx = load_pnmtxidx(in_vidx);
      in_pos = load_pos(in_vidx);
      mv_pos = vec4f(in_pos, 1.0) * ubuf.postex_mtx[in_pnmtxidx];
      out.pos = vec4f(mv_pos, 1.0) * ubuf.proj;
    }
    let in_nrm = load_nrm(in_vidx);
    let in_clr0 = load_color(0u, in_vidx);
    let in_clr1 = load_color(1u, in_vidx);

    if (LINE_MODE == 3u) {
      let clip = vec4f(mv_pos, 1.0) * ubuf.proj;
      let viewport_scale = ubuf.render_viewport_size / max(ubuf.logical_viewport_size, vec2f(1.0));
      let point_size = ubuf.line_width * min(viewport_scale.x, viewport_scale.y);
      let x_sign = select(-1.0, 1.0, (vidx & 1u) != 0u);
      let y_sign = select(-1.0, 1.0, vidx >= 2u);
      let offset_px = vec2f(x_sign, y_sign) * (point_size / 2.0);
      let offset_ndc = (offset_px * 2.0) / ubuf.render_viewport_size;
      out.pos = vec4f(clip.xy + offset_ndc * clip.w, clip.zw);
    } else if (LINE_MODE != 0u) {
      let clip_a = vec4f(mv_pos_a, 1.0) * ubuf.proj;
      let clip_b = vec4f(mv_pos_b, 1.0) * ubuf.proj;
      let ndc_a = clip_a.xy / clip_a.w;
      let ndc_b = clip_b.xy / clip_b.w;
      let viewport_scale = ubuf.render_viewport_size / max(ubuf.logical_viewport_size, vec2f(1.0));
      let delta_px = (ndc_b - ndc_a) / 2.0 * ubuf.render_viewport_size;
      let dir_px = select(vec2f(1.0, 0.0), normalize(delta_px), dot(delta_px, delta_px) > 1e-10);
      let perp_px = vec2f(-dir_px.y, dir_px.x);
      let line_width = ubuf.line_width * min(viewport_scale.x, viewport_scale.y);
      let offset_px = perp_px * (line_width / 2.0) * select(-1.0, 1.0, (vidx & 1u) != 0u);
      let offset_ndc = (offset_px * 2.0) / ubuf.render_viewport_size;
      let clip_base = select(clip_a, clip_b, use_b);
      out.pos = vec4f(clip_base.xy + offset_ndc * clip_base.w, clip_base.zw);
    }
    let nrm_tmp = vec4f(in_nrm, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];
    let mv_nrm = select(nrm_tmp, normalize(nrm_tmp), dot(nrm_tmp, nrm_tmp) > 1e-10);

    // Color channels (rast0/rast1 in the fragment shader).
    let channels = cw(W_CHANNELS);
    for (var c = 0u; c < 2u; c++) {
      if ((channels & (1u << c)) == 0u) { continue; }
      let vtx = select(in_clr0, in_clr1, c == 1u);
      var cc: vec4f;
      // Color part.
      if (cw(W_CC + c * 5u + 4u) != 0u) {
        cc = light_channel(c, channel_src(c, true, vtx), channel_src(c, false, vtx), mv_pos, mv_nrm);
      } else {
        cc = channel_src(c, false, vtx);
      }
      // Alpha part.
      let ca = c + 2u;
      if (cw(W_CC + ca * 5u + 4u) != 0u) {
        cc.a = light_channel(ca, channel_src(ca, true, vtx), channel_src(ca, false, vtx), mv_pos, mv_nrm).a;
      } else {
        cc.a = channel_src(ca, false, vtx).a;
      }
      if (c == 0u) { out.cc0 = cc; } else { out.cc1 = cc; }
    }

    // Texture coordinate generation (shader.cpp, sampled coordinates only).
    let coords = cw(W_TEXCOORDS);
    var needs_nbt = false;
    for (var i = 0u; i < 8u; i++) {
      if ((coords & (1u << i)) == 0u) { continue; }
      let ty = cw(W_TCG + i * 4u);
      let src = cw(W_TCG + i * 4u + 1u);
      if ((ty >= TG_BUMP0 && ty <= TG_BUMP7) || src == TG_BINRM || src == TG_TANGENT) {
        needs_nbt = true;
      }
    }
    var in_binrm = vec3f(0.0);
    var in_tangent = vec3f(0.0);
    if (needs_nbt) {
      in_binrm = load_nbt_slice(in_vidx, 1u);
      in_tangent = load_nbt_slice(in_vidx, 2u);
    }
    var tc_proj: array<vec3f, 8>;
    var tc_out: array<vec3f, 8>;
    for (var i = 0u; i < 8u; i++) {
      if ((coords & (1u << i)) == 0u) { continue; }
      let ty = cw(W_TCG + i * 4u);
      if (ty >= TG_BUMP0 && ty <= TG_BUMP7) { continue; }
      let src = cw(W_TCG + i * 4u + 1u);
      let mtx = cw(W_TCG + i * 4u + 2u);
      let post_word = cw(W_TCG + i * 4u + 3u);
      let post = post_word & 0xFFFFu;
      var tc: vec4f;
      if (src >= TG_TEX0 && src <= TG_TEX7) {
        tc = vec4f(load_tex(src - TG_TEX0, in_vidx), 1.0, 1.0);
      } else if (src == TG_POS) {
        tc = vec4f(in_pos, 1.0);
      } else if (src == TG_NRM) {
        tc = vec4f(in_nrm, 1.0);
      } else if (src == TG_COLOR0) {
        tc = in_clr0;
      } else if (src == TG_COLOR1) {
        tc = in_clr1;
      } else if (src == TG_BINRM) {
        tc = vec4f(in_binrm, 1.0);
      } else if (src == TG_TANGENT) {
        tc = vec4f(in_tangent, 1.0);
      }
      var tc_tmp: vec3f;
      if (ty == TG_MTX2x4 || ty == TG_MTX3x4) {
        if (attr_type(VA_TEX0MTXIDX + i) != A_NONE) {
          let in_texmtxidx = load_u8_attr(VA_TEX0MTXIDX + i, in_vidx);
          tc_tmp = tc * ubuf.postex_mtx[in_texmtxidx / 3u];
        } else if (mtx == TM_IDENTITY) {
          tc_tmp = tc.xyz;
        } else {
          tc_tmp = tc * ubuf.postex_mtx[mtx / 3u];
        }
        if (ty == TG_MTX2x4) {
          tc_tmp.z = 1.0f;
        }
      } else if (ty == TG_SRTG) {
        tc_tmp = vec3f(tc.xy, 1.0f);
      }
      if (((post_word >> 16u) & 1u) != 0u) {
        tc_tmp = normalize(tc_tmp);
      }
      var proj: vec3f;
      if (post == PT_IDENTITY) {
        proj = tc_tmp;
      } else {
        proj = vec4f(tc_tmp.xyz, 1.0) * ubuf.postmtx[(post - PT_TEXMTX0) / 3u];
      }
      if (LINE_MODE == 3u) {
        if ((ubuf.line_texcoord_mask & (1u << i)) != 0u) {
            if ((vidx & 1u) != 0u) { proj.x += ubuf.line_tex_offset; }
            if (vidx >= 2u) { proj.y += ubuf.line_tex_offset; }
        }
      } else if (LINE_MODE != 0u) {
        if ((ubuf.line_texcoord_mask & (1u << i)) != 0u && (vidx & 1u) != 0u) {
            proj.y += ubuf.line_tex_offset;
        }
      }
      tc_proj[i] = proj;
      if (ty == TG_MTX3x4) {
        tc_out[i] = proj;
      } else {
        tc_out[i] = vec3f(proj.xy, 1.0);
      }
    }
    for (var i = 0u; i < 8u; i++) {
      if ((coords & (1u << i)) == 0u) { continue; }
      let ty = cw(W_TCG + i * 4u);
      if (ty < TG_BUMP0 || ty > TG_BUMP7) { continue; }
      // Emboss bump: offset the source coordinate by the light direction
      // projected onto tangent/binormal.
      let emboss_src = cw(W_TCG + i * 4u + 3u) >> 24u;
      let bump_ldir = normalize(ubuf.lights[ty - TG_BUMP0].pos - mv_pos);
      let bump_tan = vec4f(in_tangent, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];
      let bump_bin = vec4f(in_binrm, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];
      tc_out[i] = vec3f(tc_proj[emboss_src].xy + vec2f(dot(bump_ldir, bump_tan), dot(bump_ldir, bump_bin)), 1.0);
    }
    out.tex0 = tc_out[0];
    out.tex1 = tc_out[1];
    out.tex2 = tc_out[2];
    out.tex3 = tc_out[3];
    out.tex4 = tc_out[4];
    out.tex5 = tc_out[5];
    out.tex6 = tc_out[6];
    out.tex7 = tc_out[7];
    return out;
}

// ---- TEV (shader.cpp color_arg_reg / alpha_arg_reg / tev_op) ------------------
fn swz(v: vec4f, ch: u32) -> f32 { return v[ch]; }

fn alpha_bump(stage_base: u32, chan: u32, ind: array<vec3f, 4>) -> f32 {
  let w5 = cw(stage_base + 5u);
  let w6 = cw(stage_base + 6u);
  let ind_stage = byte_at(w5, 3u);
  let alpha_sel = byte_at(w6, 2u);
  let fmt = byte_at(w6, 0u);
  if (ind_stage >= cw(W_NUM_IND) || alpha_sel == ITBA_OFF) {
    return 0.0;
  }
  var base: f32;
  if (alpha_sel == ITBA_S) {
    base = ind[ind_stage].x;
  } else if (alpha_sel == ITBA_T) {
    base = ind[ind_stage].y;
  } else {
    base = ind[ind_stage].z;
  }
  var mask = 0xF8u;
  if (fmt == ITF_5) {
    mask = 0xE0u;
  } else if (fmt == ITF_4) {
    mask = 0xF0u;
  }
  var alpha = (f32(u32(base) & mask) / 255.0);
  if (chan == CH_BUMPN) {
    alpha = (alpha * (255.0 / 248.0));
  }
  return alpha;
}

fn konst_color(sel: u32) -> vec3f {
  if (sel <= KC_1_8) {
    return vec3f(f32(8u - sel) / 8.0);
  }
  if (sel < KC_K0_R) {
    return ubuf.kcolor[sel - KC_K0].rgb;
  }
  let k = (sel - KC_K0_R) & 3u;
  let comp = (sel - KC_K0_R) >> 2u;
  return vec3f(ubuf.kcolor[k][comp]);
}

fn konst_alpha(sel: u32) -> f32 {
  if (sel <= KA_1_8) {
    return f32(8u - sel) / 8.0;
  }
  let k = (sel - KA_K0_R) & 3u;
  let comp = (sel - KA_K0_R) >> 2u;
  return ubuf.kcolor[k][comp];
}

fn tev_bias_scale3(v_in: vec3f, bias: u32, scale: u32) -> vec3f {
  var v = v_in;
  if (bias == TB_ADDHALF) { v = v + 0.5; } else if (bias == TB_SUBHALF) { v = v - 0.5; }
  if (scale == CS_SCALE_2) { v = v * 2.0; } else if (scale == CS_SCALE_4) { v = v * 4.0; }
  else if (scale == CS_DIVIDE_2) { v = v / 2.0; }
  return v;
}

fn tev_bias_scale1(v_in: f32, bias: u32, scale: u32) -> f32 {
  var v = v_in;
  if (bias == TB_ADDHALF) { v = v + 0.5; } else if (bias == TB_SUBHALF) { v = v - 0.5; }
  if (scale == CS_SCALE_2) { v = v * 2.0; } else if (scale == CS_SCALE_4) { v = v * 4.0; }
  else if (scale == CS_DIVIDE_2) { v = v / 2.0; }
  return v;
}

fn tev_color(op: u32, bias: u32, scale: u32, clamped: bool, a_in: vec3f, b_in: vec3f, c_in: vec3f, d: vec3f) -> vec3f {
  let a = tev_overflow_vec3f(a_in);
  let b = tev_overflow_vec3f(b_in);
  let c = tev_overflow_vec3f(c_in);
  var v: vec3f;
  switch op {
    case OP_ADD: { v = tev_bias_scale3((mix(a, b, c) + d), bias, scale); }
    case OP_SUB: { v = tev_bias_scale3((-mix(a, b, c) + d), bias, scale); }
    case OP_R8_GT: { v = select(vec3(0), c, round(a.r * 255.0) > round(b.r * 255.0)) + d; }
    case OP_R8_EQ: { v = select(vec3(0), c, round(a.r * 255.0) == round(b.r * 255.0)) + d; }
    case OP_GR16_GT: { v = select(vec3(0), c, round(dot(a.rg * 255.0, vec2(1.0, 256.0))) > round(dot(b.rg * 255.0, vec2(1.0, 256.0)))) + d; }
    case OP_GR16_EQ: { v = select(vec3(0), c, round(dot(a.rg * 255.0, vec2(1.0, 256.0))) == round(dot(b.rg * 255.0, vec2(1.0, 256.0)))) + d; }
    case OP_BGR24_GT: { v = select(vec3(0), c, round(dot(a.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) > round(dot(b.rgb * 255.0, vec3(1.0, 256.0, 65536.0)))) + d; }
    case OP_BGR24_EQ: { v = select(vec3(0), c, round(dot(a.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) == round(dot(b.rgb * 255.0, vec3(1.0, 256.0, 65536.0)))) + d; }
    case OP_RGB8_GT: { v = select(vec3(0), c, round(a * 255.0) > round(b * 255.0)) + d; }
    case OP_RGB8_EQ: { v = select(vec3(0), c, round(a * 255.0) == round(b * 255.0)) + d; }
    default: { v = d; }
  }
  if (clamped) {
    return clamp(v, vec3f(0.0), vec3f(1.0));
  }
  return clamp(v, vec3f(-4.0), vec3f(4.0));
}

fn tev_alpha(op: u32, bias: u32, scale: u32, clamped: bool, a_in: f32, b_in: f32, c_in: f32, d: f32) -> f32 {
  let a = tev_overflow_f32(a_in);
  let b = tev_overflow_f32(b_in);
  let c = tev_overflow_f32(c_in);
  var v: f32;
  switch op {
    case OP_ADD: { v = tev_bias_scale1((mix(a, b, c) + d), bias, scale); }
    case OP_SUB: { v = tev_bias_scale1((-mix(a, b, c) + d), bias, scale); }
    case OP_RGB8_GT, OP_R8_GT, OP_GR16_GT, OP_BGR24_GT: { v = select(0.0, c, round(a * 255.0) > round(b * 255.0)) + d; }
    case OP_RGB8_EQ, OP_R8_EQ, OP_GR16_EQ, OP_BGR24_EQ: { v = select(0.0, c, round(a * 255.0) == round(b * 255.0)) + d; }
    default: { v = d; }
  }
  if (clamped) {
    return clamp(v, 0.0, 1.0);
  }
  return clamp(v, -4.0, 4.0);
}

fn color_arg(arg: u32, regs: array<vec4f, 4>, tex: vec4f, ras_c: vec3f, ras_a: f32, konst: vec3f) -> vec3f {
  switch arg {
    case CC_CPREV: { return regs[0].rgb; }
    case CC_APREV: { return vec3f(regs[0].a); }
    case CC_C0: { return regs[1].rgb; }
    case CC_A0: { return vec3f(regs[1].a); }
    case CC_C1: { return regs[2].rgb; }
    case CC_A1: { return vec3f(regs[2].a); }
    case CC_C2: { return regs[3].rgb; }
    case CC_A2: { return vec3f(regs[3].a); }
    case CC_TEXC: { return tex.rgb; }
    case CC_TEXA: { return vec3f(tex.a); }
    case CC_RASC: { return ras_c; }
    case CC_RASA: { return vec3f(ras_a); }
    case CC_ONE: { return vec3f(1.0); }
    case CC_HALF: { return vec3f(0.5); }
    case CC_KONST: { return konst; }
    default: { return vec3f(0.0); }
  }
}

fn alpha_arg(arg: u32, regs: array<vec4f, 4>, tex: vec4f, ras_a: f32, konst: f32) -> f32 {
  switch arg {
    case CA_APREV: { return regs[0].a; }
    case CA_A0: { return regs[1].a; }
    case CA_A1: { return regs[2].a; }
    case CA_A2: { return regs[3].a; }
    case CA_TEXA: { return tex.a; }
    case CA_RASA: { return ras_a; }
    case CA_KONST: { return konst; }
    default: { return 0.0; }
  }
}

fn alpha_test(comp: u32, ref_v: u32, value: u32) -> bool {
  switch comp {
    case CMP_NEVER: { return false; }
    case CMP_LESS: { return value < ref_v; }
    case CMP_EQUAL: { return value == ref_v; }
    case CMP_LEQUAL: { return value <= ref_v; }
    case CMP_GREATER: { return value > ref_v; }
    case CMP_NEQUAL: { return value != ref_v; }
    case CMP_GEQUAL: { return value >= ref_v; }
    default: { return true; }
  }
}

fn ind_wrap(wrap: u32, coord: f32) -> f32 {
  switch wrap {
    case ITW_256: { return (coord % 256.0); }
    case ITW_128: { return (coord % 128.0); }
    case ITW_64: { return (coord % 64.0); }
    case ITW_32: { return (coord % 32.0); }
    case ITW_16: { return (coord % 16.0); }
    case ITW_0: { return 0.0; }
    default: { return coord; }
  }
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let loads = cw(W_LOADS);
    var regs: array<vec4f, 4>;
    for (var r = 0u; r < 4u; r++) {
      if ((loads & (1u << r)) != 0u) {
        regs[r] = ubuf.tevreg[r];
      }
    }
    let rast0 = in.cc0;
    let rast1 = in.cc1;

    var tex_in: array<vec3f, 8>;
    tex_in[0] = in.tex0;
    tex_in[1] = in.tex1;
    tex_in[2] = in.tex2;
    tex_in[3] = in.tex3;
    tex_in[4] = in.tex4;
    tex_in[5] = in.tex5;
    tex_in[6] = in.tex6;
    tex_in[7] = in.tex7;
    var tex_uv: array<vec2f, 8>;
    let coords = cw(W_TEXCOORDS);
    for (var i = 0u; i < 8u; i++) {
      if ((coords & (1u << i)) == 0u) { continue; }
      if (cw(W_TCG + i * 4u) == TG_MTX3x4) {
        tex_uv[i] = tex_in[i].xy / tex_in[i].z;
      } else {
        tex_uv[i] = tex_in[i].xy;
      }
    }

    // Indirect stages: each sampled once.
    var t_ind: array<vec3f, 4>;
    let ind_used = cw(W_IND_USED);
    for (var i = 0u; i < 4u; i++) {
      if ((ind_used & (1u << i)) == 0u) { continue; }
      let tc = cw(W_IND + i * 4u);
      let map = cw(W_IND + i * 4u + 1u);
      let scale = vec2f(1.0 / f32(1u << cw(W_IND + i * 4u + 2u)), 1.0 / f32(1u << cw(W_IND + i * 4u + 3u)));
      t_ind[i] = 255.0 * sample_tex(map, tex_uv[tc] * ubuf.texcoord_scale[tc].xy * scale / ubuf.tex_size_bias[map].xy,
                                    ubuf.tex_size_bias[map].z).abg;
    }
    let any_ind = cw(W_ANY_IND) != 0u;
    var t_tex = vec2f(0.0);

    let stage_count = cw(W_STAGE_COUNT);
    for (var s = 0u; s < stage_count; s++) {
      let base = W_STAGE + s * STAGE_WORDS;
      let w4 = cw(base + 4u);
      let w5 = cw(base + 5u);
      let w6 = cw(base + 6u);
      let w7 = cw(base + 7u);
      let flags = cw(base + 8u);
      let tex_coord_id = byte_at(w4, 2u);
      let tex_map_id = byte_at(w4, 3u);
      let channel_id = byte_at(w5, 0u);
      let swap_ras = byte_at(w5, 1u);
      let swap_tex = byte_at(w5, 2u);
      let ind_stage = byte_at(w5, 3u);
      let ind_fmt = byte_at(w6, 0u);
      let ind_bias_sel = byte_at(w6, 1u);
      let ind_mtx_id = byte_at(w6, 3u);
      let wrap_s = byte_at(w7, 0u);
      let wrap_t = byte_at(w7, 1u);
      let add_prev = byte_at(w7, 3u) != 0u;
      let needs_sample = (flags & 4u) != 0u;

      // Texture coordinate and sample (fragmentFnPre in shader.cpp).
      var sampled = vec4f(0.0);
      let needs_ind_coord = ind_mtx_id != ITM_OFF;
      let has_ind_stage = ind_stage < cw(W_NUM_IND);
      let needs_tev_coord = needs_ind_coord || wrap_s != ITW_OFF || wrap_t != ITW_OFF || add_prev;
      if (needs_tev_coord || needs_sample) {
        let has_base_coord = tex_coord_id != TEXCOORD_NULL && tex_map_id != TEXMAP_NULL;
        let has_base_texture = tex_map_id != TEXMAP_NULL;
        var uv_set = false;
        var uv: vec2f;
        if (needs_tev_coord) {
          var offset: vec2f;
          var has_offset = false;
          if (needs_ind_coord && has_ind_stage) {
            var coord: vec3f;
            if (ind_fmt == ITF_8) {
              coord = t_ind[ind_stage];
            } else if (ind_fmt == ITF_5) {
              coord = floor(t_ind[ind_stage] / 8.0);
            } else if (ind_fmt == ITF_4) {
              coord = floor(t_ind[ind_stage] / 16.0);
            } else {
              coord = floor(t_ind[ind_stage] / 32.0);
            }
            if (ind_bias_sel != ITB_NONE) {
              let bias = select(1.0, -128.0, ind_fmt == ITF_8);
              let bs = select(0.0, 1.0, ind_bias_sel == ITB_S || ind_bias_sel == ITB_ST || ind_bias_sel == ITB_SU ||
                                        ind_bias_sel == ITB_STU);
              let bt = select(0.0, 1.0, ind_bias_sel == ITB_T || ind_bias_sel == ITB_ST || ind_bias_sel == ITB_TU ||
                                        ind_bias_sel == ITB_STU);
              let bu = select(0.0, 1.0, ind_bias_sel == ITB_U || ind_bias_sel == ITB_SU || ind_bias_sel == ITB_TU ||
                                        ind_bias_sel == ITB_STU);
              coord = coord + vec3f(bs, bt, bu) * bias;
            }
            if (ind_mtx_id >= ITM_0 && ind_mtx_id <= ITM_2) {
              let m = ind_mtx_id - ITM_0;
              let c0 = ubuf.ind_mtx[m][0];
              let c1 = ubuf.ind_mtx[m][1];
              offset = vec2f(dot(vec3f(c0.xz, c1.x), coord), dot(vec3f(c0.yw, c1.y), coord)) * c1.z;
              has_offset = true;
            } else if (ind_mtx_id >= ITM_S0 && ind_mtx_id <= ITM_S2 && has_base_coord) {
              let m = ind_mtx_id - ITM_S0;
              offset = tex_uv[tex_coord_id] * ubuf.texcoord_scale[tex_coord_id].xy * coord.x * ubuf.ind_mtx[m][1][2] / 256.0;
              has_offset = true;
            } else if (ind_mtx_id >= ITM_T0 && ind_mtx_id <= ITM_T2 && has_base_coord) {
              let m = ind_mtx_id - ITM_T0;
              offset = tex_uv[tex_coord_id] * ubuf.texcoord_scale[tex_coord_id].xy * coord.y * ubuf.ind_mtx[m][1][2] / 256.0;
              has_offset = true;
            }
          }
          let simple = ind_mtx_id == ITM_OFF && !add_prev;
          var wrapped: vec2f;
          if (has_base_coord) {
            if (simple) {
              wrapped = tex_uv[tex_coord_id];
            } else {
              wrapped = tex_uv[tex_coord_id] * ubuf.texcoord_scale[tex_coord_id].xy;
            }
            if (wrap_s != ITW_OFF || wrap_t != ITW_OFF) {
              wrapped = vec2f(ind_wrap(wrap_s, wrapped.x), ind_wrap(wrap_t, wrapped.y));
            }
          }
          var final_coord: vec2f;
          var has_final = true;
          if (has_base_coord && has_offset) {
            final_coord = wrapped + (offset);
          } else if (has_base_coord) {
            final_coord = wrapped;
          } else if (has_offset) {
            final_coord = offset;
          } else {
            has_final = false;
          }
          if (any_ind && has_final) {
            if (add_prev) {
              t_tex += final_coord;
            } else {
              t_tex = final_coord;
            }
            if (needs_sample && has_base_texture) {
              if (simple) {
                uv = t_tex;
              } else {
                uv = t_tex / ubuf.tex_size_bias[tex_map_id].xy;
              }
              uv_set = true;
            }
          }
        }
        if (needs_sample) {
          if (!uv_set) {
            uv = tex_uv[tex_coord_id];
          }
          sampled = sample_tex(tex_map_id, uv, ubuf.tex_size_bias[tex_map_id].z);
        }
      }

      // Stage inputs.
      let sw_tex = W_SWAP + swap_tex * 4u;
      let tex = vec4f(swz(sampled, cw(sw_tex)), swz(sampled, cw(sw_tex + 1u)), swz(sampled, cw(sw_tex + 2u)),
                      swz(sampled, cw(sw_tex + 3u)));
      var ras_c = vec3f(0.0);
      var ras_a = 0.0;
      if (channel_id == CH_BUMP || channel_id == CH_BUMPN) {
        ras_a = alpha_bump(base, channel_id, t_ind);
        ras_c = vec3f(ras_a);
      } else if (channel_id != CH_ZERO && channel_id != CH_NULL) {
        let r = select(rast0, rast1, channel_id == CH_COLOR1 || channel_id == CH_ALPHA1 || channel_id == CH_COLOR1A1);
        let sw_ras = W_SWAP + swap_ras * 4u;
        ras_c = vec3f(swz(r, cw(sw_ras)), swz(r, cw(sw_ras + 1u)), swz(r, cw(sw_ras + 2u)));
        ras_a = swz(r, cw(sw_ras + 3u));
      }
      let kc = konst_color(byte_at(w4, 0u));
      let ka = konst_alpha(byte_at(w4, 1u));

      // Color, then alpha (shader.cpp: alpha reads registers after the color write).
      let cpass = cw(base);
      let cop = cw(base + 2u);
      let c_out = byte_at(cop, 3u);
      let c_val = tev_color(byte_at(cop, 0u), byte_at(cop, 1u), byte_at(cop, 2u), (flags & 1u) != 0u,
                            color_arg(byte_at(cpass, 0u), regs, tex, ras_c, ras_a, kc),
                            color_arg(byte_at(cpass, 1u), regs, tex, ras_c, ras_a, kc),
                            color_arg(byte_at(cpass, 2u), regs, tex, ras_c, ras_a, kc),
                            color_arg(byte_at(cpass, 3u), regs, tex, ras_c, ras_a, kc));
      regs[c_out] = vec4f(c_val, regs[c_out].a);
      let apass = cw(base + 1u);
      let aop = cw(base + 3u);
      let a_out = byte_at(aop, 3u);
      regs[a_out].a = tev_alpha(byte_at(aop, 0u), byte_at(aop, 1u), byte_at(aop, 2u), (flags & 2u) != 0u,
                                alpha_arg(byte_at(apass, 0u), regs, tex, ras_a, ka),
                                alpha_arg(byte_at(apass, 1u), regs, tex, ras_a, ka),
                                alpha_arg(byte_at(apass, 2u), regs, tex, ras_a, ka),
                                alpha_arg(byte_at(apass, 3u), regs, tex, ras_a, ka));
    }
    var prev = regs[0];
    if (stage_count > 0u) {
      let last = W_STAGE + (stage_count - 1u) * STAGE_WORDS;
      let c_out = byte_at(cw(last + 2u), 3u);
      let a_out = byte_at(cw(last + 3u), 3u);
      if (c_out != 0u) {
        prev = vec4f(regs[c_out].rgb, prev.a);
      }
      if (a_out != 0u) {
        prev.a = regs[a_out].a;
      }
    }

    let fog_type = cw(W_FOG);
    if (fog_type != FOG_NONE) {
      var fogF = clamp((ubuf.fog.a / (ubuf.fog.b - (1.0 - in.pos.z))) - ubuf.fog.c, 0.0, 1.0);
      var fogZ: f32;
      if (fog_type == FOG_P_LIN || fog_type == FOG_O_LIN) {
        fogZ = fogF;
      } else if (fog_type == FOG_P_EXP || fog_type == FOG_O_EXP) {
        fogZ = 1.0 - exp2(-8.0 * fogF);
      } else if (fog_type == FOG_P_EXP2 || fog_type == FOG_O_EXP2) {
        fogZ = 1.0 - exp2(-8.0 * fogF * fogF);
      } else if (fog_type == FOG_P_REVEXP || fog_type == FOG_O_REVEXP) {
        fogZ = exp2(-8.0 * (1.0 - fogF));
      } else {
        fogF = 1.0 - fogF;
        fogZ = exp2(-8.0 * fogF * fogF);
      }
      prev = vec4f(mix(prev.rgb, ubuf.fog.color.rgb, clamp(fogZ, 0.0, 1.0)), prev.a);
    }
    prev = tev_overflow_vec4f(prev);
    if (cw(W_AC_EN) != 0u) {
      let alphaCompare = u32(round(clamp(prev.a, 0.0, 1.0) * 255.0));
      let p0 = alpha_test(cw(W_AC_COMP0), cw(W_AC_REF0), alphaCompare);
      let p1 = alpha_test(cw(W_AC_COMP1), cw(W_AC_REF1), alphaCompare);
      var pass_: bool;
      switch cw(W_AC_OP) {
        case AOP_AND: { pass_ = p0 && p1; }
        case AOP_OR: { pass_ = p0 || p1; }
        case AOP_XOR: { pass_ = p0 != p1; }
        default: { pass_ = p0 == p1; }
      }
      if (!pass_) { discard; }
    }
    return prev;
}
)";

std::mutex g_uberMutex;
std::array<wgpu::ShaderModule, 4> g_uberModules;

wgpu::ShaderModule uber_module(u8 lineMode) {
  std::lock_guard lock{g_uberMutex};
  auto& module = g_uberModules[lineMode & 3];
  if (!module) {
    const auto source = build_uber_shader_source(lineMode);
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = source.c_str();
    const auto label = fmt::format("GX Ubershader (line mode {})", lineMode);
    const wgpu::ShaderModuleDescriptor desc{.nextInChain = &wgsl, .label = label.c_str()};
    module = webgpu::g_device.CreateShaderModule(&desc);
  }
  return module;
}
} // namespace

std::string build_uber_shader_source(u8 lineMode) noexcept {
  std::string fetch;
  for (std::string_view buf : {"vbuf", "abuf"}) {
    std::string text{FetchTemplate};
    for (size_t at = text.find("BUF"); at != std::string::npos; at = text.find("BUF", at)) {
      text.replace(at, 3, buf);
      at += buf.size();
    }
    fetch += text;
  }
  std::string body{UberBody};
  const auto cfgVec4s = fmt::format("{}", (UberConfigWords + 3) / 4);
  for (size_t at = body.find("CFG_VEC4S"); at != std::string::npos; at = body.find("CFG_VEC4S", at)) {
    body.replace(at, 9, cfgVec4s);
  }
  return fmt::format("const LINE_MODE = {}u;\nconst LINE_STEP = {}u;\n", lineMode, lineMode == 1 ? 2 : 1) +
         wgsl_constants() + std::string{shader_helpers()} + body + fetch;
}

PipelineConfig uber_pipeline_config(const PipelineConfig& config) noexcept {
  PipelineConfig uber = config;
  const u8 lineMode = config.shaderConfig.lineMode;
  uber.shaderConfig = ShaderConfig{};
  uber.shaderConfig.lineMode = lineMode;
  return uber;
}

wgpu::RenderPipeline create_uber_pipeline(const PipelineConfig& config) {
  const auto label = fmt::format("GX Uber Pipeline {:x}", xxh3_hash(config));
  return build_pipeline(config, {}, uber_module(config.shaderConfig.lineMode), label.c_str());
}

void clear_uber_shaders() noexcept {
  std::lock_guard lock{g_uberMutex};
  g_uberModules = {};
}

gfx::Range build_uber_uniform(const ShaderConfig& config, const ShaderInfo& info, u32 vtxStart,
                              const BindGroupRanges& ranges) noexcept {
  auto buf = gfx::begin_uniform(gfx::align_uniform(UberUniformSize));
  if (!buf) {
    g_gxState.stateDirty = false;
    return {};
  }
  // Header, as build_uniform writes it for non-compact matrices.
  buf.append(vtxStart);
  buf.append<u32>(g_gxState.currentPnMtx);
  buf.append<f32>(g_gxState.renderViewport.width);
  buf.append<f32>(g_gxState.renderViewport.height);
  buf.append<f32>(g_gxState.logicalViewport.width);
  buf.append<f32>(g_gxState.logicalViewport.height);
  buf.append_zeroes(8);
  for (const auto& vaRange : ranges.vaRanges) {
    buf.append<u32>(vaRange.offset);
  }
  if (config.lineMode == 3) {
    buf.append<f32>(static_cast<f32>(g_gxState.pointSize) / 6.f);
    buf.append<f32>(1.0f);
    buf.append<f32>(uber_tex_offset(g_gxState.pointTexOffset));
    buf.append<u32>(uber_point_texcoord_mask());
  } else if (config.lineMode != 0) {
    buf.append<f32>(static_cast<f32>(g_gxState.lineWidth) / 6.f);
    buf.append<f32>(g_gxState.lineHalfAspect ? 0.5f : 1.f);
    buf.append<f32>(uber_tex_offset(g_gxState.lineTexOffset));
    buf.append<u32>(uber_line_texcoord_mask());
  } else {
    buf.append_zeroes(16);
  }
  auto proj = viewport_clip_projection(g_gxState.proj);
  if constexpr (UseReversedZ) {
    proj.m2 = proj.m2 * Vec4{-1.f, -1.f, -1.f, -1.f};
  } else {
    proj.m2 = proj.m2 + proj.m3;
  }
  buf.append(proj);
  for (int i = 0; i < MaxPnMtx; i++) {
    buf.append(g_gxState.pnMtx[i].pos);
  }
  for (int i = 0; i < MaxTexMtx; i++) {
    buf.append(g_gxState.texMtxs[i]);
  }
  for (int i = 0; i < MaxPnMtx; i++) {
    buf.append(g_gxState.pnMtx[i].nrm);
  }
  for (int i = 0; i < MaxTevRegs; ++i) {
    buf.append(g_gxState.colorRegs[i]);
  }
  static_assert(sizeof(g_gxState.lights) == 80 * GX::MaxLights);
  buf.append(g_gxState.lights);
  for (int i = 0; i < 4; ++i) {
    buf.append<u32>(g_gxState.colorChannelState[i].lightMask.to_ulong());
  }
  for (int i = 0; i < 4; ++i) {
    buf.append(g_gxState.colorChannelState[i].ambColor);
  }
  for (int i = 0; i < 4; ++i) {
    buf.append(g_gxState.colorChannelState[i].matColor);
  }
  for (int i = 0; i < MaxKColors; ++i) {
    buf.append(g_gxState.kcolors[i]);
  }
  for (int i = 0; i < MaxPTTexMtx; ++i) {
    buf.append(g_gxState.ptTexMtxs[i]);
  }
  {
    const auto& state = g_gxState.fog;
    Fog fog{.color = state.color, .a = state.a, .b = state.b, .c = state.c};
    buf.append(fog);
  }
  for (const auto& scale : g_gxState.texCoordScales) {
    buf.append(Vec4{static_cast<f32>(scale.scaleS) + 1.0f, static_cast<f32>(scale.scaleT) + 1.0f, 0.0f, 0.0f});
  }
  for (int i = 0; i < MaxIndTexMtxs; ++i) {
    const auto& mtx = g_gxState.indTexMtxs[i];
    buf.append(Vec4{mtx.mtx.m0.x, mtx.mtx.m0.y, mtx.mtx.m1.x, mtx.mtx.m1.y});
    buf.append(Vec4{mtx.mtx.m2.x, mtx.mtx.m2.y, std::exp2f(mtx.scaleExp), 0.0f});
  }
  for (int i = 0; i < MaxTextures; ++i) {
    buf.append(uber_texture_size_bias(get_texture(static_cast<GXTexMapID>(i))));
  }
  const auto words = pack_config(config, info);
  for (const u32 word : words) {
    buf.append<u32>(word);
  }
  g_gxState.stateDirty = false;
  return buf.finish();
}
} // namespace aurora::gx
