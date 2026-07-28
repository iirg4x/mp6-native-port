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
#include "saf_safe.h"

#include <jni.h>
#include <stdio.h>
#include <string.h>

/* g_jni contains process-local JavaVM/class/method handles. A savestate must
 * never copy those foreign JNI pointers into another process. Keep this after
 * all headers so only this TU's own file-scope state is carved out. */
#include "mp6_host_section.h"

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
    SafJni candidate;
    JNIEnv *env;
    jobject activity;
    jclass cls = NULL;
    if (g_jni.ready) return &g_jni;

    memset(&candidate, 0, sizeof(candidate));
    env = SDL_GetAndroidJNIEnv();
    if (env == NULL) {
        printf("[CONTENT] saf_bridge: no JNI env available\n");
        fflush(stdout);
        return NULL;
    }
    activity = (jobject)SDL_GetAndroidActivity();
    if (saf_clear_exception(env, "SDL_GetAndroidActivity") || activity == NULL) {
        if (activity != NULL) (*env)->DeleteLocalRef(env, activity);
        printf("[CONTENT] saf_bridge: no Android activity available\n");
        fflush(stdout);
        return NULL;
    }
    cls = (*env)->GetObjectClass(env, activity);
    (*env)->DeleteLocalRef(env, activity); /* SDL documents this as a local ref. */
    if (saf_clear_exception(env, "GetObjectClass") || cls == NULL) {
        if (cls != NULL) (*env)->DeleteLocalRef(env, cls);
        return NULL;
    }

#define MP6_SAF_GET_METHOD(field, name, signature) do {                       \
    candidate.field = (*env)->GetStaticMethodID(env, cls, name, signature);   \
    if (saf_clear_exception(env, "GetStaticMethodID(" name ")") ||           \
        candidate.field == NULL) goto method_failure;                         \
} while (0)

    MP6_SAF_GET_METHOD(openPicker, "mp6OpenFolderPicker", "()V");
    MP6_SAF_GET_METHOD(pollPick, "mp6PollFolderPick", "()Ljava/lang/String;");
    MP6_SAF_GET_METHOD(importStart, "mp6TreeImportStart",
                       "(Ljava/lang/String;Ljava/lang/String;)Z");
    MP6_SAF_GET_METHOD(importPoll, "mp6TreeImportPoll", "()[J");
    MP6_SAF_GET_METHOD(importCurrent, "mp6TreeImportCurrent", "()Ljava/lang/String;");
    MP6_SAF_GET_METHOD(importError, "mp6TreeImportError", "()Ljava/lang/String;");
    MP6_SAF_GET_METHOD(importCancel, "mp6TreeImportCancel", "()V");
#undef MP6_SAF_GET_METHOD

    candidate.activityClass = (jclass)(*env)->NewGlobalRef(env, cls);
    if (saf_clear_exception(env, "NewGlobalRef(activityClass)") ||
        candidate.activityClass == NULL) {
        if (candidate.activityClass != NULL) {
            (*env)->DeleteGlobalRef(env, candidate.activityClass);
        }
        (*env)->DeleteLocalRef(env, cls);
        return NULL;
    }
    (*env)->DeleteLocalRef(env, cls);
    candidate.ready = 1;
    g_jni = candidate; /* publish only a complete method/class set */
    return &g_jni;

method_failure:
    printf("[CONTENT] saf_bridge: Mp6Activity mp6* methods not found (Java glue out of date?)\n");
    fflush(stdout);
    (*env)->DeleteLocalRef(env, cls);
    return NULL;
}

/* Copies a returned jstring only when its complete modified-UTF8 payload fits.
 * The caller's previous buffer remains intact on failure. */
static int saf_take_string(JNIEnv *env, jstring js, char *buf, size_t n)
{
    const char *utf = NULL;
    jsize utfLen;
    int result = -1;
    if (env == NULL || buf == NULL || n == 0) {
        if (env != NULL && js != NULL) (*env)->DeleteLocalRef(env, js);
        return -1;
    }
    if (js == NULL) {
        buf[0] = '\0';
        return 0;
    }
    utfLen = (*env)->GetStringUTFLength(env, js);
    if (saf_clear_exception(env, "GetStringUTFLength") || utfLen < 0 ||
        (size_t)utfLen >= n) goto done;
    utf = (*env)->GetStringUTFChars(env, js, NULL);
    if (saf_clear_exception(env, "GetStringUTFChars") || utf == NULL) goto done;
    memcpy(buf, utf, (size_t)utfLen);
    buf[utfLen] = '\0';
    result = 0;
done:
    if (utf != NULL) (*env)->ReleaseStringUTFChars(env, js, utf);
    (*env)->DeleteLocalRef(env, js);
    return result;
}

int mp6_saf_open_tree_picker(void)
{
    SafJni *j = saf_jni();
    if (j == NULL) return -1;
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    if (env == NULL) return -1;
    (*env)->CallStaticVoidMethod(env, j->activityClass, j->openPicker);
    if (saf_clear_exception(env, "mp6OpenFolderPicker")) return -1;
    return 0;
}

int mp6_saf_poll_tree_pick(char *uriOut, size_t n)
{
    char buf[MP6_SAF_URI_CAP] = {0};
    if (uriOut == NULL || n == 0) return MP6_SAF_PICK_ERROR;
    SafJni *j = saf_jni();
    if (j == NULL) return MP6_SAF_PICK_ERROR;
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    if (env == NULL) return MP6_SAF_PICK_ERROR;
    jstring js = (jstring)(*env)->CallStaticObjectMethod(env, j->activityClass, j->pollPick);
    if (saf_clear_exception(env, "mp6PollFolderPick")) {
        if (js != NULL) (*env)->DeleteLocalRef(env, js);
        return MP6_SAF_PICK_ERROR;
    }
    if (saf_take_string(env, js, buf, sizeof(buf)) != 0) return MP6_SAF_PICK_ERROR;
    if (buf[0] == '\0') return MP6_SAF_PICK_PENDING;
    if (strcmp(buf, "!cancelled") == 0) return MP6_SAF_PICK_CANCELLED;
    if (mp6_saf_copy_preserve(uriOut, n, buf) != 0) return MP6_SAF_PICK_ERROR;
    return MP6_SAF_PICK_READY;
}

