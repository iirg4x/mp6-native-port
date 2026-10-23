/* MP6 native port -- the mic host: process-owned half of the GameCube
 * Microphone (include/mp6_mic.h has the design).
 *
 * Owns everything that describes the RUNNING process rather than the game:
 * the published setting, the talk button sources, the backend session the
 * engine asked for, the result queue backends post into (from any thread),
 * and the PCM ring behind the Options mic test. mp6_mic_host_pump() is the
 * only caller of the platform backend; it runs once per frame on the main
 * loop's real thread stack.
 *
 * MP6_MIC_SCRIPT replaces the backend with a scripted one for automated
 * runs: a comma list of things to "say", one per listening session (or one
 * every ~half second while continuous listening is on). The token "hold"
 * holds the talk button for the whole run ("hold" alone keeps the real
 * recognizer); "silence" says nothing usable. */

#include "mp6_mic.h"
#include "mic_internal.h"
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SAVESTATE CARVE-OUT: backend sessions, the queue and the setting belong to
 * the running process. Must sit AFTER this TU's own includes and at
 * preprocessor TOP LEVEL. See mp6_host_section.h. */
#include "mp6_host_section.h"

#define MIC_QUEUE_SIZE 8
#define MIC_PCM_RING 32768 /* power of two */

/* Off until the launcher publishes game.microphone (default on there):
 * automation runs never read the config, and must not open the microphone
 * unless they ask for it (MP6_MIC=1 or MP6_MIC_SCRIPT). */
static int g_setting = 0;
static int g_env = -2; /* -2 unread, -1 unset, else 0/1 */
static volatile unsigned g_talk;
static int g_inUse;

static int g_mutexReady;
static Mp6Mutex g_mutex;

/* What the engine wants (written on the game thread, read by the pump). */
static struct {
    int serial;      /* session the engine wants; 0 = none */
    int finish;      /* serial whose utterance should finish now */
    int continuous;
    int count;
    char phrase[MIC_HOST_MAX_PHRASES][MIC_HOST_PHRASE_LEN];
} g_want;
static int g_nextSerial;

/* What the backend is doing (pump only, plus g_postSerial under the mutex). */
static int g_runSerial;
static int g_runFinished;
static int g_postSerial; /* session that backend posts are tagged with */

static MicHostEvent g_queue[MIC_QUEUE_SIZE];
static int g_queueSerial[MIC_QUEUE_SIZE];
static int g_queueHead, g_queueCount;

static char g_status[128];
static int g_failed;

static int16_t g_pcm[MIC_PCM_RING];
static uint32_t g_pcmHead;
static int g_pcmOpen;
static int g_pcmIdle = 1000;

static void mic_lock(void)
{
    if (!g_mutexReady) {
        mp6_host_mutex_init(&g_mutex);
        g_mutexReady = 1;
    }
    mp6_host_mutex_lock(&g_mutex);
}

static void mic_unlock(void)
{
    mp6_host_mutex_unlock(&g_mutex);
}

static int mic_env(void)
{
    if (g_env == -2) {
        const char *value = getenv("MP6_MIC");
        if (value == NULL || value[0] == '\0') g_env = -1;
        else g_env = (value[0] == '0' && value[1] == '\0') ? 0 : 1;
    }
    return g_env;
}

void mp6_mic_set_enabled(int enabled)
{
    enabled = enabled ? 1 : 0;
    if (enabled != g_setting) {
        printf("[MIC] Microphone: %s\n", enabled ? "on" : "off");
        fflush(stdout);
    }
    g_setting = enabled;
}

int mp6_mic_enabled(void)
{
    const int env = mic_env();
    if (env >= 0) return env;
    if (getenv("MP6_MIC_SCRIPT") != NULL) return 1;
    return g_setting;
}

int mp6_mic_env_override(void)
{
    return mic_env() >= 0;
}

void mp6_mic_set_talk_button(unsigned source, int held)
{
    if (held) g_talk |= source;
    else g_talk &= ~source;
}

