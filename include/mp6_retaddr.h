/* MP6 native port -- the caller's return address, as a debug tag.
 *
 * The game's allocators and the GX draw hooks tag records with their
 * caller's return address (what MWCC's `mflr` read on the console). On the
 * web, emscripten implements __builtin_return_address by formatting a full
 * JavaScript stack trace and parsing it -- and still returns 0 for wasm
 * frames -- so every draw call and allocation paid for a stack trace: most
 * of a board frame's CPU time in a desktop browser. There it is 0 for free.
 */
#ifndef MP6_RETADDR_H
#define MP6_RETADDR_H

#ifdef __EMSCRIPTEN__
#define MP6_RETURN_ADDRESS() ((void *)0)
#else
#define MP6_RETURN_ADDRESS() __builtin_return_address(0)
#endif

#endif
