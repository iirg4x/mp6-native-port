/* MP6 native port -- Windows mic backend: SAPI 5's in-process recognizer
 * (include/mp6_mic.h has the seam).
 *
 * Windows ships an offline speech recognizer ("Microsoft Speech Recognizer
 * 8.0 for Windows", per installed language). Fed a grammar that holds only
 * the active minigame's words -- the same shape as the original engine's
 * grammars on the disc -- it listens to the default capture device and can
 * only ever answer with one of those words, which is why it is far more
 * reliable here than free dictation would be.
 *
 * Everything runs on the main loop thread (mp6_mic_host_pump): COM objects
 * are created there and recognition events are polled once per frame, so
 * there is no callback thread to synchronise with.
 *
 * MP6_MIC_SAPI_WAV=<file.wav> recognises from a WAV file instead of the
 * microphone (automated tests; the file plays once per listening session). */

#include "mp6_mic.h"

#if defined(_WIN32) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
#define MP6_MIC_SAPI 1
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <sapi.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SAVESTATE CARVE-OUT: COM objects of the running process. Must sit AFTER
 * this TU's own includes and at preprocessor TOP LEVEL. */
#include "mp6_host_section.h"

#ifdef MP6_MIC_SAPI

/* GUIDs, local so no import library is needed for them. */
static const CLSID kClsidInprocRecognizer = {0x41b89b6b, 0x9399, 0x11d2, {0x96, 0x23, 0x00, 0xc0, 0x4f, 0x8e, 0xe6, 0x28}};
static const CLSID kClsidMMAudioIn = {0xcf3d2e50, 0x53f2, 0x11d2, {0x96, 0x0c, 0x00, 0xc0, 0x4f, 0x8e, 0xe6, 0x28}};
static const CLSID kClsidStream = {0x715d9c59, 0x4442, 0x11d2, {0x96, 0x05, 0x00, 0xc0, 0x4f, 0x8e, 0xe6, 0x28}};
static const IID kIidRecognizer = {0xc2b5f241, 0xdaa0, 0x4507, {0x9e, 0x16, 0x5a, 0x1e, 0xaa, 0x2b, 0x7a, 0x5c}};
static const IID kIidStream = {0x12e3cca9, 0x7518, 0x44c5, {0xa5, 0xe7, 0xba, 0x5a, 0x79, 0xcb, 0x92, 0x9e}};
static const IID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

#define SAPI_FLAGCHECK ((1ULL << SPEI_RESERVED1) | (1ULL << SPEI_RESERVED2))
#define SAPI_EVENT(e) ((1ULL << (e)) | SAPI_FLAGCHECK)

static int g_comReady;
static int g_probed, g_available;
static ISpRecognizer *g_recognizer;
static ISpRecoContext *g_context;
static ISpRecoGrammar *g_grammar;
static int g_listening;
static int g_finishing;
static char g_hypothesis[256];
static char g_phrases[160][64];
static int g_phraseCount;

static void sapi_release_all(void)
{
    if (g_grammar) ISpRecoGrammar_Release(g_grammar);
    if (g_context) ISpRecoContext_Release(g_context);
    if (g_recognizer) ISpRecognizer_Release(g_recognizer);
    g_grammar = NULL;
    g_context = NULL;
    g_recognizer = NULL;
}

static int sapi_com_init(void)
{
    if (!g_comReady) {
        /* S_FALSE (already initialised) and RPC_E_CHANGED_MODE (someone made
         * this an STA first) both leave COM usable on this thread. */
        const HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        g_comReady = (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) ? 1 : -1;
    }
    return g_comReady == 1;
}

/* Input: the default capture device, or MP6_MIC_SAPI_WAV. */
static IUnknown *sapi_make_input(void)
{
    const char *wav = getenv("MP6_MIC_SAPI_WAV");
    IUnknown *input = NULL;
    if (wav != NULL && wav[0] != '\0') {
        ISpStream *stream = NULL;
        WCHAR path[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, wav, -1, path, MAX_PATH);
        if (SUCCEEDED(CoCreateInstance(&kClsidStream, NULL, CLSCTX_ALL, &kIidStream, (void **)&stream))) {
            if (SUCCEEDED(ISpStream_BindToFile(stream, path, SPFM_OPEN_READONLY, NULL, NULL, 0))) {
                ISpStream_QueryInterface(stream, &kIidUnknown, (void **)&input);
            }
            ISpStream_Release(stream);
        }
        return input;
    }
    CoCreateInstance(&kClsidMMAudioIn, NULL, CLSCTX_ALL, &kIidUnknown, (void **)&input);
    return input;
}

