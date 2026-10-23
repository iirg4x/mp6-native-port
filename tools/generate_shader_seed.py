#!/usr/bin/env python3
"""Generate the bundled shader (pipeline) seed: res/initial_pipeline_cache.db.

Plays every player-facing module of the game in the native build -- the boot
and menus, every board, every minigame and mode -- each in a hidden window
with scripted input, and records every pipeline each scene draws. Aurora's
unified pipeline cache (third_party/aurora, docs/SHADER_CACHE.md) writes
those recordings itself; this tool only drives the runs and merges their
caches into one seed that every platform ships (Windows next to the exe,
Android as an APK asset, the web in its preloaded res/ tree), so a player's
first visit to a scene prewarms its shaders while it loads.

  python tools/generate_shader_seed.py                  # everything, 4 at once
  python tools/generate_shader_seed.py --scenes w01dll m602dll --jobs 2
  python tools/generate_shader_seed.py --merge-only     # re-merge earlier runs

Recording uses 4x MSAA so draws to the main render target are told apart
from offscreen ones; the cache replays those at whatever anti-aliasing the
player uses.

What a scripted computer party reaches by chance is forced instead. Every
scene is recorded once per character set (CHARACTER_SETS, MP6_TEST_CHARS), so
every playable character's materials are seen in every scene, and boards run
with MP6_TEST_CAPSULE_SWEEP: each computer turn uses the next capsule, so
every capsule effect is recorded (the Metal Mushroom's metal characters, the
creatures and objects the capsules bring in), on a different character in
each set. Runs are independent: re-recording some scenes and merging again
is fine. Needs a Windows release build (tools/build.py --configuration release).
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build' / 'shader-seed'
SEED = ROOT / 'res' / 'initial_pipeline_cache.db'
SCHEMA = 1  # Aurora's PipelineCacheSchema

# Not player scenes: REL sources that aren't overlays, and developer viewers.
NOT_SCENES = {'framework', 'executor.c', 'runtime.c', 'empty.c', 'bootdll',
              'actmandll', 'meschkdll', 'motchkdll'}
# The plain boot (title, file select, main menu) is recorded as its own run.
BOOT_RUN = 'boot'
# Every playable character (charman.h CHARNO_MARIO..CHARNO_KINOPICO), four
# per run, so each scene sees all of them.
CHARACTER_SETS = ('0,1,2,3', '4,5,6,7', '8,9,10,0')


def scene_names(decomp):
    rel = Path(decomp) / 'src' / 'REL'
    return sorted(p.name.lower() for p in rel.iterdir() if p.is_dir() and p.name.lower() not in NOT_SCENES)


# Boards (and their story/solo stages) run turns of computer players; give
# them longer than a menu or a single minigame.
BOARD = re.compile(r'^(w\d\d|s\d\d)dll$')


def runs_for(scene):
    """(scene, character set index) for every recorded run of a scene."""
    if scene == BOOT_RUN:
        return [(scene, 0)]
    return [(scene, i) for i in range(len(CHARACTER_SETS))]


def run_name(run):
    scene, charset = run
    return scene if scene == BOOT_RUN else f'{scene}-chars{charset}'


def run_scene(exe, disc, run, cache, seconds):
    """One hidden, scripted run. Returns (run name, seconds, last scene line)."""
    scene, charset = run
    name = run_name(run)
    logs = OUT / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith('MP6_')}
    env.update(MP6_LAUNCHER='0', MP6_BACKGROUND_QA='1', MP6_DISC_ROOT=disc,
               MP6_GPU_CACHE_PATH=str(cache), MP6_TICK_HZ='0',  # unlocked: more play per second
               MP6_ENH_AA='1',  # 4x MSAA: main-target draws recorded as such
               MP6_ENH_WIDESCREEN='1', MP6_WIDESCREEN='1',
               MP6_ENH_HEAP_SCALE='4',  # forced loads of boards need the bigger heap
               SDL_AUDIODRIVER='dummy', SDL_AUDIO_DRIVER='dummy')
    if scene == BOOT_RUN:
        # Title, file select and the menus, pressing A through them.
        script = 'period:30;timeout:1000000;pressuntil:a/qa.never/done;'
    else:
        env.update(MP6_BOOT_TO='mdsel', MP6_TEST_LOAD_DLL=scene,
                   MP6_TEST_LOAD_DLL_HOLD='1',  # stay in the scene until the wall timeout
                   MP6_TEST_CHARS=CHARACTER_SETS[charset])
        if BOARD.match(scene):
            # Each computer turn uses the next capsule; each character set
            # starts the rotation elsewhere, so every capsule lands on a
            # different character in each run.
            env['MP6_TEST_CAPSULE_SWEEP'] = str(charset * 9)
        if scene == 'mdseldll':
            env.pop('MP6_TEST_LOAD_DLL')
        # Reach the scene, then keep pressing A (menus, instructions, results).
        script = ('period:20;timeout:1000000;pressuntil:start/ovl.start/' + scene + ';'
                  'period:30;timeout:1000000;pressuntil:a/qa.never/done;')
    # Runs end by being killed at the wall timeout (or a game crash), which
    # can leave Aurora's prewarm crash sentinel behind; it would make the next
    # run in this slot discard everything the slot recorded so far.
    for sentinel in Path(cache).rglob('pipeline_prewarm.sentinel'):
        sentinel.unlink()
    start = time.time()
    with open(logs / (name + '.log'), 'w', encoding='utf-8', errors='replace') as log:
        # Below normal priority: the recording shares the PC with other work.
        flags = getattr(subprocess, 'BELOW_NORMAL_PRIORITY_CLASS', 0)
        proc = subprocess.Popen([str(exe), '100000000', '--input-script', script], cwd=exe.parent, env=env,
                                stdout=log, stderr=subprocess.STDOUT, creationflags=flags)
        try:
            # Boards: a 20-turn party long enough for the capsule sweep.
            proc.wait(timeout=seconds * (6 if BOARD.match(scene) else 1))
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    text = (logs / (name + '.log')).read_text(encoding='utf-8', errors='replace')
    lines = [l.split('] ', 1)[-1] for l in text.splitlines() if 'Pipeline scene' in l]
    return name, time.time() - start, lines[-1] if lines else 'no scene line'


def record(args, scenes):
    exe = Path(args.exe).resolve()
    if not exe.is_file():
        print('missing', exe, '(build with tools/build.py --configuration release)')
        return 1
    # One cache per job slot: a slot's runs share Dawn's compiled shaders, so
    # pipelines common to many scenes compile once per slot.
    slots = [OUT / 'cache' / f'slot{i}' for i in range(args.jobs)]
    for slot in slots:
        slot.mkdir(parents=True, exist_ok=True)
    free = list(slots)
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        pending = {}
        done_file = OUT / 'done.txt'
        finished_runs = set(done_file.read_text().split()) if args.resume and done_file.exists() else set()
        queue = [run for scene in scenes for run in runs_for(scene) if run_name(run) not in finished_runs]
        total = len(queue)
        while queue or pending:
            while queue and free:
                slot = free.pop()
                run = queue.pop(0)
                pending[pool.submit(run_scene, exe, args.disc, run, slot, args.seconds)] = slot
            finished, _ = concurrent.futures.wait(pending, return_when=concurrent.futures.FIRST_COMPLETED)
            for future in finished:
                free.append(pending.pop(future))
                scene, seconds, line = future.result()
                with open(OUT / 'done.txt', 'a') as done_log:
                    done_log.write(scene + '\n')
                done += 1
                print(f'[{done}/{total}] {scene}: {seconds:.0f}s, {line}', flush=True)
    return 0


def coverage():
    """What the recorded runs reached, from their logs: per scene, the
    character sets it was recorded with and, for boards, the capsules used
    and how many the game lists. Stored in the seed for
    tests/test_shader_seed_coverage.py."""
    result = {}
    for log in sorted((OUT / 'logs').glob('*.log')):
        m = re.match(r'^(.*)-chars(\d+)$', log.stem)
        scene, charset = (m.group(1), int(m.group(2))) if m else (log.stem, 0)
        text = log.read_text(encoding='utf-8', errors='replace')
        entry = result.setdefault(scene, {'charsets': [], 'capsules': [], 'listed': 0})
        if 'Pipeline scene' in text and charset not in entry['charsets']:
            entry['charsets'].append(charset)
        for capsule, listed in re.findall(r'\[CAPSULE-SWEEP\] turn \d+ player \d+ holds capsule (\d+) \((\d+) listed\)', text):
            if int(capsule) not in entry['capsules']:
                entry['capsules'].append(int(capsule))
            entry['listed'] = int(listed)
    for entry in result.values():
        entry['charsets'].sort()
        entry['capsules'].sort()
    return result


def merge(out):
    sources = sorted((OUT / 'cache').rglob('pipeline_cache.db'))
    if not sources:
        print('nothing recorded under', OUT / 'cache')
        return 1
    tmp = out.with_suffix('.tmp')
    tmp.unlink(missing_ok=True)
    con = sqlite3.connect(tmp)
    con.executescript(f'''
        CREATE TABLE aurora_schema(value INTEGER);
        INSERT INTO aurora_schema VALUES ({SCHEMA});
        CREATE TABLE pipeline_cache (
          type INTEGER NOT NULL, hash INTEGER NOT NULL, config_version INTEGER NOT NULL,
          config_size INTEGER NOT NULL, config BLOB NOT NULL, first_frame_used INTEGER NOT NULL,
          PRIMARY KEY (type, hash));
        CREATE INDEX pipeline_cache_load_order_idx ON pipeline_cache(type, config_version, first_frame_used);
        CREATE TABLE pipeline_scene_v2 (
          scene INTEGER NOT NULL, type INTEGER NOT NULL, hash INTEGER NOT NULL,
          first_use INTEGER NOT NULL, main_target INTEGER NOT NULL,
          PRIMARY KEY (scene, type, hash));
        CREATE TABLE aurora_meta (key TEXT PRIMARY KEY NOT NULL, value TEXT NOT NULL);
    ''')
    for src in sources:
        con.execute('ATTACH DATABASE ? AS rec', (str(src),))
        con.execute('''INSERT INTO pipeline_cache SELECT type, hash, config_version, config_size, config,
                         first_frame_used FROM rec.pipeline_cache WHERE true
                       ON CONFLICT(type, hash) DO UPDATE SET
                         first_frame_used = MIN(first_frame_used, excluded.first_frame_used)''')
        con.execute('''INSERT INTO pipeline_scene_v2 SELECT scene, type, hash, first_use, main_target
                         FROM rec.pipeline_scene_v2 WHERE true
                       ON CONFLICT(scene, type, hash) DO UPDATE SET
                         first_use = MIN(first_use, excluded.first_use),
                         main_target = MAX(main_target, excluded.main_target)''')
        con.commit()
        con.execute('DETACH DATABASE rec')
    # Only pipelines some scene uses; only scene entries whose config exists.
    con.execute('''DELETE FROM pipeline_cache WHERE NOT EXISTS (SELECT 1 FROM pipeline_scene_v2 s
                     WHERE s.type = pipeline_cache.type AND s.hash = pipeline_cache.hash)''')
    con.execute('''DELETE FROM pipeline_scene_v2 WHERE NOT EXISTS (SELECT 1 FROM pipeline_cache c
                     WHERE c.type = pipeline_scene_v2.type AND c.hash = pipeline_scene_v2.hash)''')
    # Configs are stored zlib-compressed (Aurora decodes a blob whose size
    # differs from config_size); recordings by older builds stored them raw.
    raw = con.execute('SELECT rowid, config FROM pipeline_cache WHERE length(config) = config_size').fetchall()
    for rowid, config in raw:
        packed = zlib.compress(config, 9)
        if len(packed) < len(config):
            con.execute('UPDATE pipeline_cache SET config = ? WHERE rowid = ?', (packed, rowid))
    digest = hashlib.sha256()
    for row in con.execute('SELECT type, hash, config FROM pipeline_cache ORDER BY type, hash'):
        digest.update(repr(row[:2]).encode())
        digest.update(row[2])
    for row in con.execute('SELECT scene, type, hash, main_target FROM pipeline_scene_v2 ORDER BY 1, 2, 3'):
        digest.update(repr(row).encode())
    seed_id = digest.hexdigest()[:16]
    con.execute("INSERT INTO aurora_meta VALUES ('seed_id', ?)", (seed_id,))
    con.execute("INSERT INTO aurora_meta VALUES ('coverage', ?)",
                (json.dumps({'character_sets': list(CHARACTER_SETS), 'scenes': coverage()}, sort_keys=True),))
    con.commit()
    pipelines = con.execute('SELECT COUNT(*) FROM pipeline_cache').fetchone()[0]
    scenes, entries = con.execute('SELECT COUNT(DISTINCT scene), COUNT(*) FROM pipeline_scene_v2').fetchone()
    con.execute('VACUUM')
    con.close()
    os.replace(tmp, out)
    print(f'{out}: seed {seed_id}, {pipelines} pipelines, {scenes} scenes, {entries} scene entries, '
          f'{out.stat().st_size / 1e6:.1f} MB')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--exe', default=str(ROOT / 'build' / 'release' / 'mp6native.exe'))
    parser.add_argument('--decomp', default=os.environ.get('MP6_DECOMP_DIR', str(ROOT / 'build' / 'deps' / 'marioparty6')))
    parser.add_argument('--disc', default=os.environ.get('MP6_DISC_ROOT', str(ROOT / 'build' / 'disc-cache' / 'orig' / 'GP6E01')))
    parser.add_argument('--seconds', type=int, default=60, help='wall time per scene')
    parser.add_argument('--jobs', type=int, default=2, help='scenes recorded at once (each is a game, ~1.4 GB)')
    parser.add_argument('--scenes', nargs='*', help='scene DLL names (default: the boot and every REL scene)')
    parser.add_argument('--fresh', action='store_true', help='discard earlier recordings first')
    parser.add_argument('--resume', action='store_true', help='skip runs an interrupted recording finished')
    parser.add_argument('--merge-only', action='store_true', help='merge earlier recordings into the seed')
    parser.add_argument('--out', default=str(SEED))
    args = parser.parse_args()
    if args.fresh and (OUT / 'cache').exists():
        shutil.rmtree(OUT / 'cache')
        (OUT / 'done.txt').unlink(missing_ok=True)
    if not args.merge_only:
        scenes = args.scenes or [BOOT_RUN] + scene_names(args.decomp)
        if record(args, scenes):
            return 1
    return merge(Path(args.out))


if __name__ == '__main__':
    sys.exit(main())
