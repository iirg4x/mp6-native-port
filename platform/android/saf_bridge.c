/* MP6 native port -- Android SAF bridge implementation. See saf_bridge.h
 * for the design/threading contract and Mp6Activity.java for the Java
 * half (picker intent, DocumentsContract traversal, import thread).
 *
 * JNI discipline: the env comes from SDL_GetAndroidJNIEnv() (the SDL main
 * thread is Java-attached); the Mp6Activity class is resolved through
 * GetObjectClass on SDL_GetAndroidActivity()'s instance -- NOT FindClass,
 * whose system classloader cannot see app classes from native threads.
 * Method IDs are cached once. Every entry point checks + clears pending
 * Java exceptions so a Java-side failure degrades to an error return, not
 * a JNI abort. */
#include "saf_bridge.h"

#include <jni.h>
#include <stdio.h>
#include <string.h>

/* SDL3 SDL_system.h JNI getters, declared by hand (same discipline as
 * main_native.c's storage getters -- keeps this TU SDL-header-free). */
extern JNIEnv *SDL_GetAndroidJNIEnv(void);
extern void *SDL_GetAndroidActivity(void);

typedef struct SafJni {
    int ready;
    jclass activityClass; /* global ref */
    jmethodID openPicker;      /* static void mp6OpenFolderPicker() */
    jmethodID pollPick;        /* static String mp6PollFolderPick() */
    jmethodID importStart;     /* static boolean mp6TreeImportStart(String,String) */
    jmethodID importPoll;      /* static long[] mp6TreeImportPoll() */
    jmethodID importCurrent;   /* static String mp6TreeImportCurrent() */
    jmethodID importError;     /* static String mp6TreeImportError() */
    jmethodID importCancel;    /* static void mp6TreeImportCancel() */
} SafJni;

static SafJni g_jni;

static int saf_clear_exception(JNIEnv *env, const char *where)
{
    if ((*env)->ExceptionCheck(env)) {
        printf("[CONTENT] saf_bridge: Java exception in %s (cleared)\n", where);
        fflush(stdout);
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        return 1;
    }
    return 0;
}

static SafJni *saf_jni(void)
{
    if (g_jni.ready) return &g_jni;

    JNIEnv *env = SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    if (env == NULL || activity == NULL) {
        printf("[CONTENT] saf_bridge: no JNI env/activity available\n");
        fflush(stdout);
        return NULL;
    }
    jclass cls = (*env)->GetObjectClass(env, activity);
    if (cls == NULL || saf_clear_exception(env, "GetObjectClass")) return NULL;

    g_jni.openPicker = (*env)->GetStaticMethodID(env, cls, "mp6OpenFolderPicker", "()V");
    g_jni.pollPick = (*env)->GetStaticMethodID(env, cls, "mp6PollFolderPick", "()Ljava/lang/String;");
    g_jni.importStart =
        (*env)->GetStaticMethodID(env, cls, "mp6TreeImportStart", "(Ljava/lang/String;Ljava/lang/String;)Z");
    g_jni.importPoll = (*env)->GetStaticMethodID(env, cls, "mp6TreeImportPoll", "()[J");
    g_jni.importCurrent = (*env)->GetStaticMethodID(env, cls, "mp6TreeImportCurrent", "()Ljava/lang/String;");
    g_jni.importError = (*env)->GetStaticMethodID(env, cls, "mp6TreeImportError", "()Ljava/lang/String;");
    g_jni.importCancel = (*env)->GetStaticMethodID(env, cls, "mp6TreeImportCancel", "()V");
    if (saf_clear_exception(env, "GetStaticMethodID") || g_jni.openPicker == NULL ||
        g_jni.pollPick == NULL || g_jni.importStart == NULL || g_jni.importPoll == NULL ||
        g_jni.importCurrent == NULL || g_jni.importError == NULL || g_jni.importCancel == NULL) {
        printf("[CONTENT] saf_bridge: Mp6Activity mp6* methods not found (Java glue out of date?)\n");
        fflush(stdout);
        (*env)->DeleteLocalRef(env, cls);
        return NULL;
    }
    g_jni.activityClass = (jclass)(*env)->NewGlobalRef(env, cls);
    (*env)->DeleteLocalRef(env, cls);
    if (g_jni.activityClass == NULL) return NULL;
    g_jni.ready = 1;
    return &g_jni;
}

/* Copies a returned jstring into buf (empty string on NULL) and releases
 * the local ref. Returns the copied length. */
