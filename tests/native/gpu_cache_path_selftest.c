/* Actual cache selector, with filesystem calls observed rather than performed. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *pref_root="C:/Users/Test/AppData/Roaming/mp6native/";
static int pref_calls, create_calls, create_ok=1;
static char created[512];
static char *SDL_GetPrefPath(const char *org, const char *app) {
    (void)org;(void)app;pref_calls++;
    if (!pref_root) return NULL;
    size_t n=strlen(pref_root)+1;
    char *p=malloc(n);memcpy(p,pref_root,n);return p;
}
static bool SDL_CreateDirectory(const char *path) {
    create_calls++;snprintf(created,sizeof(created),"%s",path);return create_ok;
}
static const char *SDL_GetError(void) {return "test directory failure";}
#define SDL_free free
#include "board_subject.inc"
#define CHECK(c) do {if (!(c)) {printf("FAIL line %d: %s\n",__LINE__,#c);return 1;}} while(0)
int main(void) {
    char *p=mp6_bridge_gpu_cache_path("mp6",NULL);
    CHECK(p && !strcmp(p,"C:/Users/Test/AppData/Roaming/mp6native/gpu-cache-v3"));
    CHECK(pref_calls==1 && create_calls==1 && !strcmp(created,p));free(p);
    p=mp6_bridge_gpu_cache_path("mp6","");
    CHECK(p && pref_calls==2);free(p);
    p=mp6_bridge_gpu_cache_path("mp6","build/test-cache\\");
    CHECK(p && !strcmp(p,"build/test-cache/gpu-cache-v3") && pref_calls==2);free(p);
    pref_root="/data/user/0/com.mp6.game/files/";
    p=mp6_bridge_gpu_cache_path("mp6",NULL);
    CHECK(p && !strcmp(p,"/data/user/0/com.mp6.game/files/gpu-cache-v3"));free(p);
    create_ok=0;
    CHECK(mp6_bridge_gpu_cache_path("mp6","build/read-only")==NULL);
    pref_root=NULL;
    int before=create_calls;
    CHECK(mp6_bridge_gpu_cache_path("mp6",NULL)==NULL && create_calls==before);
    puts("Graphics cache generation: desktop, Android, override, and fail-closed: PASS");
    return 0;
}
