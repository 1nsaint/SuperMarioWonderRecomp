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

#include "mods_data.h"
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

// Hor+ widescreen (what Hades II does, and the Hephaistos mod for Hades on PC): the engine's virtual screen stays
// 1080 high and becomes 1080 * aspect wide, so a wider screen shows more of the scene instead of a stretched 16:9.
// The engine keeps that virtual size in globals its static initializers fill (FULL_SCREEN (1920f, 1080f), the
// screen centres (960f, 540f), the virtual resolution (1920, 1080)): each translation unit has its own copies,
// 234 in all. After the initializers, rewrite every one of them in main's data and bss.
uint32_t g_virtual_width = 1920;

void apply_widescreen() {
    uint32_t w = 0, h = 0;
    plat::screen_size(w, h);
    if (!w || !h || plat::setting("widescreen") == "0" || plat::setting("fullscreen") == "0") return;
    uint32_t vw = uint32_t(1080.0 * w / h + 0.5) & ~1u;  // even, so the centre is whole
    if (vw <= 1920) return;  // 16:9 or taller: the engine's own layout
    g_virtual_width = vw;
    const uint64_t base = nx_module_main.base;
    const uint64_t from = base + nx_module_main.data, to = base + nx_module_main.bss + nx_module_main.bss_size;
    auto f32 = [](float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return uint64_t(u);
    };
    const uint64_t full = f32(1920) | f32(1080) << 32, centre = f32(960) | f32(540) << 32, ires = 1920ull | 1080ull << 32;
    const uint64_t new_full = f32(float(vw)) | f32(1080) << 32, new_centre = f32(float(vw / 2)) | f32(540) << 32,
                   new_ires = uint64_t(vw) | 1080ull << 32;
    int n_full = 0, n_centre = 0, n_ires = 0;
    for (uint64_t a = from; a + 8 <= to; a += 4) {
        uint64_t v = ld<uint64_t>(a);
        if (v == full) st<uint64_t>(a, new_full), n_full++;
        else if (v == centre) st<uint64_t>(a, new_centre), n_centre++;
        else if (v == ires) st<uint64_t>(a, new_ires), n_ires++;
    }
    log("widescreen: virtual screen %ux1080 for %ux%u (%d full-screen, %d centre, %d resolution globals)", vw, w, h,
        n_full, n_centre, n_ires);

    // The Lua side: the Hephaistos mod (runtime/mods/hephaistos, MIT) re-anchors the HUD and screens and fixes the
    // camera clamp for the new ScreenWidth; the game loads it through an Import appended to RoomManager.lua.
    for (const ModFile& m : kModFiles) add_virtual_file(std::string("Content/") + m.path, m.data);
    add_virtual_file("Content/Mods/Hephaistos/HephaistosConfig.lua",
                     "Hephaistos.ScreenWidth = " + std::to_string(vw) +
                         "\nHephaistos.ScreenHeight = 1080\nHephaistos.CenterHUD = " +
                         (plat::setting("center_hud") == "1" ? "true" : "false") + "\n");
    std::string rm = read_game_file("Content/Scripts/RoomManager.lua");
    if (rm.empty()) {
        log("widescreen: Scripts/RoomManager.lua not found, Lua layout left 16:9");
        return;
    }
    add_virtual_file("Content/Scripts/RoomManager.lua", rm + "\nImport \"../Mods/Hephaistos/Hephaistos.lua\"\n");
    log("widescreen: Hephaistos Lua mod loaded (%zu files)", sizeof kModFiles / sizeof kModFiles[0]);
}

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
