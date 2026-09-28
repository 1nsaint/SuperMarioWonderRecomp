// Android host (NativeActivity): memory (mmap at fixed addresses), pthreads, the app's window (ANativeWindow),
// game controllers + touch, AAudio, the Vulkan loader and android surface, a signal crash handler and
// android_main. Game data lives in the app's external files dir (see docs/ANDROID.md):
//   <files>/analysis/{main,sdk}.bin   module images (tools/nso2bin.py)
//   <files>/extract/romfs/...         the RomFS
//   <files>/user/...                  saves
//   <files>/config.txt                optional: resolution=WxH
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace nx {
void boot();
}

namespace nx::plat {

namespace {
android_app* g_app = nullptr;
std::string g_root;
std::atomic<ANativeWindow*> g_window{nullptr};
std::atomic<bool> g_visible{false};
}  // namespace

// ---- memory ---------------------------------------------------------------------------------------------------

bool reserve(uint64_t addr, uint64_t size) {
    void* p = mmap(reinterpret_cast<void*>(addr), size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (uint64_t(p) != addr) {  // old kernels ignore the flag and treat addr as a hint
        munmap(p, size);
        return false;
    }
    return true;
}

bool commit(uint64_t addr, uint64_t size) {
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    return mprotect(reinterpret_cast<void*>(a), e - a, PROT_READ | PROT_WRITE) == 0;
}

void decommit(uint64_t addr, uint64_t size) {
    madvise(reinterpret_cast<void*>(addr), size, MADV_DONTNEED);
    mprotect(reinterpret_cast<void*>(addr), size, PROT_NONE);
}

// ---- threads and time -----------------------------------------------------------------------------------------

struct ThreadStart {
    ThreadFn fn;
    void* arg;
};

static void* thread_entry(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    s.fn(s.arg);
    return nullptr;
}

void start_thread(ThreadFn fn, void* arg, size_t stack_size) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack_size);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &a, thread_entry, new ThreadStart{fn, arg});
    pthread_attr_destroy(&a);
}

void exit_thread() { pthread_exit(nullptr); }
void exit_process(int code) { _exit(code); }

void sleep_ns(int64_t ns) {
    timespec ts{time_t(ns / 1000000000), long(ns % 1000000000)};
    nanosleep(&ts, nullptr);
}

void yield() { sched_yield(); }

uint64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// ---- files and output -----------------------------------------------------------------------------------------

FILE* fopen_utf8(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }
int64_t ftell64(FILE* f) { return ftello(f); }
int fseek64(FILE* f, int64_t off, int origin) { return fseeko(f, off, origin); }
bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

void log_line(const char* line) { __android_log_write(ANDROID_LOG_INFO, "hades", line); }

std::string data_root() { return g_root; }

static std::string config(const std::string& key) {
    std::ifstream f(g_root + "/config.txt");
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos && line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return {};
}

// ---- window ---------------------------------------------------------------------------------------------------

void* create_window() {
    while (!g_window) sleep_ns(10'000'000);
    return g_window.load();
}

bool window_minimized() { return !g_visible || !g_window; }

void window_size(uint32_t& w, uint32_t& h) {
    std::string r = config("resolution");
    unsigned cw = 0, ch = 0;
    if (!r.empty() && std::sscanf(r.c_str(), "%ux%u", &cw, &ch) == 2 && cw && ch) {
        w = cw;
        h = ch;
        return;
    }
    ANativeWindow* win = static_cast<ANativeWindow*>(create_window());
    // The phone's own screen, landscape.
    int32_t a = ANativeWindow_getWidth(win), b = ANativeWindow_getHeight(win);
    w = uint32_t(std::max(a, b));
    h = uint32_t(std::min(a, b));
}

// ---- input: game controllers + touch --------------------------------------------------------------------------
// Touch: left half = virtual stick (relative to where the finger went down); right half = four buttons in a
// diamond around the thumb (Y attack up-left, X special up-right, B dash down-left, A interact down-right), top
// corners L / R, top centre +.

