/* Real launcher config parser/writer, with only filesystem plumbing replaced. */
#include "../../src/gx/ui/launcher_state.hpp"
#include "mp6_json.h"
#include "mp6_path.h"
#include "mp6_enhancements.h"
#include <cassert>
#include <cmath>
#include <fstream>
#include <sstream>
static Mp6LauncherConfig g_cfg;
static char g_configPath[1024];
static FILE *mp6_fopen_utf8(const char *path, const char *mode) { return fopen(path, mode); }
static int mp6_replace_utf8(const char *from, const char *to) { return rename(from, to); }
static int mp6_remove_utf8(const char *path) { return remove(path); }
#include "touch_config_subject.inc"
int main(int argc, char **argv)
{
    assert(argc == 2);
    snprintf(g_configPath, sizeof(g_configPath), "%s", argv[1]);
    mp6_launcher_defaults();
    assert(g_cfg.ambientOcclusion == 0); // upgrades never silently enable AO
    assert(mp6_launcher_parse_config("{\"enhancements.ambient_occlusion\":2,\"enhancements.preset\":\"modern\"}"));
    assert(g_cfg.ambientOcclusion == 2);
    auto aoValues = mp6_launcher_enh_values();
    assert(mp6_enh_preset_derive(&aoValues) == MP6_ENH_PRESET_CUSTOM);
    assert(!mp6_launcher_parse_config("{\"enhancements.ambient_occlusion\":0,\"enhancements.ambient_occlusion\":1}"));
    assert(g_cfg.ambientOcclusion == 2); // duplicate key cannot partially apply
    assert(mp6_launcher_parse_config("{\"enhancements.ambient_occlusion\":1e100}"));
    assert(g_cfg.ambientOcclusion == 0);
    assert(mp6_launcher_parse_config("{\"enhancements.ambient_occlusion\":1}"));
    assert(mp6_launcher_parse_config("{\"audio.master_volume\":75}"));
    assert(g_cfg.masterVolume == 75 && g_cfg.touch.enabled && g_cfg.touch.size == 100 &&
           g_cfg.touch.opacity == 55);
    assert(mp6_launcher_parse_config(
        "{\"touch.enabled\":false,\"touch.size\":125,\"touch.opacity\":35,\"touch.floating\":true}"));
    assert(!g_cfg.touch.enabled && g_cfg.touch.size == 125 && g_cfg.touch.opacity == 35 &&
           g_cfg.touch.floating);
    auto before = g_cfg.touch;
    assert(!mp6_launcher_parse_config("{\"touch.size\":100,\"touch.size\":75}"));
    assert(!memcmp(&before, &g_cfg.touch, sizeof(before)));
    assert(!mp6_launcher_parse_config("{\"touch.opacity\":50,broken}"));
    assert(!memcmp(&before, &g_cfg.touch, sizeof(before)));
    assert(mp6_launcher_parse_config("{\"touch.layout\":\"1;99999999999999999;\"}"));
    assert(!memcmp(&before, &g_cfg.touch, sizeof(before)));
    assert(mp6_launcher_parse_config("{\"touch.size\":1e100,\"touch.opacity\":-9}"));
    assert(g_cfg.touch.size == 100 && g_cfg.touch.opacity == 55);
    g_cfg.touch = before;
    g_cfg.touch.visible &= ~(1u << MP6_TOUCH_B);
    g_cfg.touch.x[MP6_TOUCH_STICK] = 1234;
    g_cfg.touch.y[MP6_TOUCH_STICK] = 5678;
    before = g_cfg.touch;
    mp6_launcher_config_save();
    std::ifstream file(g_configPath);
    std::stringstream text;
    text << file.rdbuf();
    assert(!text.str().empty());
    mp6_launcher_defaults();
    assert(mp6_launcher_parse_config(text.str().c_str()));
    assert(!memcmp(&before, &g_cfg.touch, sizeof(before)) && g_cfg.masterVolume == 75);
    assert(g_cfg.ambientOcclusion == 1);
    puts("PASS: old configs, strict touch keys, bounds, atomic parse and save/load round-trip");
}
