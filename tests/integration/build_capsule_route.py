"""Build a separate Windows game with a Red Mushroom in the first human turn.

The inventory fixture and completion logging are generated only under build/;
the capsule menu, activation, effects, renderer and cleanup remain real code.
Normal release binaries and dependency checkouts are never overwritten.
Run after the normal Windows release build, with the same disc-cache env vars.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build

UNFIXED = '--unfixed' in sys.argv
OUTPUT = ROOT / ('build/capsule-route-unfixed' if UNFIXED else 'build/capsule-route')
OUTPUT.mkdir(exist_ok=True)
configure_original = build.configure_windows
collect_original = build.collect_units


def configure(configuration):
    configure_original(configuration)
    global original_objects
    original_objects = Path(build.OBJ_DIR)
    build.BUILD_DIR = str(OUTPUT)
    build.OBJ_DIR = str(OUTPUT / 'obj')


def collect(*args, **kwargs):
    units = []
    for source, flags, name, flavor in collect_original(*args, **kwargs):
        obj = original_objects / name
        if UNFIXED and name == 'board_capevent.o':
            # Negative control: only this TU loses the port layout fix. The
            # pinned original is read-only; the production source cache stays fixed.
            source = str(Path(build.DECOMP) / 'src/board/capevent.c')
            obj = OUTPUT / 'obj' / name
        if name == 'board_player.o':
            text = Path(source).read_text(encoding='utf-8')
            marker = '    GwPlayer[playerNo].diceNum = 1;\nrepeat:'
            assert text.count(marker) == 1
            text = text.replace(marker, '''    static BOOL capsuleFixtureGiven;
    if (!capsuleFixtureGiven && !GwPlayer[playerNo].comF) {
        capsuleFixtureGiven = TRUE;
        mbPlayerCapsuleAdd(playerNo, 0);
        OSReport("[CAPSULE-TEST] Red Mushroom added for player %d\\n", playerNo);
    }
''' + marker)
            marker = '            GwPlayer[playerNo].capsuleUseNum++;'
            assert text.count(marker) == 1
            text = text.replace(marker, marker + '''
            if (GwPlayer[playerNo].capsuleUse == 0) {
                OSReport("[CAPSULE-TEST] Red Mushroom returned; diceMode=%d remaining=%d\\n",
                    GwPlayer[playerNo].diceMode, mbPlayerCapsuleFind(playerNo, 0));
                mp6_event_post("capsule.used", GwPlayer[playerNo].diceMode, "red-mushroom");
                mp6_max_ticks = (int)mp6_tick_count + 600;
            }''')
            source = str(OUTPUT / 'player_fixture.c')
            Path(source).write_text('#include "mp6_boot.h"\n#include "mp6_events.h"\n' + text,
                                    encoding='utf-8')
            obj = OUTPUT / 'obj' / name
        units.append((source, flags, str(obj), flavor))
    return units


build.configure_windows = configure
build.collect_units = collect
sys.argv = [str(ROOT / 'tools/build.py'), '--configuration', 'release', '-j6']
raise SystemExit(build.main())
