/* Drives the real mic engine + host (src/mic/) the way game/mic.c does:
 * a word list in the disc's .wrd format, a listening session, recognizer
 * text posted by a fake backend, and the candidate nodes the game reads. */
#include "dolphin.h"
#include "dolphin/mic.h"
#include "mp6_mic.h"
#include "../../src/mic/mic_internal.h"
#include "host.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Node {
    s32 status;
    u16 count;
    u16 score;
    s32 *result;
    struct Node *next;
} Node;

typedef void (*ResultFn)(u32, u32, uintptr_t);
typedef void (*NotifyFn)(u32, s32, u32, u32);
int gsapi_Init(ResultFn, int);
int gsapi_NotifySetCallback(NotifyFn);
int gsapi_EngineOpen(int, void **);
int gsapi_EngineSetMode(void *, int);
int gsapi_ContextSetCtxData(void *, void **);
int gsapi_ContextSetWrdData(void *, void *);
int gsapi_ContextActivate(void *, void *);
int gsapi_ContextDeActivate(void *);
int gsapi_EngineStart(void *);
int gsapi_EngineRestart(int);
int gsapi_EngineStop(void *);
int gsapi_EngineGetParam(void *, int, char **);

/* The host seam pieces mic_host.c uses (single-threaded here). */
void mp6_host_mutex_init(Mp6Mutex *m) { (void)m; }
void mp6_host_mutex_lock(Mp6Mutex *m) { (void)m; }
void mp6_host_mutex_unlock(Mp6Mutex *m) { (void)m; }

/* A fake platform backend. */
static int g_backendStarts, g_backendFinishes, g_lastCount;
static char g_lastPhrases[8][64];
int mp6_mic_backend_available(void) { return 1; }
int mp6_mic_backend_start(const char *const *phrases, int count, int continuous)
{
    int i;
    (void)continuous;
    g_backendStarts++;
    g_lastCount = count;
    for (i = 0; i < count && i < 8; i++) snprintf(g_lastPhrases[i], 64, "%s", phrases[i]);
    return 1;
}
void mp6_mic_backend_finish(void) { g_backendFinishes++; }
void mp6_mic_backend_stop(void) {}
void mp6_mic_backend_poll(void) {}

static int g_results;
static Node g_seen;
static int g_seenWord, g_seenWords[4], g_seenCandidates;
static int g_notified[16];

static void on_result(u32 a, u32 b, uintptr_t node)
{
    const Node *n = (const Node *)node;
    (void)a; (void)b;
    g_results++;
    g_seen = *n;
    g_seenWord = n->count ? n->result[1] : -1;
    g_seenCandidates = 0;
    for (; n != NULL && n->count != 0 && g_seenCandidates < 4; n = n->next) {
        g_seenWords[g_seenCandidates++] = n->result[1];
    }
}

static void on_notify(u32 a, s32 event, u32 b, u32 c)
{
    (void)a; (void)b; (void)c;
    if (event >= 0 && event < 16) g_notified[event]++;
}

static size_t make_wrd(u8 *out, const char *const *words, int count)
{
    size_t len = 8;
    int i;
    for (i = 0; i < count; i++) {
        strcpy((char *)out + len, words[i]);
        len += strlen(words[i]) + 1;
    }
    out[0] = (u8)(len >> 24); out[1] = (u8)(len >> 16); out[2] = (u8)(len >> 8); out[3] = (u8)len;
    out[4] = 0; out[5] = 0; out[6] = 0; out[7] = (u8)count;
    return len;
}

static void say(const char *text, int final)
{
    mp6_mic_post_result(text ? &text : NULL, NULL, text ? 1 : 0, final);
    mp6_mic_host_pump();
    gsapi_EngineRestart(0);
}

static void check_normalise(const char *in, const char *want)
{
    char out[128];
    mp6_mic_normalise(in, out, sizeof(out));
    if (strcmp(out, want) != 0) {
        printf("normalise(\"%s\") = \"%s\", want \"%s\"\n", in, out, want);
        assert(0);
    }
}

