// nn::audio AudioOut at the API level. FMOD (inside main) mixes s16 stereo 48 kHz into AudioOutBuffers and appends
// them; a player thread hands each one to the platform's audio device (XAudio2 / AAudio) and gives it back to the
// game (released + system event signalled) when it has been played, so the device clock paces FMOD's mixer.
// Without a device the same pacing runs on a timer. Audio input reports no devices.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "kernel.h"
#include "platform.h"

namespace nx {

namespace {

constexpr uint64_t kBytesPerSecond = 48000 * 2 * 2;
constexpr uint32_t kMaxInFlight = 4;  // buffers queued in the device (4 x ~21 ms)

// Our AudioOutBuffer layout (only our HLE reads it): {void* data; u64 capacity; u64 size}.
struct Out {
    std::mutex lock;
    std::condition_variable cv;
    std::deque<uint64_t> queued, released;
    std::shared_ptr<EventState> event = std::make_shared<EventState>();
    bool started = false;
};
Out g_out;

void release(void* ctx) {
    std::lock_guard lk(g_out.lock);
    g_out.released.push_back(reinterpret_cast<uint64_t>(ctx));
    g_out.event->signaled = true;
    notify_all();
    g_out.cv.notify_all();
}

void player() {
    bool device = plat::audio_open(release);
    log("audio: %s", device ? "device output (48 kHz stereo s16)" : "no audio device, timer pacing");
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    for (;;) {
        uint64_t buf = 0, size = 0;
        {
            std::unique_lock lk(g_out.lock);
            auto ready = [&] {
                return g_out.started && !g_out.queued.empty() && (!device || plat::audio_queued() < kMaxInFlight);
            };
            g_out.cv.wait_for(lk, std::chrono::milliseconds(5), ready);
            if (!ready()) continue;
            buf = g_out.queued.front();
            g_out.queued.pop_front();
            size = ld<uint64_t>(buf + 16);
        }
        if (device && plat::audio_submit(reinterpret_cast<const void*>(ld<uint64_t>(buf)), size,
                                         reinterpret_cast<void*>(buf)))
            continue;
        next = std::max(next, clock::now() - std::chrono::milliseconds(50));
        next += std::chrono::microseconds(size * 1000000 / kBytesPerSecond);
        std::this_thread::sleep_until(next);
        release(reinterpret_cast<void*>(buf));
    }
}

void InitializeAudioOutParameter(CPU& c) {  // {u32 sample_rate; u16 channels; u16 reserved}
    st<uint32_t>(c.x[0], 0);
    st<uint16_t>(c.x[0] + 4, 0);
    st<uint16_t>(c.x[0] + 6, 0);
}

void OpenDefaultAudioOut(CPU& c) {  // (AudioOut*, os::SystemEvent*, const AudioOutParameter&)
    uint64_t out = c.x[0], sysev = c.x[1];
    // Give the guest a system event backed by our kernel event: nn::os::AttachSystemEvent(type, read, managed,
    // write, managed, clear mode = manual).
    auto w = std::make_shared<KEvent>();
    auto r = std::make_shared<KEvent>();
    w->state = r->state = g_out.event;
    uint32_t hr = handle_add(r), hw = handle_add(w);
    uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[0] = sysev;
    c.x[1] = hr;
    c.x[2] = 1;
    c.x[3] = hw;
    c.x[4] = 1;
    c.x[5] = 1;  // manual clear
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    st<uint64_t>(out, 0xA0D10);  // opaque
    static std::once_flag once;
    std::call_once(once, [] { std::thread(player).detach(); });
    c.x[0] = 0;
}

void StartAudioOut(CPU& c) {
    std::lock_guard lk(g_out.lock);
    g_out.started = true;
    g_out.cv.notify_all();
    c.x[0] = 0;
}

void StopAudioOut(CPU&) {
    std::lock_guard lk(g_out.lock);
    g_out.started = false;
}

void CloseAudioOut(CPU&) {}

void SetAudioOutBufferInfo(CPU& c) {  // (AudioOutBuffer*, void* data, size_t capacity, size_t size)
    st<uint64_t>(c.x[0], c.x[1]);
    st<uint64_t>(c.x[0] + 8, c.x[2]);
    st<uint64_t>(c.x[0] + 16, c.x[3]);
}

void GetAudioOutBufferDataPointer(CPU& c) { c.x[0] = ld<uint64_t>(c.x[0]); }

void AppendAudioOutBuffer(CPU& c) {  // (AudioOut*, AudioOutBuffer*)
    std::lock_guard lk(g_out.lock);
    g_out.queued.push_back(c.x[1]);
    g_out.cv.notify_all();
    c.x[0] = 1;  // bool: appended
}

void GetReleasedAudioOutBuffer(CPU& c) {
    std::lock_guard lk(g_out.lock);
    if (g_out.released.empty()) {
        c.x[0] = 0;
        return;
    }
    c.x[0] = g_out.released.front();
    g_out.released.pop_front();
    if (g_out.released.empty()) g_out.event->signaled = false;
}

void ListAudioIns(CPU& c) { c.x[0] = 0; }
void audio_in_fail(CPU& c) { c.x[0] = (1u << 9) | 153u; }  // audio: not found
void zero(CPU& c) { c.x[0] = 0; }

// SDK 16 (Super Mario Bros. Wonder): the AudioRenderer. The sdk carries the renderer's server side too and can
// reach it in process ("direct function call") instead of over IPC to audren:u: hand out that one.
// Both return nn::sf::SharedPointer<IAudioRendererManager> through x8.
void CreateAudioRendererManagerByHipc(CPU& c) {
    static uint64_t dfc = find_symbol("_ZN2nn5audio31CreateAudioRendererManagerByDfcEv");
    if (!dfc) fatal("CreateAudioRendererManagerByDfc not found");
    log("audio: AudioRenderer manager in process (DFC) instead of audren:u");
    call(c, dfc);
}

// The AudioRenderer, in process (DFC). On the console the ADSP renders one audio frame every 5 ms and signals the
// renderer's system event, which paces the game's sound thread. In process nothing does that on its own: the
// renderer object (AudioRendererManagerImpl::AudioRendererImpl) has ExecuteAudioRendererRendering, which generates
// the frame's commands and runs the sdk's own DSP command processor on the calling thread, and its render system
// (impl + 0x60) signals its own event, which has no kernel object in process. A guest thread of ours renders every
// 5 ms and signals the game's event itself. (Sending the final mix to the
// host device is the next step of phase NA; for now the frames are rendered and dropped.)
constexpr uint64_t kRendererVtable = 0xd04910;     // sdk: vtable of the sf object wrapping AudioRendererImpl
constexpr uint64_t kExecuteRendering = 0x20cac8;   // sdk: AudioRendererImpl::ExecuteAudioRendererRendering

// The in-process render system hands the client a readable handle of 0 for the renderer's event (there is no
// kernel event behind it), so the SystemEvent the game waits on is ours: an InterProcessEventType keeps its
// readable handle at +0x14 and its writable one at +0x18 (nn::os::detail::WaitInterProcessEvent, sdk+1305a0).
std::shared_ptr<EventState> g_audio_frame = std::make_shared<EventState>();

std::mutex g_renderers_lock;
std::vector<uint64_t> g_renderers;  // AudioRendererImpl*

uint64_t sdk_base() {
    static uint64_t base = [] {
        const LoadedModule* m = module_at(find_symbol("nvnBootstrapLoader"));
        return m ? m->base : 0;
    }();
    return base;
}

// The AudioRendererImpl behind an AudioRendererHandle: an sf object whose vtable pointer is that one, within two
// pointer hops of the handle.
uint64_t find_renderer(uint64_t handle) {
    uint64_t vptr = sdk_base() + kRendererVtable + 0x10;
    auto valid = [](uint64_t p) { return p >= kModuleBase && p < kKernelBase + kKernelSize && (p & 7) == 0; };
    for (uint64_t a = 0; a < 0x40; a += 8) {
        uint64_t p = ld<uint64_t>(handle + a);
        if (!valid(p)) continue;
        if (ld<uint64_t>(p) == vptr) return p + 0x10;
        for (uint64_t b = 0; b < 0x40; b += 8) {
            uint64_t q = ld<uint64_t>(p + b);
            if (valid(q) && ld<uint64_t>(q) == vptr) return q + 0x10;
        }
    }
    return 0;
}

void audio_render_thread(CPU& c) {
    log("audio: render thread running");
    uint64_t next = plat::monotonic_ns();
    for (;;) {
        next += 5'000'000;
        int64_t wait = int64_t(next - plat::monotonic_ns());
        if (wait > 0) plat::sleep_ns(wait);
        std::vector<uint64_t> list;
        {
            std::lock_guard lk(g_renderers_lock);
            list = g_renderers;
        }
        static uint64_t frames = 0, busy_ns = 0, last_rc = 0;
        uint64_t t0 = plat::monotonic_ns();
        for (uint64_t impl : list) {
            uint64_t saved[31];
            std::memcpy(saved, c.x, sizeof saved);
            c.x[0] = impl;
            call(c, sdk_base() + kExecuteRendering);
            last_rc = c.x[0];
            std::memcpy(c.x, saved, sizeof saved);
        }
        busy_ns += plat::monotonic_ns() - t0;
        if (++frames % 2000 == 0)
            log("audio: %llu frames rendered, %.1f us each, last result %llx", (unsigned long long)frames,
                double(busy_ns) / 1000.0 / 2000.0, (unsigned long long)last_rc), busy_ns = 0;
        g_audio_frame->signaled = true;
        notify_all();
    }
}

void start_render_thread(CPU& c) {
    uint64_t create = find_symbol("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi");
    uint64_t start = find_symbol("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
    if (!create || !start) {
        log("audio: nn::os::CreateThread/StartThread not found, no render thread");
        return;
    }
    uint64_t thread = kernel_alloc(0x1000), stack = kernel_alloc(0x40000);
    uint64_t saved[31];
    std::memcpy(saved, c.x, sizeof saved);
    c.x[0] = thread, c.x[1] = hle_address("__audio_render_thread"), c.x[2] = 0, c.x[3] = stack, c.x[4] = 0x40000;
    c.x[5] = 16;  // high priority, like the audio system's own threads
    call(c, create);
    uint64_t rc = c.x[0];
    c.x[0] = thread;
    if (rc == 0) call(c, start);
    std::memcpy(c.x, saved, sizeof saved);
    if (rc) log("audio: CreateThread failed (%llx)", (unsigned long long)rc);
}

// OpenAudioRenderer(AudioRendererHandle*, os::SystemEvent*, const AudioRendererParameter&, void* work, size_t)
void OpenAudioRenderer(CPU& c) {
    static uint64_t real = find_symbol(
        "_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm");
    uint64_t handle = c.x[0], sysev = c.x[1];
    call(c, real);
    if (c.x[0] == 0 && ld<uint32_t>(sysev + 0x14) == 0) {
        auto r = std::make_shared<KEvent>(), w = std::make_shared<KEvent>();
        r->state = w->state = g_audio_frame;
        st<uint32_t>(sysev + 0x14, handle_add(r));
        st<uint32_t>(sysev + 0x18, handle_add(w));
    }
    uint64_t rc = c.x[0];
    uint64_t impl = rc == 0 ? find_renderer(handle) : 0;
    log("audio: renderer opened (result %llx), impl %llx", (unsigned long long)rc, (unsigned long long)impl);
    if (!impl) return;
    // ExecuteAudioRendererRendering only renders in the manual execution mode on the CPU rendering device
    // (AudioRenderSystem::GetExecutionMode / GetRenderingDevice == 1, at render system + 0x2d0 / + 0x2d4); the game
    // opens the renderer in the automatic mode, where the ADSP renders on its own. Our 5 ms thread is that ADSP, so
    // the renderer is switched to manual/CPU; otherwise every call failed with 0x40299 and nothing was ever rendered
    // (streamed music never advanced, and the opening cutscene waited for it forever).
    uint64_t system = impl + 0x60;
    log("audio: execution mode %u, rendering device %u -> 1, 1", ld<uint32_t>(system + 0x2d0), ld<uint32_t>(system + 0x2d4));
    st<uint32_t>(system + 0x2d0, 1);
    st<uint32_t>(system + 0x2d4, 1);
    {
        std::lock_guard lk(g_renderers_lock);
        g_renderers.push_back(impl);
    }
    static bool started = false;
    if (!started) {
        started = true;
        start_render_thread(c);
    }
    c.x[0] = rc;
}

// The audio device (output) side: one stereo output that never changes.
constexpr const char* kOutputName = "AudioTvOutput";

void AcquireAudioDeviceNotificationForOutput(CPU& c) {  // (os::SystemEvent*): an event that is never signaled
    auto w = std::make_shared<KEvent>(), r = std::make_shared<KEvent>();
    r->state = w->state = std::make_shared<EventState>();
    uint32_t hr = handle_add(r), hw = handle_add(w);
    uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;  // x0: the SystemEvent's SystemEventType
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    c.x[0] = 0;
}

void ListAudioOutputDeviceName(CPU& c) {  // int (AudioDeviceName* names, int count): 256 chars each
    if (int(c.x[1]) < 1) {
        c.x[0] = 0;
        return;
    }
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 256);
    std::strcpy(reinterpret_cast<char*>(c.x[0]), kOutputName);
    c.x[0] = 1;
}

void GetActiveAudioOutputDeviceName(CPU& c) {  // (AudioDeviceName*)
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 256);
    std::strcpy(reinterpret_cast<char*>(c.x[0]), kOutputName);
    c.x[0] = 0;
}

void two(CPU& c) { c.x[0] = 2; }

}  // namespace

