"""Test one GPU dependency change: immutable frame-buffer uploads before draws.

Private Release sources/archives only. Texture uploads remain at their original
operations. No shader, draw, resolution, game object or production file changes.
MP6_UPLOAD_MODE=0 is control, 1 candidate, 2 same-process A/B/B/A.
"""
import argparse
import difflib
import json
import os
from pathlib import Path
import re
import shutil
import sys

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'fifo_packets'))
from build_renderer import compile_flags, sha, run, write_json, inputs_for_link


def replace(text, old, new):
    assert text.count(old) == 1, old[:100]
    return text.replace(old, new)


def modify_common(text):
    text = '#include "upload_abba.hpp"\n#include <cstdio>\n#include <cstdlib>\n' + text
    text = replace(text, '  wgpu::CommandEncoder encoder;\n',
                   '  wgpu::CommandEncoder encoder;\n  wgpu::CommandEncoder uploadEncoder;\n  bool batchUploads = false;\n')
    text = replace(text, 'static std::array<FramePacket, FrameSlotCount> g_framePackets;', '''static wgpu::CommandBuffer finish_frame_uploads(FramePacket& frame);
static int upload_mode() {
  static const int mode = [] { const char* e = std::getenv("MP6_UPLOAD_MODE"); return e ? std::atoi(e) : 0; }();
  return mode;
}
static mp6_bundle_abba::Window g_uploadWindow;
static void upload_present(uint32_t index, uint64_t draws, int64_t presentNs) {
  if (upload_mode() != 2) return;
  if (g_uploadWindow.observe({index, draws, 0, 0, 0}, presentNs)) {
    std::fprintf(stderr, "[MP6-UPLOAD-ABBA] phase=%u mode=%u valid=%u frames=%u draws=%llu first_ns=%lld last_ns=%lld\\n",
        g_uploadWindow.phase, unsigned(mp6_bundle_abba::enabled(index)), unsigned(g_uploadWindow.valid),
        g_uploadWindow.frames, (unsigned long long)g_uploadWindow.draws,
        (long long)g_uploadWindow.firstNs, (long long)g_uploadWindow.lastNs);
  }
}
static std::array<FramePacket, FrameSlotCount> g_framePackets;''')
    text = replace(text, '  frame.frameIndex = g_frameIndex;', '''  frame.frameIndex = g_frameIndex;
  frame.batchUploads = upload_mode() == 1 ||
      (upload_mode() == 2 && mp6_bundle_abba::enabled(frame.frameIndex));''')
    text = replace(text, '''    g_framePackets[frameSlot].encoder = g_device.CreateCommandEncoder(&EncoderDescriptor);
    webgpu::gpu_prof::frame_begin(g_framePackets[frameSlot].encoder);''', '''    auto& frame = g_framePackets[frameSlot];
    frame.encoder = g_device.CreateCommandEncoder(&EncoderDescriptor);
    if (frame.batchUploads) {
      static constexpr wgpu::CommandEncoderDescriptor UploadDescriptor{.label = "Frame immutable uploads"};
      frame.uploadEncoder = g_device.CreateCommandEncoder(&UploadDescriptor);
    }
    webgpu::gpu_prof::frame_begin(frame.batchUploads ? frame.uploadEncoder : frame.encoder);''')
    text = replace(text, '    g_stagingBuffers[stagingSlot].Unmap();', '''    const auto uploadCommands = finish_frame_uploads(packet);
    const auto uploadFrameIndex = packet.frameIndex;
    g_stagingBuffers[stagingSlot].Unmap();''')
    text = replace(text, '''    if (callback) {
      callback(encoder);
    }
    g_frameSlots.release(frameSlot);''', '''    const auto previousPresent = g_lastPresentNs.load(std::memory_order_acquire);
    if (callback) {
      callback(encoder, uploadCommands);
    }
    const auto present = g_lastPresentNs.load(std::memory_order_acquire);
    upload_present(uploadFrameIndex, stats.drawCallCount, present != previousPresent ? present : 0);
    g_frameSlots.release(frameSlot);''')
    text = replace(text, '''  if (highWater.verts > frame.copied.verts || highWater.uniforms > frame.copied.uniforms ||
      highWater.indices > frame.copied.indices || highWater.storage > frame.copied.storage) {''', '''  if (!frame.batchUploads && (highWater.verts > frame.copied.verts || highWater.uniforms > frame.copied.uniforms ||
      highWater.indices > frame.copied.indices || highWater.storage > frame.copied.storage)) {''')
    block = '''  copy_staging_buffer_range(cmd, frame, frame.copied.verts, highWater.verts, VertexStagingOffset, g_vertexBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.uniforms, highWater.uniforms, UniformStagingOffset,
                            g_uniformBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.indices, highWater.indices, IndexStagingOffset, g_indexBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.storage, highWater.storage, StorageStagingOffset, g_storageBuffer);'''
    text = replace(text, block, '  if (!frame.batchUploads) {\n' + block + '\n  }')
    anchor = 'static bool needs_staging_copy(const FramePacket& frame, const FrameOp& op) {'
    text = replace(text, anchor, '''static wgpu::CommandBuffer finish_frame_uploads(FramePacket& frame) {
  if (!frame.batchUploads) return {};
  auto& cmd = frame.uploadEncoder;
  {
    const webgpu::gpu_prof::Zone zone{cmd, "Frame buffer uploads"};
    // These four arenas are append-only snapshots. They are never GPU-written.
    // One prelude replaces repeated whole-buffer read/write dependencies between
    // render passes. Texture uploads are deliberately NOT moved: repeated image
    // updates may be consumed by earlier draws in this same frame.
    copy_staging_buffer_range(cmd, frame, frame.copied.verts, frame.stats.lastVertSize, VertexStagingOffset, g_vertexBuffer);
    copy_staging_buffer_range(cmd, frame, frame.copied.uniforms, frame.stats.lastUniformSize, UniformStagingOffset, g_uniformBuffer);
    copy_staging_buffer_range(cmd, frame, frame.copied.indices, frame.stats.lastIndexSize, IndexStagingOffset, g_indexBuffer);
    copy_staging_buffer_range(cmd, frame, frame.copied.storage, frame.stats.lastStorageSize, StorageStagingOffset, g_storageBuffer);
  }
  return cmd.Finish();
}

''' + anchor)
    return text


