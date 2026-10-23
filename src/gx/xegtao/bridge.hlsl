// SPDX-License-Identifier: MIT
//
// Offline XeGTAO feasibility bridge.
//
// The implementation below intentionally delegates all AO math, depth MIP
// construction, normal reconstruction, and denoising to the pinned Intel
// XeGTAO headers.  This file supplies only the renderer-independent shader
// interface needed to compile those headers with Slang to WGSL.

#define VA_SATURATE(x) saturate(x)

// WGSL exposes storage textures as f32/i32/u32; the upstream HLSL wrapper
// uses an unorm-qualified float edge UAV.  The bridge preserves the upstream
// packed 8-bit edge values while erasing only that unsupported HLSL storage
// qualifier for WGSL code generation.
#define unorm

// The initial baseline is FP32 and spatial-only.  This avoids a hidden
// precision change while comparing the upstream implementation on WebGPU.
#define XE_GTAO_USE_HALF_FLOAT_PRECISION 0
// The R16 storage build defines MP6_XEGTAO_R16_STORAGE on the compiler
// command line.  Leaving XE_GTAO_FP32_DEPTHS undefined in that build selects
// XeGTAO's upstream FP16-depth bias while all arithmetic remains FP32.
#if !defined(MP6_XEGTAO_R16_STORAGE)
#define XE_GTAO_FP32_DEPTHS 1
#endif
#define XE_GTAO_USE_DEFAULT_CONSTANTS 1
#define XE_GTAO_DEFAULT_THIN_OBJECT_HEURISTIC 0

#include "../../../third_party/xegtao/XeGTAO.h"
#include "../../../third_party/xegtao/XeGTAO.hlsli"

// One constants block and one point-clamp sampler are shared by all logical
// stages.  Binding groups are deliberately stable: group 0 is common state;
// groups 1, 2, and 3 are prefilter, main, and normals/denoise resources.
[[vk::binding(0, 0)]] cbuffer GTAOConstantBuffer
{
    GTAOConstants g_GTAOConsts;
};

[[vk::binding(1, 0)]] SamplerState g_samplerPointClamp;

// Prefilter: source NDC depth -> five view-space depth MIP outputs.
[[vk::binding(0, 1)]] Texture2D<float> g_srcRawDepthPrefilter;
[[vk::binding(1, 1)]] RWTexture2D<float> g_outWorkingDepthMIP0;
[[vk::binding(2, 1)]] RWTexture2D<float> g_outWorkingDepthMIP1;
[[vk::binding(3, 1)]] RWTexture2D<float> g_outWorkingDepthMIP2;
[[vk::binding(4, 1)]] RWTexture2D<float> g_outWorkingDepthMIP3;
[[vk::binding(5, 1)]] RWTexture2D<float> g_outWorkingDepthMIP4;

// Main AO: prepared view-space depth and optional normal input -> AO/edge terms.
[[vk::binding(0, 2)]] Texture2D<float> g_srcWorkingDepthMain;
[[vk::binding(1, 2)]] Texture2D<uint> g_srcNormalmapMain;
[[vk::binding(2, 2)]] Texture2D<uint> g_srcHilbertLUTMain;
[[vk::binding(3, 2)]] RWTexture2D<uint> g_outWorkingAOTerm;
[[vk::binding(4, 2)]] RWTexture2D<float> g_outWorkingEdges;

// Separate normal-generation resource space keeps the binding contract
// explicit when this optional pass is compiled as its own pipeline.
[[vk::binding(0, 3)]] Texture2D<float> g_srcRawDepthNormals;
[[vk::binding(1, 3)]] RWTexture2D<uint> g_outNormalmap;

// Denoise: packed AO/edges -> final packed visibility term.
[[vk::binding(2, 3)]] Texture2D<uint> g_srcWorkingAOTerm;
[[vk::binding(3, 3)]] Texture2D<float> g_srcWorkingEdges;
[[vk::binding(4, 3)]] RWTexture2D<uint> g_outFinalAOTerm;

float3 xegtao_load_normal(int2 pos)
{
    const uint packedInput = g_srcNormalmapMain.Load(int3(pos, 0)).x;
    const float3 encoded = XeGTAO_R11G11B10_UNORM_to_FLOAT3(packedInput);
    return normalize(encoded * 2.0.xxx - 1.0.xxx);
}