int main(void)
{
    static const char *const words[] = {
        "Fire", "Missile", "Missiles", "m665_Missile2", "Laser", "Drop bombs",
        "m665_Goomba0", "m665_Goomba1", "micquiz_chara_Koopa_Kid0", "12", ".S", ".S",
    };
    u8 wrd[512];
    void *engine = NULL, *ctx = NULL;
    char *name = NULL;

    check_normalise("Gimme a hint!", "give me a hint");
    check_normalise("  FIRE, fire ", "fire fire");
    check_normalise("12", "twelve");
    check_normalise("21", "twenty one");
    check_normalise("Cheep-Cheep", "cheep cheep");
    check_normalise("I'm", "im");

    make_wrd(wrd, words, 12);
    assert(gsapi_Init(on_result, 0) == 0);
    gsapi_NotifySetCallback(on_notify);
    gsapi_EngineOpen(0, &engine);
    assert(engine != NULL);
    assert(gsapi_ContextSetCtxData(NULL, &ctx) == 0 && ctx != NULL);
    assert(gsapi_ContextSetWrdData(ctx, wrd) == 0);
    assert(gsapi_ContextActivate(engine, ctx) == 0);
    mp6_mic_set_enabled(1);
    mp6_mic_host_pump();

    /* Slot B looks occupied and mounts. */
    assert(MICProbeEx(1) == MIC_RESULT_READY);
    assert(MICMount(1, NULL, 0, NULL) == MIC_RESULT_READY);
    assert(mp6_mic_in_use() == 1);

    /* One answer: the backend gets the distinct, derived phrases. */
    gsapi_EngineSetMode(engine, 1);
    gsapi_EngineStart(engine);
    assert(g_notified[2] == 1);
    mp6_mic_host_pump();
    assert(g_backendStarts == 1);
    assert(g_lastCount == 8);
    assert(strcmp(g_lastPhrases[0], "fire") == 0);
    assert(strcmp(g_lastPhrases[5], "goomba") == 0);
    assert(strcmp(g_lastPhrases[6], "koopa kid") == 0);
    assert(strcmp(g_lastPhrases[7], "twelve") == 0);
    assert(mp6_mic_in_use() == 2);

    /* An interim that is not a whole word does nothing; a final names it. */
    say("drop", 0);
    assert(g_results == 0);
    say("drop bombs", 1);
    assert(g_results == 1 && g_seenWord == 5 && g_seen.score == 10000 && g_seen.count == 3);
    /* One-shot: later results of the same session are ignored. */
    say("laser", 1);
    assert(g_results == 1);
    gsapi_EngineStop(engine);

    /* Continuous: an exact interim answers early, its final is skipped,
     * the next utterance answers again. Misheard spellings still land. */
    gsapi_EngineSetMode(engine, 2);
    gsapi_EngineStart(engine);
    mp6_mic_host_pump();
    say("fire", 0);
    assert(g_results == 2 && g_seenWord == 0);
    say("fire", 1);
    assert(g_results == 2);
    say("goombas", 1);
    assert(g_results == 3 && (g_seenWord == 6 || g_seenWord == 7));
    say("koopa kids", 1);
    assert(g_results == 4 && g_seenWord == 8);
    say("12", 1);
    assert(g_results == 5 && g_seenWord == 9);
    say("missiles please", 1);
    assert(g_results == 6 && g_seenWord >= 1 && g_seenWord <= 3);
    /* Nothing usable: an empty answer, which the game reports as "no result". */
    say("banana", 1);
    assert(g_results == 7 && g_seen.count == 0);
    say(NULL, 1);
    assert(g_results == 8 && g_seen.count == 0);
    assert(gsapi_EngineGetParam(engine, 4, &name) == 0 && strcmp(name, "Laser") == 0);

    /* Releasing the talk button finishes the utterance (held at start). */
    gsapi_EngineStop(engine);
    mp6_mic_set_talk_button(MP6_MIC_TALK_KEYBOARD, 1);
    gsapi_EngineSetMode(engine, 1);
    gsapi_EngineStart(engine);
    mp6_mic_host_pump();
    gsapi_EngineRestart(0);
    {
        u32 button = 0;
        assert(MICGetButton(1, &button) == MIC_RESULT_READY && button == MIC_BUTTON_TALK);
    }
    mp6_mic_set_talk_button(MP6_MIC_TALK_KEYBOARD, 0);
    gsapi_EngineRestart(0);
    mp6_mic_host_pump();
    assert(g_backendFinishes == 1);
    gsapi_EngineStop(engine);

    /* Off = an empty slot B again. */
    gsapi_ContextDeActivate(ctx);
    MICUnmount(1);
    assert(mp6_mic_in_use() == 0);
    mp6_mic_set_enabled(0);
    assert(MICProbeEx(1) == MIC_RESULT_NOCARD);
    printf("mic engine selftest: ok\n");
    return 0;
}
