/* MP6 native port -- the browser window's safe area.
 *
 * SDL's Emscripten backend reports the whole canvas as safe. On an iPhone the
 * notch covers the sides in landscape, and the bottom edge belongs to the
 * system: swipe-home (installed app) and Safari's toolbar (in the browser)
 * take touches there, so a menu hint bar or touch button drawn at the very
 * bottom could not be tapped. The page measures the CSS safe-area insets,
 * with a minimum bottom margin on iOS (globalThis.mp6SafeInsets in
 * packaging/pwa/index.html), and the launcher menus and touch controls use
 * this instead of SDL_GetWindowSafeArea. */
#include "mp6_web_content.h"

#include <SDL3/SDL_video.h>
#include <emscripten.h>

/* Top, right, bottom, left insets in CSS pixels (= SDL window coordinates
 * with SDL_WINDOW_FILL_DOCUMENT). */
EM_JS(int, mp6_web_safe_insets, (int *out), {
    const read = globalThis.mp6SafeInsets;
    const v = read ? read() : null;
    if (!v) return 0;
    for (let i = 0; i < 4; i++) HEAP32[(out >> 2) + i] = Math.max(0, Math.round(v[i] || 0));
    return 1;
});

bool mp6_web_window_safe_area(SDL_Window *window, SDL_Rect *rect)
{
    int w = 0, h = 0;
    int insets[4] = {0, 0, 0, 0};
    if (window == NULL || rect == NULL || !SDL_GetWindowSize(window, &w, &h) || w <= 0 || h <= 0) {
        return false;
    }
    if (!mp6_web_safe_insets(insets)) {
        return SDL_GetWindowSafeArea(window, rect);
    }
    rect->x = insets[3];
    rect->y = insets[0];
    rect->w = w - insets[3] - insets[1];
    rect->h = h - insets[0] - insets[2];
    return rect->w > 0 && rect->h > 0;
}
