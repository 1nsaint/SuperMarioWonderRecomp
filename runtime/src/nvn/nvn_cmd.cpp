// NVN command buffers (docs/NVN.md, N3): recording. Each nvnCommandBuffer* call appends a Cmd to the buffer's list;
// nvnCommandBufferEndRecording turns the list into an NVNcommandHandle, which the game submits (possibly many times)
// or calls from other command buffers. Replaying into Vulkan is nvn_vk.cpp's job.
//
// Signatures from the game's call sites (nvn_trace.txt): SetRenderTargets(cmd, n, const NVNtexture* const* colors,
// const NVNtextureView* const* views, const NVNtexture* depth, const NVNtextureView* depthView);
// ClearColor(cmd, int index, const float* rgba, int channelMask); ClearDepthStencil(cmd, float depth [s0],
// NVNboolean depthMask, int stencil, int stencilMask); SetViewport / SetScissor(cmd, int x, int y, int w, int h);
// CallCommands(cmd, int count, const NVNcommandHandle*).
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <deque>

#include "nvn.h"

namespace nx::nvn {

namespace {

struct CommandBuffer : Object {
    std::mutex lock;
    std::shared_ptr<CmdList> recording = std::make_shared<CmdList>();
    // The memory the game hands over (AddCommandMemory / AddControlMemory) and what recording "uses" of it. Engines
    // look at the used amounts right after EndRecording (a list that used nothing is dropped, never submitted) and
    // at the free ones (to add memory in time), so usage grows with the commands recorded, survives EndRecording
    // and BeginRecording like the real memory does, and is given back only when memory is added. It never reaches
    // the total.
    uint64_t command_size = 0, control_size = 0;
    uint64_t command_base = 0, control_base = 0;  // usage before this recording
};

constexpr uint64_t kCommandBytes = 16, kControlBytes = 4;  // per recorded command

uint64_t used(uint64_t size, uint64_t base, size_t cmds, uint64_t per) {
    return std::min(base + cmds * per, size > 64 ? size - 64 : 0);
}

std::mutex g_handles_lock;
std::map<uint64_t, std::shared_ptr<CmdList>> g_handles;
std::deque<uint64_t> g_handle_order;  // oldest first: handles are dropped after kKeepHandles newer ones
constexpr size_t kKeepHandles = 1 << 16;
std::atomic<uint64_t> g_next_handle{1};


void CommandBufferInitialize(CPU& c) {  // NVNboolean (NVNcommandBuffer*, NVNdevice*)
    make<CommandBuffer>(c.x[0]);
    c.x[0] = 1;
}

void CommandBufferFinalize(CPU& c) { destroy(c.x[0]); }

void CommandBufferBeginRecording(CPU& c) {
    if (auto* cb = get<CommandBuffer>(c.x[0])) {
        std::lock_guard lk(cb->lock);
        cb->recording = std::make_shared<CmdList>();
    }
}

void AddCommandMemory(CPU& c) {  // (cmd, const NVNmemoryPool*, ptrdiff_t offset, size_t size)
    if (auto* cb = get<CommandBuffer>(c.x[0])) {
        std::lock_guard lk(cb->lock);
        cb->command_size = c.x[3];
        cb->command_base = 0;
    }
}

void AddControlMemory(CPU& c) {  // (cmd, void* memory, size_t size)
    if (auto* cb = get<CommandBuffer>(c.x[0])) {
        std::lock_guard lk(cb->lock);
        cb->control_size = c.x[2];
        cb->control_base = 0;
    }
}

template <bool Command, int What>  // What: 0 used, 1 size, 2 free
void memory_query(CPU& c) {
    auto* cb = get<CommandBuffer>(c.x[0]);
    if (!cb) {
        c.x[0] = 0;
        return;
    }
    std::lock_guard lk(cb->lock);
    uint64_t size = Command ? cb->command_size : cb->control_size;
    uint64_t u = used(size, Command ? cb->command_base : cb->control_base, cb->recording->cmds.size(),
                      Command ? kCommandBytes : kControlBytes);
    c.x[0] = What == 0 ? u : What == 1 ? size : size - u;
}

std::atomic<uint64_t> g_ends{0}, g_end_cmds{0}, g_calls{0}, g_no_cb{0};

void CommandBufferEndRecording(CPU& c) {  // NVNcommandHandle
    std::shared_ptr<CmdList> list = std::make_shared<CmdList>();
    if (auto* cb = get<CommandBuffer>(c.x[0])) {
        std::lock_guard lk(cb->lock);
        size_t n = cb->recording->cmds.size() + 1;  // + the end of the list, so even an empty one used something
        cb->command_base = used(cb->command_size, cb->command_base, n, kCommandBytes);
        cb->control_base = used(cb->control_size, cb->control_base, n, kControlBytes);
        list.swap(cb->recording);
    }
    g_ends++;
    g_end_cmds += list->cmds.size();
    if ((g_ends & 4095) == 0)
        log("nvn: %llu lists ended (%llu commands), %llu CallCommands, %llu records into unknown command buffers",
            (unsigned long long)g_ends.load(), (unsigned long long)g_end_cmds.load(), (unsigned long long)g_calls.load(),
            (unsigned long long)g_no_cb.load());
    uint64_t h = g_next_handle++;
    static const bool trace_lists = std::getenv("NX_NVN_LISTS") != nullptr;
    if (trace_lists && g_ends > 20000 && g_ends < 20200) {
        size_t draws = 0, calls = 0;
        for (auto& k : list->cmds) {
            draws += k.op == Cmd::DrawArrays || k.op == Cmd::DrawElements;
            calls += k.op == Cmd::CallList;
        }
        log("nvn list: end %llx -> handle %llu: %zu cmds, %zu draws, %zu calls, lr %s", (unsigned long long)c.x[0],
            (unsigned long long)h, list->cmds.size(), draws, calls, describe(c.x[30]).c_str());
    }
    {
        std::lock_guard lk(g_handles_lock);
        g_handles[h] = list;
        g_handle_order.push_back(h);
        while (g_handle_order.size() > kKeepHandles) {
            g_handles.erase(g_handle_order.front());
            g_handle_order.pop_front();
        }
    }
    c.x[0] = h;
}

void SetRenderTargets(CPU& c) {
    Cmd cmd{Cmd::SetRenderTargets};
    uint32_t n = std::min<uint32_t>(uint32_t(c.x[1]), 8);
    cmd.n = n;
    for (uint32_t k = 0; k < n && c.x[2]; k++) cmd.a[k] = ld<uint64_t>(c.x[2] + 8 * k);
    cmd.a[8] = c.x[4];  // depth/stencil
    // The views pick the level and layer rendered to (a bloom chain renders each mip of one texture in turn).
    TargetViews tv;
    for (uint32_t k = 0; k < n && c.x[3]; k++) {
        ViewInfo v = read_view(c, ld<uint64_t>(c.x[3] + 8 * k));
        tv.level[k] = uint8_t(v.base_level), tv.layer[k] = uint16_t(v.min_layer);
    }
    ViewInfo dv = read_view(c, c.x[5]);
    tv.level[8] = uint8_t(dv.base_level), tv.layer[8] = uint16_t(dv.min_layer);
    record(c.x[0], cmd, &tv, sizeof tv);
}

void ClearColor(CPU& c) {
    Cmd cmd{Cmd::ClearColor};
    cmd.n = uint32_t(c.x[1]);
    if (c.x[2]) std::memcpy(cmd.f, reinterpret_cast<const void*>(c.x[2]), 16);
    cmd.mask = uint32_t(c.x[3]);
    record(c.x[0], cmd);
}

void ClearDepthStencil(CPU& c) {
    Cmd cmd{Cmd::ClearDepthStencil};
    cmd.f[0] = c.v[0].f32[0];
    cmd.mask = (c.x[1] & 1) | ((uint32_t(c.x[3]) & 0xFF) << 8);  // depth mask, stencil mask
    cmd.i[0] = int32_t(c.x[2]);
    record(c.x[0], cmd);
}

void SetViewport(CPU& c) {
    Cmd cmd{Cmd::SetViewport};
    for (int k = 0; k < 4; k++) cmd.i[k] = int32_t(c.x[1 + k]);
    record(c.x[0], cmd);
}

void SetScissor(CPU& c) {
    Cmd cmd{Cmd::SetScissor};
    for (int k = 0; k < 4; k++) cmd.i[k] = int32_t(c.x[1 + k]);
    record(c.x[0], cmd);
}

void CallCommands(CPU& c) {
    uint32_t n = uint32_t(c.x[1]);
    g_calls += n;
    for (uint32_t k = 0; k < n; k++) {
        Cmd cmd{Cmd::CallList};
        cmd.a[0] = ld<uint64_t>(c.x[2] + 8 * k);
        record(c.x[0], cmd);
    }
}

}  // namespace

void record(uint64_t guest, Cmd cmd, const void* blob, size_t size) {
    auto* cb = get<CommandBuffer>(guest);
    if (!cb) {
        g_no_cb++;
        return;
    }
    std::lock_guard lk(cb->lock);
    if (blob) {
        cmd.blob = uint32_t(cb->recording->blobs.size());
        auto* p = static_cast<const uint8_t*>(blob);
        cb->recording->blobs.emplace_back(p, p + size);
    }
    cb->recording->cmds.push_back(cmd);
}

std::shared_ptr<CmdList> command_list(uint64_t handle) {
    std::lock_guard lk(g_handles_lock);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second;
}

NX_HLE("nvnCommandBufferInitialize", CommandBufferInitialize);
NX_HLE("nvnCommandBufferFinalize", CommandBufferFinalize);
NX_HLE("nvnCommandBufferBeginRecording", CommandBufferBeginRecording);
NX_HLE("nvnCommandBufferEndRecording", CommandBufferEndRecording);
NX_HLE("nvnCommandBufferSetRenderTargets", SetRenderTargets);
NX_HLE("nvnCommandBufferClearColor", ClearColor);
NX_HLE("nvnCommandBufferClearDepthStencil", ClearDepthStencil);
NX_HLE("nvnCommandBufferSetViewport", SetViewport);
NX_HLE("nvnCommandBufferSetScissor", SetScissor);
NX_HLE("nvnCommandBufferCallCommands", CallCommands);
NX_HLE("nvnCommandBufferAddCommandMemory", AddCommandMemory);
NX_HLE("nvnCommandBufferAddControlMemory", AddControlMemory);
NX_HLE("nvnCommandBufferGetCommandMemoryUsed", (memory_query<true, 0>));
NX_HLE("nvnCommandBufferGetCommandMemorySize", (memory_query<true, 1>));
NX_HLE("nvnCommandBufferGetCommandMemoryFree", (memory_query<true, 2>));
NX_HLE("nvnCommandBufferGetControlMemoryUsed", (memory_query<false, 0>));
NX_HLE("nvnCommandBufferGetControlMemorySize", (memory_query<false, 1>));
NX_HLE("nvnCommandBufferGetControlMemoryFree", (memory_query<false, 2>));

}  // namespace nx::nvn
