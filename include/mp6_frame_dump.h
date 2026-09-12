/* MP6 native port -- debug lever: MP6_FRAME_DUMP.
 *
 * WHAT IT IS
 * ----------
 * An env-gated burst capture of EVERY PRESENTED FRAME, taken from inside
 * the engine at the present site. It exists because external screen
 * capture cannot answer the questions this port actually has: a screenshot
 * tool sampling ~3 frames/second of a mostly-idle scene can neither see
 * nor time a one-frame defect. Flicker is a phenomenon of CONSECUTIVE
 * PRESENTED FRAMES -- you must have frame N-1, N and N+1, in order, with
 * nothing dropped in between, to say anything true about it.
 *
 * WHAT A FRAME IS HERE
 * --------------------
 * Every present. That deliberately includes the interpolated frames the
 * Unlocked-FPS layer (src/gx/frame_interp.c) pushes through
 * aurora_end_frame() during the tick's idle window -- they are presented,
 * the user sees them, and they are a prime suspect for the reported
 * flicker, so a capture that skipped them would be lying by omission. Each
 * dumped frame records whether it was a real tick frame or a replay frame.
 *
 * WHAT THE PIXELS ARE
 * -------------------
 * aurora's present source (lib/webgpu/gpu.cpp present_source(): g_frameBuffer,
 * or g_frameBufferResolved under MSAA) -- the exact texture end_frame()
 * blits to the swapchain, read back at NATIVE framebuffer resolution
 * BEFORE the present-time resample/letterbox pass and BEFORE the ImGui and
 * RmlUi overlay passes. So: no FPS counter, no launcher chrome, no
 * aspect-fit black bars, no resampler filtering -- the frame's own pixels
 * and nothing else. That is what you want for a diff-based flicker
 * detector; it is NOT a faithful "what the monitor showed" screenshot.
 *
 * COST -- AND THE ONE TRAP THAT MATTERS
 * -------------------------------------
 * A GPU->CPU readback plus a synchronous render-worker stall, per captured
 * frame. Nothing is allocated, opened, or read back unless MP6_FRAME_DUMP
 * is set, but while a burst IS running the stall is large enough to matter:
 * a MEASURED full-screen burst at 1280x960 cost ~9.8 ms/frame and consumed
 * the entire tick idle window, so the Unlocked-FPS layer never got to
 * present a single interpolated frame -- every captured frame came back
 * replay=0. A full-screen burst therefore CANNOT be used to study the
 * interpolation layer: it destroys the population it is trying to observe.
 *
 * MP6_FRAME_DUMP_RECT is the fix, and that is why the crop is applied in
 * the GPU copy (aurora-patches/0025) instead of afterwards on the CPU: a
 * small rect makes the readback cheap enough that the idle window survives
 * and interpolated presents keep happening. Aim the rect at the suspect
 * region, check the resulting index.csv actually contains replay=1 rows,
 * and only then believe anything the diff says about interpolated frames.
 *
 * ENV CONTRACT
 * ------------
 *   MP6_FRAME_DUMP=<dir>       enable; frames are written into <dir>
 *                              (created if missing). Unset/empty = the
 *                              whole lever is off and costs one pointer
 *                              test per present.
 *   MP6_FRAME_DUMP_COUNT=<n>   frames to capture in the burst (default 240,
 *                              max 100000).
 *   MP6_FRAME_DUMP_TRIGGER=<s> if set, the burst does not arm until an
 *                              event line whose "key=value" text CONTAINS
 *                              the substring <s> is posted (mp6_event_post,
 *                              include/mp6_events.h). If unset, the
 *                              burst arms at the first present.
 *   MP6_FRAME_DUMP_DELAY=<n>   presents to skip after arming (default 0).
 *   MP6_FRAME_DUMP_STRIDE=<n>  capture every n-th present (default 1 --
 *                              CONSECUTIVE frames, which is the point;
 *                              raise it only when you deliberately want a
 *                              longer time span at lower fidelity, and know
 *                              the flicker detector's A-B-A test is then
 *                              meaningless).
 *   MP6_FRAME_DUMP_DOWNSCALE=<n>  integer point decimation applied on the
 *                              CPU after readback (default 1 = none). Cuts
 *                              file size by n^2; safe for region-level diff
 *                              work, destroys single-pixel evidence.
 *   MP6_FRAME_DUMP_RECT=x,y,w,h   crop, in SOURCE FRAMEBUFFER pixels (which
 *                              are not window pixels: SSAA and the present
 *                              letterbox both sit downstream -- read the
 *                              "src WxH" figure the first captured frame
 *                              prints, or index.csv, and aim at that).
 *                              Applied GPU-side in the copy, so it is also
 *                              the throughput lever -- see COST above.
 *
 * OUTPUT
 * ------
 *   <dir>/f%06u.mfd   one file per captured frame: a 96-byte little-endian
 *                     header then tightly packed w*h*4 bytes, in the source
 *                     texture's channel order (the header's format code says
 *                     which). Format documented in tools/framedump_to_png.py.
 *   <dir>/index.csv   one row per captured frame (seq, present index, tick,
 *                     wall-clock ns, replay flag, geometry) -- the timeline
 *                     the offline tools join against.
 *
 * OFFLINE TOOLS
 *   tools/framedump_to_png.py    .mfd -> .png (stdlib only, no Pillow).
 *   tools/framedump_diff.py      consecutive-frame per-region diff stats
 *                                and the A-B-A single-frame anomaly test.
 *
 * BUILD SCOPE
 * -----------
 * Windowed desktop build only. Headless has no renderer to read back, and
 * the Android aurora archive does not carry the aurora-patches/0025
 * readback entry point -- both compile to standing no-ops, the same split
 * src/gx/shadow_dump.c uses for aurora-patches/0016.
 */
#ifndef MP6_FRAME_DUMP_H
#define MP6_FRAME_DUMP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Call immediately after every aurora_end_frame(). `replayFrame` is 1 for a
 * frame_interp.c interpolated/replay present, 0 for the real per-tick frame.
 * No-op unless a burst is armed and still running. */
void mp6_frame_dump_present(int replayFrame);

/* Feed event text ("key=value") to the MP6_FRAME_DUMP_TRIGGER matcher.
 * Called from mp6_event_post(); safe to call before/after a burst. */
void mp6_frame_dump_trigger(const char *text);

/* 1 while a burst is armed and has frames left to capture. Lets a caller
 * skip work it only wants to do outside a capture window. */
int mp6_frame_dump_active(void);

#ifdef __cplusplus
}
#endif

#endif /* MP6_FRAME_DUMP_H */
