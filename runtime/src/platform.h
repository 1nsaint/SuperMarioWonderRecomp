// What the runtime needs from the host OS. platform_win.cpp and platform_android.cpp implement it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace nx::plat {

// ---- memory: fixed-address reservations (guest pointers are host pointers) ----
bool reserve(uint64_t addr, uint64_t size);  // exactly at addr, no access
bool commit(uint64_t addr, uint64_t size);   // read/write
void decommit(uint64_t addr, uint64_t size);

// ---- threads and time ----
using ThreadFn = void (*)(void*);
void start_thread(ThreadFn fn, void* arg, size_t stack_size);
[[noreturn]] void exit_thread();
[[noreturn]] void exit_process(int code);
void sleep_ns(int64_t ns);
void yield();
void set_thread_name(const char* name);
int64_t current_thread_id();  // the host OS thread id
// Samples the given threads (thread CPU time, 1 kHz) for `seconds`: host pc, a few host frames and the guest lr
// (read through guest_lr[k], the thread's x30). Writes one line per sample to `path`:
//   <tid> <guest lr> <module>:<offset> [<module>:<offset> ...]   (host pc first, then callers)
// False where not implemented.
bool profile(const int64_t* tids, const uint64_t* const* guest_lr, size_t count, int seconds, const std::string& path);  // the host thread's name (debuggers, profilers, top)
uint64_t monotonic_ns();

// ---- files ----
FILE* fopen_utf8(const std::string& path, const char* mode);
int64_t ftell64(FILE* f);
int fseek64(FILE* f, int64_t off, int origin);
bool make_dirs(const std::string& path);

// ---- output ----
void log_line(const char* line);  // stderr / logcat
std::string data_root();
std::string setting(const std::string& key);  // a player setting: config.txt (Android), NX_<KEY> (PC); "" if unset          // where private/ lives ("private" on PC, the app's external files dir on Android)

// ---- display, input, audio ----
void* create_window();       // native window handle for the Vulkan surface (HWND / ANativeWindow*), kept alive
void release_window(void* w);  // done with a handle from create_window
uint64_t window_generation();  // changes when the OS replaced the window (Android: app back from the background)
bool window_minimized();     // no surface to draw to (minimized / app in background)
void window_size(uint32_t& w, uint32_t& h);  // client size in pixels

// NpadButton bits (Nintendo layout: A is the right face button, B the bottom one).
enum : uint64_t {
    kA = 1ull << 0, kB = 1ull << 1, kX = 1ull << 2, kY = 1ull << 3, kStickL = 1ull << 4, kStickR = 1ull << 5,
    kL = 1ull << 6, kR = 1ull << 7, kZL = 1ull << 8, kZR = 1ull << 9, kPlus = 1ull << 10, kMinus = 1ull << 11,
    kLeft = 1ull << 12, kUp = 1ull << 13, kRight = 1ull << 14, kDown = 1ull << 15,
    kStickLLeft = 1ull << 16, kStickLUp = 1ull << 17, kStickLRight = 1ull << 18, kStickLDown = 1ull << 19,
    kStickRLeft = 1ull << 20, kStickRUp = 1ull << 21, kStickRRight = 1ull << 22, kStickRDown = 1ull << 23,
};
constexpr int32_t kStickMax = 0x7FFF;

struct PadState {
    uint64_t buttons = 0;  // NpadButton bits
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
};
void read_pad(PadState& p);  // player 1

// Audio: s16 stereo 48 kHz. `done` is called (on any thread) when a submitted buffer has been played.
using AudioDone = void (*)(void* ctx);
bool audio_open(AudioDone done);
bool audio_submit(const void* data, size_t bytes, void* ctx);  // false: no device (caller paces itself)
uint32_t audio_queued();
void audio_pause(bool paused);  // app in the background

// Vulkan: the loader's vkGetInstanceProcAddr and the surface extension of this platform.
void* vulkan_get_instance_proc_addr();
const char* vulkan_surface_extension();
// Creates the platform surface for `native_window` (VkInstance, VkSurfaceKHR*), returns a VkResult.
int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void* gipa);

}  // namespace nx::plat
