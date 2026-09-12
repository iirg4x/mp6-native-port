"""Test-only full-frame workload and CPU backpressure instrumentation.

The renderer and game source are transformed into build/ outputs, never edited.
This deliberately adds profiling overhead; its timings are not release FPS.
"""
from pathlib import Path
import hashlib
import json
import re
import shlex
import shutil
import subprocess

HEADER = r'''
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <array>
#include <cstring>
namespace mp6_frame_profile {
inline std::atomic<unsigned> age{0};
// The GX command processor and AO admission run only on the game thread.
struct FrontCounts { uint64_t draws=0,depthWrites=0,perspectiveDepth=0,orthographicDepth=0; };
inline FrontCounts frontCounts;
inline uint64_t now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
enum class FrontStage { Fifo, Draw, PipelineConfig, PipelineLookup, TextureResolve, Bindings, Uniforms, Storage, ArrayBindings, CommandRecord, Count };
constexpr size_t FrontStageCount=size_t(FrontStage::Count);
inline constexpr std::array<const char*,FrontStageCount> frontNames{
    "FIFO", "Draw preparation", "Pipeline config", "Pipeline lookup",
    "Texture resolve", "Texture bindings", "Uniform packing", "Storage copy",
    "Array bindings", "Command recording"};
struct FrontStats {
    std::array<uint64_t,FrontStageCount> inclusive{},exclusive{},calls{};
    std::array<uint64_t,5> cpWrites{},cpRepeats{};
    uint64_t textureLoads=0,textureRepeats=0,paletteLoads=0,paletteRepeats=0;
    uint64_t depthRequests=0,depthColorOnlyCandidates=0;
};
inline thread_local FrontStats frontStats;
inline thread_local bool frontEnabled=false;
inline constexpr std::array<const char*,5> cpNames{"VCD low","VCD high","VAT A","VAT B","VAT C"};
// Census only: always decode the production write. Track the warm register
// state even outside the timing window; no memory/array/matrix commands qualify.
inline std::array<uint32_t,256> cpValues{};
inline std::array<bool,256> cpValid{};
inline std::array<std::array<uint8_t,34>,8> textureMetadata{};
inline std::array<std::array<uint8_t,23>,256> paletteMetadata{};
inline std::array<bool,8> textureMetadataValid{};
inline std::array<bool,256> paletteMetadataValid{};
inline void observe_texture_metadata(const uint8_t* data,bool palette) {
    const auto slot=data[0];
    if(!palette && slot>=textureMetadata.size()) return;
    auto* cached=palette ? paletteMetadata[slot].data() : textureMetadata[slot].data();
    const size_t bytes=palette ? 23 : 34;
    auto& valid=palette ? paletteMetadataValid[slot] : textureMetadataValid[slot];
    const bool same=valid && std::memcmp(cached,data,bytes)==0;
    if(frontEnabled) {
        if(palette) { ++frontStats.paletteLoads; frontStats.paletteRepeats+=same; }
        else { ++frontStats.textureLoads; frontStats.textureRepeats+=same; }
    }
    std::memcpy(cached,data,bytes); valid=true;
}
inline void observe_cp(uint8_t addr,uint32_t value) {
    size_t slot;
    if(addr==0x50) slot=0;
    else if(addr==0x60) slot=1;
    else if(addr>=0x70 && addr<=0x77) slot=2;
    else if(addr>=0x80 && addr<=0x87) slot=3;
    else if(addr>=0x90 && addr<=0x97) slot=4;
    else return;
    if(frontEnabled) {
        ++frontStats.cpWrites[slot];
        if(cpValid[addr] && cpValues[addr]==value) ++frontStats.cpRepeats[slot];
    }
    cpValues[addr]=value;cpValid[addr]=true;
}
inline thread_local std::array<unsigned,FrontStageCount> frontDepth{};
struct FrontTimer;
inline thread_local FrontTimer* frontTop=nullptr;
struct FrontTimer {
    size_t slot;
    uint64_t start=0,children=0;
    FrontTimer* parent=nullptr;
    explicit FrontTimer(FrontStage stage):slot(size_t(stage)) {
        if(!frontEnabled) return;
        start=now();parent=frontTop;frontTop=this;
        ++frontStats.calls[slot];++frontDepth[slot];
    }
    ~FrontTimer() {
        if(!start) return;
        const auto elapsed=now()-start;
        frontStats.exclusive[slot]+=elapsed-children;
        if(--frontDepth[slot]==0) frontStats.inclusive[slot]+=elapsed;
        if(parent) parent->children+=elapsed;
        frontTop=parent;
    }
};
struct Stats {
    unsigned boardAge=0;
    bool enabled=false;
    uint64_t frameAdmission=0, stagingAdmission=0, encode=0, acquire=0, finish=0, submit=0, present=0;
    uint64_t queueDelayMax=0, copies=0, copyBytes=0, passes=0, draws=0, vertices=0, indices=0, drawElements=0;
    std::vector<std::string> inventory;
    FrontStats front;
};
inline thread_local Stats* current=nullptr;
struct Context {
    Stats* prior;
    explicit Context(Stats& s):prior(current) { current=s.enabled ? &s : nullptr; }
    ~Context() { current=prior; }
};
struct Timer {
    uint64_t Stats::*field;
    uint64_t start;
    explicit Timer(uint64_t Stats::*f):field(f),start(current ? now() : 0) {}
    ~Timer() { if(start && current) current->*field += now()-start; }
};
inline void draw(uint64_t vertices,uint64_t indices,uint64_t instances) {
    if(!current) return;
    ++current->draws;
    current->vertices+=vertices;
    current->indices+=indices;
    current->drawElements+=(indices ? indices : vertices)*instances;
}
inline void copy(uint64_t bytes) {
    if(!current) return;
    ++current->copies;
    current->copyBytes+=bytes;
}
inline void complete(const Stats& s) {
    if(!s.enabled) return;
    // Only the render worker calls this, after this frame's present callback.
    static unsigned frames=0;
    static Stats sum{};
    ++frames;
    #define MP6_SUM(field) sum.field+=s.field
    MP6_SUM(frameAdmission); MP6_SUM(stagingAdmission); MP6_SUM(encode);
    MP6_SUM(acquire); MP6_SUM(finish); MP6_SUM(submit); MP6_SUM(present);
    MP6_SUM(queueDelayMax); MP6_SUM(copies); MP6_SUM(copyBytes); MP6_SUM(passes);
    MP6_SUM(draws); MP6_SUM(vertices); MP6_SUM(indices); MP6_SUM(drawElements);
    for(size_t i=0;i<FrontStageCount;++i) {
        sum.front.inclusive[i]+=s.front.inclusive[i];
        sum.front.exclusive[i]+=s.front.exclusive[i];
        sum.front.calls[i]+=s.front.calls[i];
    }
    for(size_t i=0;i<cpNames.size();++i) {
        sum.front.cpWrites[i]+=s.front.cpWrites[i];
        sum.front.cpRepeats[i]+=s.front.cpRepeats[i];
    }
    sum.front.textureLoads+=s.front.textureLoads;
    sum.front.textureRepeats+=s.front.textureRepeats;
    sum.front.paletteLoads+=s.front.paletteLoads;
    sum.front.paletteRepeats+=s.front.paletteRepeats;
    sum.front.depthRequests+=s.front.depthRequests;
    sum.front.depthColorOnlyCandidates+=s.front.depthColorOnlyCandidates;
    #undef MP6_SUM
    if(s.boardAge!=5799) return;
    const double ms=1e-6/frames, perFrame=1.0/frames;
    std::fprintf(stderr,"[FRAME-PROFILE] frames=%u frameAdmission=%.6f stagingAdmission=%.6f encode=%.6f acquire=%.6f finish=%.6f submit=%.6f present=%.6f queueDelayMax=%.6f\n",
        frames,sum.frameAdmission*ms,sum.stagingAdmission*ms,sum.encode*ms,sum.acquire*ms,
        sum.finish*ms,sum.submit*ms,sum.present*ms,sum.queueDelayMax*ms);
    std::fprintf(stderr,"[FRAME-WORK] passes=%.3f gxDraws=%.3f sourceVertices=%.3f indices=%.3f drawElements=%.3f copies=%.3f copyBytes=%.3f\n",
        sum.passes*perFrame,sum.draws*perFrame,sum.vertices*perFrame,sum.indices*perFrame,
        sum.drawElements*perFrame,sum.copies*perFrame,sum.copyBytes*perFrame);
    for(const auto& line:s.inventory) std::fprintf(stderr,"[FRAME-PASS] %s\n",line.c_str());
    for(size_t i=0;i<FrontStageCount;++i)
        std::fprintf(stderr,"[FRAME-FRONT] stage=%s inclusive=%.6f exclusive=%.6f calls=%.3f\n",
            frontNames[i],sum.front.inclusive[i]*ms,sum.front.exclusive[i]*ms,sum.front.calls[i]*perFrame);
    for(size_t i=0;i<cpNames.size();++i)
        std::fprintf(stderr,"[FRAME-CP] register=%s writes=%.3f repeats=%.3f\n",
            cpNames[i],sum.front.cpWrites[i]*perFrame,sum.front.cpRepeats[i]*perFrame);
    std::fprintf(stderr,"[FRAME-TEXTURE-LOADS] texture=%.3f identical=%.3f palette=%.3f identicalPalette=%.3f\n",
        sum.front.textureLoads*perFrame,sum.front.textureRepeats*perFrame,
        sum.front.paletteLoads*perFrame,sum.front.paletteRepeats*perFrame);
    std::fprintf(stderr,"[FRAME-DEPTH-DEMAND] requests=%.3f colorOnlyCandidates=%.3f\n",
        sum.front.depthRequests*perFrame,sum.front.depthColorOnlyCandidates*perFrame);
}
}
'''

