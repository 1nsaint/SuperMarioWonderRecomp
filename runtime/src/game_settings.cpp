// Hades' engine settings: console variables the engine registers in static constructors (main+24f5c0, with their
// defaults) and then overrides from the command line. The runtime sets them right after the constructors, before
// nnMain, from the player's settings (config.txt on Android, NX_<NAME> on PC).
//
//   fullscreen=1 (default)  fill a screen wider than 16:9 (Letterbox=0, UseSideBarArt=0) instead of pillarboxing
//                           the 16:9 view with the side art
//   <Variable>=<value>      any variable of the table, e.g. SwapInterval=1
#include <cstdlib>
#include <cstring>
#include <string>

#include <nx/runtime.h>

#include "platform.h"

extern const nx::ModuleInfo nx_module_main;

namespace nx {

namespace {

struct EngineVar {
    const char* name;
    uint64_t name_at;  // the name string in main (checked, so another game version is left alone)
    uint64_t value_at;
    bool is_bool;
};

constexpr EngineVar kVars[] = {
    {"NoBorder", 0x696842, 0xeeeb38, true},
    {"SwapInterval", 0x697a5b, 0xeeeb3c, false},
    {"NoFixedStep", 0x681e6a, 0xeeeb40, true},
    {"Letterbox", 0x67b86d, 0xeeeb44, true},
    {"UseSideBarArt", 0x697a68, 0xeeeb48, true},
};

}  // namespace

void apply_game_settings() {
    const uint64_t base = nx_module_main.base;
    bool fullscreen = plat::setting("fullscreen") != "0";
    for (const EngineVar& v : kVars) {
        if (std::strcmp(reinterpret_cast<const char*>(base + v.name_at), v.name) != 0) {
            log("settings: %s not at main+%llx (another game version?), engine settings left alone", v.name,
                (unsigned long long)v.name_at);
            return;
        }
        std::string s = plat::setting(v.name);
        if (s.empty() && fullscreen && (std::strcmp(v.name, "Letterbox") == 0 || std::strcmp(v.name, "UseSideBarArt") == 0))
            s = "0";
        if (s.empty()) continue;
        int32_t value = s == "true" ? 1 : s == "false" ? 0 : int32_t(std::strtol(s.c_str(), nullptr, 10));
        if (v.is_bool)
            st<uint8_t>(base + v.value_at, value ? 1 : 0);
        else
            st<uint32_t>(base + v.value_at, uint32_t(value));
        log("settings: %s = %d", v.name, value);
    }
}

// The engine variables as they are now (logged on the first frame: shows what the engine did with them).
void log_game_settings() {
    const uint64_t base = nx_module_main.base;
    for (const EngineVar& v : kVars)
        log("settings: now %s = %u", v.name, v.is_bool ? ld<uint8_t>(base + v.value_at) : ld<uint32_t>(base + v.value_at));
}

}  // namespace nx
