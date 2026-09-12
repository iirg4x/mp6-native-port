"""Private S22 test of the already quality-tested fused AO denoiser.

Stock renderer, except frame-level A/B/B/A accounting and the denoise dispatch.
No upload-batching experiment, source edits, staging or APK distribution.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE.parent/'fifo_packets'))
from build_renderer import compile_flags,sha,run,write_json,inputs_for_link


def replace(text,old,new):
    assert text.count(old)==1,old[:100]
    return text.replace(old,new)


def ao_source(text):
    text='extern "C" bool mp6_denoise_candidate_frame();\n'+text
    text=replace(text,'std::array<wgpu::RenderPipeline, 3> g_preparedPipelines;', '''std::array<wgpu::RenderPipeline, 3> g_preparedPipelines;
wgpu::BindGroupLayout g_tiledLayout;
std::array<wgpu::ComputePipeline,2> g_tiledPipelines;''')
    anchor='void encode(const gfx::EncoderTaskContext &ctx, const wgpu::CommandEncoder &encoder,'
    helper='''void tiled_denoise(const gfx::EncoderTaskContext &ctx, const wgpu::CommandEncoder &encoder, Job &job) {
    if (!g_tiledLayout) {
        std::array<wgpu::BindGroupLayoutEntry,4> entries{};
        entries[0].binding=0; entries[0].visibility=wgpu::ShaderStage::Compute;
        entries[0].buffer.type=wgpu::BufferBindingType::Uniform;
        entries[0].buffer.minBindingSize=sizeof(Params);
        for (unsigned i=1;i<3;++i) {
            entries[i].binding=i; entries[i].visibility=wgpu::ShaderStage::Compute;
            entries[i].texture.sampleType=wgpu::TextureSampleType::UnfilterableFloat;
            entries[i].texture.viewDimension=wgpu::TextureViewDimension::e2D;
        }
        entries[3].binding=6; entries[3].visibility=wgpu::ShaderStage::Compute;
        entries[3].storageTexture.access=wgpu::StorageTextureAccess::WriteOnly;
        entries[3].storageTexture.format=wgpu::TextureFormat::RGBA16Float;
        entries[3].storageTexture.viewDimension=wgpu::TextureViewDimension::e2D;
        const wgpu::BindGroupLayoutDescriptor desc{.entryCount=entries.size(),.entries=entries.data()};
        g_tiledLayout=ctx.device.CreateBindGroupLayout(&desc);
    }
    auto &pipeline=g_tiledPipelines[job.preparedDepth];
    if (!pipeline) {
        const wgpu::PipelineLayoutDescriptor ld{.bindGroupLayoutCount=1,.bindGroupLayouts=&g_tiledLayout};
        auto layout=ctx.device.CreatePipelineLayout(&ld);
        const wgpu::ComputePipelineDescriptor desc{.label="MP6 fused denoise diagnostic",.layout=layout,
            .compute={.module=job.preparedDepth ? g_preparedShader : g_shader,.entryPoint="cs_denoise"}};
        pipeline=ctx.device.CreateComputePipeline(&desc);
    }
    const std::array entries{
        wgpu::BindGroupEntry{.binding=0,.buffer=job.uniform,.size=sizeof(Params)},
        wgpu::BindGroupEntry{.binding=1,.textureView=job.depth},
        wgpu::BindGroupEntry{.binding=2,.textureView=job.ao[0]},
        wgpu::BindGroupEntry{.binding=6,.textureView=job.ao[1]},
    };
    const wgpu::BindGroupDescriptor bindingsDesc{.layout=g_tiledLayout,.entryCount=entries.size(),.entries=entries.data()};
    auto bindings=gfx::cached_bind_group(bindingsDesc);
    const wgpu::ComputePassDescriptor desc{.label="MP6 tiled AO denoise",
#ifdef AURORA_GPU_STAT_ROWS
        .timestampWrites=gfx::gpu_pass_timestamps("AO tiled denoise"),
#endif
    };
    auto pass=encoder.BeginComputePass(&desc);
    pass.SetPipeline(pipeline); pass.SetBindGroup(0,bindings);
    pass.DispatchWorkgroups((job.width+15)/16,(job.height+7)/8);
    pass.End();
}

'''
    text=replace(text,anchor,helper+anchor)
    text=replace(text,'.label = "MP6 AO visibility and normal",\n            .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,',
        '.label = "MP6 AO visibility and normal",\n            .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::StorageBinding,')
    text=replace(text,'    for (size_t i = 0; i < pipelines.size(); ++i) {', '''    const bool tiled=mp6_denoise_candidate_frame();
    for (size_t i = 0; i < pipelines.size(); ++i) {
        if (i==1 && tiled) { tiled_denoise(ctx,encoder,job); break; }''')
    text=replace(text,'    job.composite = make_bind_group(ctx.device, job, job.ao[0], true);',
        '    job.composite = make_bind_group(ctx.device, job, job.ao[tiled ? 1 : 0], true);')
    text=replace(text,'    g_shader = nullptr;', '''    g_shader = nullptr;
    g_tiledLayout=nullptr;
    for (auto &pipeline:g_tiledPipelines) pipeline=nullptr;''')
    return text


def common_source(text):
    text='#include "denoise_abba.hpp"\n#include <cstdio>\n#include <cstdlib>\n'+text
    text=replace(text,'static std::array<FramePacket, FrameSlotCount> g_framePackets;', '''static int denoise_mode() {
  static const int mode=[] { const char* v=std::getenv("MP6_DENOISE_MODE"); return v ? std::atoi(v) : 0; }();
  return mode;
}
static bool g_denoiseFrame=false;
extern "C" bool mp6_denoise_candidate_frame() { return g_denoiseFrame; }
static mp6_bundle_abba::Window g_denoiseWindow;
static void denoise_present(uint32_t index,uint64_t draws,int64_t presentNs) {
  if (denoise_mode()!=2) return;
  if (g_denoiseWindow.observe({index,draws,0,0,0},presentNs))
    std::fprintf(stderr,"[MP6-DENOISE-ABBA] phase=%u mode=%u valid=%u frames=%u draws=%llu first_ns=%lld last_ns=%lld\\n",
      g_denoiseWindow.phase,unsigned(mp6_bundle_abba::enabled(index)),unsigned(g_denoiseWindow.valid),
      g_denoiseWindow.frames,(unsigned long long)g_denoiseWindow.draws,
      (long long)g_denoiseWindow.firstNs,(long long)g_denoiseWindow.lastNs);
}
static std::array<FramePacket, FrameSlotCount> g_framePackets;''')
    text=replace(text,'static void encode_op(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {', '''static void encode_op(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {
  g_denoiseFrame=denoise_mode()==1 || (denoise_mode()==2 && mp6_bundle_abba::enabled(frame.frameIndex));''')
    text=replace(text,'    const auto stats = packet.stats;',
        '    const auto stats = packet.stats;\n    const auto denoiseFrameIndex = packet.frameIndex;')
    text=replace(text,'''    if (callback) {
      callback(encoder);
    }
    g_frameSlots.release(frameSlot);''', '''    const auto beforePresent=g_lastPresentNs.load(std::memory_order_acquire);
    if (callback) callback(encoder);
    const auto afterPresent=g_lastPresentNs.load(std::memory_order_acquire);
    denoise_present(denoiseFrameIndex,stats.drawCallCount,afterPresent!=beforePresent ? afterPresent : 0);
    g_frameSlots.release(frameSlot);''')
    return text


def main():
    dest=ROOT/'build/ao-denoise-20260912/android-r5'
    assert not dest.exists()
    source=ROOT/'build/android-aurora-source'; tree=ROOT/'build/android-aurora'
    prior=ROOT/'build/fifo-packets-20260912/android'
    proof=json.loads((prior/'provenance.json').read_text())
    watched=dict(proof['production_unchanged'])
    for path,digest in watched.items(): assert sha(path)==digest,path
    for path in inputs_for_link(proof['link']): watched[str(path)]=sha(path)
    for top in ('lib','include'): shutil.copytree(source/top,dest/'source'/top)
    relative='lib/gfx/common.cpp'
    (dest/'source'/relative).write_text(common_source((source/relative).read_text()))
    timing=(HERE.parent/'render_bundles/abba.hpp').read_text().replace('PhaseFrames = 240, SettleFrames = 24','PhaseFrames = 120, SettleFrames = 12')
    (dest/'source/lib/gfx/denoise_abba.hpp').write_text(timing)
    driver=proof['compile'][0][:2]
    flags=[x.replace(source.as_posix(),(dest/'source').as_posix()) for x in compile_flags((tree/'build.ninja').read_text(),source/relative)]
    obj=dest/'common.cpp.o'
    command=[*driver,*flags,'-c',str(dest/'source'/relative),'-o',str(obj)]
    result=run(command,cwd=tree); (dest/'common-compile.log').write_text(result.stdout+result.stderr)
    archive=dest/'libaurora_gx.a'; shutil.copy2(tree/archive.name,archive)
    run([Path(driver[0]).parent/'llvm-ar.exe','r',archive,obj])
    original_obj=ROOT/'build/android/obj/plat_ambient_occlusion_aurora.o'
    record=json.loads(Path(str(original_obj)+'.cmd.json').read_text())
    ao=ROOT/'src/gx/ambient_occlusion.cpp'; private_ao=dest/ao.name
    private_ao.write_text(ao_source(ao.read_text()))
    compile_ao=list(record['command'])
    compile_ao[compile_ao.index('-c')+1]=str(private_ao)
    compile_ao[compile_ao.index('-o')+1]=str(dest/original_obj.name)
    if '-MF' in compile_ao: compile_ao[compile_ao.index('-MF')+1]=str(dest/'ao.d')
    compile_ao+=['-iquote',str(ao.parent)]
    try: result=run(compile_ao,cwd=ROOT)
    except subprocess.CalledProcessError as error:
        (dest/'ao-compile.log').write_text(error.stdout+error.stderr)
        raise RuntimeError(error.stderr) from error
    (dest/'ao-compile.log').write_text(result.stdout+result.stderr)
    link=[str(archive) if Path(x)==prior/archive.name else str(dest/original_obj.name) if Path(x)==original_obj else x for x in proof['link']]
    assert str(archive) in link and str(dest/original_obj.name) in link
    native=dest/'libmp6game.so'; link[link.index('-o')+1]=str(native)
    rsp=dest/'link.rsp'; rsp.write_text('\n'.join('"'+x.replace('\\','\\\\').replace('"','\\"')+'"' for x in link[1:])+'\n')
    result=run([driver[0],'@'+str(rsp)],cwd=ROOT); (dest/'link.log').write_text(result.stdout+result.stderr)
    stripped=dest/'stripped/libmp6game.so'; stripped.parent.mkdir()
    run([Path(driver[0]).parent/'llvm-strip.exe','--strip-unneeded','-o',stripped,native])
    for path,digest in watched.items(): assert sha(path)==digest,path
    write_json(dest/'provenance.json',dict(experimental_only=True,adopted=False,short_phases=True,
        artifacts={str(f):sha(f) for f in (archive,native,stripped)},production_unchanged=watched,
        compile=[command,compile_ao],link=link,ao_source_sha256=sha(private_ao),authored_sha256=sha(__file__)))
    print('Private AO fused-denoise Release test linked; production unchanged.')


if __name__=='__main__': main()
