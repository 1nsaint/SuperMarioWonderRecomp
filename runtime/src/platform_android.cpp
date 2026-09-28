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
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <nx/game.h>
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
std::atomic<uint64_t> g_window_gen{0};  // bumped for every new window
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
void set_thread_name(const char* name) { pthread_setname_np(pthread_self(), name); }
int64_t current_thread_id() { return gettid(); }

// ---- sampling profiler (simpleperf is not always allowed on retail phones) ------------------------------------

namespace {
constexpr uint32_t kMaxSamples = 1u << 18;
constexpr int kFrames = 12;
struct Sample {
    int32_t tid;
    uint64_t guest_lr;
    uint64_t pc[kFrames];  // host pc, lr, then return addresses from the frame-pointer chain
};
std::atomic<uint32_t> g_nsamples{0};
Sample* g_samples = nullptr;
const int64_t* g_prof_tids = nullptr;
const uint64_t* const* g_prof_lr = nullptr;
size_t g_prof_count = 0;

void on_sample(int, siginfo_t*, void* uc) {
    uint32_t i = g_nsamples.fetch_add(1, std::memory_order_relaxed);
    if (i >= kMaxSamples) return;
    Sample& s = g_samples[i];
    const mcontext_t& m = static_cast<ucontext_t*>(uc)->uc_mcontext;
    s.tid = gettid();
    s.guest_lr = 0;
    for (size_t k = 0; k < g_prof_count; k++)
        if (g_prof_tids[k] == s.tid) s.guest_lr = *g_prof_lr[k];
    std::memset(s.pc, 0, sizeof s.pc);
    s.pc[0] = m.pc;
    s.pc[1] = m.regs[30];
    // Frame records (fp -> [prev fp, lr]) on this thread's stack only: between sp and sp + 256 MiB, going up.
    uint64_t fp = m.regs[29], lo = m.sp, hi = m.sp + (256ull << 20);
    for (int k = 2; k < kFrames && fp >= lo && fp < hi && (fp & 7) == 0; k++) {
        const uint64_t* rec = reinterpret_cast<const uint64_t*>(fp);
        s.pc[k] = rec[1];
        if (rec[0] <= fp) break;
        fp = rec[0];
    }
}
}  // namespace

bool profile(const int64_t* tids, const uint64_t* const* guest_lr, size_t count, int seconds, const std::string& path) {
    if (!g_samples) g_samples = new Sample[kMaxSamples];
    g_prof_tids = tids;
    g_prof_lr = guest_lr;
    g_prof_count = count;
    g_nsamples = 0;
    struct sigaction sa{};
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    std::vector<timer_t> timers;
    for (size_t k = 0; k < count; k++) {
        // The thread's CPU-time clock (Linux: MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)).
        clockid_t clock = clockid_t((~uint32_t(tids[k]) << 3) | 6);
        sigevent sev{};
        sev.sigev_notify = SIGEV_THREAD_ID;
        sev.sigev_signo = SIGPROF;
        sev.sigev_notify_thread_id = int(tids[k]);
        timer_t t;
        if (timer_create(clock, &sev, &t) != 0) continue;
        itimerspec its{{0, 1'000'000}, {0, 1'000'000}};
        timer_settime(t, 0, &its, nullptr);
        timers.push_back(t);
    }
    sleep_ns(int64_t(seconds) * 1'000'000'000);
    for (timer_t t : timers) timer_delete(t);
    sleep_ns(10'000'000);
    g_prof_count = 0;
    uint32_t n = std::min(g_nsamples.load(), kMaxSamples);
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    Dl_info self{};
    dladdr(reinterpret_cast<void*>(&on_sample), &self);
    for (uint32_t i = 0; i < n; i++) {
        const Sample& s = g_samples[i];
        std::fprintf(f, "%d %llx", s.tid, (unsigned long long)s.guest_lr);
        for (int k = 0; k < kFrames && s.pc[k]; k++) {
            Dl_info d{};
            if (dladdr(reinterpret_cast<void*>(s.pc[k]), &d) && d.dli_fbase) {
                const char* name = d.dli_fbase == self.dli_fbase ? "self" : d.dli_fname;
                const char* slash = std::strrchr(name, '/');
                std::fprintf(f, " %s:%llx", slash ? slash + 1 : name,
                             (unsigned long long)(s.pc[k] - uint64_t(d.dli_fbase)));
            } else {
                std::fprintf(f, " ?:%llx", (unsigned long long)s.pc[k]);
            }
        }
        std::fputc('\n', f);
    }
    std::fclose(f);
    return true;
}

uint64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// ---- files and output -----------------------------------------------------------------------------------------

FILE* fopen_utf8(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }
int64_t ftell64(FILE* f) { return ftello(f); }
int fseek64(FILE* f, int64_t off, int origin) { return fseeko(f, off, origin); }
uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size) {
    // A size that does not fit ssize_t (the game passes -1) reads nothing, as fread did (and the game expects).
    if (size > uint64_t(SSIZE_MAX)) return 0;
    int fd = fileno(f);
    uint64_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, static_cast<uint8_t*>(buf) + done, size_t(size - done), off + int64_t(done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += uint64_t(n);
    }
    return done;
}
bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

