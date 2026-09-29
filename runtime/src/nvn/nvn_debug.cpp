// Debug readback of any NVN texture (docs/NVN.md): NX_DUMP_TEXTURES=<hex NVNtexture*>[,...] writes, at every frame
// NX_DUMP_FRAMES dumps, each listed texture's level 0 / layer 0 to <data root>/frames/NNNNN_<texture>.ppm. The image
// is blitted to RGBA8 first, so float and packed formats come out as the GPU would display them (clamped to 0..1).
// Runs under the GPU lock, after the frame's work completed, on its own command buffer.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

using namespace vk;

VkQueue vk_queue();
uint32_t vk_queue_family();

namespace {

std::vector<uint64_t> dump_list() {
    std::vector<uint64_t> v;
    const char* e = std::getenv("NX_DUMP_TEXTURES");
    while (e && *e) {
        char* end = nullptr;
        uint64_t t = std::strtoull(e, &end, 16);
        if (end == e) break;
        v.push_back(t);
        e = *end == ',' ? end + 1 : end;
    }
    return v;
}

struct Scratch {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
    void destroy(VkDevice d) {
        if (image) p_vkDestroyImage(d, image, nullptr);
        if (image_memory) p_vkFreeMemory(d, image_memory, nullptr);
        if (buffer) p_vkDestroyBuffer(d, buffer, nullptr);
        if (buffer_memory) p_vkFreeMemory(d, buffer_memory, nullptr);
    }
};

bool make_scratch(Scratch& s, uint32_t w, uint32_t h) {
    VkDevice d = vk_device();
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (p_vkCreateImage(d, &ii, nullptr, &s.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(d, s.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (p_vkAllocateMemory(d, &ai, nullptr, &s.image_memory) != VK_SUCCESS) return false;
    p_vkBindImageMemory(d, s.image, s.image_memory, 0);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = uint64_t(w) * h * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (p_vkCreateBuffer(d, &bi, nullptr, &s.buffer) != VK_SUCCESS) return false;
    p_vkGetBufferMemoryRequirements(d, s.buffer, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (p_vkAllocateMemory(d, &ai, nullptr, &s.buffer_memory) != VK_SUCCESS) return false;
    p_vkBindBufferMemory(d, s.buffer, s.buffer_memory, 0);
    return true;
}

void write_ppm(const Scratch& s, uint32_t w, uint32_t h, uint64_t frame, uint64_t texture) {
    void* p = nullptr;
    if (p_vkMapMemory(vk_device(), s.buffer_memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return;
    std::string dir = plat::data_root() + "/frames";
    plat::make_dirs(dir);
    char name[64];
    std::snprintf(name, sizeof name, "/%05llu_%llx.ppm", (unsigned long long)frame, (unsigned long long)texture);
    if (FILE* f = plat::fopen_utf8(dir + name, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        const uint8_t* px = static_cast<const uint8_t*>(p);
        for (size_t k = 0; k < size_t(w) * h; k++) std::fwrite(px + k * 4, 1, 3, f);
        std::fclose(f);
    }
    p_vkUnmapMemory(vk_device(), s.buffer_memory);
}

}  // namespace

void debug_dump_textures(uint64_t frame) {
    static const std::vector<uint64_t> list = dump_list();
    if (list.empty()) return;
    VkDevice d = vk_device();
    static VkCommandPool pool = VK_NULL_HANDLE;
    static VkCommandBuffer cb = VK_NULL_HANDLE;
    static VkFence fence = VK_NULL_HANDLE;
    if (!pool) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = vk_queue_family();
        p_vkCreateCommandPool(d, &pi, nullptr, &pool);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        p_vkAllocateCommandBuffers(d, &ai, &cb);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        p_vkCreateFence(d, &fi, nullptr, &fence);
    }
    for (uint64_t texture : list) {
        GpuImage* src = image_of(texture);
        if (!src || !src->image || src->layout == VK_IMAGE_LAYOUT_UNDEFINED || !(src->aspect & VK_IMAGE_ASPECT_COLOR_BIT))
            continue;
        uint32_t w = src->width, h = src->height;
        Scratch s;
        if (!make_scratch(s, w, h)) {
            s.destroy(d);
            continue;
        }
        p_vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        p_vkBeginCommandBuffer(cb, &bi);
        VkImageLayout back = src->layout;
        transition(cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        GpuImage dst;
        dst.image = s.image;
        dst.format = VK_FORMAT_R8G8B8A8_UNORM;
        transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (std::getenv("NX_DUMP_RAW")) {  // 4-byte texels as stored, to <frame>_<texture>.raw (w, h, vk format, data)
            transition(cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy r{};
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.imageExtent = {w, h, 1};
            p_vkCmdCopyImageToBuffer(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buffer, 1, &r);
            transition(cb, *src, back);
            p_vkEndCommandBuffer(cb);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;
            p_vkResetFences(d, 1, &fence);
            void* p = nullptr;
            if (p_vkQueueSubmit(vk_queue(), 1, &si, fence) == VK_SUCCESS &&
                p_vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS &&
                p_vkMapMemory(d, s.buffer_memory, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
                char name[64];
                std::snprintf(name, sizeof name, "/frames/%05llu_%llx.raw", (unsigned long long)frame,
                              (unsigned long long)texture);
                if (FILE* f = plat::fopen_utf8(plat::data_root() + name, "wb")) {
                    uint32_t head[3] = {w, h, uint32_t(src->format)};
                    std::fwrite(head, 4, 3, f);
                    std::fwrite(p, 4, size_t(w) * h, f);
                    std::fclose(f);
                }
                p_vkUnmapMemory(d, s.buffer_memory);
            }
            s.destroy(d);
            continue;
        }
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(w), int32_t(h), 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[1] = blit.srcOffsets[1];
        p_vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {w, h, 1};
        p_vkCmdCopyImageToBuffer(cb, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buffer, 1, &r);
        transition(cb, *src, back);
        p_vkEndCommandBuffer(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        p_vkResetFences(d, 1, &fence);
        if (p_vkQueueSubmit(vk_queue(), 1, &si, fence) == VK_SUCCESS) {
            p_vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX);
            write_ppm(s, w, h, frame, texture);
        }
        s.destroy(d);
    }
}

}  // namespace nx::nvn
