/* MP6 native port -- the game-event bus.
 *
 * WHY THIS EXISTS
 * ---------------
 * Driving the game from the outside (automation, gates, capture runs) used
 * to mean guessing tick offsets: "the title screen is probably live around
 * tick 20000, press START there". That is structurally unreliable in this
 * port, and provably so -- the boot sequence mixes FRAME-counted waits
 * (`for (frame = 0; frame < 90; frame++) HuPrcVSleep()`) with WALL-CLOCK
 * waits (`while (OSTicksToMilliseconds(OSGetTick() - start) < 1500)`), so
 * the tick at which any given screen becomes input-ready moves with host
 * speed, DVD/host-file latency, GPU frame pacing and shader compilation.
 * A tick-scheduled press can and does land in the wrong screen entirely,
 * and the failure is silent: the press is simply swallowed.
 *
 * The fix is to make the game's own state OBSERVABLE, then attach input to
 * observed state instead of to the clock. This module is that observable
 * surface: a tiny keyed event table that anything in the port can post to,
 * that prints one stable line per event, and that the input-script engine
 * (platform/gx/aurora_bridge.c) can BLOCK on.
 *
 * EMISSION FORMAT
 * ---------------
 * Exactly one line per posted event, always this shape:
 *
 *     [EVENT] <key>=<value> num=<n> tick=<t> seq=<s>
 *
 *   key   -- dotted, stable, lowercase (e.g. "ovl.start", "dll.bound")
 *   value -- short string form ("w01dll"); "-" when the event is numeric only
 *   num   -- long numeric form (overlay id, SE id, ...); 0 when not meaningful
 *   tick  -- mp6_tick_count at the moment of posting (the VI tick)
 *   seq   -- global monotonically increasing event counter, 1-based
 *
 * The line is greppable, ordered and machine-parseable; `seq` lets a
 * consumer say "fired AFTER the point I started waiting" without any clock.
 *
 * COST
 * ----
 * A post is a linear scan of at most MP6_EVENT_SLOT_MAX short keys plus a
 * printf. Events are posted only at genuine state changes (an overlay
 * starts, a DLL binds, a sound effect fires), never per-frame, so the
 * whole mechanism is far below noise on a 60Hz budget. There is no
 * allocation, no locking and no dependency beyond <stdio.h>.
 *
 * WHERE EVENTS COME FROM
 * ----------------------
 * Most of them cost NOTHING to produce, because the game already narrates
 * its own overlay/DLL lifecycle through OSReport. platform/null/
 * shims_manual.c's OSReport formats into a buffer and hands each finished
 * line to mp6_event_scan_line() below, which recognises the game's own
 * existing wording and republishes it as a typed event. No decomp source
 * is patched to produce those. The remaining events are posted directly by
 * port code at the seam that already knows the fact (the SE mixer, the
 * board runtime).
 */
#ifndef MP6_EVENTS_H
#define MP6_EVENTS_H

#ifdef __cplusplus
extern "C" {
#endif

#define MP6_EVENT_SLOT_MAX 32 /* distinct keys tracked; posts beyond this still print */
#define MP6_EVENT_KEY_MAX 24
#define MP6_EVENT_VAL_MAX 40

/* Publish one event. `sval` may be NULL (prints "-"). Always prints; also
 * records the key's latest value and bumps the global sequence counter. */
void mp6_event_post(const char *key, long nval, const char *sval);

/* The global sequence counter (number of events posted so far). A waiter
 * samples this when it starts waiting, then asks whether its key has fired
 * with a strictly greater seq. */
unsigned long mp6_event_seq(void);

/* The seq at which `key` last fired, or 0 if it never has. */
unsigned long mp6_event_last_seq(const char *key);

/* Nonzero when `key` has fired at least once AND (want == NULL, or `want`
 * equals either the recorded string value or the decimal spelling of the
 * recorded numeric value). This dual match is deliberate: a caller may
 * write either `ovl.start/w01dll` or `ovl.start/123` and mean the same
 * thing, without needing to know which form the producer chose. */
int mp6_event_matches(const char *key, const char *want);

/* Recognise the game's OWN OSReport wording and republish it as typed
 * events (see the header comment). Safe to call with any text; unknown
 * lines are ignored. Cheap: a handful of anchored prefix compares. */
void mp6_event_scan_line(const char *line);

/* Overlay number -> the decomp's own overlay name (from include/
 * ovl_table.h, the same table game/ovllist.c builds its filename list
 * from), or NULL when out of range. */
const char *mp6_event_ovl_name(long ovlNo);

#ifdef __cplusplus
}
#endif

#endif /* MP6_EVENTS_H */
