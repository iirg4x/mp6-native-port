#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mp6_console.h"

int main(void) {
    mp6_console_set_available(1);
    mp6_console_open();
    const char *matches[MP6_CONSOLE_PANEL_COUNT] = {0};
    int start = -1;
    assert(mp6_console_complete("stat ", NULL, 0, &start) == MP6_CONSOLE_PANEL_COUNT);
    assert(start == 5);
    assert(mp6_console_complete("stat ", matches, MP6_CONSOLE_PANEL_COUNT, &start) == MP6_CONSOLE_PANEL_COUNT);
    for (int i = 0; i < MP6_CONSOLE_PANEL_COUNT; ++i) {
        assert(matches[i] && strcmp(matches[i], mp6_console_panel_name(i)) == 0);
        char command[80];
        snprintf(command, sizeof(command), "stat %s", matches[i]);
        mp6_console_exec(command);
        if (i != MP6_CONSOLE_PANEL_NONE) assert(mp6_console_overlay(i));
    }
    assert(mp6_console_overlay_count() == MP6_CONSOLE_PANEL_COUNT - 1);
    assert(strcmp(matches[MP6_CONSOLE_PANEL_COUNT - 1], "board") == 0);
    mp6_console_exec("stat none");
    assert(mp6_console_overlay_count() == 0);
    assert(mp6_console_complete("stat g", matches, MP6_CONSOLE_PANEL_COUNT, &start) == 2);
    assert(strcmp(matches[0], "game") == 0 && strcmp(matches[1], "gpu") == 0);
    mp6_console_exec("stat gpu");
    mp6_console_exec("stat scenerendering");
    mp6_console_exec("echo hello");
    assert(mp6_console_overlay_count() == 2 && mp6_console_log_count() > 0);
    int generation = mp6_console_log_total();
    mp6_console_exec("clear");
    assert(mp6_console_overlay_count() == 0 && mp6_console_log_count() == 0);
    assert(mp6_console_log_total() > generation && mp6_console_is_open());
    mp6_console_exec("stat gpu");
    mp6_console_exec("clear log");
    assert(mp6_console_overlay_count() == 1 && mp6_console_log_count() == 0);
    mp6_console_exec("echo retained");
    mp6_console_exec("clear stats");
    assert(mp6_console_overlay_count() == 0 && mp6_console_log_count() > 0);
    mp6_console_exec("stat fps");
    mp6_console_exec("clear unknown");
    assert(mp6_console_overlay_count() == 1);
    mp6_console_exec("clear");
    mp6_console_close();
    assert(!mp6_console_stats_armed());
    puts("PASS: every stat command is completable and executable; clear resets output/overlays");
}
