/* MP6 native port -- SDL3's file dialogs for the browser build.
 *
 * Emscripten's SDL3 port has no dialog backend. The PWA shell supplies the
 * game content itself (see packaging/pwa/), so the in-game content setup
 * page's "browse" buttons report a cancelled pick: the callback receives an
 * empty file list, exactly what SDL delivers when the user closes a dialog. */
#include <SDL3/SDL_dialog.h>

static const char *const kNoFiles[] = { NULL };

void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
                            const SDL_DialogFileFilter *filters, int nfilters,
                            const char *default_location, bool allow_many)
{
    (void)window; (void)filters; (void)nfilters; (void)default_location; (void)allow_many;
    if (callback) callback(userdata, kNoFiles, -1);
}

void SDL_ShowOpenFolderDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
                              const char *default_location, bool allow_many)
{
    (void)window; (void)default_location; (void)allow_many;
    if (callback) callback(userdata, kNoFiles, -1);
}
