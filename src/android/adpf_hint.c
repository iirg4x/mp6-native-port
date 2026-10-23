/* MP6 native port -- Android performance hints (ADPF) for the game thread.
 *
 * Each tick the game thread does a few ms of work (game logic, the real
 * frame's submit, Unlocked FPS replays) and then waits for the next tick.
 * Without a hint the CPU governor sees mostly-idle time and lowers the
 * clocks, which stretches that work: measured on a Galaxy S22+ (Towering
 * Treetop), game logic went from 3.3 to 4.9 ms per tick whenever the replay
 * window slept. APerformanceHint (Android 13+, libandroid.so) lets the
 * scheduler size the clocks to the real target instead: one session for the
 * game thread with the tick period as its target, and the measured active
 * work reported once per tick.
 *
 * Loaded with dlsym so the APK still runs on older Android (minSdk 28);
 * there it is a no-op. MP6_ADPF=0 disables it.
 *
 * The same file holds the display refresh vote (mp6_android_frame_rate_tick):
 * with Unlocked FPS on, the game's surface asks for the panel's highest
 * refresh rate through ANativeWindow_setFrameRate (Android 11+). Without a
 * vote the compositor picks a rate from its own content heuristics, and on a
 * Galaxy S22+ in Adaptive mode it held the game at 60 Hz (measured
 * 2026-10-21: every build, 40053 included, showed exactly 60 frames/s where
 * the same build had shown 112 that morning), so the in-between frames were
 * never displayed. MP6_FRAME_RATE_VOTE=0 disables it.
 */
#include <stdint.h>

#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <android/log.h>
#include <jni.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "mp6_host_section.h" /* statics are host state (tools/build.py carve-out) */

#ifdef __ANDROID__
typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
typedef APerformanceHintManager *(*PfnGetManager)(void);
typedef APerformanceHintSession *(*PfnCreateSession)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
typedef int (*PfnUpdateTarget)(APerformanceHintSession *, int64_t);
typedef int (*PfnReportActual)(APerformanceHintSession *, int64_t);

static int s_state; /* 0 untried, 1 active, -1 unavailable/disabled */
static APerformanceHintSession *s_session;
static PfnUpdateTarget s_updateTarget;
static PfnReportActual s_reportActual;
static int64_t s_targetNs;

static void adpf_init(int64_t targetNs)
{
    const char *env = getenv("MP6_ADPF");
    void *lib;
    PfnGetManager getManager;
    PfnCreateSession createSession;
    APerformanceHintManager *manager;
    int32_t tid;
    s_state = -1;
    if (env != NULL && *env == '0') return;
    lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (lib == NULL) {
        __android_log_print(ANDROID_LOG_INFO, "MP6", "ADPF unavailable: no libandroid.so");
        return;
    }
    getManager = (PfnGetManager)dlsym(lib, "APerformanceHint_getManager");
    createSession = (PfnCreateSession)dlsym(lib, "APerformanceHint_createSession");
    s_updateTarget = (PfnUpdateTarget)dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
    s_reportActual = (PfnReportActual)dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    if (getManager == NULL || createSession == NULL || s_updateTarget == NULL || s_reportActual == NULL) {
        __android_log_print(ANDROID_LOG_INFO, "MP6", "ADPF unavailable: API missing (Android 13+ needed)");
        return;
    }
    manager = getManager();
    if (manager == NULL) {
        __android_log_print(ANDROID_LOG_INFO, "MP6", "ADPF unavailable: no hint manager");
        return;
    }
    tid = (int32_t)gettid();
    s_session = createSession(manager, &tid, 1, targetNs);
    if (s_session == NULL) {
        typedef int64_t (*PfnRate)(APerformanceHintManager *);
        PfnRate rate = (PfnRate)dlsym(lib, "APerformanceHint_getPreferredUpdateRateNanos");
        __android_log_print(ANDROID_LOG_INFO, "MP6", "ADPF unavailable: the device declined a hint session "
                            "(tid %d, preferred update rate %lld ns)", (int)tid,
                            rate != NULL ? (long long)rate(manager) : -2LL);
        return;
    }
    s_targetNs = targetNs;
    s_state = 1;
    __android_log_print(ANDROID_LOG_INFO, "MP6", "ADPF performance hint session active (target %.2f ms)",
                        (double)targetNs / 1e6);
}

/* Once per tick from the game thread: the work the tick actually did and
 * the period it has to fit in. */
void mp6_adpf_report_tick(int64_t workNs, int64_t targetNs)
{
    if (workNs <= 0 || targetNs <= 0) return;
    if (s_state == 0) adpf_init(targetNs);
    if (s_state != 1) return;
    if (targetNs != s_targetNs) {
        s_updateTarget(s_session, targetNs);
        s_targetNs = targetNs;
    }
    s_reportActual(s_session, workNs);
}

/* ---- Display refresh vote -------------------------------------------- */
typedef int32_t (*PfnSetFrameRate)(void *window, float frameRate, int8_t compatibility);