float2 xegtao_spatiotemporal_noise(uint2 pixCoord, uint temporalIndex)
{
    // The initial baseline is deliberately temporalIndex == 0.  Hilbert/R2
    // noise is the upstream spatial sequence. The immutable 64x64 table stores
    // the exact upstream HilbertIndex values, avoiding its integer loop per
    // pixel while retaining the same periodic sequence at every resolution.
    uint index = g_srcHilbertLUTMain.Load(int3(pixCoord % XE_HILBERT_WIDTH, 0)).x;
    index += 288 * (temporalIndex % 64);
    return frac(0.5 + index * float2(0.75487766624669276005,
                                    0.5698402909980532659114));
}

[numthreads(8, 8, 1)]
void CSPrefilterDepths16x16(uint2 dispatchThreadID : SV_DispatchThreadID,
                            uint2 groupThreadID : SV_GroupThreadID)
{
    XeGTAO_PrefilterDepths16x16(dispatchThreadID, groupThreadID,
                                g_GTAOConsts, g_srcRawDepthPrefilter,
                                g_samplerPointClamp,
                                g_outWorkingDepthMIP0,
                                g_outWorkingDepthMIP1,
                                g_outWorkingDepthMIP2,
                                g_outWorkingDepthMIP3,
                                g_outWorkingDepthMIP4);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGTAOLow(uint2 pixCoord : SV_DispatchThreadID)
{
    XeGTAO_MainPass(pixCoord, 1, 2,
                    xegtao_spatiotemporal_noise(pixCoord, g_GTAOConsts.NoiseIndex),
                    xegtao_load_normal(int2(pixCoord)), g_GTAOConsts,
                    g_srcWorkingDepthMain, g_samplerPointClamp,
                    g_outWorkingAOTerm, g_outWorkingEdges);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGTAOMedium(uint2 pixCoord : SV_DispatchThreadID)
{
    XeGTAO_MainPass(pixCoord, 2, 2,
                    xegtao_spatiotemporal_noise(pixCoord, g_GTAOConsts.NoiseIndex),
                    xegtao_load_normal(int2(pixCoord)), g_GTAOConsts,
                    g_srcWorkingDepthMain, g_samplerPointClamp,
                    g_outWorkingAOTerm, g_outWorkingEdges);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGTAOHigh(uint2 pixCoord : SV_DispatchThreadID)
{
    XeGTAO_MainPass(pixCoord, 3, 3,
                    xegtao_spatiotemporal_noise(pixCoord, g_GTAOConsts.NoiseIndex),
                    xegtao_load_normal(int2(pixCoord)), g_GTAOConsts,
                    g_srcWorkingDepthMain, g_samplerPointClamp,
                    g_outWorkingAOTerm, g_outWorkingEdges);
}

// Upstream Ultra preset, available to the offline quality harness. Runtime
// admission remains separate from shader availability and requires profiling.
[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGTAOUltra(uint2 pixCoord : SV_DispatchThreadID)
{
    XeGTAO_MainPass(pixCoord, 9, 3,
                    xegtao_spatiotemporal_noise(pixCoord, g_GTAOConsts.NoiseIndex),
                    xegtao_load_normal(int2(pixCoord)), g_GTAOConsts,
                    g_srcWorkingDepthMain, g_samplerPointClamp,
                    g_outWorkingAOTerm, g_outWorkingEdges);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSDenoisePass(uint2 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixCoordBase = dispatchThreadID * uint2(2, 1);
    XeGTAO_Denoise(pixCoordBase, g_GTAOConsts, g_srcWorkingAOTerm,
                   g_srcWorkingEdges, g_samplerPointClamp,
                   g_outFinalAOTerm, false);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSDenoiseLastPass(uint2 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixCoordBase = dispatchThreadID * uint2(2, 1);
    XeGTAO_Denoise(pixCoordBase, g_GTAOConsts, g_srcWorkingAOTerm,
                   g_srcWorkingEdges, g_samplerPointClamp,
                   g_outFinalAOTerm, true);
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)]
void CSGenerateNormals(uint2 pixCoord : SV_DispatchThreadID)
{
    const float3 viewspaceNormal = XeGTAO_ComputeViewspaceNormal(
        pixCoord, g_GTAOConsts, g_srcRawDepthNormals, g_samplerPointClamp);
    g_outNormalmap[pixCoord] = XeGTAO_FLOAT3_to_R11G11B10_UNORM(
        saturate(viewspaceNormal * 0.5 + 0.5));
}
