"""Executable native-text coverage for board-window inserted messages."""

from pathlib import Path
import subprocess
import tempfile
import unittest

from tools import apply_patches, build
from tests.test_native_modes import function_body


ROOT = Path(__file__).resolve().parents[1]


def patched_source(decomp, relative):
    """Apply one real compatibility patch without replacing the source fixture."""
    original = (decomp / relative).read_text(encoding="utf-8")
    patch = ROOT / "compat" / "decomp" / (relative + ".patch")
    return apply_patches.apply_unified_diff(
        original, patch.read_text(encoding="utf-8"), relative
    )


def setter_probe(board_source):
    """Build a probe from the patched board setter definitions themselves."""
    definitions = "\n\n".join(
        function_body(board_source, name)
        for name in (
            "mbWinInsertMesSet",
            "mbWinTopInsertMesSet",
            "mp6_board_window_insert_text",
            "mp6_board_window_top_insert_text",
        )
    )
    return r'''#include <assert.h>
#include <stdint.h>
#include <string.h>

/* Load the patched board record before mp6_board_window.h includes it again. */
#include "game/board/window.h"
#include "mp6_board_window.h"
#include "mp6_window.h"

#if UINTPTR_MAX <= UINT32_MAX
#error "native text probe requires a wider pointer than an archive message ID"
#endif

static MBWIN board_storage[MBWIN_MAX];
static MBWIN *mbWinData;
static u8 mbWinStack[MBWIN_MAX];
static u8 mbWinTopNo;
HUWIN winData[HUWIN_MAX];

static int hu_insert_calls;
static HUWINID hu_insert_window;
static u32 hu_insert_message;
static s16 hu_insert_slot;
static int measure_calls;
static const char *measured_text;
static s16 measured_slot;

void HuWinInsertMesSet(HUWINID window, u32 message, s16 slot)
{
    ++hu_insert_calls;
    hu_insert_window = window;
    hu_insert_message = message;
    hu_insert_slot = slot;
    if (window >= 0 && window < HUWIN_MAX && slot >= 0 && slot < HUWIN_INSERTMES_MAX) {
        /* Model the archive-ID branch of the real window setter. */
        winData[window].messDataInsert[slot] = (char *)(uintptr_t)message;
    }
}

void mp6_window_measure_insert_text(const char *text, s16 slot)
{
    ++measure_calls;
    measured_text = text;
    measured_slot = slot;
}

''' + definitions + r'''

static void reset_state(void)
{
    int i;
    int j;

    memset(board_storage, 0, sizeof(board_storage));
    memset(mbWinStack, 0, sizeof(mbWinStack));
    memset(winData, 0, sizeof(winData));
    mbWinData = board_storage;
    mbWinTopNo = 0;
    hu_insert_calls = 0;
    hu_insert_window = HUWIN_NONE;
    hu_insert_message = 0;
    hu_insert_slot = -1;
    measure_calls = 0;
    measured_text = NULL;
    measured_slot = -1;
    for (i = 0; i < MBWIN_MAX; ++i) {
        board_storage[i].winId = HUWIN_NONE;
        for (j = 0; j < HUWIN_INSERTMES_MAX; ++j) {
            board_storage[i].insertMes[j] = MBWIN_MES_NONE;
            board_storage[i].insertText[j] = NULL;
        }
    }
}

int main(void)
{
    static char text[] = "native inserted text";
    const int slot = 3;
    const u32 message = 0x00012345u;
    MBWIN before;
    char *old_live;

    /* Queueing keeps the full host pointer in the board record and does not
     * touch the not-yet-created HUWIN record. */
    reset_state();
    board_storage[2].winId = HUWIN_NONE;
    old_live = winData[7].messDataInsert[slot];
    mp6_board_window_insert_text(2, text, slot);
    assert(board_storage[2].insertMes[slot] == MBWIN_MES_NONE);
    assert(board_storage[2].insertText[slot] == text);
    assert((uintptr_t)board_storage[2].insertText[slot] == (uintptr_t)text);
    assert(winData[7].messDataInsert[slot] == old_live);
    assert(hu_insert_calls == 0);

    /* A live HUWIN receives the same native pointer immediately. */
    reset_state();
    board_storage[2].winId = 7;
    mp6_board_window_insert_text(2, text, slot);
    assert(board_storage[2].insertMes[slot] == MBWIN_MES_NONE);
    assert(board_storage[2].insertText[slot] == text);
    assert(winData[7].messDataInsert[slot] == text);
    assert(hu_insert_calls == 0);

    /* The legacy numeric setter clears a previous native pointer before
     * replaying the archive message into a live window. */
    mbWinInsertMesSet(2, message, slot);
    assert(board_storage[2].insertText[slot] == NULL);
    assert(board_storage[2].insertMes[slot] == message);
    assert(hu_insert_calls == 1);
    assert(hu_insert_window == 7);
    assert(hu_insert_message == message);
    assert(hu_insert_slot == slot);
    assert(winData[7].messDataInsert[slot] == (char *)(uintptr_t)message);

    /* The top-of-stack path reaches the real native setter, while an empty
     * stack is a no-op and cannot index before mbWinStack. */
    reset_state();
    board_storage[2].winId = HUWIN_NONE;
    mbWinTopNo = 0;
    before = board_storage[2];
    mp6_board_window_top_insert_text(text, slot);
    assert(memcmp(&before, &board_storage[2], sizeof(before)) == 0);
    assert(hu_insert_calls == 0);
    mbWinTopNo = 1;
    mbWinStack[0] = 2;
    board_storage[2].winId = 7;
    mp6_board_window_top_insert_text(text, slot);
    assert(board_storage[2].insertText[slot] == text);
    assert(board_storage[2].insertMes[slot] == MBWIN_MES_NONE);
    mbWinTopInsertMesSet(message, slot);
    assert(board_storage[2].insertText[slot] == NULL);
    assert(board_storage[2].insertMes[slot] == message);
    assert(hu_insert_calls == 1);

    /* NULL text is still a valid clear for a live slot. */
    reset_state();
    board_storage[2].winId = 7;
    board_storage[2].insertText[slot] = text;
    winData[7].messDataInsert[slot] = text;
    mp6_board_window_insert_text(2, NULL, slot);
    assert(board_storage[2].insertMes[slot] == MBWIN_MES_NONE);
    assert(board_storage[2].insertText[slot] == NULL);
    assert(winData[7].messDataInsert[slot] == NULL);

    /* All native bounds checks leave the record untouched, including a
     * temporarily unavailable manager allocation. */
    reset_state();
    board_storage[2].winId = HUWIN_NONE;
    before = board_storage[2];
    mp6_board_window_insert_text(-1, text, slot);
    mp6_board_window_insert_text(MBWIN_MAX, text, slot);
    mp6_board_window_insert_text(2, text, -1);
    mp6_board_window_insert_text(2, text, HUWIN_INSERTMES_MAX);
    assert(memcmp(&before, &board_storage[2], sizeof(before)) == 0);
    mbWinData = NULL;
    mp6_board_window_insert_text(2, text, slot);
    assert(memcmp(&before, &board_storage[2], sizeof(before)) == 0);

    /* Keep the measurement stub linked so the probe also verifies that the
     * real header declarations remain ABI-compatible. */
    assert(measure_calls == 0);
    assert(measured_text == NULL && measured_slot == -1);
    return 0;
}
'''


class BoardWindowNativeTextTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.decomp = Path(apply_patches.require_pinned_decomp())

    def test_board_initialization_replay_and_measurement_contracts(self):
        board = patched_source(self.decomp, "src/board/window.c")
        game = patched_source(self.decomp, "src/game/window.c")

        proc = function_body(board, "mbWinProc")
        center = function_body(board, "mbWinCenterSet")
        create = function_body(board, "mbWinCreate")
        replay = function_body(board, "mbWinCenterInsertGet")
        self.assertIn("mp6_window_insert_text", proc)
        self.assertIn("window->insertText[entryIndex]", proc)
        self.assertIn("mp6_window_measure_insert_text", center)
        self.assertIn("window->insertText[insertIndex]", center)
        self.assertIn("window->insertText[slotIndex] = NULL", create)
        self.assertIn("mp6_window_insert_text", replay)
        self.assertIn("window->insertText[insertIndex]", replay)

        measure = function_body(game, "mp6_window_measure_insert_text")
        measure_loop = function_body(game, "GetMesMaxSizeSub2")
        get_size = function_body(game, "GetMesMaxSizeSub")
        self.assertRegex(get_size, r"GetMesMaxSizeSub\s*\(\s*uintptr_t\s+messNum")
        self.assertIn("GetMesMaxSizeSub((uintptr_t)text)", measure)
        self.assertIn("mp6_window_measure_insert_text", measure_loop)

    def test_native_board_text_setters_preserve_pointer_and_bounds(self):
        board = patched_source(self.decomp, "src/board/window.c")
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            temporary = Path(temporary)
            patched_root = temporary / "patched"
            apply_patches.apply_all(
                decomp_root=str(self.decomp),
                out_dir=str(patched_root),
            )
            source = temporary / "board_window_native_text_probe.c"
            executable = temporary / "board_window_native_text_probe.exe"
            source.write_text(setter_probe(board), encoding="utf-8")
            result = subprocess.run(
                [
                    build.ZIG,
                    "cc",
                    "-I",
                    str(patched_root / "include"),
                    "-I",
                    str(ROOT / "include"),
                    *build.COMMON_FLAGS,
                    "-UNDEBUG",
                    str(source),
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=60,
            )
            self.assertEqual(
                result.returncode,
                0,
                f"board native text probe compile failed:\n{result.stderr}",
            )
            result = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(
                result.returncode,
                0,
                f"board native text probe failed:\n{result.stdout}\n{result.stderr}",
            )


if __name__ == "__main__":
    unittest.main()
