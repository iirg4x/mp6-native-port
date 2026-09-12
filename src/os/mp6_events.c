/* MP6 native port -- the game-event bus. See include/mp6_events.h for
 * the full rationale (short version: attach automation input to observed
 * game state, never to guessed tick offsets).
 *
 * This file is deliberately dependency-light: <stdio.h>/<string.h>/
 * <stdlib.h> plus mp6_boot.h for the shared VI tick counter. It links into
 * BOTH build modes -- the headless build is where a byte-stable event
 * transcript is most useful, and the OSReport tap that feeds it lives in
 * shims_manual.c, which is also in both.
 */
#include "mp6_events.h"
#include "mp6_boot.h" /* mp6_tick_count -- the shared VI tick */
#include "mp6_diag_probe.h" /* the chronological tail ring -- see mp6_event_post */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The decomp's own overlay table, consumed as pure data: ovl_table.h is a
 * list of DLL(name) invocations, so redefining DLL() turns it into a string
 * table whose indices are exactly the OMOVL enum values include/game/
 * omovl.h derives from the same file. No decomp source is modified and no
 * overlay id is hard-coded here -- if the table ever changes, this tracks
 * it automatically. */
#define DLL(name) #name,
static const char *const kOvlNames[] = {
#include "ovl_table.h"
};
#undef DLL

#define OVL_NAME_COUNT ((long)(sizeof(kOvlNames) / sizeof(kOvlNames[0])))

const char *mp6_event_ovl_name(long ovlNo)
{
    if (ovlNo < 0 || ovlNo >= OVL_NAME_COUNT) {
        return NULL;
    }
    return kOvlNames[ovlNo];
}

typedef struct {
    char key[MP6_EVENT_KEY_MAX];
    char sval[MP6_EVENT_VAL_MAX];
    long nval;
    unsigned long seq;   /* seq at last fire; 0 == never fired */
    unsigned long count; /* how many times this key has fired */
} MP6EventSlot;

static MP6EventSlot s_slots[MP6_EVENT_SLOT_MAX];
static int s_slotCount;
static unsigned long s_seq;

/* CHRONOLOGICAL TAIL. The slot table above is keyed by event NAME and keeps
 * only each key's latest value -- deliberately, because that is what
 * automation waits on. It cannot answer "what just happened, in order", which
 * is the first question anyone asks when a board stops responding. The ring
 * below is filled from the same place the [EVENT] line is printed, formatted
 * from the same fields, so the tail and the log can never disagree. Fixed
 * storage, one snprintf per event, no allocation. */
#define EV_TAIL_LINE 120
static char s_tail[MP6_DIAG_EVENT_TAIL][EV_TAIL_LINE];
static int  s_tailHead;
static int  s_tailCount;

unsigned long mp6_event_seq(void)
{
    return s_seq;
}

static MP6EventSlot *slot_find(const char *key)
{
    int i;
    for (i = 0; i < s_slotCount; i++) {
        if (strcmp(s_slots[i].key, key) == 0) {
            return &s_slots[i];
        }
    }
    return NULL;
}

