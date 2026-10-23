"""Stress the real missing-game return with the instruction archive handoff.

Uses real archives, heaps, stub prologs and overlay returns. Only the isolated
bridge driver is instrumented; the production fallback owns the cleanup.
--before-fix removes that cleanup in a generated probe for a negative control.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from build_overlay_probe import build_probe

ROOT = Path(__file__).resolve().parents[2]

DRIVER = r'''
void mp6_dll_bridge_selftest_check_env(void)
{
    static int cursor, visits, pending, archive, finished;
    static OMOVL root = DLL_NONE;
    if (finished || omcurovl == DLL_NONE || omcurovl == DLL_bootdll || fadeStat) return;
    if (root == DLL_NONE) root = omcurovl;
    if (pending) {
        if (!qaArchiveReturned || omcurovl != root) return;
        if (HuDataGetDirPtr(archive) != NULL) {
            fprintf(stderr, "[ARCHIVE-QA] retained archive=%08x visit=%d\n", archive, visits);
            exit(3);
        }
        printf("[ARCHIVE-QA] released visit=%d dvdUsed=%d\n", ++visits,
               HuMemUsedMallocSizeGet(HEAP_DVD));
        fflush(stdout);
        pending = qaArchiveReturned = 0;
        if (visits == 32) {
            printf("[ARCHIVE-QA] PASS distinct_visits=%d\n", visits);
            fflush(stdout);
            finished = 1;
            mp6_max_ticks = (int)mp6_tick_count + 1;
            return;
        }
    }
    for (; (s16)MgDataTbl[cursor].ovl != DLL_NONE; ++cursor) {
        OMOVL overlay = MgDataTbl[cursor].ovl;
        const char *name = mp6_event_ovl_name(overlay);
        char path[96];
        if (!name) continue;
        snprintf(path, sizeof(path), "dll/%s.rel", name);
        if (synth_dll_for_path(path) != MP6_DLL_MINIGAME_STUB) continue;
        g_forcedTestOverlay = overlay;
        archive = MgDataTbl[cursor++].dataDir;
        pending = 1;
        if (!HuPrcChildCreate(mp6_dll_bridge_selftest_child, 0x100, 0x2000, 0,
                             HuPrcCurrentGet())) exit(4);
        return;
    }
    fprintf(stderr, "[ARCHIVE-QA] insufficient unavailable games: %d\n", visits);
    exit(5);
}
'''


def bridge_probe(source):
    source = '#include "game/data.h"\nstatic int qaArchiveReturned;\n' + source
    anchor = '    mp6_unavailable_overlay_return();'
    assert source.count(anchor) == 1
    # Set only our observation flag, before the real return destroys the child.
    source = source.replace(anchor, '    qaArchiveReturned = 1;\n' + anchor)
    anchor = '    mp6_dll_bridge_selftest_teams(g_forcedTestOverlay);'
    preload = r'''
    int game = MgNoGet(g_forcedTestOverlay);
    int status = HuDataDirReadAsync(MgDataTbl[game].dataDir);
    while (!HuDataGetAsyncStat(status)) HuPrcVSleep();
    printf("[ARCHIVE-QA] preload overlay=%d archive=%08x dvdUsed=%d\n",
           g_forcedTestOverlay, MgDataTbl[game].dataDir,
           HuMemUsedMallocSizeGet(HEAP_DVD));
    fflush(stdout);
'''
    assert source.count(anchor) == 1
    source = source.replace(anchor, anchor + preload)
    source = source.replace('mp6_max_ticks = (int)mp6_tick_count + 600;',
                            'mp6_max_ticks = (int)mp6_tick_count + 100000;')
    pattern = r'^void mp6_dll_bridge_selftest_check_env\(void\)\n\{.*?^\}'
    source, count = re.subn(pattern, lambda match: DRIVER.strip(), source, flags=re.M | re.S)
    assert count == 1
    return source


def fallback_probe(source, before_fix):
    source = '#include <stdio.h>\n#include <stdlib.h>\n' + source
    anchor = '    if (minigame >= 0) HuDataDirClose(MgDataTbl[minigame].dataDir);'
    assert source.count(anchor) == 1
    cleanup = '' if before_fix else anchor
    # Check before omOvlReturnEx: mdsel closes all archives during its own
    # setup, which would otherwise conceal the exact leak seen in mgmfree.
    return source.replace(anchor, cleanup + r'''
    if (minigame >= 0 && HuDataGetDirPtr(MgDataTbl[minigame].dataDir) != NULL) {
        fprintf(stderr, "[ARCHIVE-QA] retained before transition archive=%08x\n",
                MgDataTbl[minigame].dataDir);
        exit(3);
    }
''')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before-fix', action='store_true')
    args = parser.parse_args()
    name = 'unavailable-archives-' + ('before' if args.before_fix else 'after')
    instruments = {('os', 'dll_bridge.c'): bridge_probe,
                   ('os', 'overlay_fallback.c'): lambda source: fallback_probe(source, args.before_fix)}
    code = build_probe(name, instruments, jobs=2)
    if code:
        return code
    directory = ROOT / 'build' / name / 'run'
    directory.mkdir(exist_ok=True)
    exe = directory.parent / 'release/mp6native.exe'
    env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}
    env.update(MP6_LAUNCHER='0', MP6_BOOT_TO='mdsel', MP6_TICK_HZ='0',
               MP6_VSYNC='0', MP6_ENH_HEAP_SCALE='4', MP6_WINDOW_HIDDEN='1',
               MP6_ENH_AMBIENT_OCCLUSION='0', MP6_ENH_AA='0',
               MP6_DISC_ROOT=str(ROOT / 'build/disc-cache/orig/GP6E01'),
               SDL_AUDIODRIVER='dummy', SDL_AUDIO_DRIVER='dummy')
    with (directory / 'game.log').open('w') as log:
        result = subprocess.run([str(exe), '200000', '--input-script',
            'period:20;timeout:1000000;pressuntil:start/ovl.start/mdseldll;wait:200000'],
            cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT,
            timeout=180, creationflags=subprocess.CREATE_NO_WINDOW)
    text = (directory / 'game.log').read_text(errors='replace')
    released = re.findall(r'\[ARCHIVE-QA\] released visit=(\d+) dvdUsed=(\d+)', text)
    passed = result.returncode == 0 and '[ARCHIVE-QA] PASS distinct_visits=32' in text
    retained = '[ARCHIVE-QA] retained before transition archive=' in text
    report = dict(passed=passed, exit_code=result.returncode, before_fix=args.before_fix,
                  retained_before_transition=retained,
                  released=released, exe=hashlib.sha256(exe.read_bytes()).hexdigest())
    (directory / 'result.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report))
    print('Evidence:', directory)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