NX_HLE("_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", AcquireAudioDeviceNotificationForOutput);
NX_HLE("_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi", ListAudioOutputDeviceName);
NX_HLE("_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", GetActiveAudioOutputDeviceName);
NX_HLE("_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", zero);
NX_HLE("_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", two);
NX_HLE("_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", InitializeAudioOutParameter);
NX_HLE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE", OpenDefaultAudioOut);
NX_HLE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", StartAudioOut);
NX_HLE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", StopAudioOut);
NX_HLE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", CloseAudioOut);
NX_HLE("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", SetAudioOutBufferInfo);
NX_HLE("_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", GetAudioOutBufferDataPointer);
NX_HLE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", AppendAudioOutBuffer);
NX_HLE("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", GetReleasedAudioOutBuffer);
NX_HLE("_ZN2nn5audio12ListAudioInsEPNS0_11AudioInInfoEi", ListAudioIns);
NX_HLE("_ZN2nn5audio11OpenAudioInEPNS0_7AudioInEPKcRKNS0_16AudioInParameterE", audio_in_fail);
NX_HLE("_ZN2nn5audio26InitializeAudioInParameterEPNS0_16AudioInParameterE", zero);
NX_HLE("_ZN2nn5audio32CreateAudioRendererManagerByHipcEv", CreateAudioRendererManagerByHipc);
NX_HLE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", OpenAudioRenderer);
NX_HLE("__audio_render_thread", audio_render_thread);

}  // namespace nx