static int sapi_open(void)
{
    IUnknown *input;
    HRESULT hr;
    if (g_context != NULL) return 1;
    if (!sapi_com_init()) return 0;
    hr = CoCreateInstance(&kClsidInprocRecognizer, NULL, CLSCTX_ALL, &kIidRecognizer, (void **)&g_recognizer);
    if (FAILED(hr)) {
        printf("[MIC] SAPI: no in-process recognizer (0x%08lx)\n", (unsigned long)hr);
        return 0;
    }
    input = sapi_make_input();
    if (input == NULL) {
        printf("[MIC] SAPI: no capture device\n");
        sapi_release_all();
        return 0;
    }
    hr = ISpRecognizer_SetInput(g_recognizer, input, TRUE);
    IUnknown_Release(input);
    if (SUCCEEDED(hr)) hr = ISpRecognizer_CreateRecoContext(g_recognizer, &g_context);
    if (SUCCEEDED(hr)) {
        hr = ISpRecoContext_SetInterest(g_context,
                                        SAPI_EVENT(SPEI_RECOGNITION) | SAPI_EVENT(SPEI_FALSE_RECOGNITION) |
                                            SAPI_EVENT(SPEI_HYPOTHESIS) | SAPI_EVENT(SPEI_END_SR_STREAM),
                                        SAPI_EVENT(SPEI_RECOGNITION) | SAPI_EVENT(SPEI_FALSE_RECOGNITION) |
                                            SAPI_EVENT(SPEI_HYPOTHESIS) | SAPI_EVENT(SPEI_END_SR_STREAM));
    }
    if (SUCCEEDED(hr)) hr = ISpRecoContext_SetNotifyWin32Event(g_context);
    if (FAILED(hr)) {
        printf("[MIC] SAPI: recognizer setup failed (0x%08lx)\n", (unsigned long)hr);
        sapi_release_all();
        return 0;
    }
    return 1;
}

int mp6_mic_backend_available(void)
{
    if (!g_probed) {
        /* Cheap and prompt-free: a recognizer must be registered and some
         * capture device present. Creating COM objects waits for first use. */
        HKEY key;
        g_probed = 1;
        g_available = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Speech\\Recognizers\\Tokens", 0,
                                    KEY_READ, &key) == ERROR_SUCCESS;
        if (g_available) RegCloseKey(key);
        printf("[MIC] Windows speech recognizer: %s\n", g_available ? "available" : "not available");
        fflush(stdout);
    }
    return g_available;
}

static void sapi_stop_listening(void)
{
    if (g_recognizer != NULL && g_listening) ISpRecognizer_SetRecoState(g_recognizer, SPRST_INACTIVE);
    if (g_grammar != NULL) {
        ISpRecoGrammar_SetRuleState(g_grammar, NULL, NULL, SPRS_INACTIVE);
        ISpRecoGrammar_Release(g_grammar);
        g_grammar = NULL;
    }
    g_listening = 0;
    g_finishing = 0;
    g_hypothesis[0] = '\0';
}

int mp6_mic_backend_start(const char *const *phrases, int count, int continuous)
{
    SPSTATEHANDLE state = NULL;
    HRESULT hr;
    int i;
    (void)continuous; /* SAPI keeps listening; the engine takes what it needs */
    if (!sapi_open()) {
        mp6_mic_post_error("Windows speech recognition is not available");
        return 0;
    }
    sapi_stop_listening();
    g_phraseCount = 0;
    for (i = 0; i < count && i < 160; i++) {
        snprintf(g_phrases[g_phraseCount++], sizeof(g_phrases[0]), "%s", phrases[i]);
    }
    /* A file input plays once: rebind it for every session. */
    if (getenv("MP6_MIC_SAPI_WAV") != NULL) {
        IUnknown *input = sapi_make_input();
        if (input != NULL) {
            ISpRecognizer_SetInput(g_recognizer, input, TRUE);
            IUnknown_Release(input);
        }
    }
    hr = ISpRecoContext_CreateGrammar(g_context, 1, &g_grammar);
    if (SUCCEEDED(hr)) {
        hr = ISpRecoGrammar_GetRule(g_grammar, L"mp6words", 0, SPRAF_TopLevel | SPRAF_Active, TRUE, &state);
    }
    for (i = 0; SUCCEEDED(hr) && i < count; i++) {
        WCHAR text[128];
        MultiByteToWideChar(CP_UTF8, 0, phrases[i], -1, text, 128);
        hr = ISpRecoGrammar_AddWordTransition(g_grammar, state, NULL, text, L" ", SPWT_LEXICAL, 1.0f, NULL);
    }
    if (SUCCEEDED(hr)) hr = ISpRecoGrammar_Commit(g_grammar, 0);
    if (SUCCEEDED(hr)) hr = ISpRecoGrammar_SetRuleState(g_grammar, NULL, NULL, SPRS_ACTIVE);
    if (SUCCEEDED(hr)) hr = ISpRecognizer_SetRecoState(g_recognizer, SPRST_ACTIVE);
    if (FAILED(hr)) {
        printf("[MIC] SAPI: could not start listening (0x%08lx)\n", (unsigned long)hr);
        fflush(stdout);
        sapi_stop_listening();
        return 0;
    }
    g_listening = 1;
    return 1;
}

