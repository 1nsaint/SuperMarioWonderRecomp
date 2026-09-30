// NVN queues, command buffers, windows and syncs (docs/NVN.md, N3). Synchronously every command completes at
// submit: fences are signaled right away, the window hands out its textures in turn, and presents are counted. With
// NX_ASYNC_GPU a fence is the submit it follows, and waiting for it waits for that submit (nvn_frames.cpp). Return
// values follow the SDK driver (nvnSyncWait sdk+45a960: 0 once the fence has passed; nvnWindowAcquireTexture
// sdk+45e050: 0 on success).
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <vector>

#include "nvn.h"
#include "platform.h"

namespace nx::nvn {

namespace {

// ---- syncs ------------------------------------------------------------------------------------------------------

struct Sync : Object {
    std::atomic<bool> signaled{false};
    std::atomic<uint64_t> serial{0};  // NX_ASYNC_GPU: the submit it waits for (0: none)
    std::atomic<uint64_t> job{0};     // the replay-thread job that fences it (0: none)
};

void SyncInitialize(CPU& c) {  // NVNboolean (NVNsync*, NVNdevice*)
    make<Sync>(c.x[0]);
    c.x[0] = 1;
}

void SyncFinalize(CPU& c) { destroy(c.x[0]); }

void signal_sync(uint64_t guest) {
    if (auto* s = get<Sync>(guest)) s->serial = 0, s->job = 0, s->signaled = true;
}

// NVNsyncWaitResult (const NVNsync*, u64 timeout). Always 0 (already signaled), as before: a fenced submit is waited
// for whatever the timeout, 2 s at most (then the GPU hung and the process ends). Correct results before latency.
void SyncWait(CPU& c) {
    auto* s = get<Sync>(c.x[0]);
    if (s) worker_wait(s->job);
    if (uint64_t serial = s ? s->serial.load() : 0) gpu_wait(serial);
    c.x[0] = 0;
}

// ---- queues -----------------------------------------------------------------------------------------------------

std::atomic<uint64_t> g_presents{0};

void QueueInitialize(CPU& c) { c.x[0] = 1; }  // NVNboolean (NVNqueue*, const NVNqueueBuilder*)
void QueueFinalize(CPU&) {}
void QueueSubmitCommands(CPU& c) {  // (NVNqueue*, int count, const NVNcommandHandle*)
    std::vector<std::shared_ptr<CmdList>> lists;
    static uint64_t submitted = 0, missing = 0;
    for (uint32_t k = 0; k < uint32_t(c.x[1]); k++) {
        submitted++;
        if (auto l = command_list(ld<uint64_t>(c.x[2] + 8 * k)))
            lists.push_back(std::move(l));
        else if (++missing <= 5)
            log("nvn: submitted handle %llx is unknown", (unsigned long long)ld<uint64_t>(c.x[2] + 8 * k));
    }
    static const bool trace_lists = std::getenv("NX_NVN_LISTS") != nullptr;
    if (trace_lists && g_presents > 900 && g_presents < 905)
        for (uint32_t k = 0; k < uint32_t(c.x[1]); k++) {
            auto l = command_list(ld<uint64_t>(c.x[2] + 8 * k));
            log("nvn list: submit %llx handle %llu (%zu cmds)", (unsigned long long)c.x[0],
                (unsigned long long)ld<uint64_t>(c.x[2] + 8 * k), l ? l->cmds.size() : 0);
        }
    if ((submitted & 1023) == 0) log("nvn: %llu handles submitted, %llu unknown", (unsigned long long)submitted, (unsigned long long)missing);
    // NX_DEFERRED_SNAPSHOT=1: read the Deferred arrays here; by default they are read when the submit is replayed.
    static const bool snap = std::getenv("NX_DEFERRED_SNAPSHOT") != nullptr;
    auto deferred = snap ? std::make_shared<const DeferredValues>(deferred_snapshot(lists)) : nullptr;
    worker_run([lists = std::move(lists), deferred] { gpu_submit(lists, deferred); });
}
void QueueFlush(CPU&) {}
void QueueFinish(CPU&) {
    worker_finish();
    gpu_wait(gpu_serial());
}
// (NVNqueue*, NVNsync*, NVNsyncCondition, int flags): every condition taken as "all previous work complete".
void QueueFenceSync(CPU& c) {
    uint64_t guest = c.x[1];  // the fence goes after the work queued so far: its serial is taken on the replay thread
    if (auto* s = get<Sync>(guest)) {
        s->signaled = true;
        s->job = worker_run([guest] {
            if (auto* s = get<Sync>(guest)) s->serial = gpu_serial();
        });
    }
}
void QueueWaitSync(CPU& c) { c.x[0] = 1; }  // NVNboolean (NVNqueue*, const NVNsync*): one queue, in order

// ---- windows ----------------------------------------------------------------------------------------------------

void WindowInitialize(CPU& c) {  // NVNboolean (NVNwindow*, const NVNwindowBuilder*)
    uint64_t guest = c.x[0], builder = c.x[1];
    auto& w = make<Window>(guest);
    int n = int(driver(c, "nvnWindowBuilderGetNumTextures", {builder}));
    for (int k = 0; k < n; k++) w.textures.push_back(driver(c, "nvnWindowBuilderGetTexture", {builder, uint64_t(k)}));
    w.active = int(driver(c, "nvnWindowBuilderGetNumActiveTextures", {builder}));
    if (w.active <= 0 || w.active > n) w.active = n;
    w.interval = int(driver(c, "nvnWindowBuilderGetPresentInterval", {builder}));
    log("nvn: window %llx: %d textures (%d active), present interval %d", (unsigned long long)guest, n, w.active, w.interval);
    c.x[0] = 1;
}

void WindowFinalize(CPU& c) { destroy(c.x[0]); }

// NVNwindowAcquireTextureResult (NVNwindow*, NVNsync* textureAvailableSync, int* textureIndex)
void WindowAcquireTexture(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    int index = 0;
    if (w && w->active > 0) {
        index = w->next;
        w->next = (w->next + 1) % w->active;
    }
    if (c.x[2]) st<int32_t>(c.x[2], index);
    signal_sync(c.x[1]);
    c.x[0] = 0;
}

void WindowGetNumTextures(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    c.x[0] = w ? w->textures.size() : 0;
}

void WindowGetNumActiveTextures(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    c.x[0] = w ? uint64_t(w->active) : 0;
}

void WindowSetNumActiveTextures(CPU& c) {
    if (auto* w = get<Window>(c.x[0])) w->active = std::max(1, std::min(int(c.x[1]), int(w->textures.size())));
}

void WindowGetPresentInterval(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    c.x[0] = w ? uint64_t(w->interval) : 1;
}

// Wonder's sead::GameFramework::procFrame_ override (main+e15ec) picks the interval every frame from the main
// thread's last frame time, fw[0x118] (GetSystemTick ticks from the end of the previous present wait to the end of
// this frame's calc and draw recording): 2 when it reaches 15.83 ms, locked after 3 slow frames, back to 1 after 15
// fast ones (docs/nvn/DEBUG.md #43). From that call site (lr main+e16ac, x19 = the framework): a histogram of that
// time every 600 frames, and NX_MAX_INTERVAL=1 clamps the game's own maximum to 1 (fields 0x24c/0x264/0x268).
void frame_time_probe(CPU& c) {
    const LoadedModule* m = module_at(c.x[30]);
    if (!m || c.x[30] - m->base != 0xe16ac || !c.x[19]) return;
    static const char* max_env = std::getenv("NX_MAX_INTERVAL");
    if (max_env && std::atoi(max_env) >= 1) {
        uint32_t max = uint32_t(std::atoi(max_env));
        st<uint32_t>(c.x[19] + 0x24c, max);
        if (ld<uint32_t>(c.x[19] + 0x268) > max) st<uint32_t>(c.x[19] + 0x268, max);
        if (ld<uint32_t>(c.x[19] + 0x264) > max) st<uint32_t>(c.x[19] + 0x264, max);
        if (int(c.x[1]) > int(max)) c.x[1] = max;
    }
    static uint64_t bins[5] = {}, frames = 0;
    double ms = double(ld<uint64_t>(c.x[19] + 0x118)) / 19200.0;
    bins[ms < 12 ? 0 : ms < 15.83 ? 1 : ms < 20 ? 2 : ms < 33 ? 3 : 4]++;
    if (++frames % 600 == 0) {
        log("nvn: main-thread frame time over 600 frames: <12 ms %llu, <15.8 %llu, <20 %llu, <33 %llu, more %llu",
            (unsigned long long)bins[0], (unsigned long long)bins[1], (unsigned long long)bins[2],
            (unsigned long long)bins[3], (unsigned long long)bins[4]);
        for (auto& b : bins) b = 0;
    }
}

void WindowSetPresentInterval(CPU& c) {
    frame_time_probe(c);
    if (auto* w = get<Window>(c.x[0])) {
        static int logged = 0;
        if (w->interval != int(c.x[1]) && logged++ < 50)
            log("nvn: window present interval %d -> %d (frame %llu)", w->interval, int(c.x[1]), (unsigned long long)g_frame.load());
        w->interval = int(c.x[1]);
    }
}

// (NVNqueue*, NVNwindow*, int textureIndex)
// Frame pacing: a present returns no earlier than the window's present interval (in 60 Hz vsyncs) after the previous
// one. On the Switch the texture-available sync of the next acquire does this; ours is signaled at once, so without
// it the game runs as fast as it can once it is faster than 60 fps. A late frame is not snapped to the next vsync
// (an 18 ms frame stays 18 ms, not 33). NX_NO_FRAME_LIMIT=1: unpaced.
void pace_present(int interval) {
    static const bool off = std::getenv("NX_NO_FRAME_LIMIT") != nullptr;
    static uint64_t next = 0;
    if (off || interval <= 0) return;
    uint64_t now = plat::monotonic_ns();
    if (next > now) plat::sleep_ns(next - now), now = plat::monotonic_ns();
    static uint64_t presents = 0, at2 = 0;  // how often the game asks for 30 fps (interval 2), per 600 presents
    at2 += interval >= 2;
    if (++presents % 600 == 0) log("nvn: %llu of the last 600 presents at interval >= 2", (unsigned long long)at2), at2 = 0;
    uint64_t period = uint64_t(interval) * 16'666'667ull;
    next = (next > now || now - next < period ? next : now) + period;
}

void QueuePresentTexture(CPU& c) {
    if (auto* w = get<Window>(c.x[1])) {
        // At most two presents queued: the game runs at most two frames ahead of the replay.
        static uint64_t queued[2] = {};
        static int next_slot = 0;
        worker_wait(queued[next_slot]);
        if (int(c.x[2]) >= 0 && size_t(c.x[2]) < w->textures.size()) {
            uint64_t texture = w->textures[size_t(c.x[2])];
            std::array<int32_t, 4> crop{w->crop[0], w->crop[1], w->crop[2], w->crop[3]};
            queued[next_slot] = worker_run([texture, crop] { gpu_present(texture, crop.data()); });
            next_slot ^= 1;
        }
        pace_present(w->interval);
    }
    uint64_t n = ++g_presents;
    if (n == 1 || (n & (n - 1)) == 0) log("nvn: present #%llu (texture %d)", (unsigned long long)n, int(c.x[2]));
    g_frame++;
}

// (NVNwindow*, int x, int y, int width, int height): the part of the window textures shown. In handheld mode
// (NX_HANDHELD) the game renders 1280x720 into its 1920x1080 window textures and crops to it.
void WindowSetCrop(CPU& c) {
    if (auto* w = get<Window>(c.x[0])) {
        int32_t v[4] = {int32_t(c.x[1]), int32_t(c.x[2]), int32_t(c.x[3]), int32_t(c.x[4])};
        if (std::memcmp(v, w->crop, sizeof v) != 0) {
            static int logged = 0;
            if (logged++ < 20) log("nvn: window crop %d,%d %dx%d", v[0], v[1], v[2], v[3]);
            std::memcpy(w->crop, v, sizeof v);
        }
    }
}

}  // namespace

NX_HLE("nvnWindowSetCrop", WindowSetCrop);
NX_HLE("nvnSyncInitialize", SyncInitialize);
NX_HLE("nvnSyncFinalize", SyncFinalize);
NX_HLE("nvnSyncWait", SyncWait);
NX_HLE("nvnQueueInitialize", QueueInitialize);
NX_HLE("nvnQueueFinalize", QueueFinalize);
NX_HLE("nvnQueueSubmitCommands", QueueSubmitCommands);
NX_HLE("nvnQueueFlush", QueueFlush);
NX_HLE("nvnQueueFinish", QueueFinish);
NX_HLE("nvnQueueFenceSync", QueueFenceSync);
NX_HLE("nvnQueueWaitSync", QueueWaitSync);
NX_HLE("nvnQueuePresentTexture", QueuePresentTexture);
NX_HLE("nvnWindowInitialize", WindowInitialize);
NX_HLE("nvnWindowFinalize", WindowFinalize);
NX_HLE("nvnWindowAcquireTexture", WindowAcquireTexture);
NX_HLE("nvnWindowGetNumTextures", WindowGetNumTextures);
NX_HLE("nvnWindowGetNumActiveTextures", WindowGetNumActiveTextures);
NX_HLE("nvnWindowSetNumActiveTextures", WindowSetNumActiveTextures);
NX_HLE("nvnWindowGetPresentInterval", WindowGetPresentInterval);
NX_HLE("nvnWindowSetPresentInterval", WindowSetPresentInterval);

}  // namespace nx::nvn
