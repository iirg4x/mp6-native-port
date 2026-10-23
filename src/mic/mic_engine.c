/* MP6 native port -- the mic engine: the SDK's MIC driver, its M2S stream
 * helper and the third-party gsapi_* speech recognizer, as one native
 * replacement (include/mp6_mic.h has the design).
 *
 * game/mic.c stays the decomp's own code. It loads each mode's grammar from
 * /mic/ctx (.ctx/.gcd/.wrd/.bin), hands the files to gsapi_*, and turns the
 * engine's ranked candidates into game word numbers with the .bin table:
 * a candidate is a list of offsets into the .wrd file's spellings, and the
 * .bin file groups consecutive spellings into one game word. So the engine
 * only has to answer "which .wrd spelling was said", which it does by
 * matching what the platform recognizer heard against those spellings.
 *
 * Spellings come in two forms. Plain ones are the words themselves ("Fire",
 * "Drop bombs", "Gimme a hint", "12"). Pronunciation variants carry a
 * pseudo name ("m665_Goomba3", "micquiz_chara_Koopa_Kid0", "mselect_Fly_Guy0")
 * whose sound lived in the grammar's phoneme table; the capitalised part of
 * the name is the word, so it is matched as "goomba", "koopa kid", "fly guy".
 *
 * Game state: the context tables live in ordinary statics (captured by
 * savestates with the game that owns them); everything the running process
 * owns is in mic_host.c. */

#include "dolphin.h"
#include "dolphin/mic.h"
#include "dolphin/m2s.h"

#include "mp6_mic.h"
#include "mic_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * Text normalisation and matching (shared by every backend)
 * --------------------------------------------------------------------- */

static const char *const kSmallNumbers[] = {
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
    "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
    "seventeen", "eighteen", "nineteen"
};
static const char *const kTens[] = {
    "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"
};

/* Words recognizers commonly return for a spoken number or each other. */
static const char *const kSoundAlike[][2] = {
    { "to", "two" }, { "too", "two" }, { "for", "four" }, { "fore", "four" },
    { "won", "one" }, { "ate", "eight" }, { "gimme", "give me" },
    { "ok", "okay" }, { "yeah", "yes" }, { "yep", "yes" }, { "nope", "no" },
};

static int append_word(char *out, int len, int outSize, const char *word)
{
    int n = (int)strlen(word);
    if (len > 0 && len < outSize - 1) out[len++] = ' ';
    if (len + n >= outSize) n = outSize - 1 - len;
    if (n > 0) {
        memcpy(out + len, word, (size_t)n);
        len += n;
    }
    out[len] = '\0';
    return len;
}

static int append_token(char *out, int len, int outSize, const char *token)
{
    size_t i;
    int isNumber = token[0] != '\0';
    for (i = 0; token[i] != '\0'; i++) {
        if (token[i] < '0' || token[i] > '9') isNumber = 0;
    }
    if (isNumber && strlen(token) <= 2) {
        const int value = atoi(token);
        if (value < 20) return append_word(out, len, outSize, kSmallNumbers[value]);
        len = append_word(out, len, outSize, kTens[value / 10]);
        if (value % 10 != 0) len = append_word(out, len, outSize, kSmallNumbers[value % 10]);
        return len;
    }
    for (i = 0; i < sizeof(kSoundAlike) / sizeof(kSoundAlike[0]); i++) {
        if (strcmp(token, kSoundAlike[i][0]) == 0) {
            return append_word(out, len, outSize, kSoundAlike[i][1]);
        }
    }
    return append_word(out, len, outSize, token);
}

