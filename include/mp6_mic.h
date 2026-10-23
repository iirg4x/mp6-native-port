/* MP6 native port -- the GameCube Microphone (DOL-022 in memory card slot B).
 *
 * The game drives its mic through three layers that did not survive into
 * the decomp: the SDK's MIC driver (MIC_*), its M2S speech-stream helper
 * (M2S*), and a third-party speech recognizer (gsapi_*) fed with grammar
 * files from the disc's /mic/ctx directory. src/mic/mic_engine.c replaces
 * all three with one shared engine:
 *
 *   - the word lists come from the disc itself: every context's .wrd file
 *     lists the spellings (and pronunciation variants) the original engine
 *     matched, and its offsets are exactly what game/mic.c's MicResultGet
 *     maps back to the minigame's word numbers;
 *   - recognition is delegated to the platform's own recognizer through the
 *     small backend seam below (Windows SAPI with a grammar of the game's
 *     words, Android's SpeechRecognizer, the browser's SpeechRecognition),
 *     and whatever text a backend hears is matched back onto the active
 *     word list by the shared matcher, so no minigame needs its own code;
 *   - the Mic's talk button is a host input (keyboard, gamepad Back/Select,
 *     on-screen button) published with mp6_mic_set_talk_button.
 *
 * Threading: game code calls the engine from HuPrc coroutines, whose stacks
 * must never make JNI or COM calls. The engine therefore only records what
 * it wants; mp6_mic_host_pump(), called once per frame from the main loop's
 * real thread stack, is the only place backends are started or stopped.
 * Backends may deliver results from any thread through mp6_mic_post_result.
 *
 * Types: included from both compile flavors, so stdint only.
 */
#ifndef MP6_MIC_H
#define MP6_MIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Settings and input (launcher / input pump side)
 * --------------------------------------------------------------------- */

/* The launcher's published game.microphone setting. Off = the game sees an
 * empty slot B, exactly like before this feature (pad-choice fallback).
 * MP6_MIC=0/1 overrides it. Automation runs (no launcher) keep it off
 * unless MP6_MIC=1 or MP6_MIC_SCRIPT asks for the mic. */
void mp6_mic_set_enabled(int enabled);
int mp6_mic_enabled(void);
int mp6_mic_env_override(void);

/* The Mic's talk button, held = 1. Several sources may hold it at once
 * (keyboard, gamepad, touch); each owns one bit. */
enum {
    MP6_MIC_TALK_KEYBOARD = 1 << 0,
    MP6_MIC_TALK_GAMEPAD = 1 << 1,
    MP6_MIC_TALK_TOUCH = 1 << 2,
    MP6_MIC_TALK_SCRIPT = 1 << 3
};
void mp6_mic_set_talk_button(unsigned source, int held);

/* 1 while a game mode has the mic mounted (for showing an on-screen talk
 * button only when it matters). 2 while it is also listening. */
int mp6_mic_in_use(void);

/* Once per frame from the main loop (real thread stack): applies the
 * engine's wanted backend state and polls the backend. */
void mp6_mic_host_pump(void);

/* Short user-facing status of the last backend problem ("" when none),
 * e.g. permission denied or no recognizer installed. */
const char *mp6_mic_status_text(void);

/* ---------------------------------------------------------------------
 * Backend seam (one implementation per platform; the engine links weak
 * "no recognizer" defaults, so headless and unsupported targets build).
 * --------------------------------------------------------------------- */

/* 1 when the platform has a capture device and a recognizer it can try.
 * Must be cheap and must not prompt for permission. */
int mp6_mic_backend_available(void);

/* Start listening for any of `phrases` (plain lowercase text, already
 * normalised by the engine). continuous = keep listening after each
 * utterance until stop. Called from mp6_mic_host_pump only. */
int mp6_mic_backend_start(const char *const *phrases, int count, int continuous);

/* The talk button was released: finish the current utterance quickly and
 * deliver its final result (or an empty one). */
void mp6_mic_backend_finish(void);

/* Stop listening and drop anything pending. */
void mp6_mic_backend_stop(void);

/* Per-frame poll for backends that need one (may be empty). */
void mp6_mic_backend_poll(void);

/* Raw PCM for the game's volume meter (Options > Mic test). Mono s16 at any
 * rate; read returns the number of samples copied (0 when none). */
int mp6_mic_pcm_open(void);
int mp6_mic_pcm_read(int16_t *dst, int maxSamples);
void mp6_mic_pcm_close(void);

/* Backend -> engine. alternatives[0] is the best guess; confidences may be
 * NULL (rank order is used then). final = 0 for interim hypotheses, which
 * the engine only accepts when they match a word exactly. count = 0 with
 * final = 1 means "heard nothing usable". Any thread. */
void mp6_mic_post_result(const char *const *alternatives, const float *confidences,
                         int count, int final);

/* Backend -> engine: listening ended on its own (error, timeout) while the
 * engine still wanted it; the engine reports it like the original engine's
 * "stop listen" notification so the game restarts listening if needed. */
void mp6_mic_post_stopped(const char *reason);

/* Backend -> engine: a permanent problem (permission denied, no
 * recognizer). Shown by mp6_mic_status_text. */
void mp6_mic_post_error(const char *message);

/* The shared matcher, exposed for the backends that want to pre-filter and
 * for tests: normalises `text` into `out` (lowercase words separated by
 * single spaces, digits spelled out). Returns the length written. */
int mp6_mic_normalise(const char *text, char *out, int outSize);

#ifdef __cplusplus
}
#endif

#endif /* MP6_MIC_H */