namespace {
std::mutex g_in;
PadState g_pad_buttons;  // controller state
struct Touch {
    int32_t id;
    float x0, y0, x, y;
    bool left;
    uint64_t button;
};
std::vector<Touch> g_touches;
float g_w = 1, g_h = 1;

uint64_t key_to_button(int32_t k) {
    switch (k) {
        case AKEYCODE_BUTTON_A: return kB;  // bottom face button = Nintendo B
        case AKEYCODE_BUTTON_B: return kA;  // right face button = Nintendo A
        case AKEYCODE_BUTTON_X: return kY;
        case AKEYCODE_BUTTON_Y: return kX;
        case AKEYCODE_BUTTON_L1: return kL;
        case AKEYCODE_BUTTON_R1: return kR;
        case AKEYCODE_BUTTON_L2: return kZL;
        case AKEYCODE_BUTTON_R2: return kZR;
        case AKEYCODE_BUTTON_START: return kPlus;
        case AKEYCODE_BUTTON_SELECT: return kMinus;
        case AKEYCODE_BUTTON_THUMBL: return kStickL;
        case AKEYCODE_BUTTON_THUMBR: return kStickR;
        case AKEYCODE_DPAD_LEFT: return kLeft;
        case AKEYCODE_DPAD_UP: return kUp;
        case AKEYCODE_DPAD_RIGHT: return kRight;
        case AKEYCODE_DPAD_DOWN: return kDown;
        case AKEYCODE_BACK: return kMinus;
        default: return 0;
    }
}

uint64_t touch_button(float x, float y) {
    float nx_ = x / g_w, ny = y / g_h;
    if (ny < 0.2f) return nx_ < 0.6f ? (nx_ < 0.45f ? kL : kPlus) : (nx_ > 0.85f ? kR : kZR);
    bool up = ny < 0.6f, right = nx_ > 0.8f;
    if (up) return right ? kX : kY;
    return right ? kA : kB;
}

int32_t on_input(android_app*, AInputEvent* e) {
    std::lock_guard lk(g_in);
    int32_t type = AInputEvent_getType(e);
    int32_t src = AInputEvent_getSource(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        uint64_t b = key_to_button(AKeyEvent_getKeyCode(e));
        if (!b) return 0;
        if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN)
            g_pad_buttons.buttons |= b;
        else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP)
            g_pad_buttons.buttons &= ~b;
        return 1;
    }
    if (type != AINPUT_EVENT_TYPE_MOTION) return 0;
    if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        auto axis = [&](int32_t a) {
            float v = AMotionEvent_getAxisValue(e, a, 0);
            return std::fabs(v) < 0.15f ? 0 : int32_t(std::clamp(v, -1.f, 1.f) * kStickMax);
        };
        g_pad_buttons.lx = axis(AMOTION_EVENT_AXIS_X);
        g_pad_buttons.ly = -axis(AMOTION_EVENT_AXIS_Y);
        g_pad_buttons.rx = axis(AMOTION_EVENT_AXIS_Z);
        g_pad_buttons.ry = -axis(AMOTION_EVENT_AXIS_RZ);
        float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
        float hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
        g_pad_buttons.buttons &= ~(kLeft | kRight | kUp | kDown | kZL | kZR);
        if (hx < -0.5f) g_pad_buttons.buttons |= kLeft;
        if (hx > 0.5f) g_pad_buttons.buttons |= kRight;
        if (hy < -0.5f) g_pad_buttons.buttons |= kUp;
        if (hy > 0.5f) g_pad_buttons.buttons |= kDown;
        if (AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0) > 0.4f ||
            AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0) > 0.4f)
            g_pad_buttons.buttons |= kZL;
        if (AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0) > 0.4f ||
            AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0) > 0.4f)
            g_pad_buttons.buttons |= kZR;
        return 1;
    }
    if ((src & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN) return 0;
    int32_t action = AMotionEvent_getAction(e);
    int32_t kind = action & AMOTION_EVENT_ACTION_MASK;
    size_t index = size_t((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    if (g_window) {
        g_w = float(ANativeWindow_getWidth(g_window));
        g_h = float(ANativeWindow_getHeight(g_window));
    }
    if (kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        float x = AMotionEvent_getX(e, index), y = AMotionEvent_getY(e, index);
        bool left = x < g_w * 0.5f;
        g_touches.push_back({AMotionEvent_getPointerId(e, index), x, y, x, y, left, left ? 0 : touch_button(x, y)});
    } else if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_POINTER_UP ||
               kind == AMOTION_EVENT_ACTION_CANCEL) {
        int32_t id = AMotionEvent_getPointerId(e, index);
        if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_CANCEL) g_touches.clear();
        g_touches.erase(std::remove_if(g_touches.begin(), g_touches.end(), [&](const Touch& t) { return t.id == id; }),
                        g_touches.end());
    } else if (kind == AMOTION_EVENT_ACTION_MOVE) {
        for (size_t k = 0; k < AMotionEvent_getPointerCount(e); k++) {
            int32_t id = AMotionEvent_getPointerId(e, k);
            for (auto& t : g_touches)
                if (t.id == id) {
                    t.x = AMotionEvent_getX(e, k);
                    t.y = AMotionEvent_getY(e, k);
                }
        }
    }
    return 1;
}
}  // namespace

