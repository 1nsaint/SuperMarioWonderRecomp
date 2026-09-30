// Asynchronous GPU submission (NX_ASYNC_GPU=<frames in flight>, off by default; the design, and what stays
// synchronous and why: private/wonder/patches/async_submit.md). The synchronous path waits for every submit's fence
// before returning to the game, so the game's render thread stalls for the whole GPU time of each frame. Here a
// submit or a present takes one of a few slots (command buffer, fence, descriptor pool, the stretch of the upload
// ring its replay filled, the compute results to copy back) and returns once queued. A slot is retired (its fence
// waited, 2 s at most, as the synchronous path) when it is reused, when the ring needs its memory back, or when the
// game waits for it (nvnSyncWait after nvnQueueFenceSync, nvnQueueFinish).
//
// Replays copy everything the GPU reads from guest memory into the ring at submit time, so the game may change its
// memory while a submit runs. Only compute results flow back (write-backs): a submit that has them is retired before
// the next replay begins (it may read them) and before a wait on it returns. Also the images of destroyed textures
// (freed once the submits that could use them are retired) are handled here, in both modes.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

VkQueue vk_queue();                // nvn_vk.cpp
std::recursive_mutex& gpu_lock();  // nvn_vk.cpp

namespace {

constexpr uint32_t kMaxSlots = 8;
constexpr uint64_t kFenceTimeout = 2'000'000'000ull;  // a frame's work takes milliseconds; longer is a hung shader

struct Slot {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;          // a present's swapchain image
    VkDescriptorPool descriptors = VK_NULL_HANDLE;  // made on first use (presents need none)
    bool descriptors_used = false;
    uint64_t serial = 0;    // in flight until retired; 0: free
    uint64_t ring_end = 0;  // the ring position after this submit's data
    std::vector<WriteBack> write_backs;
};

Slot g_slots[kMaxSlots];
uint32_t g_slot_count = 0, g_next = 0;
Slot* g_current = nullptr;               // the slot being recorded
VkSemaphore g_rendered[kMaxSlots] = {};  // per swapchain image: signaled for its present
std::atomic<uint64_t> g_submitted{0}, g_completed{0};
std::atomic<uint64_t> g_barrier{0};  // the last submit with write-backs: retired before the next replay
uint64_t g_wait_ns = 0;

// The upload ring (the synchronous path's upload buffer). Positions only grow; position p is byte p % size. What
// lies before g_tail belongs to retired submits; g_begin is where the submit being recorded started.
VkBuffer g_ring = VK_NULL_HANDLE;
uint8_t* g_ring_ptr = nullptr;
uint64_t g_ring_size = 0, g_head = 0, g_tail = 0, g_begin = 0;

// Images of destroyed textures, with the serial of the last submit that may still use them.
std::mutex g_grave_lock;
std::vector<std::pair<uint64_t, std::shared_ptr<GpuImage>>> g_grave;

// GPU-written images of destroyed textures, kept a few submits for a new texture over the same memory with the same
// shape. Wonder re-creates ~180 NVNtexture objects per frame over the same render-target memory: each got a new
// image, and its first draw ended the render pass to set it up (most of the ~300 passes per frame, each reloading
// and storing its targets; docs/nvn/DEBUG.md #34).
struct ReuseKey {
    uint64_t memory = 0;
    uint32_t format = 0, width = 0, height = 0, depth = 0, levels = 0, target = 0, samples = 0;
    bool operator<(const ReuseKey& o) const {
        return std::tie(memory, format, width, height, depth, levels, target, samples) <
               std::tie(o.memory, o.format, o.width, o.height, o.depth, o.levels, o.target, o.samples);
    }
};
ReuseKey reuse_key(const Texture& t) {
    return {t.memory, t.format, t.width, t.height, t.depth, t.levels, t.target, t.samples};
}
std::map<ReuseKey, std::pair<uint64_t, std::shared_ptr<GpuImage>>> g_reusable;  // key -> (kept at, image)
uint64_t g_reuse_clock = 0, g_reused = 0;

// Waits for the oldest submit in flight and finishes it: compute results to guest memory, its descriptor pool and
// ring stretch free again, images destroyed before it freed. False when nothing is in flight. Fences complete in
// submission order on the one queue, so retiring oldest first keeps write-backs in order.
bool retire_oldest() {
    Slot* s = nullptr;
    for (uint32_t k = 0; k < g_slot_count; k++)
        if (g_slots[k].serial && (!s || g_slots[k].serial < s->serial)) s = &g_slots[k];
    if (!s) return false;
    uint64_t t0 = plat::monotonic_ns();
    VkResult r = p_vkWaitForFences(vk_device(), 1, &s->fence, VK_TRUE, kFenceTimeout);
    if (r == VK_TIMEOUT) gpu_hung("a submit did not finish in 2 s");
    if (r == VK_ERROR_DEVICE_LOST) gpu_hung("vkWaitForFences");
    if (r != VK_SUCCESS) log("nvn/vk: vkWaitForFences failed (%d)", int(r));
    g_wait_ns += plat::monotonic_ns() - t0;
    for (const WriteBack& w : s->write_backs) std::memcpy(w.to, w.from, w.size);
    s->write_backs.clear();
    if (s->descriptors_used) p_vkResetDescriptorPool(vk_device(), s->descriptors, 0);
    s->descriptors_used = false;
    g_tail = std::max(g_tail, s->ring_end);
    g_completed = s->serial;
    s->serial = 0;
    free_retired_images(g_completed);
    return true;
}

void retire_through(uint64_t serial) {
    while (g_completed < serial && retire_oldest()) {
    }
}

}  // namespace