def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'expected one source anchor: {old[:100]!r}, got {text.count(old)}')
    return text.replace(old,new)

def instrument_common(text):
    text='#include "mp6_frame_profile.hpp"\n'+text
    text=replace_once(text,'  wgpu::TextureView unchangedDepthSnapshot;',
        '  wgpu::TextureView unchangedDepthSnapshot;\n  uint64_t profileDepthSerial=0;')
    text=replace_once(text,'  const bool copyDepth = wantDepth && !reuseDepth;', '''
  // Census only: a potential GX-color-only segment is not permission to reuse
  // depth across arbitrary custom draws. Production decisions remain intact.
  if (mp6_frame_profile::frontEnabled && wantDepth && !g_inOffscreen) {
    ++mp6_frame_profile::frontStats.depthRequests;
    if (!reuseDepth && !prevPass.clearDepth && prevPass.unchangedDepthSnapshot &&
        prevPass.profileDepthSerial == gx::fifo::efb_depth_write_serial())
      ++mp6_frame_profile::frontStats.depthColorOnlyCandidates;
  }
  const bool copyDepth = wantDepth && !reuseDepth;''')
    text=replace_once(text,'      prevPass.snapshotDepthDst = entry.depth.view;',
        '      prevPass.snapshotDepthDst = entry.depth.view;\n'
        '      prevPass.profileDepthSerial = gx::fifo::efb_depth_write_serial();')
    resume='static void resume_efb_pass_loading(const RenderPass& prevPass) {'
    start=text.index(resume)
    text=text[:start]+replace_once(text[start:],
        '  current_render_passes().emplace_back(std::move(newPass));',
        '  newPass.profileDepthSerial=prevPass.profileDepthSerial;\n'
        '  current_render_passes().emplace_back(std::move(newPass));')
    text=replace_once(text, 'void push_draw_command(gx::DrawData data) {',
        'void push_draw_command(gx::DrawData data) {\n'
        '  mp6_frame_profile::FrontTimer timer(mp6_frame_profile::FrontStage::CommandRecord);')
    text=replace_once(text, '  AuroraStats stats{};\n};',
        '  AuroraStats stats{};\n  mp6_frame_profile::Stats profile;\n};')
    text=replace_once(text, '  const auto opIndex = frame.nextOpIndex++;',
        '  const auto opIndex = frame.nextOpIndex++;\n  const auto profileEnqueued=frame.profile.enabled ? mp6_frame_profile::now() : 0;')
    text=replace_once(text, '[frameSlot, op = std::move(op)] {', '[frameSlot, op = std::move(op), profileEnqueued] {')
    text=replace_once(text, '    encode_op(packet.encoder, packet, op);', '''
    mp6_frame_profile::Context context(packet.profile);
    if(profileEnqueued) packet.profile.queueDelayMax=std::max(packet.profile.queueDelayMax,mp6_frame_profile::now()-profileEnqueued);
    mp6_frame_profile::Timer profileEncode(&mp6_frame_profile::Stats::encode);
    encode_op(packet.encoder, packet, op);''')
    text=replace_once(text, 'bool begin_frame() {\n  ZoneScoped;', '''bool begin_frame() {
  ZoneScoped;
  mp6_frame_profile::Stats profile{};
  profile.boardAge=mp6_frame_profile::age.load(std::memory_order_relaxed);
  profile.enabled=profile.boardAge>=1800 && profile.boardAge<5800;
  mp6_frame_profile::Context profileContext(profile);''')
    text=replace_once(text, '    frameSlot = acquire_frame_slot();\n    stagingSlot = acquire_mapped_staging_buffer();', '''
    { mp6_frame_profile::Timer timer(&mp6_frame_profile::Stats::frameAdmission); frameSlot = acquire_frame_slot(); }
    { mp6_frame_profile::Timer timer(&mp6_frame_profile::Stats::stagingAdmission); stagingSlot = acquire_mapped_staging_buffer(); }''')
    text=replace_once(text, '  frame.stagingBuffer = *stagingSlot;',
        '  frame.stagingBuffer = *stagingSlot;\n  frame.profile=std::move(profile);\n'
        '  mp6_frame_profile::frontStats={};\n  mp6_frame_profile::frontEnabled=frame.profile.enabled;')
    text=replace_once(text, '  frame.stats.drawCallCount = g_drawCallCount;',
        '  frame.profile.front=mp6_frame_profile::frontStats;\n  mp6_frame_profile::frontEnabled=false;\n'
        '  frame.stats.drawCallCount = g_drawCallCount;')
    text=replace_once(text, '    const auto stats = packet.stats;\n    recycle_frame_packet(packet);', '''
    const auto stats = packet.stats;
    auto profile=std::move(packet.profile);
    mp6_frame_profile::Context profileContext(profile);
    recycle_frame_packet(packet);''')
    completion = '    expire_cached_bind_groups();\n    map_staging_buffer(stagingSlot, true);'
    text=replace_once(text, completion, '    mp6_frame_profile::complete(profile);\n'+completion)
    text=replace_once(text, '  copied = highWater;',
        '  copied = highWater;\n  mp6_frame_profile::copy(copyEnd-copyStart);')
    text=replace_once(text, '        gx::render(draw.gx, pass, gxState);',
        '        mp6_frame_profile::draw(draw.gx.vtxCount,draw.gx.indexCount,draw.gx.instanceCount);\n        gx::render(draw.gx, pass, gxState);')
    text=replace_once(text, '    auto pass = cmd.BeginRenderPass(&renderPassDescriptor);\n    render_pass(pass, frame, passInfo);', '''
    if(mp6_frame_profile::current) {
      ++mp6_frame_profile::current->passes;
      if(frame.profile.boardAge==5799) {
        unsigned gxDraws=0; uint64_t vertices=0,indices=0;
        for(const auto& command:passInfo.commands) if(command.type==CommandType::Draw && command.data.draw.type==ShaderType::GX) {
          ++gxDraws; vertices+=command.data.draw.gx.vtxCount; indices+=command.data.draw.gx.indexCount;
        }
        frame.profile.inventory.push_back(fmt::format("index={} label={} size={}x{} samples={} colorLoad={} depthLoad={} draws={} vertices={} indices={} colorSnapshot={} depthSnapshot={}",
          passIndex,passInfo.label,passInfo.targetSize.width,passInfo.targetSize.height,passInfo.msaaSamples,
          int(attachments[0].loadOp),int(depthStencilAttachment.depthLoadOp),gxDraws,vertices,indices,
          bool(passInfo.snapshotColorDst),bool(passInfo.snapshotDepthDst)));
      }
    }
    auto pass = cmd.BeginRenderPass(&renderPassDescriptor);
    render_pass(pass, frame, passInfo);''')
    text+='\nextern "C" void mp6_frame_profile_set_age(unsigned age) { mp6_frame_profile::age.store(age,std::memory_order_relaxed); }\n'
    return text

