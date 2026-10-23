#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mp6_enhancements.h"
#include "mp6_heap_scale.h"

int main(void)
{
    Mp6EnhValues settings;
    mp6_enh_defaults(&settings);
    assert(settings.ambientOcclusion == 0 && mp6_enh_ambient_occlusion() == 0);
    settings.ambientOcclusion = 2;
    mp6_enh_set_values(&settings);
    assert(mp6_enh_ambient_occlusion() == 2);
    settings.ambientOcclusion = 0;
    settings.heapScale = 1;
    mp6_enh_set_values(&settings);
    assert(!mp6_heap_scale_restart_pending());
    settings.heapScale = 4;
    mp6_enh_set_values(&settings);
    assert(!mp6_heap_scale_restart_pending()); /* prelaunch must not latch */
    assert(mp6_heap_scale_active() == 4);
    settings.heapScale = 1;
    mp6_enh_set_values(&settings);
    assert(mp6_heap_scale_restart_pending()); /* existing pointers cannot move */
    assert(mp6_heap_scale_active() == 4);
    settings.heapScale = 4;
    mp6_enh_set_values(&settings);
    assert(!mp6_heap_scale_restart_pending());

    puts("runtime settings: PASS");
    return 0;
}