int mp6_mic_normalise(const char *text, char *out, int outSize)
{
    char token[64];
    int tokenLen = 0, len = 0;
    const unsigned char *p = (const unsigned char *)text;
    if (outSize <= 0) return 0;
    out[0] = '\0';
    for (;; p++) {
        const unsigned char c = *p;
        const int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                          c == '\'';
        if (alnum) {
            if (c != '\'' && tokenLen < (int)sizeof(token) - 1) {
                token[tokenLen++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
            }
            continue;
        }
        if (tokenLen > 0) {
            token[tokenLen] = '\0';
            len = append_token(out, len, outSize, token);
            tokenLen = 0;
        }
        if (c == '\0') break;
    }
    return len;
}

/* The word a .wrd spelling stands for, normalised. */
static void spelling_to_phrase(const char *raw, char *out, int outSize)
{
    char word[96];
    const char *underscore = strchr(raw, '_');
    out[0] = '\0';
    if (raw[0] == '.' || raw[0] == '\0') return; /* engine silence markers */
    if (underscore != NULL && raw[0] >= 'a' && raw[0] <= 'z') {
        /* Pseudo name: drop the lowercase prefix tokens and the variant
         * number, keep the capitalised word ("mgg_Mario_Party0"). */
        const char *p = raw;
        int n = 0;
        while (*p != '\0') {
            const char *end = strchr(p, '_');
            if (end == NULL) end = p + strlen(p);
            if (*p >= 'A' && *p <= 'Z') break;
            p = *end == '_' ? end + 1 : end;
        }
        for (; *p != '\0' && n < (int)sizeof(word) - 1; p++) word[n++] = *p == '_' ? ' ' : *p;
        while (n > 0 && word[n - 1] >= '0' && word[n - 1] <= '9') n--;
        word[n] = '\0';
        mp6_mic_normalise(word, out, outSize);
        return;
    }
    mp6_mic_normalise(raw, out, outSize);
}

static int levenshtein(const char *a, const char *b)
{
    int prev[MIC_HOST_ALT_LEN + 1], cur[MIC_HOST_ALT_LEN + 1];
    int la = (int)strlen(a), lb = (int)strlen(b), i, j;
    if (la > MIC_HOST_ALT_LEN) la = MIC_HOST_ALT_LEN;
    if (lb > MIC_HOST_ALT_LEN) lb = MIC_HOST_ALT_LEN;
    for (j = 0; j <= lb; j++) prev[j] = j;
    for (i = 1; i <= la; i++) {
        cur[0] = i;
        for (j = 1; j <= lb; j++) {
            int best = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            if (prev[j] + 1 < best) best = prev[j] + 1;
            if (cur[j - 1] + 1 < best) best = cur[j - 1] + 1;
            cur[j] = best;
        }
        memcpy(prev, cur, sizeof(int) * (size_t)(lb + 1));
    }
    return prev[lb];
}

static float similarity(const char *a, const char *b)
{
    const int la = (int)strlen(a), lb = (int)strlen(b);
    const int longest = la > lb ? la : lb;
    if (longest == 0) return 0.0f;
    return 1.0f - (float)levenshtein(a, b) / (float)longest;
}

/* How well a heard (normalised) text names a phrase, 0..1. */
static float phrase_score(const char *heard, const char *phrase)
{
    char padded[MIC_HOST_ALT_LEN + 2], needle[MIC_HOST_PHRASE_LEN + 2];
    float best;
    if (phrase[0] == '\0' || heard[0] == '\0') return 0.0f;
    if (strcmp(heard, phrase) == 0) return 1.0f;
    /* Said among other words ("fire fire", "uh, missile please"). */
    snprintf(padded, sizeof(padded), " %s ", heard);
    snprintf(needle, sizeof(needle), " %s ", phrase);
    if (strstr(padded, needle) != NULL) return 0.92f;
    /* Misheard spelling ("goomba" -> "gumba", "waluigi" -> "wa luigi"). */
    {
        char a[MIC_HOST_ALT_LEN], b[MIC_HOST_PHRASE_LEN];
        int i, n = 0;
        for (i = 0; heard[i] != '\0' && n < (int)sizeof(a) - 1; i++) if (heard[i] != ' ') a[n++] = heard[i];
        a[n] = '\0';
        n = 0;
        for (i = 0; phrase[i] != '\0' && n < (int)sizeof(b) - 1; i++) if (phrase[i] != ' ') b[n++] = phrase[i];
        b[n] = '\0';
        best = similarity(a, b);
    }
    return best * 0.9f;
}

/* ---------------------------------------------------------------------
 * Recognition contexts (one per loaded .ctx/.wrd)
 * --------------------------------------------------------------------- */

#define MIC_CONTEXTS 12
#define MIC_SPELLINGS 128
#define MIC_RAW_LEN 48

typedef struct MicContext {
    int used;
    int count;                             /* spellings, silence markers included */
    char raw[MIC_SPELLINGS][MIC_RAW_LEN];  /* as on disc, for gsapi_EngineGetParam */
    char phrase[MIC_SPELLINGS][MIC_HOST_PHRASE_LEN];
} MicContext;

/* game/mic.c's candidate node, field for field (MicResultNode_s). */
typedef struct MicResultNode {
    s32 status;
    u16 count;
    u16 score;
    s32 *result;
    struct MicResultNode *next;
} MicResultNode;

typedef void (*MicResultFn)(u32 context, u32 data, uintptr_t node);
typedef void (*MicNotifyFn)(u32 context, s32 event, u32 sample, u32 value);

enum { NOTIFY_THRESHOLD = 0, NOTIFY_LISTENING = 2, NOTIFY_STOP_LISTEN = 3 };

#define MIC_CANDIDATES 4
#define MIC_ACCEPT 0.62f
#define MIC_RUNNER_UP 0.12f

static MicContext g_ctx[MIC_CONTEXTS];
static int g_active = -1;       /* activated context */
static int g_mode = 1;          /* 1 = one utterance, 2 = continuous */
static int g_serial;            /* host session while listening, else 0 */
static int g_delivered;         /* one-shot: answered already */
static int g_interimDelivered;  /* this utterance already answered early */
static int g_talkAtStart, g_talkPrev;
static MicResultFn g_resultFn;
static MicNotifyFn g_notifyFn;
static int g_engineToken;
static MicResultNode g_nodes[MIC_CANDIDATES];
static s32 g_nodeWords[MIC_CANDIDATES][3];

/* Driver state (slot B). */
static int g_mounted, g_started;
static MICCallback g_detach;

static int ctx_slot(void *handle)
{
    const intptr_t slot = (intptr_t)handle - 1;
    return (slot >= 0 && slot < MIC_CONTEXTS && g_ctx[slot].used) ? (int)slot : -1;
}

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static void parse_wrd(MicContext *ctx, const u8 *wrd)
{
    const u32 size = be32(wrd);
    const u32 count = be32(wrd + 4);
    const char *p = (const char *)wrd + 8;
    const char *end = p + size;
    int i;
    ctx->count = 0;
    for (i = 0; i < (int)count && i < MIC_SPELLINGS && p < end; i++) {
        snprintf(ctx->raw[i], MIC_RAW_LEN, "%s", p);
        spelling_to_phrase(ctx->raw[i], ctx->phrase[i], MIC_HOST_PHRASE_LEN);
        p += strlen(p) + 1;
        ctx->count++;
    }
}

/* Distinct phrases of the active context, for the backend's grammar. */
static int active_phrases(char (*out)[MIC_HOST_PHRASE_LEN])
{
    int n = 0, i, j;
    if (g_active < 0) return 0;
    for (i = 0; i < g_ctx[g_active].count; i++) {
        const char *phrase = g_ctx[g_active].phrase[i];
        if (phrase[0] == '\0') continue;
        for (j = 0; j < n; j++) if (strcmp(out[j], phrase) == 0) break;
        if (j == n && n < MIC_HOST_MAX_PHRASES) {
            snprintf(out[n++], MIC_HOST_PHRASE_LEN, "%s", phrase);
        }
    }
    return n;
}

static void notify(s32 event)
{
    if (g_notifyFn != NULL) g_notifyFn(0, event, 0, 0);
}

static int deliver(const MicHostEvent *event, int interimOnly)
{
    const MicContext *ctx;
    float bestScore[MIC_CANDIDATES];
    int bestWord[MIC_CANDIDATES];
    int found = 0, a, i, k;

    if (g_active < 0 || g_resultFn == NULL) return 0;
    ctx = &g_ctx[g_active];
    for (a = 0; a < event->count; a++) {
        char heard[MIC_HOST_ALT_LEN];
        mp6_mic_normalise(event->alt[a], heard, sizeof(heard));
        for (i = 0; i < ctx->count; i++) {
            float score = phrase_score(heard, ctx->phrase[i]);
            int slot, dup = 0;
            if (interimOnly && score < 1.0f) continue;
            score -= 0.04f * (float)a;
            if (score < MIC_ACCEPT) continue;
            /* One entry per phrase: keep its best spelling's score. */
            for (k = 0; k < found; k++) {
                if (strcmp(ctx->phrase[bestWord[k]], ctx->phrase[i]) == 0) {
                    dup = 1;
                    if (score > bestScore[k]) bestScore[k] = score;
                    break;
                }
            }
            if (dup) continue;
            if (found < MIC_CANDIDATES) slot = found++;
            else {
                slot = MIC_CANDIDATES - 1;
                if (score <= bestScore[slot]) continue;
            }
            bestScore[slot] = score;
            bestWord[slot] = i;
            /* keep sorted, best first */
            for (k = slot; k > 0 && bestScore[k] > bestScore[k - 1]; k--) {
                const float s = bestScore[k];
                const int w = bestWord[k];
                bestScore[k] = bestScore[k - 1];
                bestWord[k] = bestWord[k - 1];
                bestScore[k - 1] = s;
                bestWord[k - 1] = w;
            }
        }
    }
    if (interimOnly && found == 0) return 0;

    memset(g_nodes, 0, sizeof(g_nodes));
    k = 0;
    for (i = 0; i < found; i++) {
        if (i > 0 && bestScore[i] < bestScore[0] - MIC_RUNNER_UP) break;
        g_nodeWords[k][0] = -1;
        g_nodeWords[k][1] = bestWord[i];
        g_nodeWords[k][2] = -1;
        g_nodes[k].count = 3; /* boundary, word, boundary */
        g_nodes[k].score = (u16)(bestScore[i] >= 1.0f ? 10000 : (int)(bestScore[i] * 10000.0f));
        g_nodes[k].result = g_nodeWords[k];
        if (k > 0) g_nodes[k - 1].next = &g_nodes[k];
        k++;
    }
    printf("[MIC] answer: %s (%d)\n", k > 0 ? ctx->raw[bestWord[0]] : "<nothing>",
           k > 0 ? g_nodes[0].score : 0);
    fflush(stdout);
    g_resultFn(0, 0, (uintptr_t)&g_nodes[0]);
    if (g_mode == 1) g_delivered = 1;
    return 1;
}

static void engine_pump(void)
{
    MicHostEvent event;
    const int talk = mic_host_talk_held();

    if (g_serial == 0) {
        g_talkPrev = talk;
        return;
    }
    /* Releasing the talk button ends the utterance, as on the real Mic. */
    if (g_talkAtStart && g_talkPrev && !talk) mic_host_request_finish(g_serial);
    g_talkPrev = talk;

    while (mic_host_take(g_serial, &event)) {
        if (event.type == MIC_HOST_EVENT_STOPPED) {
            notify(NOTIFY_STOP_LISTEN);
            continue;
        }
        if (g_delivered) continue;
        if (!event.final) {
            /* An interim hypothesis answers early only when it names a word
             * exactly; its utterance's final result is then skipped. */
            if (!g_interimDelivered && deliver(&event, 1)) g_interimDelivered = 1;
            continue;
        }
        if (g_interimDelivered) {
            g_interimDelivered = 0; /* the final of an utterance already answered */
            continue;
        }
        deliver(&event, 0);
    }
}

/* ---------------------------------------------------------------------
 * gsapi_* (signatures as game/mic.c calls them; include/mp6_gsapi_compat.h)
 * --------------------------------------------------------------------- */

int gsapi_Init(MicResultFn resultFn, int unused)
{
    (void)unused;
    g_resultFn = resultFn;
    return 0;
}

int gsapi_Close(void) { return 0; }
int gsapi_SetUserData(int value) { (void)value; return 0; }
int gsapi_LanguageLoadBuffer(void *data, int flags) { (void)data; (void)flags; return 0; }
int gsapi_LanguageUnLoad(void) { return 0; }

int gsapi_NotifySetCallback(MicNotifyFn notifyFn)
{
    g_notifyFn = notifyFn;
    return 0;
}

int gsapi_EngineOpen(int flags, void **engine)
{
    (void)flags;
    *engine = &g_engineToken;
    return 0;
}

int gsapi_EngineClose(void *engine)
{
    (void)engine;
    if (g_serial != 0) mic_host_request_stop();
    g_serial = 0;
    return 0;
}

int gsapi_EngineSetMode(void *engine, int mode)
{
    (void)engine;
    g_mode = mode;
    return 0;
}

int gsapi_EngineSetParam(void *engine, int param, int value)
{
    (void)engine; (void)param; (void)value;
    return 0;
}

int gsapi_EngineGetParam(void *engine, int wordOffset, char **name)
{
    (void)engine;
    if (g_active >= 0 && wordOffset >= 0 && wordOffset < g_ctx[g_active].count) {
        *name = g_ctx[g_active].raw[wordOffset];
        return 0;
    }
    *name = (char *)"";
    return -1;
}

int gsapi_ContextSetCtxData(void *ctxData, void **context)
{
    int slot;
    (void)ctxData;
    for (slot = 0; slot < MIC_CONTEXTS; slot++) {
        if (!g_ctx[slot].used) break;
    }
    if (slot == MIC_CONTEXTS) {
        *context = NULL;
        return -1;
    }
    memset(&g_ctx[slot], 0, sizeof(g_ctx[slot]));
    g_ctx[slot].used = 1;
    *context = (void *)(intptr_t)(slot + 1);
    return 0;
}

int gsapi_ContextSetGcdData(void *context, void *gcdData)
{
    (void)context; (void)gcdData;
    return 0;
}

int gsapi_ContextSetWrdData(void *context, void *wrdData)
{
    const int slot = ctx_slot(context);
    if (slot < 0 || wrdData == NULL) return -1;
    parse_wrd(&g_ctx[slot], (const u8 *)wrdData);
    printf("[MIC] word list %d: %d spellings (%s, %s, ...)\n", slot, g_ctx[slot].count,
           g_ctx[slot].count > 0 ? g_ctx[slot].phrase[0] : "", g_ctx[slot].count > 1 ? g_ctx[slot].phrase[1] : "");
    fflush(stdout);
    return 0;
}

int gsapi_ContextSetParam(void *context, int param, int value)
{
    (void)context; (void)param; (void)value;
    return 0;
}

int gsapi_ContextActivate(void *engine, void *context)
{
    const int slot = ctx_slot(context);
    (void)engine;
    if (slot < 0) return -1;
    g_active = slot;
    return 0;
}

int gsapi_ContextDeActivate(void *context)
{
    const int slot = ctx_slot(context);
    if (slot < 0) return -1;
    if (g_active == slot) {
        if (g_serial != 0) mic_host_request_stop();
        g_serial = 0;
        g_active = -1;
    }
    g_ctx[slot].used = 0;
    return 0;
}

int gsapi_EngineStart(void *engine)
{
    static char phrases[MIC_HOST_MAX_PHRASES][MIC_HOST_PHRASE_LEN];
    int count;
    (void)engine;
    if (g_active < 0) return -1;
    count = active_phrases(phrases);
    printf("[MIC] listening (%s) for %d words of list %d\n", g_mode == 2 ? "continuous" : "one answer", count,
           g_active);
    fflush(stdout);
    g_serial = mic_host_request_start((const char (*)[MIC_HOST_PHRASE_LEN])phrases, count, g_mode == 2);
    g_delivered = 0;
    g_interimDelivered = 0;
    g_talkAtStart = mic_host_talk_held();
    g_talkPrev = g_talkAtStart;
    notify(NOTIFY_LISTENING);
    return 0;
}

int gsapi_EngineRestart(int unused)
{
    (void)unused;
    engine_pump();
    return 0;
}

int gsapi_EngineStop(void *engine)
{
    (void)engine;
    if (g_serial != 0) {
        /* Collect a result that arrived since the last frame first. */
        engine_pump();
        mic_host_request_stop();
    }
    g_serial = 0;
    return 0;
}

/* Speaker adaptation sessions: the original engine exported per-player
 * voice profiles here. Nothing to carry; leave the game's handles alone
 * (one of its export targets is a u32 slot that cannot hold a pointer). */
int gsapi_EngineSessionDataExport(void *engine, void *out)
{
    (void)engine; (void)out;
    return 0;
}

int gsapi_EngineSessionDataImport(void *engine, void *data, int flags)
{
    (void)engine; (void)data; (void)flags;
    return 0;
}

int gsapi_EngineSessionDataFree(void *data)
{
    (void)data;
    return 0;
}

/* ---------------------------------------------------------------------
 * MIC driver (slot B) and M2S
 * --------------------------------------------------------------------- */

void MICInit(void) {}

s32 MICProbeEx(s32 chan)
{
    if (chan != 1) return MIC_RESULT_NOCARD;
    return mp6_mic_backend_available_now() ? MIC_RESULT_READY : MIC_RESULT_NOCARD;
}

s32 MICMount(s32 chan, s16 *buffer, s32 size, MICCallback detachCallback)
{
    (void)buffer; (void)size;
    if (MICProbeEx(chan) != MIC_RESULT_READY) return MIC_RESULT_NOCARD;
    g_detach = detachCallback;
    if (!g_mounted) {
        printf("[MIC] slot B mounted\n");
        fflush(stdout);
    }
    g_mounted = 1;
    mic_host_set_in_use(1);
    return MIC_RESULT_READY;
}

s32 MICUnmount(s32 chan)
{
    if (chan != 1) return MIC_RESULT_NOCARD;
    if (g_serial != 0) mic_host_request_stop();
    g_serial = 0;
    g_mounted = 0;
    g_started = 0;
    mic_host_set_in_use(0);
    return MIC_RESULT_READY;
}

s32 MICGetDeviceID(s32 chan, u32 *id)
{
    if (chan != 1 || !g_mounted) return MIC_RESULT_NOCARD;
    *id = 0; /* the Microphone, not some other slot device */
    return MIC_RESULT_READY;
}

s32 MICSetGain(s32 chan, s32 gain)
{
    (void)gain;
    return (chan == 1 && g_mounted) ? MIC_RESULT_READY : MIC_RESULT_NOCARD;
}

s32 MICStart(s32 chan)
{
    if (chan != 1 || !g_mounted) return MIC_RESULT_NOCARD;
    g_started = 1;
    return MIC_RESULT_READY;
}

s32 MICStop(s32 chan)
{
    if (chan != 1) return MIC_RESULT_NOCARD;
    g_started = 0;
    return MIC_RESULT_READY;
}

BOOL MICIsActive(s32 chan)
{
    return chan == 1 && g_mounted && g_started;
}

BOOL MICIsAttached(s32 chan)
{
    return chan == 1 && g_mounted;
}

s32 MICGetButton(s32 chan, u32 *button)
{
    if (chan != 1 || !g_mounted) return MIC_RESULT_NOCARD;
    *button = mic_host_talk_held() ? MIC_BUTTON_TALK : 0;
    return MIC_RESULT_READY;
}

/* The meter only wants the newest audio: a reader further behind than this
 * skips ahead instead of replaying a backlog. */
#define MIC_PCM_MAX_LAG 4096

s32 MICGetSamples(s32 chan, s16 *buffer, s32 index, s32 samples)
{
    const uint32_t head = mic_host_pcm_head();
    if (!MICIsActive(chan) || samples <= 0) return 0;
    if (head - (uint32_t)index > MIC_PCM_MAX_LAG) {
        index = (s32)(head - (uint32_t)(samples < MIC_PCM_MAX_LAG ? samples : MIC_PCM_MAX_LAG));
    }
    return mic_host_pcm_read((uint32_t)index, (int16_t *)buffer, (int)samples);
}

s32 MICGetSamplesLeft(s32 chan, s32 index)
{
    if (!MICIsActive(chan)) return 0;
    return (s32)(mic_host_pcm_head() - (uint32_t)index);
}

s32 MICUpdateIndex(s32 chan, s32 index, s32 samples)
{
    const uint32_t head = mic_host_pcm_head();
    const uint32_t next = (uint32_t)index + (uint32_t)samples;
    (void)chan;
    return (s32)(head - next > MIC_PCM_MAX_LAG ? head : next);
}

void M2SInit(void) {}
BOOL M2SSetBuffer(s16 *buffer) { (void)buffer; return TRUE; }
BOOL M2SSetActiveChannel(s32 chan) { (void)chan; return TRUE; }
BOOL M2SSetMode(s32 mode) { (void)mode; return TRUE; }
BOOL M2SSetPrerecordSamples(s32 milliseconds) { (void)milliseconds; return TRUE; }
BOOL M2SSetShifts(s32 shifts) { (void)shifts; return TRUE; }
void M2SOpen(void) {}
void M2SClose(void) {}
void M2SStart(void) {}
void M2SStop(void) {}
s32 M2SGetSamples(s16 **buffer, s32 samples) { (void)buffer; (void)samples; return 0; }
s32 M2SGetSamplesLeft(void) { return 0; }
void M2SAdvanceBuffer(s32 samples) { (void)samples; }
