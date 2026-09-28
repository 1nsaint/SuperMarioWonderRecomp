// NVN queues, command buffers, windows and syncs (docs/NVN.md, N3). Without the Vulkan side every command
// completes at submit: fences are signaled right away, the window hands out its textures in turn, and presents are
// counted. Return values follow the SDK driver (nvnSyncWait sdk+45a960: 0 once the fence has passed;
// nvnWindowAcquireTexture sdk+45e050: 0 on success).
#include <algorithm>
#include <atomic>
#include <vector>

#include "nvn.h"

namespace nx::nvn {

namespace {

// ---- syncs ------------------------------------------------------------------------------------------------------

struct Sync : Object {
    std::atomic<bool> signaled{false};
};

void SyncInitialize(CPU& c) {  // NVNboolean (NVNsync*, NVNdevice*)
    make<Sync>(c.x[0]);
    c.x[0] = 1;
}

void SyncFinalize(CPU& c) { destroy(c.x[0]); }

void signal_sync(uint64_t guest) {
    if (auto* s = get<Sync>(guest)) s->signaled = true;
}

void SyncWait(CPU& c) { c.x[0] = 0; }  // NVNsyncWaitResult (const NVNsync*, u64 timeout): already signaled

// ---- queues -----------------------------------------------------------------------------------------------------

std::atomic<uint64_t> g_presents{0};

void QueueInitialize(CPU& c) { c.x[0] = 1; }  // NVNboolean (NVNqueue*, const NVNqueueBuilder*)
void QueueFinalize(CPU&) {}
void QueueSubmitCommands(CPU&) {}  // (NVNqueue*, int count, const NVNcommandHandle*)
void QueueFlush(CPU&) {}
void QueueFinish(CPU&) {}
void QueueFenceSync(CPU& c) { signal_sync(c.x[1]); }  // (NVNqueue*, NVNsync*, NVNsyncCondition, int flags)
void QueueWaitSync(CPU& c) { c.x[0] = 1; }            // NVNboolean (NVNqueue*, const NVNsync*)

// ---- windows ----------------------------------------------------------------------------------------------------

struct Window : Object {
    std::vector<uint64_t> textures;  // guest NVNtexture*
    int active = 0, next = 0, interval = 1;
};

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

void WindowSetPresentInterval(CPU& c) {
    if (auto* w = get<Window>(c.x[0])) w->interval = int(c.x[1]);
}

// (NVNqueue*, NVNwindow*, int textureIndex)
void QueuePresentTexture(CPU& c) {
    uint64_t n = ++g_presents;
    if (n == 1 || (n & (n - 1)) == 0) log("nvn: present #%llu (texture %d)", (unsigned long long)n, int(c.x[2]));
    g_frame++;
}

// ---- command buffers --------------------------------------------------------------------------------------------

std::atomic<uint64_t> g_next_handle{1};

void CommandBufferInitialize(CPU& c) { c.x[0] = 1; }  // NVNboolean (NVNcommandBuffer*, NVNdevice*)
void CommandBufferFinalize(CPU&) {}
void CommandBufferBeginRecording(CPU&) {}
void CommandBufferEndRecording(CPU& c) { c.x[0] = g_next_handle++; }  // NVNcommandHandle

}  // namespace

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
NX_HLE("nvnCommandBufferInitialize", CommandBufferInitialize);
NX_HLE("nvnCommandBufferFinalize", CommandBufferFinalize);
NX_HLE("nvnCommandBufferBeginRecording", CommandBufferBeginRecording);
NX_HLE("nvnCommandBufferEndRecording", CommandBufferEndRecording);

}  // namespace nx::nvn