void log_line(const char* line) { __android_log_write(ANDROID_LOG_INFO, nx::game::kInfo.id, line); }

std::string data_root() { return g_root; }

std::string setting(const std::string& key) {
    std::ifstream f(g_root + "/config.txt");
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        auto eq = line.find('=');
        if (eq != std::string::npos && line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return {};
}

// ---- window ---------------------------------------------------------------------------------------------------

// The window handed out stays valid for good (we hold a reference): Android destroys the activity's window when
// the app goes to the background or the screen locks (APP_CMD_TERM_WINDOW), and until then we wait for one.
void* create_window() {
    ANativeWindow* w;
    while (!(w = g_window.load())) sleep_ns(10'000'000);
    ANativeWindow_acquire(w);
    return w;
}

void release_window(void* w) { ANativeWindow_release(static_cast<ANativeWindow*>(w)); }

uint64_t window_generation() { return g_window_gen; }

bool window_minimized() { return !g_visible || !g_window; }

void screen_size(uint32_t& w, uint32_t& h) {
    ANativeWindow* win = static_cast<ANativeWindow*>(create_window());
    int32_t a = ANativeWindow_getWidth(win), b = ANativeWindow_getHeight(win);
    ANativeWindow_release(win);
    w = uint32_t(std::max(a, b));  // landscape
    h = uint32_t(std::min(a, b));
}

// The render size: resolution=WxH if set, else the screen's size times the render scale (render_scale=0.5..1, or
// the GPU's default), never taller than 1080 (the game's art is made for 720p/1080p; more only costs GPU time).
void window_size(uint32_t& w, uint32_t& h) {
    std::string r = setting("resolution");
    unsigned cw = 0, ch = 0;
    if (!r.empty() && std::sscanf(r.c_str(), "%ux%u", &cw, &ch) == 2 && cw && ch) {
        w = cw;
        h = ch;
        return;
    }
    uint32_t sw = 0, sh = 0;
    screen_size(sw, sh);
    std::string rs = setting("render_scale");
    float scale = rs.empty() ? default_render_scale() : std::strtof(rs.c_str(), nullptr);
    scale = std::clamp(scale, 0.25f, 1.0f);
    scale = std::min(scale, 1080.0f / float(sh));
    w = uint32_t(float(sw) * scale + 0.5f) & ~1u;
    h = uint32_t(float(sh) * scale + 0.5f) & ~1u;
}

// A GPU tier from the Vulkan device name (queried once, before the game starts its renderer): current Adreno 7xx
// and 8xx, Mali-G7xx and newer, Immortalis, Xclipse render at 1; Adreno 6xx at 0.85; anything older at 2/3
// (about 720p, the Switch's handheld resolution).
float default_render_scale() {
    static float scale = [] {
        auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(vulkan_get_instance_proc_addr());
        if (!gipa) return 2.0f / 3;
        auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        VkInstance inst = VK_NULL_HANDLE;
        if (!create || create(&ci, nullptr, &inst) != VK_SUCCESS) return 2.0f / 3;
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
        auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(inst, "vkGetPhysicalDeviceProperties"));
        auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
        uint32_t n = 1;
        VkPhysicalDevice pd = VK_NULL_HANDLE;
        float s = 2.0f / 3;
        if (enumerate(inst, &n, &pd) >= 0 && n && pd) {
            VkPhysicalDeviceProperties p{};
            props(pd, &p);
            std::string name = p.deviceName;
            int adreno = 0, mali = 0;
            std::sscanf(name.c_str() + (name.find("Adreno") != std::string::npos ? name.find("Adreno") : name.size()),
                        "Adreno (TM) %d", &adreno);
            if (auto m = name.find("Mali-G"); m != std::string::npos) std::sscanf(name.c_str() + m, "Mali-G%d", &mali);
            if (adreno >= 700 || mali >= 710 || name.find("Immortalis") != std::string::npos ||
                name.find("Xclipse") != std::string::npos)
                s = 1.0f;
            else if (adreno >= 610 || mali >= 76)
                s = 0.85f;
            nx::log("gpu: %s -> default render scale %.2f", p.deviceName, s);
        }
        destroy(inst, nullptr);
        return s;
    }();
    return scale;
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

void audio_pause(bool paused) {
    if (!g_stream) return;
    if (paused)
        AAudioStream_requestPause(g_stream);
    else
        AAudioStream_requestStart(g_stream);
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

struct sigaction g_previous[32];  // the handlers before ours (debuggerd's)

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
    // Hand the signal to the previous handler, Android's debuggerd: it logs the host backtrace (a tombstone).
    const struct sigaction& prev = g_previous[sig];
    if (prev.sa_flags & SA_SIGINFO) {
        if (prev.sa_sigaction) prev.sa_sigaction(sig, info, uc);
    } else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN && prev.sa_handler) {
        prev.sa_handler(sig);
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

void request_immersive();  // below: runs on the UI thread

void on_cmd(android_app* app, int32_t cmd) {
    using namespace nx::plat;
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            g_window = app->window;
            g_window_gen++;
            g_visible = true;
            request_immersive();
            break;
        case APP_CMD_TERM_WINDOW:  // the window goes away once this returns
            g_window = nullptr;
            g_visible = false;
            break;
        case APP_CMD_GAINED_FOCUS:
            g_visible = g_window != nullptr;
            request_immersive();  // bars shown by a swipe hide again
            break;
        case APP_CMD_LOST_FOCUS:
            break;
        case APP_CMD_PAUSE:  // home, app switcher, screen off: the game holds (no window), so does its sound
            audio_pause(true);
            break;
        case APP_CMD_RESUME:
            audio_pause(false);
            break;
    }
}

}  // namespace

