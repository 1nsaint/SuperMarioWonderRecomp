// nn::audio AudioOut at the API level, played through XAudio2. FMOD (inside main) mixes s16 stereo 48 kHz into
// AudioOutBuffers and appends them; each one is copied into an XAudio2 source voice and handed back to the game
// (released + system event signalled) when XAudio2 has finished playing it, so the device clock paces FMOD's
// mixer. Without an audio device the same pacing runs on a timer. Audio input reports no devices.
#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>
#include <xaudio2.h>

#include "kernel.h"

namespace nx {

namespace {

constexpr uint32_t kSampleRate = 48000, kChannels = 2;
constexpr uint64_t kBytesPerSecond = kSampleRate * kChannels * 2;
constexpr uint32_t kMaxInFlight = 4;  // buffers queued in XAudio2 (4 x 21 ms)

// Our AudioOutBuffer layout (only our HLE reads it): {void* data; u64 capacity; u64 size}.
struct Out {
    std::mutex lock;
    std::condition_variable cv;
    std::deque<uint64_t> queued, released;
    std::shared_ptr<EventState> event = std::make_shared<EventState>();
    bool started = false;
    uint32_t in_flight = 0;
};
Out g_out;

void release(uint64_t buf) {
    std::lock_guard lk(g_out.lock);
    g_out.released.push_back(buf);
    g_out.event->signaled = true;
    notify_all();
}

// XAudio2 plays copies; the guest buffer goes back to the game when its copy has played.
struct Voice : IXAudio2VoiceCallback {
    struct Pending {
        uint64_t guest;
        std::vector<uint8_t> data;
    };
    void STDMETHODCALLTYPE OnBufferEnd(void* ctx) override {
        auto* p = static_cast<Pending*>(ctx);
        release(p->guest);
        delete p;
        std::lock_guard lk(g_out.lock);
        g_out.in_flight--;
        g_out.cv.notify_all();
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
};

IXAudio2* g_xa = nullptr;
IXAudio2SourceVoice* g_voice = nullptr;
Voice g_callback;

bool init_xaudio() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
    IXAudio2MasteringVoice* master = nullptr;
    if (FAILED(g_xa->CreateMasteringVoice(&master))) return false;
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = kChannels;
    wf.nSamplesPerSec = kSampleRate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = kChannels * 2;
    wf.nAvgBytesPerSec = kSampleRate * wf.nBlockAlign;
    if (FAILED(g_xa->CreateSourceVoice(&g_voice, &wf, 0, 2.0f, &g_callback))) return false;
    g_voice->Start();
    return true;
}

void player() {
    bool device = init_xaudio();
    log("audio: %s", device ? "XAudio2 output (48 kHz stereo s16)" : "no audio device, timer pacing");
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    for (;;) {
        uint64_t buf = 0, size = 0;
        {
            std::unique_lock lk(g_out.lock);
            g_out.cv.wait_for(lk, std::chrono::milliseconds(5), [] {
                return g_out.started && !g_out.queued.empty() && g_out.in_flight < kMaxInFlight;
            });
            if (!g_out.started || g_out.queued.empty() || g_out.in_flight >= kMaxInFlight) continue;
            buf = g_out.queued.front();
            g_out.queued.pop_front();
            size = ld<uint64_t>(buf + 16);
            if (device) g_out.in_flight++;
        }
        if (device) {
            auto* p = new Voice::Pending{buf, {}};
            p->data.assign(reinterpret_cast<const uint8_t*>(ld<uint64_t>(buf)),
                           reinterpret_cast<const uint8_t*>(ld<uint64_t>(buf)) + size);
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = UINT32(p->data.size());
            xb.pAudioData = p->data.data();
            xb.pContext = p;
            g_voice->SubmitSourceBuffer(&xb);
        } else {
            next = std::max(next, clock::now() - std::chrono::milliseconds(50));
            next += std::chrono::microseconds(size * 1000000 / kBytesPerSecond);
            std::this_thread::sleep_until(next);
            release(buf);
        }
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

void StopAudioOut(CPU& c) {
    std::lock_guard lk(g_out.lock);
    g_out.started = false;
}

void CloseAudioOut(CPU& c) {}

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

}  // namespace

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

}  // namespace nx