void mp6_mic_backend_finish(void)
{
    /* SAPI ends an utterance on its own after a short silence. If its
     * running hypothesis is already a whole word, answer with it now instead
     * of waiting for that ("drop" while saying "drop bombs" is not). */
    char heard[256];
    int i;
    g_finishing = 1;
    if (g_hypothesis[0] == '\0') return;
    mp6_mic_normalise(g_hypothesis, heard, sizeof(heard));
    for (i = 0; i < g_phraseCount; i++) {
        if (strcmp(heard, g_phrases[i]) == 0) {
            const char *text = g_hypothesis;
            mp6_mic_post_result(&text, NULL, 1, 1);
            g_hypothesis[0] = '\0';
            return;
        }
    }
}

void mp6_mic_backend_stop(void)
{
    sapi_stop_listening();
}

static void sapi_text(ISpRecoResult *result, char *out, int outSize)
{
    WCHAR *text = NULL;
    out[0] = '\0';
    if (SUCCEEDED(ISpRecoResult_GetText(result, SP_GETWHOLEPHRASE, SP_GETWHOLEPHRASE, TRUE, &text, NULL)) && text) {
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out, outSize, NULL, NULL);
        CoTaskMemFree(text);
    }
}

static float sapi_confidence(ISpRecoResult *result)
{
    SPPHRASE *phrase = NULL;
    float confidence = 0.0f;
    if (SUCCEEDED(ISpRecoResult_GetPhrase(result, &phrase)) && phrase) {
        confidence = phrase->Rule.SREngineConfidence;
        if (phrase->Rule.Confidence == SP_LOW_CONFIDENCE && confidence > 0.3f) confidence = 0.3f;
        CoTaskMemFree(phrase);
    }
    return confidence;
}

void mp6_mic_backend_poll(void)
{
    SPEVENT event;
    ULONG fetched = 0;
    if (g_context == NULL || !g_listening) return;
    while (SUCCEEDED(ISpRecoContext_GetEvents(g_context, 1, &event, &fetched)) && fetched == 1) {
        if (event.eEventId == SPEI_RECOGNITION && event.elParamType == SPET_LPARAM_IS_OBJECT) {
            ISpRecoResult *result = (ISpRecoResult *)event.lParam;
            char text[256];
            const char *alt = text;
            float confidence;
            sapi_text(result, text, sizeof(text));
            confidence = sapi_confidence(result);
            printf("[MIC] SAPI heard \"%s\" (confidence %.2f)\n", text, confidence);
            fflush(stdout);
            /* The grammar forces every sound onto some word; a near-zero
             * engine confidence is a cough or the TV, not an answer. */
            if (confidence > 0.0f && confidence < 0.12f) mp6_mic_post_result(NULL, NULL, 0, 1);
            else mp6_mic_post_result(&alt, NULL, 1, 1);
            g_hypothesis[0] = '\0';
        } else if (event.eEventId == SPEI_HYPOTHESIS && event.elParamType == SPET_LPARAM_IS_OBJECT) {
            sapi_text((ISpRecoResult *)event.lParam, g_hypothesis, sizeof(g_hypothesis));
        } else if (event.eEventId == SPEI_FALSE_RECOGNITION) {
            mp6_mic_post_result(NULL, NULL, 0, 1);
            g_hypothesis[0] = '\0';
        } else if (event.eEventId == SPEI_END_SR_STREAM) {
            /* The WAV test input ran out. */
            if (g_finishing) mp6_mic_post_result(NULL, NULL, 0, 1);
        }
        if (event.elParamType == SPET_LPARAM_IS_OBJECT && event.lParam != 0) {
            IUnknown_Release((IUnknown *)event.lParam);
        } else if ((event.elParamType == SPET_LPARAM_IS_POINTER || event.elParamType == SPET_LPARAM_IS_STRING) &&
                   event.lParam != 0) {
            CoTaskMemFree((void *)event.lParam);
        }
        fetched = 0;
    }
}

#endif /* MP6_MIC_SAPI */
