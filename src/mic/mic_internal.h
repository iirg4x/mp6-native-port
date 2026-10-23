/* MP6 native port -- private seam between the mic engine (game state,
 * src/mic/mic_engine.c) and the mic host (process state, src/mic/mic_host.c).
 * include/mp6_mic.h has the design. */
#ifndef MP6_MIC_INTERNAL_H
#define MP6_MIC_INTERNAL_H

#include <stdint.h>

#define MIC_HOST_MAX_PHRASES 160
#define MIC_HOST_PHRASE_LEN 64
#define MIC_HOST_MAX_ALTS 6
#define MIC_HOST_ALT_LEN 128

enum {
    MIC_HOST_EVENT_RESULT = 1,  /* a hypothesis (interim or final) */
    MIC_HOST_EVENT_STOPPED = 2  /* the backend stopped listening on its own */
};

typedef struct MicHostEvent {
    int type;
    int final;
    int count;
    float confidence[MIC_HOST_MAX_ALTS]; /* 0 = unknown */
    char alt[MIC_HOST_MAX_ALTS][MIC_HOST_ALT_LEN];
} MicHostEvent;

/* Engine -> host (game thread, any stack). Returns the new session's
 * serial; results of older sessions are dropped. */
int mic_host_request_start(const char (*phrases)[MIC_HOST_PHRASE_LEN], int count, int continuous);
void mic_host_request_finish(int serial);
void mic_host_request_stop(void);

/* Next event of the current session; 0 when none. */
int mic_host_take(int serial, MicHostEvent *out);

/* The talk button (all sources). */
int mic_host_talk_held(void);

/* The engine mounted / released slot B. */
void mic_host_set_in_use(int inUse);

/* Mic PCM ring: absolute sample counter of the newest sample written, and a
 * copy of samples [index, index + count). Calling read keeps capture open. */
uint32_t mic_host_pcm_head(void);
int mic_host_pcm_read(uint32_t index, int16_t *dst, int count);

/* 1 when slot B should look occupied: setting on, no permanent backend
 * failure, and a backend (or MP6_MIC_SCRIPT) that can try. */
int mp6_mic_backend_available_now(void);

/* 1 when a backend reported a permanent problem (permission denied...). */
int mic_host_failed(void);


#endif /* MP6_MIC_INTERNAL_H */
