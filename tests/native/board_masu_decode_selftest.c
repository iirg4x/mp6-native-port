#include <stdio.h>
#include <string.h>
#include "game/board/masu.h"
#include "be.h"
#include "board_subject.inc"

#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void)
{
    /* Both aligned and unaligned blobs; a vector, flag, attribute and link. */
    const u8 bytes[] = {
        0, 0, 0, 2,
        0x3f, 0xc0, 0, 0, 0xc0, 0x10, 0, 0, 0x43, 0x80, 0, 0,
        0x80, 1, 0x89, 0xab, 0xcd, 0xef, 0, 7
    };
    u8 storage[sizeof(bytes) + 4];
    for (int offset = 0; offset < 4; ++offset) {
        memcpy(storage + offset, bytes, sizeof(bytes));
        void *cursor;
        s32 count;
        HuVecF pos;
        u16 flag, link;
        u32 attr;
        DATA_READCOUNT(storage + offset, cursor, count);
        CHECK(count == 2 && (u8 *)cursor == storage + offset + 4);
        DATA_READVEC(cursor, pos);
        CHECK(pos.x == 1.5f && pos.y == -2.25f && pos.z == 256.0f);
        DATA_READ16(cursor, flag);
        DATA_READ32(cursor, attr);
        DATA_READLINK(cursor, link);
        CHECK(flag == 0x8001 && attr == 0x89abcdef && link == 8);
        CHECK((u8 *)cursor == storage + offset + sizeof(bytes));
    }
    puts("Masu packed native decoding: PASS");
    return 0;
}