int mp6_saf_tree_import_start(const char *treeUri, const char *destDiscRoot)
{
    SafJni *j = saf_jni();
    if (j == NULL || treeUri == NULL || destDiscRoot == NULL || treeUri[0] == '\0' ||
        destDiscRoot[0] == '\0' || strlen(treeUri) >= MP6_SAF_URI_CAP ||
        strlen(destDiscRoot) >= MP6_SAF_PATH_CAP) return -1;
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    if (env == NULL) return -1;
    jstring juri = (*env)->NewStringUTF(env, treeUri);
    jstring jdest;
    jboolean ok = JNI_FALSE;
    if (saf_clear_exception(env, "NewStringUTF(treeUri)") || juri == NULL) {
        if (juri != NULL) (*env)->DeleteLocalRef(env, juri);
        return -1;
    }
    jdest = (*env)->NewStringUTF(env, destDiscRoot);
    if (saf_clear_exception(env, "NewStringUTF(destDiscRoot)") || jdest == NULL) {
        (*env)->DeleteLocalRef(env, juri);
        if (jdest != NULL) (*env)->DeleteLocalRef(env, jdest);
        return -1;
    }
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
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    SafJni *j = saf_jni();
    if (j == NULL) {
        out->state = MP6_IMPORT_FAILED;
        snprintf(out->error, sizeof(out->error), "SAF bridge unavailable");
        return;
    }
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    if (env == NULL) {
        out->state = MP6_IMPORT_FAILED;
        snprintf(out->error, sizeof(out->error), "SAF JNI environment unavailable");
        return;
    }
    jlongArray arr = (jlongArray)(*env)->CallStaticObjectMethod(env, j->activityClass, j->importPoll);
    if (saf_clear_exception(env, "mp6TreeImportPoll") || arr == NULL) {
        if (arr != NULL) (*env)->DeleteLocalRef(env, arr);
        out->state = MP6_IMPORT_FAILED;
        snprintf(out->error, sizeof(out->error), "SAF import poll failed");
        return;
    }
    jlong vals[5] = { 0 };
    int64_t checkedVals[5];
    jsize len = (*env)->GetArrayLength(env, arr);
    if (saf_clear_exception(env, "GetArrayLength") || len != 5) {
        (*env)->DeleteLocalRef(env, arr);
        out->state = MP6_IMPORT_FAILED;
        snprintf(out->error, sizeof(out->error), "SAF import poll returned an invalid progress array");
        return;
    }
    (*env)->GetLongArrayRegion(env, arr, 0, 5, vals);
    if (saf_clear_exception(env, "GetLongArrayRegion")) {
        (*env)->DeleteLocalRef(env, arr);
        out->state = MP6_IMPORT_FAILED;
        snprintf(out->error, sizeof(out->error), "SAF import progress could not be read");
        return;
    }
    (*env)->DeleteLocalRef(env, arr);
    for (len = 0; len < 5; len++) checkedVals[len] = (int64_t)vals[len];
    if (!mp6_saf_progress_values_valid(checkedVals)) {
        out->state = MP6_IMPORT_FAILED;
        snprintf(out->error, sizeof(out->error), "SAF import returned invalid progress values");
        return;
    }
    out->state = (int)checkedVals[0];
    out->bytesDone = (uint64_t)checkedVals[1];
    out->bytesTotal = (uint64_t)checkedVals[2];
    out->filesDone = (int)checkedVals[3];
    out->filesTotal = (int)checkedVals[4];

    jstring cur = (jstring)(*env)->CallStaticObjectMethod(env, j->activityClass, j->importCurrent);
    if (saf_clear_exception(env, "mp6TreeImportCurrent")) {
        if (cur != NULL) (*env)->DeleteLocalRef(env, cur);
        out->state = MP6_IMPORT_FAILED;
        snprintf(out->error, sizeof(out->error), "SAF current-file poll failed");
        return;
    }
    if (saf_take_string(env, cur, out->currentFile, sizeof(out->currentFile)) != 0) {
        out->state = MP6_IMPORT_FAILED;
        snprintf(out->error, sizeof(out->error), "SAF current-file path is too long");
        return;
    }
    if (out->state == MP6_IMPORT_FAILED) {
        jstring err = (jstring)(*env)->CallStaticObjectMethod(env, j->activityClass, j->importError);
        if (saf_clear_exception(env, "mp6TreeImportError")) {
            if (err != NULL) (*env)->DeleteLocalRef(env, err);
        } else {
            if (saf_take_string(env, err, out->error, sizeof(out->error)) != 0) {
                snprintf(out->error, sizeof(out->error), "Java-side SAF error text is too long");
            }
        }
        if (out->error[0] == '\0') snprintf(out->error, sizeof(out->error), "unknown Java-side error");
    }
}

void mp6_saf_tree_import_cancel(void)
{
    SafJni *j = saf_jni();
    if (j == NULL) return;
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    if (env == NULL) return;
    (*env)->CallStaticVoidMethod(env, j->activityClass, j->importCancel);
    saf_clear_exception(env, "mp6TreeImportCancel");
}