def instrument_aurora(text):
    text='#include "mp6_frame_profile.hpp"\n'+text
    text=replace_once(text, '        g_surface.GetCurrentTexture(&surfaceTexture);',
        '        { mp6_frame_profile::Timer timer(&mp6_frame_profile::Stats::acquire); g_surface.GetCurrentTexture(&surfaceTexture); }')
    text=replace_once(text, '    const auto buffer = encoder.Finish(&cmdBufDescriptor);',
        '    wgpu::CommandBuffer buffer;\n    { mp6_frame_profile::Timer timer(&mp6_frame_profile::Stats::finish); buffer = encoder.Finish(&cmdBufDescriptor); }')
    text=replace_once(text, '      g_queue.Submit(1, &buffer);',
        '      { mp6_frame_profile::Timer timer(&mp6_frame_profile::Stats::submit); g_queue.Submit(1, &buffer); }')
    text=replace_once(text, '          status = g_surface.Present();',
        '          { mp6_frame_profile::Timer timer(&mp6_frame_profile::Stats::present); status = g_surface.Present(); }')
    return text

def instrument_ao(text):
    text='#include "mp6_frame_profile.hpp"\n'+text
    text=replace_once(text, '    Params params{};', '    Params params{};\n    int profileCamera=-1;\n    unsigned profileBoardAge=0;')
    text=replace_once(text, '    job->frame = g_frame;',
        '    job->frame = g_frame;\n    job->profileCamera=view->camera;\n'
        '    job->profileBoardAge=mp6_frame_profile::age.load(std::memory_order_relaxed);')
    # Dimensions now come from the ordered task's real attachment, not from
    # an earlier game-thread snapshot. Log them after worker initialization.
    text=replace_once(text, '    job.params.sizes[3] = float(aoHeight);', '''
    job.params.sizes[3] = float(aoHeight);
    if(job.profileBoardAge==5799) {
      const auto& p=job.params;
      std::fprintf(stderr,"[FRAME-AO] camera=%d full=%.0fx%.0f ao=%.0fx%.0f near=%.3f far=%.3f tanHalfFov=%.6f viewport=%.3f,%.3f,%.3f,%.3f decals=%d foliage=%d\\n",
        job.profileCamera,p.sizes[0],p.sizes[1],p.sizes[2],p.sizes[3],p.projection[2],p.projection[3],p.projection[0],
        p.viewport[0],p.viewport[1],p.viewport[2],p.viewport[3],bool(job.decalBefore && job.decalAfter),bool(job.foliage));
    }''')
    text=replace_once(text, '    const auto pipelines = job.preparedDepth ? g_preparedPipelines :', '''
    const std::array<std::string,3> profileNames={std::string("GTAO camera ")+std::to_string(job.profileCamera),
      std::string("AO blur X camera ")+std::to_string(job.profileCamera),std::string("AO blur Y camera ")+std::to_string(job.profileCamera)};
    const auto pipelines = job.preparedDepth ? g_preparedPipelines :''')
    text=replace_once(text, 'gfx::gpu_pass_timestamps(i == 0 ? "GTAO" : (i == 1 ? "AO blur X" : "AO blur Y"))',
        'gfx::gpu_pass_timestamps(profileNames[i].c_str())')
    text=replace_once(text, '    if (!gfx::push_encoder_task(g_taskType, &job, sizeof(job))) {', '''
    if(mp6_frame_profile::age.load(std::memory_order_relaxed)==5799) {
      const auto& c=mp6_frame_profile::frontCounts;
      std::fprintf(stderr,"[FRAME-AO-DRAWS] camera=%d draws=%llu depthWrites=%llu perspectiveDepth=%llu orthographicDepth=%llu\\n",
        view->camera,(unsigned long long)c.draws,(unsigned long long)c.depthWrites,(unsigned long long)c.perspectiveDepth,(unsigned long long)c.orthographicDepth);
    }
    mp6_frame_profile::frontCounts={};
    if (!gfx::push_encoder_task(g_taskType, &job, sizeof(job))) {''')
    return text

