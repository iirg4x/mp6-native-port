#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mp6_enhancements.h"

static const char *preset, *ao, *shadow, *wide;
static int queries;
char *mp6_test_getenv(const char *name)
{
    ++queries;
    if (!strcmp(name,"MP6_ENH_PRESET")) return (char *)preset;
    if (!strcmp(name,"MP6_ENH_AMBIENT_OCCLUSION")) return (char *)ao;
    if (!strcmp(name,"MP6_ENH_SHADOW_QUALITY")) return (char *)shadow;
    if (!strcmp(name,"MP6_ENH_WIDESCREEN")) return (char *)wide;
    return NULL;
}

static void check(const Mp6EnhValues *v)
{
    assert(mp6_enh_widescreen()==v->widescreen);
    assert(mp6_enh_unlocked_fps()==v->unlockedFps);
    assert(mp6_enh_shadow_quality()==v->shadowQuality);
    assert(mp6_enh_aa_mode()==v->aa);
    assert(mp6_enh_sfx_voices()==v->sfxVoices);
    assert(mp6_enh_heap_scale()==v->heapScale);
    assert(mp6_enh_ambient_occlusion()==v->ambientOcclusion);
}

int main(void)
{
    Mp6EnhValues v={0,0,1,0,16,1,0}, published;
    check(&v); /* Bootstrap remains usable before any host initialization. */
    ao="2"; assert(mp6_enh_ambient_occlusion()==2);
    ao=""; assert(mp6_enh_ambient_occlusion()==0);
    mp6_enh_cache_environment();
    queries=0;
    for (int i=0;i<10000;i++) check(&v);
    assert(queries==0); /* No environment scanning/parsing in drawing paths. */
    v=(Mp6EnhValues){1,1,8,2,32,4,2};
    mp6_enh_set_values(&v);
    queries=0; check(&v); assert(queries==0); /* Live preference changes. */
    ao="1"; shadow="7"; preset="vanilla-plus"; wide="0";
    mp6_enh_cache_environment();
    Mp6EnhValues resolved={0,1,1,2,32,4,1};
    check(&resolved); /* Per-switch override > preset > published value. */
    mp6_enh_get_values(&published);
    assert(!memcmp(&published,&v,sizeof(v))); /* UI still sees user's config. */
    v=(Mp6EnhValues){0,0,16,1,16,1,0};
    mp6_enh_set_values(&v); check(&resolved); /* Overrides survive live edits. */
    ao="bad"; shadow=""; preset="unknown"; wide=NULL;
    mp6_enh_cache_environment(); check(&v);
    mp6_enh_set_values(NULL); queries=0; check(&v); assert(queries==0);
    puts("enhancement cache: live settings, overrides, bootstrap, and zero hot-path getenv calls PASS");
    return 0;
}
