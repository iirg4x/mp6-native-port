"""Release engine benchmark: fixed initial/board RNG seeds, no capsule fixtures.

Only benchmark executables receive the seed and frame-phase counters. Reference
source overrides are restricted to an explicit frozen directory under build/.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
os.environ.setdefault('MP6_DISC_ROOT','build/disc-cache/orig/GP6E01')
os.environ.setdefault('MP6_DECOMP_INC_DATA','build/disc-cache/split/include')
sys.path.insert(0,str(ROOT/'tools'))
import build

parser=argparse.ArgumentParser()
parser.add_argument('--name',required=True,choices=('baseline','current'))
parser.add_argument('--reference-source',type=Path)
parser.add_argument('--renderer-checkpoint',type=Path,
    help='Isolated benchmark only: audited pre-change renderer archives under build/checkpoints')
parser.add_argument('--upload-stats',action='store_true')
parser.add_argument('--math-profile',action='store_true',
    help='Sample matrix/vector wrapper costs in a separate profiling executable')
parser.add_argument('--gpu-profile',action='store_true',
    help='Read the final completed GPU timing window in an isolated executable')
parser.add_argument('--frame-profile',action='store_true',
    help='Isolated renderer CPU waits/workload inventory plus GPU pass timings')
args=parser.parse_args()
if sum((args.math_profile,args.gpu_profile,args.frame_profile))>1:
    parser.error('choose one profiling mode per run')
if args.frame_profile and (args.renderer_checkpoint or args.reference_source):
    parser.error('frame profiling requires current verified sources')
reference=None
if args.reference_source:
    reference=(ROOT/args.reference_source).resolve(strict=True)
    if not reference.is_relative_to((ROOT/'build').resolve()):
        parser.error('reference must be a frozen source directory under build/')
OUTPUT=ROOT/'build/engine-benchmark'/(args.name+('-math' if args.math_profile else '-gpu' if args.gpu_profile else '-frame' if args.frame_profile else ''))
OUTPUT.mkdir(parents=True,exist_ok=True)
original_configure=build.configure_windows
original_collect=build.collect_units
original_resolve=build._resolve_aurora_link_items
original_require=build._require_aurora_artifact_stamp
checkpoint=None
profile_renderer=None
if args.renderer_checkpoint:
    checkpoint=(ROOT/args.renderer_checkpoint).resolve(strict=True)
    if not checkpoint.is_relative_to((ROOT/'build/checkpoints').resolve()):
        parser.error('renderer checkpoint must be under build/checkpoints/')
    audit=json.loads((checkpoint/'audit.json').read_text())


def checkpoint_archive(name):
    return name in ('libaurora_gx.a','libaurora_core.a','libaurora_mtx.a') and checkpoint and (checkpoint/name).is_file()


def resolve_renderer():
    global profile_renderer
    items=original_resolve()
    if args.frame_profile:
        if profile_renderer is None:
            original_require('windows',items)
            from frame_profile_source import build_renderer
            profile_renderer=build_renderer(ROOT,OUTPUT,build.ZIG)
        return [profile_renderer.get(Path(item).name,item) for item in items]
    if not checkpoint:
        return items
    return [(checkpoint/Path(item).name).as_posix()
            if checkpoint_archive(Path(item).name) else item for item in items]


def require_renderer(profile,items):
    # Retain all normal current-source, toolchain and artifact checks. An old
    # renderer is admitted only by its separately recorded full artifact hashes.
    original_require(profile,original_resolve())
    if checkpoint:
        assert profile=='windows' and items==resolve_renderer()
        for record in audit['stamp']['profiles']['windows']:
            original=Path(record['path'])
            path=checkpoint/original.name if checkpoint_archive(original.name) else original
            assert path.stat().st_size==record['bytes']
            assert hashlib.sha256(path.read_bytes()).hexdigest()==record['sha256'],str(path)
        assert build._ar_has_section(str(checkpoint/'libaurora_gx.a'),'.mp6hbss')


def configure(configuration):
    original_configure(configuration)
    build.BUILD_DIR=str(OUTPUT)
    build.OBJ_DIR=str(OUTPUT/'obj')


def collect(*a,**kw):
    if args.frame_profile:
        resolve_renderer()
    units=[]
    manifest={}
    for source,flags,name,flavor in original_collect(*a,**kw):
        # A changed recipe must never rebuild a benchmark unit into the player's
        # release-object directory, even when its source needs no instrumentation.
        obj=OUTPUT/'obj'/name
        relative=Path(source).relative_to(ROOT).as_posix()
        board_source = relative.endswith('/w01Dll/world01.c')
        if board_source or name in ('plat_aurora_bridge_aurora.o','plat_gxarray_registry_aurora.o','game_frand.o'):
            selected=reference/relative if reference and name!='game_frand.o' and (reference/relative).is_file() else Path(source)
            text=selected.read_text(encoding='utf-8')
            manifest[relative]=hashlib.sha256(text.encode()).hexdigest()
            if name=='game_frand.o':
                assert text.count('seed = OSGetTime();')==1
                text=text.replace('seed = OSGetTime();','seed = 0x12345678u; /* benchmark seed only */')
                text += '\nvoid mp6_benchmark_seed_board(void) { frand_seed = 0xA341316Cu; }\n'
            elif board_source:
                # Intro duration can consume different numbers of random values
                # before board creation. Reset only in isolated benchmark code,
                # before W01 initializes its falling leaves and other scenery.
                marker='void MB1_Create(void)\n{'
                assert text.count(marker)==1
                text=text.replace(marker,marker+'\n    extern void mp6_benchmark_seed_board(void);\n    mp6_benchmark_seed_board();')
            elif name=='plat_aurora_bridge_aurora.o':
                if args.frame_profile:
                    from frame_profile_source import replace_once
                    text=replace_once(text,'void VIWaitForRetrace(void)\n{',
                        'void VIWaitForRetrace(void)\n{\n    extern void mp6_frame_profile_set_age(unsigned);\n    extern unsigned mp6_diag_board_ticks(void);\n    mp6_frame_profile_set_age(mp6_diag_board_ticks());')
                marker='    if (tEntry > 0) g_phSamples++;'
                assert text.count(marker)==1
                text=text.replace(marker,marker+'''
    extern unsigned mp6_diag_board_ticks(void);
    unsigned qaBoardAge=mp6_diag_board_ticks();
    if (qaBoardAge>=1800 && qaBoardAge<5800) {
        static double total[5]; static unsigned count;
        total[0]+=(double)(tEntry-tPrevReturn)/1e6;
        total[1]+=(double)(tEndFrame-tEntry)/1e6;
        total[2]+=(double)(tSeal-tEndFrame)/1e6;
        total[3]+=(double)(g_phLastReturnNs-tThrottleOut)/1e6;
        total[4]+=(double)(g_phLastReturnNs-tPrevReturn)/1e6;
        if (++count==4000) { fprintf(stderr,
            "[ENGINE-BENCH] ticks=%u game=%.6f submit=%.6f seal=%.6f post=%.6f frame=%.6f\\n",
            count,total[0]/count,total[1]/count,total[2]/count,total[3]/count,total[4]/count);
            mp6_max_ticks=mp6_tick_count+60; }
    }
''')
                if args.upload_stats:
                    text='#include <stdbool.h>\n#include <aurora/gfx.h>\n'+text
                    marker='        static double total[5]; static unsigned count;'
                    assert text.count(marker)==1
                    text=text.replace(marker,marker+'''
        static double bufferTotal[4]; static double drawTotal[2]; static double textureTotal;
        const AuroraStats* uploadStats=aurora_get_stats();
        bufferTotal[0]+=uploadStats->lastVertSize;
        bufferTotal[1]+=uploadStats->lastIndexSize;
        bufferTotal[2]+=uploadStats->lastUniformSize;
        bufferTotal[3]+=uploadStats->lastStorageSize;
        textureTotal+=uploadStats->lastTextureUploadSize;
        drawTotal[0]+=uploadStats->drawCallCount;
        drawTotal[1]+=uploadStats->mergedDrawCallCount;
''')
                    marker='            mp6_max_ticks=mp6_tick_count+60; }'
                    assert text.count(marker)==1
                    text=text.replace(marker,'''            fprintf(stderr,"[ENGINE-UPLOAD] vertex=%.0f index=%.0f uniform=%.0f storage=%.0f bytes/frame\\n",
                bufferTotal[0]/count,bufferTotal[1]/count,bufferTotal[2]/count,bufferTotal[3]/count);
            fprintf(stderr,"[ENGINE-TEXTURES] samples=%u mean_upload_bytes=%.3f\\n",count,textureTotal/count);
            fprintf(stderr,"[ENGINE-DRAWS] submitted=%.3f merged=%.3f/frame\\n",drawTotal[0]/count,drawTotal[1]/count);
'''+marker)
            if args.math_profile and name=='plat_aurora_bridge_aurora.o':
                from math_profile_source import instrument
                text=instrument(text)
            if (args.gpu_profile or args.frame_profile) and name=='plat_aurora_bridge_aurora.o':
                marker='            mp6_max_ticks=mp6_tick_count+60; }'
                assert text.count(marker)==1
                text=text.replace(marker,'''
            AuroraGpuStats benchGpu;
            aurora_gpu_stats_read(&benchGpu);
            fprintf(stderr,"[ENGINE-GPU] status=%u samples=%u dropped=%u span=%.6f between=%.6f\\n",
                benchGpu.status,benchGpu.sampleCount,benchGpu.droppedFrames,
                benchGpu.spanAverageMs,benchGpu.betweenAverageMs);
            for (unsigned g=0;g<benchGpu.rowCount && g<AURORA_GPU_STAT_ROWS;++g)
                fprintf(stderr,"[ENGINE-GPU-PASS] %s avg=%.6f max=%.6f\\n",
                    benchGpu.rows[g].name,benchGpu.rows[g].averageMs,benchGpu.rows[g].maxMs);
'''+marker)
            target=OUTPUT/(name.removesuffix('.o')+'.c')
            target.write_text(text,encoding='utf-8')
            flags=[*flags,'-iquote',str(Path(source).parent)]
            source=str(target)
            obj=OUTPUT/'obj'/name
        elif reference and (reference/relative).is_file():
            # An audit checkpoint may contain only the changed port units.
            # Build those separately, never overwrite production objects or
            # combine a new AO API caller with the pre-change renderer archive.
            selected=reference/relative
            text=selected.read_text(encoding='utf-8')
            manifest[relative]=hashlib.sha256(text.encode()).hexdigest()
            target=OUTPUT/'reference'/relative
            target.parent.mkdir(parents=True,exist_ok=True)
            target.write_text(text,encoding='utf-8')
            flags=[*flags,'-iquote',str(Path(source).parent)]
            source=str(target)
            obj=OUTPUT/'obj'/name
        if args.frame_profile and name=='plat_ambient_occlusion_aurora.o':
            from frame_profile_source import instrument_ao
            text=Path(source).read_text(encoding='utf-8')
            manifest[relative]=hashlib.sha256(text.encode()).hexdigest()
            target=OUTPUT/'ambient_occlusion.cpp'
            target.write_text(instrument_ao(text),encoding='utf-8')
            flags=[*flags,'-iquote',str(Path(source).parent),'-I',str(OUTPUT/'frame-profile-renderer')]
            source=str(target)
            obj=OUTPUT/'obj'/name
        units.append((source,flags,str(obj),flavor))
    (OUTPUT/'inputs.json').write_text(json.dumps(manifest,indent=2))
    (OUTPUT/'renderer-inputs.json').write_text(json.dumps({str(path):hashlib.sha256(Path(path).read_bytes()).hexdigest()
        for path in resolve_renderer() if Path(path).name in ('libaurora_gx.a','libaurora_core.a','libaurora_mtx.a')},indent=2))
    return units


build.configure_windows=configure
build.collect_units=collect
build._resolve_aurora_link_items=resolve_renderer
build._require_aurora_artifact_stamp=require_renderer
original_subprocess_run = subprocess.run


def run_benchmark_command(command, *a, **kw):
    # Fully isolated object paths can exceed Windows' process-command limit.
    # Only redirect this benchmark's final link, never a compile or external tool.
    if (isinstance(command, list) and command[:2] == [build.ZIG, 'c++']
            and '-c' not in command and '-o' in command):
        target = Path(command[command.index('-o') + 1]).resolve()
        if target.name == 'mp6native.exe' and target.is_relative_to(OUTPUT.resolve()):
            response = OUTPUT/'link.rsp'
            response.write_text('\n'.join('"'+arg.replace('\\','\\\\').replace('"','\\"')+'"'
                                          for arg in command[2:])+'\n')
            return original_subprocess_run([command[0], command[1], '@'+str(response)], *a, **kw)
    return original_subprocess_run(command, *a, **kw)


subprocess.run = run_benchmark_command
sys.argv=['tools/build.py','--configuration','release','-j6']
try:
    raise SystemExit(build.main())
finally:
    subprocess.run = original_subprocess_run