def modify_core(text):
    text = replace(text, 'imguiDrawData = std::move(imguiDrawData)](wgpu::CommandEncoder& encoder) {',
                   'imguiDrawData = std::move(imguiDrawData)](wgpu::CommandEncoder& encoder, const wgpu::CommandBuffer& uploads) {')
    text = replace(text, '      g_queue.Submit(1, &buffer);', '''      if (uploads) {
        const std::array buffers{uploads, buffer};
        g_queue.Submit(buffers.size(), buffers.data());
      } else {
        g_queue.Submit(1, &buffer);
      }''')
    return text


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('platform', choices=('windows', 'android'))
    p.add_argument('--revision', default='r1')
    p.add_argument('--short-phases', action='store_true')
    p.add_argument('--validation', action='store_true')
    a = p.parse_args()
    assert re.fullmatch('r[0-9]+', a.revision)
    windows = a.platform == 'windows'
    source = ROOT/'build'/('aurora-release-source' if windows else 'android-aurora-source')
    tree = ROOT/'build'/('aurora-release' if windows else 'android-aurora')
    prior = ROOT/'build/fifo-packets-20260912'/a.platform
    proof = json.loads((prior/'provenance.json').read_text())
    dest = ROOT/'build/upload-batch-20260912'/(a.platform+'-'+a.revision)
    assert not dest.exists(), 'preserve prior evidence'
    watched = {str(path): sha(path) for path in inputs_for_link(proof['link'])}
    for path, digest in proof['production_unchanged'].items():
        assert sha(path) == digest, path
        watched[path] = digest
    watched[str(tree/'libaurora_core.a')] = sha(tree/'libaurora_core.a')
    watched[str(tree/'libaurora_gx.a')] = sha(tree/'libaurora_gx.a')
    for top in ('lib','include'):
        shutil.copytree(source/top, dest/'source'/top)
    changes = {
        'lib/gfx/common.cpp': modify_common,
        'lib/aurora.cpp': modify_core,
        'lib/gfx/common.hpp': lambda s: replace(s,
            'using EndFrameCallback = std::function<void(wgpu::CommandEncoder&)>;',
            'using EndFrameCallback = std::function<void(wgpu::CommandEncoder&, const wgpu::CommandBuffer&)>;'),
    }
    if a.validation:
        changes['lib/webgpu/gpu.cpp']=lambda s: replace(s,'        "skip_validation",\n','')
    patches = []
    for relative, modify in changes.items():
        original = (source/relative).read_text()
        modified = modify(original)
        (dest/'source'/relative).write_text(modified)
        patches.append(''.join(difflib.unified_diff(original.splitlines(True), modified.splitlines(True),
                         fromfile='a/'+relative, tofile='b/'+relative)))
    timing=(HERE.parent/'render_bundles/abba.hpp').read_text()
    if a.short_phases:
        timing=replace(timing,'PhaseFrames = 240, SettleFrames = 24','PhaseFrames = 120, SettleFrames = 12')
    (dest/'source/lib/gfx/upload_abba.hpp').write_text(timing)
    (dest/'candidate.patch').write_text(''.join(patches))
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT/'build/release-toolchain/zig-cache'),
               ZIG_LOCAL_CACHE_DIR=str(ROOT/'build/release-toolchain/zig-cache'))
    driver = proof['compile'][0][:4 if windows else 2]
    archive_driver = [driver[0], 'ar'] if windows else [str(Path(driver[0]).parent/'llvm-ar.exe')]
    archives = {}
    commands = []
    units=[('lib/gfx/common.cpp','gx'), ('lib/aurora.cpp','core')]
    if a.validation: units.append(('lib/webgpu/gpu.cpp','core'))
    for relative, library in units:
        ninja = (tree/'build.ninja').read_text().replace('CMakeFiles/aurora_core.dir/', 'CMakeFiles/aurora_gx.dir/')
        flags = [f.replace(source.as_posix(), (dest/'source').as_posix()) for f in compile_flags(ninja, source/relative)]
        obj = dest/(Path(relative).name + ('.obj' if windows else '.o'))
        command = [*driver, *flags, '-c', str(dest/'source'/relative), '-o', str(obj)]
        result = run(command, cwd=tree, env=env)
        (dest/(Path(relative).name+'-compile.log')).write_text(result.stdout+result.stderr)
        commands.append(command)
        archive = dest/('libaurora_'+library+'.a')
        if not archive.exists(): shutil.copy2(tree/archive.name, archive)
        assert run([*archive_driver,'t',archive]).stdout.splitlines().count(obj.name) == 1
        run([*archive_driver,'r',archive,obj])
        archives[library] = archive
    link = [str(archives['gx']) if Path(x) == prior/'libaurora_gx.a' else
            str(archives['core']) if Path(x) == tree/'libaurora_core.a' else x for x in proof['link']]
    assert str(archives['gx']) in link and str(archives['core']) in link
    output = dest/('release/mp6native.exe' if windows else 'libmp6game.so')
    output.parent.mkdir(exist_ok=True)
    link[link.index('-o')+1] = str(output)
    rsp = dest/'link.rsp'
    rsp.write_text('\n'.join('"'+x.replace('\\','\\\\').replace('"','\\"')+'"' for x in (link if windows else link[1:]))+'\n')
    result = run([driver[0], *(['c++'] if windows else []), '@'+str(rsp)], cwd=ROOT, env=env)
    (dest/'link.log').write_text(result.stdout+result.stderr)
    artifacts = {str(f):sha(f) for f in [*archives.values(), output]}
    if windows:
        for dll in (prior/'release').glob('*.dll'): shutil.copy2(dll, output.parent/dll.name)
    else:
        stripped = dest/'stripped/libmp6game.so'
        stripped.parent.mkdir()
        run([Path(driver[0]).parent/'llvm-strip.exe','--strip-unneeded','-o',stripped,output])
        artifacts[str(stripped)] = sha(stripped)
    for path,digest in watched.items(): assert sha(path) == digest, path
    write_json(dest/'provenance.json', dict(experimental_only=True, adopted=False,
        artifacts=artifacts, production_unchanged=watched, compile=commands, link=link,
        patch_sha256=sha(dest/'candidate.patch'), authored_sha256=sha(__file__),
        timing_sha256=sha(dest/'source/lib/gfx/upload_abba.hpp'), short_phases=a.short_phases, validation=a.validation,
        android_fps_gain_proven=False))
    print(a.platform+': private upload-batch Release candidate linked; production unchanged.')


if __name__ == '__main__':
    main()