void read_pad(PadState& p) {
    std::lock_guard lk(g_in);
    p.buttons |= g_pad_buttons.buttons;
    if (g_pad_buttons.lx || g_pad_buttons.ly) {
        p.lx = g_pad_buttons.lx;
        p.ly = g_pad_buttons.ly;
    }
    p.rx = g_pad_buttons.rx;
    p.ry = g_pad_buttons.ry;
    float radius = std::max(1.f, g_h * 0.12f);
    for (const auto& t : g_touches) {
        if (t.left) {
            float dx = (t.x - t.x0) / radius, dy = (t.y - t.y0) / radius;
            p.lx = int32_t(std::clamp(dx, -1.f, 1.f) * kStickMax);
            p.ly = int32_t(std::clamp(-dy, -1.f, 1.f) * kStickMax);
        } else {
            p.buttons |= t.button;
        }
    }
}

// ---- audio: AAudio (callback pulls from a queue of submitted buffers) ------------------------------------------

namespace {
struct Pending {
    const uint8_t* data;
    size_t size, pos;
    void* ctx;
};
std::mutex g_audio;
std::deque<Pending> g_pending;
AudioDone g_done = nullptr;
AAudioStream* g_stream = nullptr;

aaudio_data_callback_result_t audio_cb(AAudioStream*, void*, void* out, int32_t frames) {
    auto* dst = static_cast<uint8_t*>(out);
    size_t want = size_t(frames) * 4;
    std::vector<void*> finished;
    {
        std::lock_guard lk(g_audio);
        while (want && !g_pending.empty()) {
            Pending& p = g_pending.front();
            size_t n = std::min(want, p.size - p.pos);
            std::memcpy(dst, p.data + p.pos, n);
            dst += n;
            want -= n;
            p.pos += n;
            if (p.pos == p.size) {
                finished.push_back(p.ctx);
                g_pending.pop_front();
            }
        }
    }
    if (want) std::memset(dst, 0, want);  // underrun: silence
    for (void* c : finished) g_done(c);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
}  // namespace

bool audio_open(AudioDone done) {
    g_done = done;
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, audio_cb, nullptr);
    bool ok = AAudioStreamBuilder_openStream(b, &g_stream) == AAUDIO_OK && AAudioStream_requestStart(g_stream) == AAUDIO_OK;
    AAudioStreamBuilder_delete(b);
    return ok;
}

bool audio_submit(const void* data, size_t bytes, void* ctx) {
    if (!g_stream) return false;
    std::lock_guard lk(g_audio);
    g_pending.push_back({static_cast<const uint8_t*>(data), bytes, 0, ctx});
    return true;
}

uint32_t audio_queued() {
    std::lock_guard lk(g_audio);
    return uint32_t(g_pending.size());
}

// ---- Vulkan ---------------------------------------------------------------------------------------------------

void* vulkan_get_instance_proc_addr() {
    void* lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    return lib ? dlsym(lib, "vkGetInstanceProcAddr") : nullptr;
}

const char* vulkan_surface_extension() { return VK_KHR_ANDROID_SURFACE_EXTENSION_NAME; }

int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void* gipa) {
    auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa)(
        static_cast<VkInstance>(instance), "vkCreateAndroidSurfaceKHR"));
    VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    ci.window = static_cast<ANativeWindow*>(native_window);
    VkSurfaceKHR s = VK_NULL_HANDLE;
    VkResult r = create(static_cast<VkInstance>(instance), &ci, nullptr, &s);
    *surface = reinterpret_cast<uint64_t>(s);
    return r;
}

}  // namespace nx::plat