def instrument_processor(text):
    text='#include "mp6_frame_profile.hpp"\n'+text
    for label,palette in (('TEXOBJ','false'),('TLUT','true')):
        anchor = '    CHECK(pos + '+('34' if palette=='false' else '23')+' <= size, "GX_AURORA_LOAD_'+label+' read overrun");'
        text=replace_once(text,anchor,anchor+'\n    mp6_frame_profile::observe_texture_metadata(data + pos,'+palette+');')
    text=replace_once(text, 'static void handle_cp(u8 addr, u32 value, bool bigEndian) {',
        'static void handle_cp(u8 addr, u32 value, bool bigEndian) {\n'
        '  mp6_frame_profile::observe_cp(addr,value);')
    text=replace_once(text, 'void process(const u8* data, u32 size, bool bigEndian) {',
        'void process(const u8* data, u32 size, bool bigEndian) {\n'
        '  mp6_frame_profile::FrontTimer timer(mp6_frame_profile::FrontStage::Fifo);')
    text=replace_once(text, '  // Build pipeline, bind groups, and push draw command',
        '  mp6_frame_profile::FrontTimer timer(mp6_frame_profile::FrontStage::Draw);\n'
        '  // Build pipeline, bind groups, and push draw command')
    text=replace_once(text, '      const auto range = gfx::push_storage(static_cast<const uint8_t*>(array.data), array.size);',
        '      mp6_frame_profile::FrontTimer storageTimer(mp6_frame_profile::FrontStage::Storage);\n'
        '      const auto range = gfx::push_storage(static_cast<const uint8_t*>(array.data), array.size);')
    text=replace_once(text, '  BindGroupRanges ranges{};',
        '  BindGroupRanges ranges{};\n  { mp6_frame_profile::FrontTimer arrayTimer(mp6_frame_profile::FrontStage::ArrayBindings);')
    text=replace_once(text, '  PipelineConfig config{};\n  populate_pipeline_config(config, prim, fmt);',
        '  }\n  PipelineConfig config{};\n  populate_pipeline_config(config, prim, fmt);')
    return replace_once(text,'  // Build pipeline, bind groups, and push draw command','''
  ++mp6_frame_profile::frontCounts.draws;
  if(g_gxState.depthCompare && g_gxState.depthUpdate) {
    ++mp6_frame_profile::frontCounts.depthWrites;
    if(g_gxState.projType==GX_PERSPECTIVE) ++mp6_frame_profile::frontCounts.perspectiveDepth;
    else ++mp6_frame_profile::frontCounts.orthographicDepth;
  }
  // Build pipeline, bind groups, and push draw command''')

