/* MP6 native port -- host-owned-statics section marker. The rule: any TU
 * whose file-scope statics are owned by a non-game thread or an OS/driver
 * handle (rather than by the deterministic game simulation) force-includes
 * this header, which routes every subsequent static in that TU into the
 * `.mp6hbss`/`.mp6hdat` sections savestate capture/restore excludes by name
 * -- see platform/os/savestate.c for the full carve-out contract.
 *
 * FORCE-INCLUDED (-include) into exactly the TUs whose file-scope state is
 * owned by something other than the game thread: the SDL audio callback
 * thread's mixer, the content-import worker threads, and the modules
 * holding SDL/Dawn/OS handles. The pragma below redirects every subsequent
 * file-scope definition in those TUs into a dedicated PE section, which the
 * savestate module then excludes from both capture and restore by NAME.
 *
 * WHY A SECTION RATHER THAN A LIST OF VARIABLES. The carve-out has to be
 * exhaustive to be safe -- restoring even one mixer static races a thread
 * that is running right now, and memcpy over a live std::mutex/std::thread
 * is undefined behavior. A hand-maintained list of variable names would
 * silently rot the first time someone adds a static to one of these files.
 * A whole-TU section marker cannot: anything added to these files is
 * carved out automatically, and the invariant is checkable at build time
 * (the section must exist and be non-empty).
 *
 * COST OF BEING TOO BROAD, STATED PLAINLY. This deliberately over-selects
 * -- some statics in these TUs are ordinary derived values that would be
 * harmless to restore (e.g. cached window/render dimensions). Excluding
 * them is safe because they are re-derived from the live window every
 * frame, so the restoring process's own values are the CORRECT ones
 * anyway; a savestate captured at one window size and loaded at another
 * should use the current window, not the captured one. Over-selecting is
 * the right direction to err in: the failure mode of excluding too much is
 * a value that recomputes itself, while the failure mode of excluding too
 * little is a data race or UB.
 *
 * NOT applied to platform/os/process_native.c or platform/host/coro_arena.c
 * even though both are port code: they hold load-bearing GAME state (the
 * HuPrc scheduler table, and the coroutine wrappers that table points at)
 * and must be restored. Their host pointers stay valid because the image
 * base, arena base, and coroutine pool base are all pinned -- see
 * shim/include/mp6_savestate.h's header comment.
 */
#ifndef MP6_HOST_SECTION_H
#define MP6_HOST_SECTION_H

/* Must match MP6_HOST_STATE_SECTION_BSS/_DATA in mp6_savestate.h. Spelled
 * literally because #pragma clang section takes a string literal, not a
 * macro expansion.
 *
 * TWO sections, not one: zero-initialized (BSS) and initialized data have
 * different section TYPES, and directing both at a single name is a hard
 * "section type conflict" error -- uninitialized data occupies no space in
 * the file image while initialized data must be stored there. PE section
 * names are capped at 8 bytes, which both of these exactly fill. */
#if defined(__clang__)
#pragma clang section bss = ".mp6hbss" data = ".mp6hdat"
#endif

#endif /* MP6_HOST_SECTION_H */
