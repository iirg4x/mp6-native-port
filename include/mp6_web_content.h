#ifndef MP6_WEB_CONTENT_H
#define MP6_WEB_CONTENT_H

/* Browser build only (src/web/web_content.c): read-only game content served
 * by the page's async content provider, plain fopen for everything else. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

FILE *mp6_web_fopen(const char *path, const char *mode);

/* The disc layer's positional reads (host.h mp6_host_file_*). open returns 1
 * when the provider serves path, with its size in *sizeOut (-1 when it has
 * no such file); 0 when the path is not provider content. read_at returns
 * the bytes read (short only at end of file) or -1. */
int mp6_web_content_open(const char *path, int64_t *sizeOut);
int64_t mp6_web_content_read_at(const char *path, int64_t size, void *dst, size_t len,
                                uint64_t offset);
/* Starts the provider fetching [offset, offset+len) of path without waiting
 * (host.h mp6_host_file_prefetch). 1 when a provider is registered (with a
 * NULL path, that is all it answers), else 0. */
int mp6_web_content_prefetch(const char *path, uint64_t offset, uint64_t len);
/* read_at, but only from bytes the provider already holds: -1 rather than
 * suspending the program to fetch. */
int64_t mp6_web_content_read_cached_at(const char *path, void *dst, size_t len, uint64_t offset);

/* The window area clear of the notch and of the system's bottom gesture zone
 * (iOS swipe-home and Safari's toolbar take touches there), in window
 * coordinates, from the page's CSS safe-area insets. Stands in for
 * SDL_GetWindowSafeArea, which reports the whole canvas in a browser. */
struct SDL_Window;
struct SDL_Rect;
bool mp6_web_window_safe_area(struct SDL_Window *window, struct SDL_Rect *rect);

#ifdef __cplusplus
}
#endif

#endif