def front_function(text, signature, stage):
    return replace_once(text,signature,signature+'\n  mp6_frame_profile::FrontTimer timer(mp6_frame_profile::FrontStage::'+stage+');')

def instrument_gx(text):
    text='#include "mp6_frame_profile.hpp"\n'+text
    for signature,stage in (
        ('void populate_pipeline_config(PipelineConfig& config, GXPrimitive primitive, GXVtxFmt fmt) noexcept {','PipelineConfig'),
        ('void resolve_sampled_textures(const ShaderInfo& info) noexcept {','TextureResolve'),
        ('GXBindGroups build_bind_groups(const ShaderInfo& info) noexcept {','Bindings')):
        text=front_function(text,signature,stage)
    return text

def instrument_shader_info(text):
    return front_function('#include "mp6_frame_profile.hpp"\n'+text,
        'gfx::Range build_uniform(const ShaderInfo& info, u32 vtxStart, const BindGroupRanges& ranges) noexcept {','Uniforms')

def instrument_pipeline_cache(text):
    return front_function('#include "mp6_frame_profile.hpp"\n'+text,
        'PipelineRef find_gx_pipeline(const gx::PipelineConfig& config, gx::ShaderInfo& info) {','PipelineLookup')

TRANSFORMS = {'lib/gfx/common.cpp': ('libaurora_gx.a',instrument_common),
              'lib/gx/command_processor.cpp': ('libaurora_gx.a',instrument_processor),
              'lib/gx/gx.cpp': ('libaurora_gx.a',instrument_gx),
              'lib/gx/shader_info.cpp': ('libaurora_gx.a',instrument_shader_info),
              'lib/gfx/pipeline_cache.cpp': ('libaurora_gx.a',instrument_pipeline_cache),
              'lib/aurora.cpp': ('libaurora_core.a',instrument_aurora)}

