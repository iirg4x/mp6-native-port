/* MP6 native port -- Android mic backend: the device's SpeechRecognizer
 * through packaging/android/.../Mp6Speech.java (include/mp6_mic.h has the
 * seam).
 *
 * The record permission is asked for the first time a mode actually
 * listens (SDL_RequestAndroidPermission, which routes through SDLActivity's
 * own onRequestPermissionsResult), never at launch. While the answer is
 * pending the session waits; a refusal is reported once and every later
 * session answers "heard nothing" at once, so the game's pad-free flows
 * cannot stall on a mic that will never hear anything.
 *
 * JNI discipline follows saf_bridge.c: everything here runs from
 * mp6_mic_host_pump on the main loop's real thread stack, classes come from
 * the activity's class loader, and every Java exception is cleared. */

#include "mp6_mic.h"

#ifdef __ANDROID__
#include <jni.h>
#include <SDL3/SDL.h>
#endif

#include <stdio.h>
#include <string.h>

/* SAVESTATE CARVE-OUT: JNI handles and permission state of the running
 * process. Must sit AFTER this TU's own includes and at TOP LEVEL. */
#include "mp6_host_section.h"

#ifdef __ANDROID__

#define SPEECH_MAX_PHRASES 160

static struct {
    int ready; /* 1 ok, -1 failed for good */
    jclass cls;
    jmethodID available, start, finish, stop;
} g_jni;

static volatile int g_permission; /* 0 unknown/pending, 1 granted, -1 denied */
static int g_permissionAsked;
static int g_pending;             /* a start waits for the permission answer */
static int g_pendingContinuous;
static int g_pendingCount;
static char g_pendingPhrase[SPEECH_MAX_PHRASES][64];
static int g_availableCache = -1;

static int speech_clear(JNIEnv *env, const char *where)
{
    if ((*env)->ExceptionCheck(env)) {
        printf("[MIC] Java exception in %s (cleared)\n", where);
        fflush(stdout);
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        return 1;
    }
    return 0;
}

static void JNICALL native_result(JNIEnv *env, jclass cls, jobjectArray alternatives, jfloatArray confidences,
                                  jboolean isFinal)
{
    const char *text[6];
    jstring strings[6];
    float conf[6];
    int count = 0, i;
    (void)cls;
    if (alternatives != NULL) {
        count = (*env)->GetArrayLength(env, alternatives);
        if (count > 6) count = 6;
    }
    if (confidences != NULL && (*env)->GetArrayLength(env, confidences) >= count && count > 0) {
        (*env)->GetFloatArrayRegion(env, confidences, 0, count, conf);
    } else {
        memset(conf, 0, sizeof(conf));
    }
    for (i = 0; i < count; i++) {
        strings[i] = (jstring)(*env)->GetObjectArrayElement(env, alternatives, i);
        text[i] = strings[i] != NULL ? (*env)->GetStringUTFChars(env, strings[i], NULL) : "";
    }
    mp6_mic_post_result(text, conf, count, isFinal ? 1 : 0);
    for (i = 0; i < count; i++) {
        if (strings[i] != NULL) {
            (*env)->ReleaseStringUTFChars(env, strings[i], text[i]);
            (*env)->DeleteLocalRef(env, strings[i]);
        }
    }
}

static void JNICALL native_stopped(JNIEnv *env, jclass cls, jstring reason)
{
    const char *text = reason != NULL ? (*env)->GetStringUTFChars(env, reason, NULL) : NULL;
    (void)cls;
    mp6_mic_post_stopped(text);
    if (text != NULL) (*env)->ReleaseStringUTFChars(env, reason, text);
}

static void JNICALL native_error(JNIEnv *env, jclass cls, jstring message)
{
    const char *text = message != NULL ? (*env)->GetStringUTFChars(env, message, NULL) : NULL;
    (void)cls;
    mp6_mic_post_error(text);
    mp6_mic_post_result(NULL, NULL, 0, 1);
    if (text != NULL) (*env)->ReleaseStringUTFChars(env, message, text);
}