// ---- crash handler and entry ----------------------------------------------------------------------------------

namespace {

void crash(int sig, siginfo_t* info, void* uc) {
    nx::log("CRASH: signal %d, fault address %p", sig, info->si_addr);
    if (nx::self_thread()) {
        nx::CPU& c = nx::current_cpu();
        nx::log("  guest lr %s  sp %llx", nx::describe(c.x[30]).c_str(), (unsigned long long)c.x[31]);
        for (int k = 0; k < 32; k += 4)
            nx::log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k],
                    (unsigned long long)c.x[k + 1], (unsigned long long)c.x[k + 2], (unsigned long long)c.x[k + 3]);
        nx::backtrace(c);
    }
    _exit(128 + sig);
}

void game_thread(void*) {
    try {
        nx::boot();
    } catch (const nx::LongJmp& j) {
        nx::fatal("longjmp to buffer %llx with no setjmp frame", (unsigned long long)j.buf);
    } catch (const std::exception& e) {
        nx::fatal("host exception: %s", e.what());
    }
}

void on_cmd(android_app* app, int32_t cmd) {
    using namespace nx::plat;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            g_window = app->window;
            g_visible = true;
            break;
        case APP_CMD_TERM_WINDOW:
            g_visible = false;
            break;
        case APP_CMD_GAINED_FOCUS:
            g_visible = g_window != nullptr;
            break;
        case APP_CMD_LOST_FOCUS:
            break;
    }
}

}  // namespace

// All-files access (Android 11+): needed to read Internal storage/Hades. Asks for it by opening the system page.
static bool has_all_files_access(android_app* app) {
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);
    jclass envc = env->FindClass("android/os/Environment");
    jmethodID m = env->GetStaticMethodID(envc, "isExternalStorageManager", "()Z");
    bool ok = m && env->CallStaticBooleanMethod(envc, m);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return ok;
}

static void request_all_files_access(android_app* app) {
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);
    jclass uric = env->FindClass("android/net/Uri");
    jobject uri = env->CallStaticObjectMethod(uric, env->GetStaticMethodID(uric, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"),
                                              env->NewStringUTF("package:io.github.belmantegu.hadesrecomp"));
    jclass intentc = env->FindClass("android/content/Intent");
    jobject intent = env->NewObject(intentc, env->GetMethodID(intentc, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V"),
                                    env->NewStringUTF("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION"), uri);
    jclass actc = env->GetObjectClass(app->activity->clazz);
    env->CallVoidMethod(app->activity->clazz, env->GetMethodID(actc, "startActivity", "(Landroid/content/Intent;)V"), intent);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

static bool has_data(const std::string& root) { return access((root + "/analysis/main.bin").c_str(), R_OK) == 0; }

extern "C" void android_main(android_app* app) {
    using namespace nx::plat;
    g_app = app;
    // Game data: Internal storage/Hades (put there by the player, needs all-files access) or the app's own
    // external files folder (adb push).
    const std::string shared = "/storage/emulated/0/Hades";
    const std::string own = app->activity->externalDataPath ? app->activity->externalDataPath : shared;
    if (!has_data(own) && !has_all_files_access(app)) {
        request_all_files_access(app);
        while (!has_all_files_access(app)) {  // keep the app responsive until the switch is turned on
            int events;
            android_poll_source* src;
            while (ALooper_pollOnce(100, nullptr, &events, reinterpret_cast<void**>(&src)) >= 0) {
                if (src) src->process(app, src);
                if (app->destroyRequested) _exit(0);
            }
        }
    }
    g_root = has_data(own) ? own : shared;
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);

    struct sigaction sa{};
    sa.sa_sigaction = crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(s, &sa, nullptr);

    nx::log("Hades (Switch recomp) on Android, data at %s", g_root.c_str());
    static bool started = false;
    for (;;) {
        int events;
        android_poll_source* src;
        while (ALooper_pollOnce(started ? 0 : 50, nullptr, &events, reinterpret_cast<void**>(&src)) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) _exit(0);
        }
        if (!started && g_window) {
            started = true;
            start_thread(game_thread, nullptr, 256ull << 20);  // the guest main thread; deep host recursion
        }
        if (started) sleep_ns(2'000'000);
    }
}