def build_renderer(root, output, zig):
    """Copy two verified Release archives; replace only their instrumented TUs."""
    source = root / 'build/aurora-release-source'
    tree = root / 'build/aurora-release'
    output = output / 'frame-profile-renderer'
    output.mkdir(parents=True,exist_ok=True)
    header = output / 'mp6_frame_profile.hpp'
    header.write_text(HEADER)
    ninja = (tree / 'build.ninja').read_text()
    originals = {name: hashlib.sha256((tree/name).read_bytes()).hexdigest() for name,_ in TRANSFORMS.values()}
    for name in originals:
        shutil.copy2(tree/name,output/name)
    manifest = {'original_archives': originals, 'sources': {}, 'commands': []}
    for relative, (archive, transform) in TRANSFORMS.items():
        original = source / relative
        target = output / Path(relative).name
        text = original.read_text()
        manifest['sources'][relative] = hashlib.sha256(text.encode()).hexdigest()
        target.write_text(transform(text))
        # Read the configured compiler flags, not a hand-made approximation of
        # Release. Ninja's $ escapes are not expected in this workspace path.
        needle = original.as_posix().replace(':','$:')
        target_prefix='CMakeFiles/'+archive.removeprefix('lib').removesuffix('.a')+'.dir/'
        blocks = [m.group(1) for m in re.finditer(r'^build '+re.escape(target_prefix)+r'[^\n]* '+re.escape(needle)+r' [^\n]*\n((?:  [^\n]*\n)+)',ninja,re.M)]
        if len(blocks)!=1: raise ValueError(f'expected configured compilation for {relative}')
        flags = []
        for key in ('DEFINES','FLAGS','INCLUDES'):
            rows=re.findall(r'^  '+key+r' = (.*)$',blocks[0],re.M)
            if len(rows)!=1: raise ValueError(f'missing {key}: {relative}')
            flags += shlex.split(rows[0])
        if '-O2' not in flags or '-DNDEBUG' not in flags:
            raise ValueError('profiling requires optimized Release flags')
        obj = output / (Path(relative).name+'.obj')
        command=[str(zig),'c++','-target','x86_64-windows-gnu',*flags,
                 '-iquote',str(original.parent),'-I',str(output),'-c',str(target),'-o',str(obj)]
        manifest['commands'].append(command)
        subprocess.run(command,cwd=tree,check=True)
        members=subprocess.run([str(zig),'ar','t',str(output/archive)],capture_output=True,text=True,check=True).stdout.splitlines()
        if members.count(obj.name)!=1: raise ValueError(f'ambiguous archive member {obj.name}')
        subprocess.run([str(zig),'ar','r',str(output/archive),str(obj)],check=True)
    for name,digest in originals.items():
        if hashlib.sha256((tree/name).read_bytes()).hexdigest()!=digest:
            raise ValueError('production archive changed during isolated build')
    manifest['profile_archives']={name:hashlib.sha256((output/name).read_bytes()).hexdigest() for name in originals}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return {name:(output/name).as_posix() for name in originals}
