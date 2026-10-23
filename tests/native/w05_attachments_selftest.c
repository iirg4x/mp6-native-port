#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "REL/w05Dll/work.h"

void *lbl_1_bss_8;
#include "w05_attachment_subject.inc"

int main(void)
{
    W05_BOARD_WORK board, expected;
    lbl_1_bss_8 = &board;
    for (int value = -1; value < 4; ++value) {
        memset(&board, 0x5a, sizeof(board));
        memcpy(&expected, &board, sizeof(board));
        expected.firstHookPlayer = value;
        expected.firstHookPlayerPoint = 7;
        fn_1_80AC(value, 7);
        assert(!memcmp(&board, &expected, sizeof(board)));
        expected.firstHookObject = value;
        expected.firstHookObjectPoint = 3;
        fn_1_80D4(value, 3);
        assert(!memcmp(&board, &expected, sizeof(board)));
        expected.secondHookPlayer = value;
        expected.secondHookPlayerPoint = 2;
        fn_1_80FC(value, 2);
        assert(!memcmp(&board, &expected, sizeof(board)));
        expected.secondHookObject = value;
        expected.secondHookObjectPoint = 5;
        fn_1_8124(value, 5);
        assert(!memcmp(&board, &expected, sizeof(board)));
    }
    memset(&board, 0, sizeof(board));
    fn_1_80AC(-1, 0); fn_1_80D4(-1, 0);
    fn_1_80FC(-1, 0); fn_1_8124(-1, 0);
    assert(board.firstHookPlayer == -1 && board.secondHookPlayer == -1);
    assert(board.firstHookObject == -1 && board.secondHookObject == -1);
    puts("W05 typed ship attachments: PASS");
}