int mic_host_talk_held(void)
{
    return g_talk != 0;
}

void mic_host_set_in_use(int inUse)
{
    g_inUse = inUse;
}

int mp6_mic_in_use(void)
{
    if (!g_inUse) return 0;
    return g_want.serial != 0 ? 2 : 1;
}

const char *mp6_mic_status_text(void)
{
    return g_status;
}

int mic_host_failed(void)
{
    return g_failed;
}

/* ---------------------------------------------------------------------
 * Engine side
 * --------------------------------------------------------------------- */

int mic_host_request_start(const char (*phrases)[MIC_HOST_PHRASE_LEN], int count, int continuous)
{
    if (count > MIC_HOST_MAX_PHRASES) count = MIC_HOST_MAX_PHRASES;
    mic_lock();
    if (++g_nextSerial <= 0) g_nextSerial = 1;
    g_want.serial = g_nextSerial;
    g_want.finish = 0;
    g_want.continuous = continuous;
    g_want.count = count;
    memcpy(g_want.phrase, phrases, (size_t)count * MIC_HOST_PHRASE_LEN);
    mic_unlock();
    return g_want.serial;
}

void mic_host_request_finish(int serial)
{
    mic_lock();
    if (g_want.serial == serial) g_want.finish = serial;
    mic_unlock();
}

void mic_host_request_stop(void)
{
    mic_lock();
    g_want.serial = 0;
    g_want.finish = 0;
    mic_unlock();
}

int mic_host_take(int serial, MicHostEvent *out)
{
    int found = 0;
    mic_lock();
    while (g_queueCount > 0) {
        const int slot = g_queueHead;
        g_queueHead = (g_queueHead + 1) % MIC_QUEUE_SIZE;
        g_queueCount--;
        if (g_queueSerial[slot] == serial) {
            *out = g_queue[slot];
            found = 1;
            break;
        }
    }
    mic_unlock();
    return found;
}

uint32_t mic_host_pcm_head(void)
{
    uint32_t head;
    mic_lock();
    head = g_pcmHead;
    g_pcmIdle = 0;
    mic_unlock();
    return head;
}

int mic_host_pcm_read(uint32_t index, int16_t *dst, int count)
{
    uint32_t avail;
    int i;
    mic_lock();
    g_pcmIdle = 0;
    avail = g_pcmHead - index;
    if (avail > MIC_PCM_RING) { /* fell behind: skip to the oldest kept */
        index = g_pcmHead - MIC_PCM_RING;
        avail = MIC_PCM_RING;
    }
    if ((uint32_t)count > avail) count = (int)avail;
    for (i = 0; i < count; i++) dst[i] = g_pcm[(index + (uint32_t)i) & (MIC_PCM_RING - 1)];
    mic_unlock();
    return count;
}

/* ---------------------------------------------------------------------
 * Backend side (any thread)
 * --------------------------------------------------------------------- */

static void mic_queue_push(const MicHostEvent *event)
{
    int slot;
    mic_lock();
    if (g_postSerial != 0) {
        if (g_queueCount == MIC_QUEUE_SIZE) { /* drop the oldest */
            g_queueHead = (g_queueHead + 1) % MIC_QUEUE_SIZE;
            g_queueCount--;
        }
        slot = (g_queueHead + g_queueCount) % MIC_QUEUE_SIZE;
        g_queue[slot] = *event;
        g_queueSerial[slot] = g_postSerial;
        g_queueCount++;
    }
    mic_unlock();
}

void mp6_mic_post_result(const char *const *alternatives, const float *confidences,
                         int count, int final)
{
    MicHostEvent event;
    int i;
    memset(&event, 0, sizeof(event));
    event.type = MIC_HOST_EVENT_RESULT;
    event.final = final ? 1 : 0;
    if (count > MIC_HOST_MAX_ALTS) count = MIC_HOST_MAX_ALTS;
    for (i = 0; i < count; i++) {
        if (alternatives[i] == NULL) break;
        snprintf(event.alt[i], MIC_HOST_ALT_LEN, "%s", alternatives[i]);
        event.confidence[i] = confidences != NULL ? confidences[i] : 0.0f;
    }
    event.count = i;
    if (final) {
        printf("[MIC] heard \"%s\"%s\n", event.count > 0 ? event.alt[0] : "",
               event.count > 1 ? " (+alternatives)" : "");
        fflush(stdout);
    }
    mic_queue_push(&event);
}