static int speech_jni(JNIEnv **outEnv)
{
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    *outEnv = env;
    if (g_jni.ready != 0) return g_jni.ready == 1;
    g_jni.ready = -1;
    if (env == NULL) return 0;
    {
        static const JNINativeMethod natives[] = {
            { "nativeResult", "([Ljava/lang/String;[FZ)V", (void *)native_result },
            { "nativeStopped", "(Ljava/lang/String;)V", (void *)native_stopped },
            { "nativeError", "(Ljava/lang/String;)V", (void *)native_error },
        };
        jobject activity = (jobject)SDL_GetAndroidActivity();
        jclass activityClass, loaderClass;
        jobject loader;
        jmethodID getLoader, loadClass;
        jstring name;
        jclass cls;
        if (speech_clear(env, "SDL_GetAndroidActivity") || activity == NULL) return 0;
        activityClass = (*env)->GetObjectClass(env, activity);
        getLoader = (*env)->GetMethodID(env, activityClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
        loader = getLoader ? (*env)->CallObjectMethod(env, activity, getLoader) : NULL;
        (*env)->DeleteLocalRef(env, activity);
        (*env)->DeleteLocalRef(env, activityClass);
        if (speech_clear(env, "getClassLoader") || loader == NULL) return 0;
        loaderClass = (*env)->GetObjectClass(env, loader);
        loadClass = (*env)->GetMethodID(env, loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        name = (*env)->NewStringUTF(env, "com.mp6.game.Mp6Speech");
        cls = loadClass ? (jclass)(*env)->CallObjectMethod(env, loader, loadClass, name) : NULL;
        (*env)->DeleteLocalRef(env, name);
        (*env)->DeleteLocalRef(env, loader);
        (*env)->DeleteLocalRef(env, loaderClass);
        if (speech_clear(env, "loadClass(Mp6Speech)") || cls == NULL) return 0;
        if ((*env)->RegisterNatives(env, cls, natives, 3) != 0 || speech_clear(env, "RegisterNatives")) {
            (*env)->DeleteLocalRef(env, cls);
            return 0;
        }
        g_jni.available = (*env)->GetStaticMethodID(env, cls, "available", "()Z");
        g_jni.start = (*env)->GetStaticMethodID(env, cls, "start", "([Ljava/lang/String;Z)V");
        g_jni.finish = (*env)->GetStaticMethodID(env, cls, "finish", "()V");
        g_jni.stop = (*env)->GetStaticMethodID(env, cls, "stop", "()V");
        if (speech_clear(env, "GetStaticMethodID") || !g_jni.available || !g_jni.start || !g_jni.finish ||
            !g_jni.stop) {
            (*env)->DeleteLocalRef(env, cls);
            return 0;
        }
        g_jni.cls = (jclass)(*env)->NewGlobalRef(env, cls);
        (*env)->DeleteLocalRef(env, cls);
        g_jni.ready = 1;
    }
    return 1;
}

int mp6_mic_backend_available(void)
{
    JNIEnv *env;
    if (g_availableCache < 0) {
        if (!speech_jni(&env)) {
            g_availableCache = 0;
        } else {
            g_availableCache = (*env)->CallStaticBooleanMethod(env, g_jni.cls, g_jni.available) ? 1 : 0;
            if (speech_clear(env, "Mp6Speech.available")) g_availableCache = 0;
        }
        printf("[MIC] Android speech recognizer: %s\n", g_availableCache ? "available" : "not available");
        fflush(stdout);
    }
    return g_availableCache;
}

static void SDLCALL permission_answer(void *userdata, const char *permission, bool granted)
{
    (void)userdata; (void)permission;
    g_permission = granted ? 1 : -1;
}

static void speech_java_start(void)
{
    JNIEnv *env;
    jobjectArray array;
    jclass stringClass;
    int i;
    if (!speech_jni(&env)) return;
    stringClass = (*env)->FindClass(env, "java/lang/String");
    array = (*env)->NewObjectArray(env, g_pendingCount, stringClass, NULL);
    for (i = 0; array != NULL && i < g_pendingCount; i++) {
        jstring s = (*env)->NewStringUTF(env, g_pendingPhrase[i]);
        (*env)->SetObjectArrayElement(env, array, i, s);
        (*env)->DeleteLocalRef(env, s);
    }
    (*env)->CallStaticVoidMethod(env, g_jni.cls, g_jni.start, array, (jboolean)(g_pendingContinuous != 0));
    speech_clear(env, "Mp6Speech.start");
    if (array != NULL) (*env)->DeleteLocalRef(env, array);
    if (stringClass != NULL) (*env)->DeleteLocalRef(env, stringClass);
}

int mp6_mic_backend_start(const char *const *phrases, int count, int continuous)
{
    int i;
    if (count > SPEECH_MAX_PHRASES) count = SPEECH_MAX_PHRASES;
    g_pendingCount = count;
    g_pendingContinuous = continuous;
    for (i = 0; i < count; i++) snprintf(g_pendingPhrase[i], sizeof(g_pendingPhrase[i]), "%s", phrases[i]);
    if (g_permission < 0) {
        mp6_mic_post_result(NULL, NULL, 0, 1);
        return 1;
    }
    if (g_permission == 0 && !g_permissionAsked) {
        g_permissionAsked = 1;
        printf("[MIC] asking for the microphone permission\n");
        fflush(stdout);
        if (!SDL_RequestAndroidPermission("android.permission.RECORD_AUDIO", permission_answer, NULL)) {
            g_permission = -1;
        }
    }
    g_pending = 1;
    return 1;
}

void mp6_mic_backend_finish(void)
{
    JNIEnv *env;
    if (g_pending) { /* still waiting for the permission dialog */
        g_pending = 0;
        mp6_mic_post_result(NULL, NULL, 0, 1);
        return;
    }
    if (!speech_jni(&env)) return;
    (*env)->CallStaticVoidMethod(env, g_jni.cls, g_jni.finish);
    speech_clear(env, "Mp6Speech.finish");
}

void mp6_mic_backend_stop(void)
{
    JNIEnv *env;
    g_pending = 0;
    if (!speech_jni(&env)) return;
    (*env)->CallStaticVoidMethod(env, g_jni.cls, g_jni.stop);
    speech_clear(env, "Mp6Speech.stop");
}

void mp6_mic_backend_poll(void)
{
    if (!g_pending) return;
    if (g_permission > 0) {
        g_pending = 0;
        speech_java_start();
    } else if (g_permission < 0) {
        g_pending = 0;
        mp6_mic_post_error("Microphone permission is off for Starlit Party");
        mp6_mic_post_result(NULL, NULL, 0, 1);
    }
}

#endif /* __ANDROID__ */