// ---- immersive mode: the status and navigation bars hidden (a swipe from an edge shows them for a moment) --------
// Only the UI thread may touch the window's views; android_main runs on another. Our ANativeActivity_onCreate (on
// the UI thread) registers a pipe with the UI thread's looper: writing a byte to it runs the request there.

extern "C" void glue_ANativeActivity_onCreate(ANativeActivity* activity, void* saved, size_t size);

namespace {

ANativeActivity* g_activity = nullptr;
int g_ui_pipe[2] = {-1, -1};

void hide_system_bars(JNIEnv* env, jobject activity) {
    jclass act = env->GetObjectClass(activity);
    jobject window = env->CallObjectMethod(activity, env->GetMethodID(act, "getWindow", "()Landroid/view/Window;"));
    if (env->ExceptionCheck() || !window) {
        env->ExceptionClear();
        return;
    }
    jclass winc = env->GetObjectClass(window);
    jmethodID get_ctl = env->GetMethodID(winc, "getInsetsController", "()Landroid/view/WindowInsetsController;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    jobject ctl = get_ctl ? env->CallObjectMethod(window, get_ctl) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear(), ctl = nullptr;
    if (ctl) {  // Android 11+
        jclass cc = env->GetObjectClass(ctl);
        jclass types = env->FindClass("android/view/WindowInsets$Type");
        jint bars = env->CallStaticIntMethod(types, env->GetStaticMethodID(types, "systemBars", "()I"));
        env->CallVoidMethod(ctl, env->GetMethodID(cc, "setSystemBarsBehavior", "(I)V"), 2);  // show transient by swipe
        env->CallVoidMethod(ctl, env->GetMethodID(cc, "hide", "(I)V"), bars);
    } else {  // Android 10: immersive sticky | hide navigation | fullscreen | layout stable/hide navigation/fullscreen
        jobject decor = env->CallObjectMethod(window, env->GetMethodID(winc, "getDecorView", "()Landroid/view/View;"));
        if (decor)
            env->CallVoidMethod(decor, env->GetMethodID(env->GetObjectClass(decor), "setSystemUiVisibility", "(I)V"), 0x1706);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
}

int on_ui_request(int fd, int, void*) {
    char buf[16];
    while (read(fd, buf, sizeof buf) > 0) {
    }
    if (g_activity) hide_system_bars(g_activity->env, g_activity->clazz);
    return 1;
}

void request_immersive() {
    char c = 1;
    if (g_ui_pipe[1] >= 0) (void)write(g_ui_pipe[1], &c, 1);
}

}  // namespace

extern "C" JNIEXPORT void ANativeActivity_onCreate(ANativeActivity* activity, void* saved, size_t size) {
    g_activity = activity;
    if (g_ui_pipe[0] < 0 && pipe2(g_ui_pipe, O_NONBLOCK | O_CLOEXEC) == 0)
        ALooper_addFd(ALooper_forThread(), g_ui_pipe[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, on_ui_request, nullptr);
    glue_ANativeActivity_onCreate(activity, saved, size);
}

// All-files access (Android 11+): needed to read Internal storage/<game title>. Asks for it by opening the system page.
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
                                              env->NewStringUTF((std::string("package:") + nx::game::kInfo.android_package).c_str()));
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
    // Game data: Internal storage/<game title> (put there by the player, needs all-files access) or the app's own
    // external files folder (adb push).
    const std::string shared = std::string("/storage/emulated/0/") + nx::game::kInfo.title;
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
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(s, &sa, &g_previous[s]);

    nx::log("%s (Switch recomp) on Android, data at %s", nx::game::kInfo.title, g_root.c_str());
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