void mp6_mic_post_stopped(const char *reason)
{
    MicHostEvent event;
    memset(&event, 0, sizeof(event));
    event.type = MIC_HOST_EVENT_STOPPED;
    printf("[MIC] listening stopped: %s\n", reason != NULL ? reason : "");
    fflush(stdout);
    mic_queue_push(&event);
}

void mp6_mic_post_error(const char *message)
{
    mic_lock();
    snprintf(g_status, sizeof(g_status), "%s", message != NULL ? message : "");
    g_failed = 1;
    mic_unlock();
    printf("[MIC] error: %s\n", g_status);
    fflush(stdout);
}

/* ---------------------------------------------------------------------
 * Weak "no microphone" backend: headless and any platform whose backend TU
 * is not linked. A real backend TU overrides every one of these.
 * --------------------------------------------------------------------- */

__attribute__((weak)) int mp6_mic_backend_available(void) { return 0; }
__attribute__((weak)) int mp6_mic_backend_start(const char *const *phrases, int count, int continuous)
{
    (void)phrases; (void)count; (void)continuous;
    return 0;
}
__attribute__((weak)) void mp6_mic_backend_finish(void) {}
__attribute__((weak)) void mp6_mic_backend_stop(void) {}
__attribute__((weak)) void mp6_mic_backend_poll(void) {}
__attribute__((weak)) int mp6_mic_pcm_open(void) { return 0; }
__attribute__((weak)) int mp6_mic_pcm_read(int16_t *dst, int maxSamples)
{
    (void)dst; (void)maxSamples;
    return 0;
}
__attribute__((weak)) void mp6_mic_pcm_close(void) {}

/* ---------------------------------------------------------------------
 * Scripted backend (MP6_MIC_SCRIPT)
 * --------------------------------------------------------------------- */

static int g_scriptRead;
static char g_scriptBuf[512];
static char *g_scriptItem[32];
static int g_scriptCount, g_scriptNext;
static int g_scriptDelay, g_scriptContinuous;

static int script_active(void)
{
    if (!g_scriptRead) {
        const char *value = getenv("MP6_MIC_SCRIPT");
        char *cursor;
        g_scriptRead = 1;
        if (value != NULL && value[0] != '\0') {
            snprintf(g_scriptBuf, sizeof(g_scriptBuf), "%s", value);
            cursor = g_scriptBuf;
            while (cursor != NULL && g_scriptCount < 32) {
                char *comma = strchr(cursor, ',');
                if (comma != NULL) *comma++ = '\0';
                if (strcmp(cursor, "hold") == 0) mp6_mic_set_talk_button(MP6_MIC_TALK_SCRIPT, 1);
                else if (cursor[0] != '\0') g_scriptItem[g_scriptCount++] = cursor;
                cursor = comma;
            }
            if (g_scriptCount == 0) {
                /* "hold" alone: the real recognizer with the button held. */
                g_scriptBuf[0] = '\0';
            } else {
                printf("[MIC] scripted microphone: %d phrase(s)\n", g_scriptCount);
                fflush(stdout);
            }
        }
    }
    return g_scriptBuf[0] != '\0';
}

static void script_say(void)
{
    const char *item = g_scriptItem[g_scriptNext++ % g_scriptCount];
    if (strcmp(item, "silence") == 0) mp6_mic_post_result(NULL, NULL, 0, 1);
    else mp6_mic_post_result(&item, NULL, 1, 1);
}

static int backend_available(void)
{
    return script_active() ? 1 : mp6_mic_backend_available();
}

