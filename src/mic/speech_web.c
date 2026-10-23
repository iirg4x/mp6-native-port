/* MP6 native port -- browser mic backend (the iPhone PWA): the Web Speech
 * API's SpeechRecognition (webkitSpeechRecognition in Safari), see
 * include/mp6_mic.h for the seam.
 *
 * The browser owns the microphone and its permission prompt (the first
 * start() asks). Every hypothesis is queued on the JS side and handed to
 * the shared matcher by the per-frame poll below, so no JS ever calls into
 * the game while it runs. Continuous sessions restart recognition whenever
 * the browser ends it on its own. EM_JS bodies avoid `//` (the macro strips
 * it as a comment). */

#include "mp6_mic.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SAVESTATE CARVE-OUT: browser session state of the running page. Must sit
 * AFTER this TU's own includes and at TOP LEVEL. */
#include "mp6_host_section.h"

#ifdef __EMSCRIPTEN__

EM_JS(int, mp6_web_speech_available, (void), {
    if (typeof window === 'undefined') return 0;
    return (window.SpeechRecognition || window.webkitSpeechRecognition) ? 1 : 0;
});

EM_JS(void, mp6_web_speech_start, (const char *joined, int continuous), {
    var Ctor = window.SpeechRecognition || window.webkitSpeechRecognition;
    var st = Module.mp6Speech || (Module.mp6Speech = { queue: [], session: 0 });
    var session;
    var rec;
    if (!Ctor) return;
    if (st.rec) { try { st.rec.onend = null; st.rec.abort(); } catch (e) {} }
    st.session++;
    session = st.session;
    st.wanted = true;
    st.continuous = continuous !== 0;
    st.gotFinal = false;
    rec = new Ctor();
    st.rec = rec;
    rec.lang = 'en-US';
    rec.continuous = true;
    rec.interimResults = true;
    rec.maxAlternatives = 5;
    try {
        var List = window.SpeechGrammarList || window.webkitSpeechGrammarList;
        var words = UTF8ToString(joined).split('\n').filter(function (w) { return w.length > 0; });
        if (List && words.length > 0) {
            var list = new List();
            list.addFromString('#JSGF V1.0; grammar mp6; public <word> = ' + words.join(' | ') + ' ;', 1);
            rec.grammars = list;
        }
    } catch (e) {}
    rec.onresult = function (ev) {
        var i, j, r, alts, conf;
        if (session !== st.session) return;
        for (i = ev.resultIndex; i < ev.results.length; i++) {
            r = ev.results[i];
            alts = [];
            conf = [];
            for (j = 0; j < r.length; j++) {
                alts.push(r[j].transcript);
                conf.push(r[j].confidence || 0);
            }
            if (r.isFinal) st.gotFinal = true;
            st.queue.push({ kind: r.isFinal ? 'F' : 'I', alts: alts, conf: conf });
        }
    };
    rec.onerror = function (ev) {
        if (session !== st.session) return;
        if (ev.error === 'not-allowed' || ev.error === 'service-not-allowed') {
            st.wanted = false;
            st.queue.push({ kind: 'E', text: 'Microphone or speech recognition is blocked for this page' });
        } else if (ev.error === 'no-speech' || ev.error === 'aborted') {
            st.queue.push({ kind: 'F', alts: [], conf: [] });
        } else {
            st.queue.push({ kind: 'S', text: String(ev.error) });
        }
    };
    rec.onend = function () {
        if (session !== st.session) return;
        if (st.wanted && st.continuous) {
            try { rec.start(); return; } catch (e) {}
        }
        if (st.wanted && !st.gotFinal) st.queue.push({ kind: 'F', alts: [], conf: [] });
        st.wanted = false;
    };
    try {
        rec.start();
    } catch (e) {
        st.queue.push({ kind: 'S', text: 'start failed: ' + e });
    }
});

EM_JS(void, mp6_web_speech_finish, (void), {
    var st = Module.mp6Speech;
    if (!st || !st.rec) return;
    st.continuous = false;
    try { st.rec.stop(); } catch (e) {}
});

EM_JS(void, mp6_web_speech_stop, (void), {
    var st = Module.mp6Speech;
    if (!st) return;
    st.wanted = false;
    st.session++;
    st.queue.length = 0;
    if (st.rec) { try { st.rec.onend = null; st.rec.abort(); } catch (e) {} st.rec = null; }
});

/* Next queued event as "<kind><text>" with alternatives separated by \x1f
 * and each confidence after \x1e. 0 when the queue is empty. */
EM_JS(int, mp6_web_speech_next, (char *buf, int size), {
    var st = Module.mp6Speech;
    var e, s;
    if (!st || st.queue.length === 0) return 0;
    e = st.queue.shift();
    if (e.kind === 'E' || e.kind === 'S') {
        s = e.kind + e.text;
    } else {
        s = e.kind + e.alts.map(function (a, i) { return a + '\x1e' + (e.conf[i] || 0); }).join('\x1f');
    }
    stringToUTF8(s, buf, size);
    return 1;
});

int mp6_mic_backend_available(void)
{
    static int cached = -1;
    if (cached < 0) {
        cached = mp6_web_speech_available();
        printf("[MIC] browser speech recognition: %s\n", cached ? "available" : "not available");
        fflush(stdout);
    }
    return cached;
}

int mp6_mic_backend_start(const char *const *phrases, int count, int continuous)
{
    static char joined[160 * 64];
    int len = 0, i;
    joined[0] = '\0';
    for (i = 0; i < count; i++) {
        len += snprintf(joined + len, sizeof(joined) - (size_t)len, "%s\n", phrases[i]);
        if (len >= (int)sizeof(joined)) break;
    }
    mp6_web_speech_start(joined, continuous);
    return 1;
}

void mp6_mic_backend_finish(void)
{
    mp6_web_speech_finish();
}

void mp6_mic_backend_stop(void)
{
    mp6_web_speech_stop();
}

void mp6_mic_backend_poll(void)
{
    static char line[1024];
    while (mp6_web_speech_next(line, sizeof(line))) {
        const char kind = line[0];
        if (kind == 'E') {
            mp6_mic_post_error(line + 1);
            mp6_mic_post_result(NULL, NULL, 0, 1);
        } else if (kind == 'S') {
            mp6_mic_post_stopped(line + 1);
        } else {
            const char *alts[6];
            float conf[6];
            int count = 0;
            char *cursor = line + 1;
            while (*cursor != '\0' && count < 6) {
                char *next = strchr(cursor, '\x1f');
                char *mark;
                if (next != NULL) *next = '\0';
                mark = strchr(cursor, '\x1e');
                conf[count] = 0.0f;
                if (mark != NULL) {
                    *mark = '\0';
                    conf[count] = (float)atof(mark + 1);
                }
                alts[count++] = cursor;
                if (next == NULL) break;
                cursor = next + 1;
            }
            mp6_mic_post_result(alts, conf, count, kind == 'F');
        }
    }
}

#endif /* __EMSCRIPTEN__ */