bool async_gpu() {
    static const bool on = [] {
        const char* e = std::getenv("NX_ASYNC_GPU");
        return e && std::atoi(e) > 0;
    }();
    return on;
}

void frame_init(VkCommandPool pool, VkBuffer ring, uint8_t* ring_ptr, uint64_t ring_size) {
    // NX_ASYNC_GPU=1 (or 2): two frames in flight; 3 or 4: that many. A frame is a submit and a present.
    uint32_t frames = uint32_t(std::clamp(std::atoi(std::getenv("NX_ASYNC_GPU")), 2, 4));
    g_slot_count = std::min(kMaxSlots, frames * 2);
    VkDevice d = vk_device();
    for (uint32_t k = 0; k < g_slot_count; k++) {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        p_vkAllocateCommandBuffers(d, &cai, &g_slots[k].cb);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        p_vkCreateFence(d, &fci, nullptr, &g_slots[k].fence);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        p_vkCreateSemaphore(d, &sci, nullptr, &g_slots[k].acquired);
    }
    g_ring = ring, g_ring_ptr = ring_ptr, g_ring_size = ring_size;
    log("nvn/vk: asynchronous submits (NX_ASYNC_GPU): %u frames in flight, %u slots", frames, g_slot_count);
}

VkCommandBuffer frame_begin(bool replay) {
    if (replay && g_barrier > g_completed) retire_through(g_barrier);  // this replay may read those results
    Slot& s = g_slots[g_next];
    if (s.serial) retire_through(s.serial);
    g_next = (g_next + 1) % g_slot_count;
    g_current = &s;
    g_begin = g_head;
    p_vkResetCommandBuffer(s.cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (p_vkBeginCommandBuffer(s.cb, &bi) != VK_SUCCESS) {
        log("nvn/vk: vkBeginCommandBuffer failed");
        g_current = nullptr;
        return VK_NULL_HANDLE;
    }
    // Earlier submits may still run: their writes come before anything here (image layouts are tracked in
    // submission order, so the images already are where these commands expect them).
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    return s.cb;
}

VkSemaphore frame_acquire_semaphore() {  // the next slot's (frame_begin takes that slot)
    Slot& s = g_slots[g_next];
    if (s.serial) retire_through(s.serial);  // its last present waited on it
    return s.acquired;
}

VkSemaphore frame_rendered_semaphore(uint32_t image) {
    if (image >= kMaxSlots) image = kMaxSlots - 1;
    if (!g_rendered[image]) {
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        p_vkCreateSemaphore(vk_device(), &sci, nullptr, &g_rendered[image]);
    }
    return g_rendered[image];
}

void frame_submit(VkSemaphore wait, VkSemaphore signal, std::vector<WriteBack> write_backs, bool wait_done) {
    if (!g_current) return;
    Slot& s = *g_current;
    g_current = nullptr;
    // The host reads compute results after the fence.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    p_vkCmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr,
                           0, nullptr);
    p_vkEndCommandBuffer(s.cb);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cb;
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
        si.pWaitDstStageMask = &stage;
    }
    if (signal) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal;
    }
    p_vkResetFences(vk_device(), 1, &s.fence);
    VkResult r = p_vkQueueSubmit(vk_queue(), 1, &si, s.fence);
    if (r == VK_ERROR_DEVICE_LOST) gpu_hung("vkQueueSubmit");
    if (r != VK_SUCCESS) {  // nothing ran: no results to copy back
        log("nvn/vk: vkQueueSubmit failed (%d)", int(r));
        return;
    }
    s.serial = ++g_submitted;
    s.ring_end = g_head;
    s.write_backs = std::move(write_backs);
    if (!s.write_backs.empty()) g_barrier = s.serial;
    // NX_ASYNC_COMPUTE_WAIT=1: submits with compute results are waited for at once, as synchronously (the game then
    // finds them in its memory right after nvnQueueSubmitCommands, without waiting on a sync).
    static const bool compute_wait = std::getenv("NX_ASYNC_COMPUTE_WAIT") != nullptr;
    if (wait_done || (compute_wait && !s.write_backs.empty())) retire_through(s.serial);
}

void frame_wait_all() { retire_through(g_submitted); }

uint64_t frame_upload_used() { return g_head - g_begin; }

uint64_t frame_take_wait_ns() {
    uint64_t ns = g_wait_ns;
    g_wait_ns = 0;
    return ns;
}