static int backend_start(const char *const *phrases, int count, int continuous)
{
    if (!script_active()) return mp6_mic_backend_start(phrases, count, continuous);
    g_scriptDelay = 20;
    g_scriptContinuous = continuous;
    return 1;
}

static void backend_finish(void)
{
    if (!script_active()) {
        mp6_mic_backend_finish();
    } else if (g_scriptDelay > 0) {
        g_scriptDelay = 0;
        script_say();
    }
}

static void backend_stop(void)
{
    if (!script_active()) mp6_mic_backend_stop();
    g_scriptDelay = 0;
}

static void backend_poll(void)
{
    if (!script_active()) {
        mp6_mic_backend_poll();
        return;
    }
    if (g_scriptDelay > 0 && --g_scriptDelay == 0) {
        script_say();
        if (g_scriptContinuous) g_scriptDelay = 30;
    }
}

/* -1 = not asked yet. Asked once: a recognizer does not come and go. */
static int g_backendAvailable = -1;

int mp6_mic_backend_available_now(void)
{
#ifndef __ANDROID__
    /* Android's probe is a JNI call, which must wait for the pump (game code
     * probes from coroutine stacks); everywhere else it is safe right away,
     * so the boot-time probe already sees the mic. */
    if (g_backendAvailable < 0) g_backendAvailable = backend_available();
#endif
    return mp6_mic_enabled() && !g_failed && g_backendAvailable == 1;
}

/* ---------------------------------------------------------------------
 * The pump (main loop, real thread stack)
 * --------------------------------------------------------------------- */

void mp6_mic_host_pump(void)
{
    static const char *phrasePtr[MIC_HOST_MAX_PHRASES];
    static char phraseCopy[MIC_HOST_MAX_PHRASES][MIC_HOST_PHRASE_LEN];
    int wantSerial, wantFinish, continuous, count, i;

    if (g_backendAvailable < 0) g_backendAvailable = backend_available();

    mic_lock();
    wantSerial = g_want.serial;
    wantFinish = g_want.finish;
    continuous = g_want.continuous;
    count = g_want.count;
    if (wantSerial != g_runSerial && wantSerial != 0) {
        memcpy(phraseCopy, g_want.phrase, (size_t)count * MIC_HOST_PHRASE_LEN);
    }
    mic_unlock();

    if (wantSerial != g_runSerial) {
        if (g_runSerial != 0) {
            mic_lock();
            g_postSerial = 0;
            mic_unlock();
            backend_stop();
        }
        g_runSerial = 0;
        if (wantSerial != 0) {
            for (i = 0; i < count; i++) phrasePtr[i] = phraseCopy[i];
            mic_lock();
            g_postSerial = wantSerial;
            mic_unlock();
            if (!backend_start(phrasePtr, count, continuous)) {
                mp6_mic_post_stopped("backend could not start");
            }
            g_runFinished = 0;
        }
        g_runSerial = wantSerial;
    }
    if (g_runSerial != 0 && wantFinish == g_runSerial && !g_runFinished) {
        g_runFinished = 1;
        backend_finish();
    }
    if (g_runSerial != 0) backend_poll();

    /* PCM for the volume meter: open while the game keeps reading it. */
    if (g_pcmIdle < 120) {
        g_pcmIdle++;
        if (!g_pcmOpen) g_pcmOpen = mp6_mic_pcm_open() ? 1 : -1;
        if (g_pcmOpen == 1) {
            int16_t chunk[1024];
            int got;
            while ((got = mp6_mic_pcm_read(chunk, 1024)) > 0) {
                mic_lock();
                for (i = 0; i < got; i++) g_pcm[(g_pcmHead + (uint32_t)i) & (MIC_PCM_RING - 1)] = chunk[i];
                g_pcmHead += (uint32_t)got;
                mic_unlock();
            }
        }
    } else if (g_pcmOpen != 0) {
        if (g_pcmOpen == 1) mp6_mic_pcm_close();
        g_pcmOpen = 0;
    }
}