static void copy_bounded(char *dst, size_t dstSz, const char *src)
{
    size_t n;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= dstSz) {
        n = dstSz - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void mp6_event_post(const char *key, long nval, const char *sval)
{
    MP6EventSlot *slot;

    if (key == NULL || key[0] == '\0') {
        return;
    }
    s_seq++;

    slot = slot_find(key);
    if (slot == NULL && s_slotCount < MP6_EVENT_SLOT_MAX) {
        slot = &s_slots[s_slotCount++];
        copy_bounded(slot->key, sizeof(slot->key), key);
        slot->count = 0;
    }
    /* Over MP6_EVENT_SLOT_MAX distinct keys the table stops recording, but
     * the line is still PRINTED -- a log consumer keeps full fidelity even
     * in the (never yet reached) overflow case; only in-process waiting on
     * the overflowed key stops working. Loud rather than silent: */
    if (slot != NULL) {
        copy_bounded(slot->sval, sizeof(slot->sval), sval);
        slot->nval = nval;
        slot->seq = s_seq;
        slot->count++;
    }

    printf("[EVENT] %s=%s num=%ld tick=%ld seq=%lu\n",
           key,
           (sval != NULL && sval[0] != '\0') ? sval : "-",
           nval,
           mp6_tick_count,
           s_seq);
    fflush(stdout);

    /* Same fields, same order, into the chronological tail (see its
     * declaration). Appended here rather than in a wrapper so no future call
     * path can post an event that the tail misses. */
    snprintf(s_tail[s_tailHead], EV_TAIL_LINE, "%s=%s num=%ld tick=%ld seq=%lu",
             key,
             (sval != NULL && sval[0] != '\0') ? sval : "-",
             nval,
             mp6_tick_count,
             s_seq);
    s_tailHead = (s_tailHead + 1) % MP6_DIAG_EVENT_TAIL;
    if (s_tailCount < MP6_DIAG_EVENT_TAIL) s_tailCount++;

    /* MP6_FRAME_DUMP_TRIGGER (include/mp6_frame_dump.h): let a frame
     * capture arm on an observed game state instead of a guessed tick --
     * the same "attach to what the game reported" rule this whole bus
     * exists for. Standing no-op unless MP6_FRAME_DUMP is set (and in the
     * headless/Android builds, where the lever compiles out entirely).
     * Matched against the same "key=value" text the line above prints. */
    {
        extern void mp6_frame_dump_trigger(const char *text);
        char triggerText[160];
        snprintf(triggerText, sizeof(triggerText), "%s=%s", key,
                 (sval != NULL && sval[0] != '\0') ? sval : "-");
        mp6_frame_dump_trigger(triggerText);
    }
}

unsigned long mp6_event_last_seq(const char *key)
{
    MP6EventSlot *slot = (key != NULL) ? slot_find(key) : NULL;
    return (slot != NULL) ? slot->seq : 0u;
}

int mp6_event_matches(const char *key, const char *want)
{
    MP6EventSlot *slot = (key != NULL) ? slot_find(key) : NULL;
    char numBuf[24];

    if (slot == NULL || slot->seq == 0u) {
        return 0;
    }
    if (want == NULL || want[0] == '\0') {
        return 1;
    }
    if (strcmp(slot->sval, want) == 0) {
        return 1;
    }
    /* Numeric spelling, so `ovl.start/123` and `ovl.start/w01dll` are
     * interchangeable for the same event (see the header). */
    snprintf(numBuf, sizeof(numBuf), "%ld", slot->nval);
    return strcmp(numBuf, want) == 0;
}

/* ---------------------------------------------------------------------
 * Pull-side readers (include/mp6_diag_probe.h).
 *
 * mp6_event_seq/_last_seq/_matches above answer "has THIS key fired yet",
 * which is what a scripted wait needs. These answer the two questions a human
 * looking at a stuck screen has instead: what happened most recently, and
 * which keys have ever fired at all. Read-only; neither advances the sequence
 * nor consumes anything.
 * --------------------------------------------------------------------- */

int mp6_diag_event_tail_count(void)
{
    return s_tailCount;
}

const char *mp6_diag_event_tail(int back)
{
    int idx;
    if (back < 0 || back >= s_tailCount) return NULL;
    idx = s_tailHead - 1 - back;
    while (idx < 0) idx += MP6_DIAG_EVENT_TAIL;
    return s_tail[idx];
}

int mp6_diag_event_slot_count(void)
{
    return s_slotCount;
}

int mp6_diag_event_slot(int index, const char **key, const char **sval,
                        long *nval, unsigned long *count, unsigned long *seq)
{
    if (index < 0 || index >= s_slotCount) return 0;
    if (key != NULL) *key = s_slots[index].key;
    if (sval != NULL) *sval = s_slots[index].sval;
    if (nval != NULL) *nval = s_slots[index].nval;
    if (count != NULL) *count = s_slots[index].count;
    if (seq != NULL) *seq = s_slots[index].seq;
    return 1;
}

/* ---------------------------------------------------------------------
 * OSReport tap.
 *
 * The game already narrates its whole overlay/DLL lifecycle through
 * OSReport -- objmain.c prints the overlay it is about to start, objdll.c
 * prints every REL search/link/unlink, selmenu.c prints "SMinit:". Rather
 * than patching decomp source to emit a parallel set of markers, this
 * recognises the wording the game ALREADY uses and republishes it as typed
 * events. Every pattern below is anchored at the start of the line and
 * quoted from the decomp verbatim, so a wording change fails by producing
 * no event (visible immediately as a wait timeout) rather than by silently
 * matching something else.
 * --------------------------------------------------------------------- */

static int starts_with(const char *s, const char *prefix, const char **rest)
{
    size_t n = strlen(prefix);
    if (strncmp(s, prefix, n) != 0) {
        return 0;
    }
    if (rest != NULL) {
        *rest = s + n;
    }
    return 1;
}

/* Copies up to dstSz-1 chars from src, stopping at any of `stops` or at
 * end-of-string/newline. Used to lift a bare token (an overlay filename, a
 * module name) out of the middle of one of the game's own report lines. */
static void token_until(char *dst, size_t dstSz, const char *src, const char *stops)
{
    size_t i = 0;
    while (src[i] != '\0' && src[i] != '\n' && src[i] != '\r' &&
           strchr(stops, src[i]) == NULL && i + 1 < dstSz) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void mp6_event_scan_line(const char *line)
{
    const char *rest;
    char tok[MP6_EVENT_VAL_MAX];

    if (line == NULL) {
        return;
    }

    /* --- overlay lifecycle (src/game/objmain.c) --------------------- */

    /* omWatchOverlayProc, the single place an overlay actually BECOMES the
     * current one. This is the primary "the game is now in screen X" event. */
    if (starts_with(line, "++++++++++++++++++++ Start New OVL ", &rest)) {
        long ovl = strtol(rest, NULL, 10);
        const char *name = mp6_event_ovl_name(ovl);
        mp6_event_post("ovl.start", ovl, name);
        return;
    }
    /* omOvlCallEx -- the REQUEST, several frames before ovl.start (the old
     * overlay still has to tear down first). Waiting on this is how a
     * driver learns its confirm press was accepted, at the earliest
     * possible moment. */
    if (starts_with(line, "objman>Call New Ovl ", &rest)) {
        long ovl = strtol(rest, NULL, 10);
        mp6_event_post("ovl.call", ovl, mp6_event_ovl_name(ovl));
        return;
    }
    if (starts_with(line, "objman>Ovl Return ", &rest)) {
        mp6_event_post("ovl.return", strtol(rest, NULL, 10), NULL);
        return;
    }
    if (starts_with(line, "OvlKill ", &rest)) {
        mp6_event_post("ovl.kill", strtol(rest, NULL, 10), NULL);
        return;
    }
    /* The overlay's ObjectSetup has returned -- its objects/cameras exist.
     * The closest thing the engine has to "this screen is now built". */
    if (starts_with(line, "objman>ObjectSetup end", NULL)) {
        mp6_event_post("ovl.setup_end", 0, NULL);
        return;
    }

    /* --- REL lifecycle (src/game/objdll.c + src/os/dll_bridge.c) - */

    if (starts_with(line, "Search:dll/", &rest)) {
        token_until(tok, sizeof(tok), rest, ".");
        mp6_event_post("dll.search", 0, tok);
        return;
    }
    if (starts_with(line, "objdll>Link DLL:dll/", &rest)) {
        token_until(tok, sizeof(tok), rest, ".");
        mp6_event_post("dll.link", 0, tok);
        return;
    }
    if (starts_with(line, "objdll>End DLL:dll/", &rest)) {
        token_until(tok, sizeof(tok), rest, ".");
        mp6_event_post("dll.end", 0, tok);
        return;
    }
    /* src/os/dll_bridge.c's own "[BOOT] OSLink: bound w01Dll prolog/
     * epilog" -- the proof the module's entry points really resolved. */
    if (starts_with(line, "[BOOT] OSLink: bound ", &rest)) {
        token_until(tok, sizeof(tok), rest, " ");
        mp6_event_post("dll.bound", 0, tok);
        return;
    }

    /* --- screen-specific readiness markers ------------------------- */

    /* src/REL/selmenuDll/selmenu.c's SMInit. selmenuDll is the developer
     * overlay-select screen and has NO timeout of its own (SMMain waits for
     * input forever), so this is the one screen a driver can enter and then
     * take arbitrarily long in. */
    if (starts_with(line, "SMinit:", NULL)) {
        mp6_event_post("selmenu.ready", 0, NULL);
        return;
    }
    /* selmenu.c's SMExit, printed immediately before its omOvlCallEx. */
    if (starts_with(line, "mgNo=", &rest)) {
        mp6_event_post("selmenu.confirm", strtol(rest, NULL, 10), NULL);
        return;
    }
    /* src/REL/bootDll/boot.c's own diagnostic: the warning screen has
     * reached its input-wait loop, i.e. START is now live. */
    if (starts_with(line, "[MP6-DIAG-WARN] BootWarningExec input-wait", NULL)) {
        mp6_event_post("boot.warning_ready", 0, NULL);
        return;
    }
    if (starts_with(line, "[MP6-BOOT-ROUTE] ", &rest)) {
        token_until(tok, sizeof(tok), rest, "\n");
        mp6_event_post("boot.route", 0, tok);
        return;
    }
    if (starts_with(line, "******* Boot ObjectSetup", NULL)) {
        mp6_event_post("boot.setup", 0, NULL);
        return;
    }

    /* --- W01 (the recovered Towering Treetop overlay) --------------- */

    /* compat/decomp/src/REL/w01Dll/world01.c's own markers, already the
     * contract docs/W01_INTEGRATION.md's pass oracle is written against --
     * republished here so a driver can WAIT on them (e.g. hold the process
     * open until "render", then screenshot) instead of timing the capture. */
    if (starts_with(line, "[W01] ", &rest)) {
        token_until(tok, sizeof(tok), rest, " \n");
        mp6_event_post("w01", 0, tok);
        return;
    }
}
