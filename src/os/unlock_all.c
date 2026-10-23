/* MP6 native port -- the "Unlock All" mod store (include/mp6_unlock_all.h
 * has the contract). Dependency-free: links into every build mode. */

#include "mp6_unlock_all.h"

#include <stdio.h>
#include <stdlib.h>

/* SAVESTATE CARVE-OUT: the published setting and the environment latch
 * describe the running process, not the captured game. Must sit AFTER this
 * TU's own includes and at preprocessor TOP LEVEL. See mp6_host_section.h. */
#include "mp6_host_section.h"

static int g_setting;
/* -2 = environment not read yet; -1 = MP6_UNLOCK_ALL unset; else 0/1. */
static int g_env = -2;

static int unlock_all_env(void)
{
    if (g_env == -2) {
        const char *value = getenv("MP6_UNLOCK_ALL");
        if (value == NULL || value[0] == '\0') g_env = -1;
        else g_env = (value[0] == '0' && value[1] == '\0') ? 0 : 1;
    }
    return g_env;
}

int mp6_unlock_all_enabled(void)
{
    const int env = unlock_all_env();
    return env >= 0 ? env : g_setting;
}

void mp6_unlock_all_set_enabled(int enabled)
{
    enabled = enabled ? 1 : 0;
    if (enabled != g_setting) {
        printf("[MODS] Unlock All: %s\n", enabled ? "on" : "off");
        fflush(stdout);
    }
    g_setting = enabled;
}

int mp6_unlock_all_env_override(void)
{
    return unlock_all_env() >= 0;
}
