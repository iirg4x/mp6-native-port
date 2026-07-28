#ifndef MP6_AA_RESOLVE_H
#define MP6_AA_RESOLVE_H

#include <string.h>

typedef struct Mp6AaResolved {
    int msaa;
    float ssaa;
    int fxaa;
    int env_override;
    const char *label;
} Mp6AaResolved;

/* Resolve the three legacy input surfaces into exactly one effective mode.
 * Presence of any env lever discards config; enabled env requests then use
 * MSAA > SSAA > FXAA precedence. */
static inline Mp6AaResolved mp6_aa_resolve(const char *env_msaa,
                                            const char *env_ssaa,
                                            const char *env_fxaa,
                                            int cfg_msaa,
                                            float cfg_ssaa,
                                            int cfg_fxaa,
                                            int ssaa_supported)
{
    Mp6AaResolved out = { 1, 1.0f, 0, 0, "Off" };
    out.env_override = env_msaa != NULL || env_ssaa != NULL || env_fxaa != NULL;
    if (out.env_override) {
        if (env_msaa != NULL && strcmp(env_msaa, "4") == 0) {
            out.msaa = 4;
            out.label = "MSAA 4x";
        } else if (ssaa_supported && env_ssaa != NULL && strcmp(env_ssaa, "2") == 0) {
            out.ssaa = 2.0f;
            out.label = "SSAA 2x";
        } else if (ssaa_supported && env_ssaa != NULL && strcmp(env_ssaa, "1.5") == 0) {
            out.ssaa = 1.5f;
            out.label = "SSAA 1.5x";
        } else if (env_fxaa != NULL && strcmp(env_fxaa, "1") == 0) {
            out.fxaa = 1;
            out.label = "FXAA";
        }
        return out;
    }

    if (cfg_msaa > 1) {
        out.msaa = 4;
        out.label = "MSAA 4x";
    } else if (ssaa_supported && cfg_ssaa > 1.0f) {
        out.ssaa = cfg_ssaa >= 2.0f ? 2.0f : 1.5f;
        out.label = out.ssaa >= 2.0f ? "SSAA 2x" : "SSAA 1.5x";
    } else if (cfg_fxaa) {
        out.fxaa = 1;
        out.label = "FXAA";
    }
    return out;
}

#endif /* MP6_AA_RESOLVE_H */