static size_t saf_take_string(JNIEnv *env, jstring js, char *buf, size_t n)
{
    buf[0] = '\0';
    if (js == NULL) return 0;
    const char *utf = (*env)->GetStringUTFChars(env, js, NULL);
    if (utf != NULL) {
        snprintf(buf, n, "%s", utf);
        (*env)->ReleaseStringUTFChars(env, js, utf);
    }
    (*env)->DeleteLocalRef(env, js);
    return strlen(buf);
}

int mp6_saf_open_tree_picker(void)
{
    SafJni *j = saf_jni();
    if (j == NULL) return -1;
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    (*env)->CallStaticVoidMethod(env, j->activityClass, j->openPicker);
    if (saf_clear_exception(env, "mp6OpenFolderPicker")) return -1;
    return 0;
}

int mp6_saf_poll_tree_pick(char *uriOut, size_t n)
{
    SafJni *j = saf_jni();
    if (j == NULL) return -1;
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    jstring js = (jstring)(*env)->CallStaticObjectMethod(env, j->activityClass, j->pollPick);
    if (saf_clear_exception(env, "mp6PollFolderPick")) return -1;
    char buf[1200];
    saf_take_string(env, js, buf, sizeof(buf));
    if (buf[0] == '\0') return 0;                 /* pending */
    if (strcmp(buf, "!cancelled") == 0) return -1; /* dismissed */
    snprintf(uriOut, n, "%s", buf);
    return 1;
}

int mp6_saf_tree_import_start(const char *treeUri, const char *destDiscRoot)
{
    SafJni *j = saf_jni();
    if (j == NULL || treeUri == NULL || destDiscRoot == NULL) return -1;
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    jstring juri = (*env)->NewStringUTF(env, treeUri);
    jstring jdest = (*env)->NewStringUTF(env, destDiscRoot);
    jboolean ok = JNI_FALSE;
    if (juri != NULL && jdest != NULL) {
        ok = (*env)->CallStaticBooleanMethod(env, j->activityClass, j->importStart, juri, jdest);
        if (saf_clear_exception(env, "mp6TreeImportStart")) ok = JNI_FALSE;
    }
    if (juri != NULL) (*env)->DeleteLocalRef(env, juri);
    if (jdest != NULL) (*env)->DeleteLocalRef(env, jdest);
    if (ok) {
        printf("[CONTENT] import (SAF folder) starting: %s -> %s\n", treeUri, destDiscRoot);
        fflush(stdout);
    }
    return ok ? 0 : -1;
}

void mp6_saf_tree_import_poll(Mp6ImportStatus *out)
{
    memset(out, 0, sizeof(*out));
    SafJni *j = saf_jni();
    if (j == NULL) {
        out->state = MP6_IMPORT_FAILED;
        snprintf(out->error, sizeof(out->error), "SAF bridge unavailable");
        return;
    }
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    jlongArray arr = (jlongArray)(*env)->CallStaticObjectMethod(env, j->activityClass, j->importPoll);
    if (saf_clear_exception(env, "mp6TreeImportPoll") || arr == NULL) {
        out->state = MP6_IMPORT_FAILED;
        snprintf(out->error, sizeof(out->error), "SAF import poll failed");
        return;
    }
    jlong vals[5] = { 0 };
    jsize len = (*env)->GetArrayLength(env, arr);
    if (len > 5) len = 5;
    (*env)->GetLongArrayRegion(env, arr, 0, len, vals);
    (*env)->DeleteLocalRef(env, arr);
    out->state = (int)vals[0];
    out->bytesDone = (uint64_t)vals[1];
    out->bytesTotal = (uint64_t)vals[2];
    out->filesDone = (int)vals[3];
    out->filesTotal = (int)vals[4];

    jstring cur = (jstring)(*env)->CallStaticObjectMethod(env, j->activityClass, j->importCurrent);
    if (!saf_clear_exception(env, "mp6TreeImportCurrent")) {
        saf_take_string(env, cur, out->currentFile, sizeof(out->currentFile));
    }
    if (out->state == MP6_IMPORT_FAILED) {
        jstring err = (jstring)(*env)->CallStaticObjectMethod(env, j->activityClass, j->importError);
        if (!saf_clear_exception(env, "mp6TreeImportError")) {
            saf_take_string(env, err, out->error, sizeof(out->error));
        }
        if (out->error[0] == '\0') snprintf(out->error, sizeof(out->error), "unknown Java-side error");
    }
}

void mp6_saf_tree_import_cancel(void)
{
    SafJni *j = saf_jni();
    if (j == NULL) return;
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    (*env)->CallStaticVoidMethod(env, j->activityClass, j->importCancel);
    saf_clear_exception(env, "mp6TreeImportCancel");
}