Upload frame_upload_alloc(uint64_t size, uint64_t align) {
    uint64_t at = (g_head + align - 1) & ~(align - 1);
    if (at % g_ring_size + size > g_ring_size) at += g_ring_size - at % g_ring_size;  // never split: the next lap
    while (at + size > g_tail + g_ring_size)  // the oldest submits' data is still read: wait for them
        if (!retire_oldest()) {
            log("nvn/vk: upload buffer full (%llu bytes in this submit)", (unsigned long long)(at + size - g_begin));
            g_tail = at + size - g_ring_size;  // this submit wraps over its own first data, as the synchronous path
            replay::dedup_reset();
            break;
        }
    g_head = at + size;
    uint64_t off = at % g_ring_size;
    return {g_ring, off, g_ring_ptr + off};
}

VkDescriptorSet frame_descriptor_set(VkDescriptorSetLayout layout) {
    if (!g_current) return VK_NULL_HANDLE;
    Slot& s = *g_current;
    if (!s.descriptors) s.descriptors = make_descriptor_pool();
    s.descriptors_used = true;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s.descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (p_vkAllocateDescriptorSets(vk_device(), &ai, &set) != VK_SUCCESS) log("nvn/vk: descriptor pool exhausted");
    return set;
}

// nvn.h: what nvnQueueFenceSync records and nvnSyncWait / nvnQueueFinish wait for. The synchronous path has
// nothing in flight when the game gets control back: 0, and no wait.
uint64_t gpu_serial() { return async_gpu() ? g_submitted.load() : 0; }
uint64_t gpu_write_back_serial() { return async_gpu() ? g_barrier.load() : 0; }

bool gpu_wait(uint64_t serial) {
    if (!serial || g_completed >= serial) return false;
    std::lock_guard lk(gpu_lock());
    if (g_completed >= serial) return false;
    retire_through(serial);
    return true;
}

void free_retired_images(uint64_t completed) {
    std::vector<std::shared_ptr<GpuImage>> dead;
    {
        std::lock_guard lk(g_grave_lock);
        g_reuse_clock++;  // once per submit and present
        for (auto it = g_reusable.begin(); it != g_reusable.end();)
            if (it->second.first + 8 < g_reuse_clock) {
                g_grave.push_back({g_submitted + 1, std::move(it->second.second)});
                it = g_reusable.erase(it);
            } else {
                ++it;
            }
        if (g_reuse_clock % 3600 == 0 && g_reused) log("nvn/vk: %llu images reused", (unsigned long long)g_reused);
        auto done = std::stable_partition(g_grave.begin(), g_grave.end(),
                                          [&](const auto& e) { return e.first > completed; });
        for (auto it = done; it != g_grave.end(); ++it) dead.push_back(std::move(it->second));
        g_grave.erase(done, g_grave.end());
    }
    for (auto& img : dead) {
        if (!img || !img->image) continue;
        if (img.use_count() > 1) continue;  // another live texture shares it (image_of): freed with the last one
        forget_image_views(*img);
        if (img->view) p_vkDestroyImageView(vk_device(), img->view, nullptr);
        p_vkDestroyImage(vk_device(), img->image, nullptr);
        if (img->memory) p_vkFreeMemory(vk_device(), img->memory, nullptr);
        img->image = VK_NULL_HANDLE, img->memory = VK_NULL_HANDLE, img->view = VK_NULL_HANDLE;
    }
}

// Images of destroyed textures are freed once no submit can use them: the one being recorded now included (a replay
// on another thread may hold it), hence the serial after the last submitted. Before the graveyard no image was ever
// freed: the opening creates textures continuously and the process reached 85 GB (the machine froze).
Texture::~Texture() {
    if (!image) return;
    std::lock_guard lk(g_grave_lock);
    if (image->rendered && image->image && memory) {
        auto& slot = g_reusable[reuse_key(*this)];
        if (slot.second) g_grave.push_back({g_submitted + 1, std::move(slot.second)});
        slot = {g_reuse_clock, std::move(image)};
        return;
    }
    g_grave.push_back({g_submitted + 1, std::move(image)});
}

std::shared_ptr<GpuImage> reuse_image(const Texture& t) {
    // Off unless NX_IMAGE_REUSE=1: a CPU-written texture placed over a freed render target's memory would sample the
    // old render, and swizzle/alpha_from_r come from the old texture (review of #34). The pass breaks it avoided came
    // mostly from destroyed pool textures (fixed separately).
    static const bool off = std::getenv("NX_IMAGE_REUSE") == nullptr;
    if (!t.memory || off) return nullptr;
    std::lock_guard lk(g_grave_lock);
    auto it = g_reusable.find(reuse_key(t));
    if (it == g_reusable.end()) return nullptr;
    std::shared_ptr<GpuImage> img = std::move(it->second.second);
    g_reusable.erase(it);
    g_reused++;
    return img;
}

}  // namespace nx::nvn