static int s_voteState; /* 0 untried, 1 available, -1 unavailable/disabled */
static PfnSetFrameRate s_setFrameRate;
static void *s_votedWindow;
static float s_votedRate = -1.0f;

static float frame_rate_panel_max(SDL_Window *win)
{
    float best = 0.0f;
    SDL_DisplayID display = SDL_GetDisplayForWindow(win);
    int count = 0, i;
    SDL_DisplayMode **modes = display ? SDL_GetFullscreenDisplayModes(display, &count) : NULL;
    if (modes != NULL) {
        for (i = 0; i < count; ++i)
            if (modes[i] != NULL && modes[i]->refresh_rate > best) best = modes[i]->refresh_rate;
        SDL_free(modes);
    }
    if (best <= 0.0f) {
        const SDL_DisplayMode *cur = display ? SDL_GetCurrentDisplayMode(display) : NULL;
        if (cur != NULL) best = cur->refresh_rate;
    }
    return best;
}

/* Once per tick from the game thread. wantHigh: Unlocked FPS is presenting
 * frames between ticks, so the panel's highest rate is worth asking for;
 * otherwise the vote is cleared (0 = no preference). Re-applied whenever
 * the surface is recreated (the window pointer changes). */
void mp6_android_frame_rate_tick(int wantHigh)
{
    SDL_Window **windows;
    SDL_Window *win;
    void *native;
    int count = 0;
    float rate;
    if (s_voteState == 0) {
        const char *env = getenv("MP6_FRAME_RATE_VOTE");
        void *lib = (env != NULL && *env == '0') ? NULL : dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        s_setFrameRate = lib ? (PfnSetFrameRate)dlsym(lib, "ANativeWindow_setFrameRate") : NULL;
        s_voteState = s_setFrameRate ? 1 : -1;
    }
    if (s_voteState != 1) return;
    windows = SDL_GetWindows(&count);
    win = (windows != NULL && count > 0) ? windows[0] : NULL;
    SDL_free(windows);
    if (win == NULL) return;
    native = SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
    if (native == NULL) return;
    rate = wantHigh ? frame_rate_panel_max(win) : 0.0f;
    if (native == s_votedWindow && rate == s_votedRate) return;
    /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT (0): a game's own rate. */
    if (s_setFrameRate(native, rate, 0) == 0) {
        __android_log_print(ANDROID_LOG_INFO, "MP6", "display refresh vote: %.0f Hz (%s)", (double)rate,
                            rate > 0.0f ? "Unlocked FPS" : "cleared");
    }
    s_votedWindow = native;
    s_votedRate = rate;
}

/* The rate the panel is actually refreshing at right now
 * (Display.getRefreshRate through JNI), or 0 if unknown. SDL's display mode
 * reports the panel's mode list, which on an Adaptive Samsung panel stays at
 * 120 Hz while the compositor runs the game at 60. Called from the game
 * thread at most every few dozen ticks. */
float mp6_android_display_refresh_hz(void)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    float hz = 0.0f;
    if (env == NULL || activity == NULL) return 0.0f;
    {
        jclass actCls = (*env)->GetObjectClass(env, activity);
        jmethodID getWm = (*env)->GetMethodID(env, actCls, "getWindowManager", "()Landroid/view/WindowManager;");
        jobject wm = getWm ? (*env)->CallObjectMethod(env, activity, getWm) : NULL;
        if (wm != NULL && !(*env)->ExceptionCheck(env)) {
            jclass wmCls = (*env)->GetObjectClass(env, wm);
            jmethodID getDisp = (*env)->GetMethodID(env, wmCls, "getDefaultDisplay", "()Landroid/view/Display;");
            jobject disp = getDisp ? (*env)->CallObjectMethod(env, wm, getDisp) : NULL;
            if (disp != NULL && !(*env)->ExceptionCheck(env)) {
                jclass dispCls = (*env)->GetObjectClass(env, disp);
                jmethodID getRate = (*env)->GetMethodID(env, dispCls, "getRefreshRate", "()F");
                if (getRate != NULL) hz = (*env)->CallFloatMethod(env, disp, getRate);
                (*env)->DeleteLocalRef(env, dispCls);
                (*env)->DeleteLocalRef(env, disp);
            }
            (*env)->DeleteLocalRef(env, wmCls);
            (*env)->DeleteLocalRef(env, wm);
        }
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            hz = 0.0f;
        }
        (*env)->DeleteLocalRef(env, actCls);
    }
    (*env)->DeleteLocalRef(env, activity);
    return hz;
}
#else
float mp6_android_display_refresh_hz(void)
{
    return 0.0f;
}

void mp6_android_frame_rate_tick(int wantHigh)
{
    (void)wantHigh;
}

void mp6_adpf_report_tick(int64_t workNs, int64_t targetNs)
{
    (void)workNs;
    (void)targetNs;
}
#endif
