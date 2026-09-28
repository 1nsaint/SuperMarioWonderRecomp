// What the shared runtime asks of the game it is built for. Each game implements this in games/<game>/game.cpp
// (CMake: -DNX_GAME=<game>); everything that only makes sense for one game lives there.
#pragma once

#include <cstdint>

namespace nx::game {

struct Info {
    const char* id;               // lowercase: CMake target, library, executable, log tag ("hades")
    const char* title;            // for the player: window title, Android data folder ("Hades")
    uint64_t program_id;          // svcGetInfo(ProgramId)
    const char* android_package;  // the APK's package (games/<game>/AndroidManifest.xml)
    const char* pc_data_root;     // PC: where analysis/ and extract/ are ("private/<game>")
};
extern const Info kInfo;

// After the modules' static constructors, right before nnMain: patch globals, add virtual files.
void after_init();
// The game starts its renderer (vkCreateInstance): settings the engine may have reset while loading its config.
void renderer_start();
// The first frame was presented.
void first_frame();

}  // namespace nx::game
