// The Vulkan side of NVN, shared between nvn_vk.cpp (device, images, submit, present) and nvn_draw.cpp (pipelines,
// resources, draws). Everything here runs under the GPU lock taken by gpu_submit / gpu_present.
#pragma once

#include "nvn.h"
#include "vk_gen.h"

namespace nx::nvn {

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;  // every level and layer
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0, height = 0, levels = 1, layers = 1;
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool rendered = false;  // written by the GPU: never re-uploaded from guest memory
    bool uploaded = false;  // filled from the texture's pool memory
};

VkDevice vk_device();
bool vk_depth_clip_control();
VkPhysicalDevice vk_physical();
uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want);

// The image behind an NVN texture (guest NVNtexture*), made on first use.
GpuImage* image_of(uint64_t texture);
void transition(VkCommandBuffer cb, GpuImage& img, VkImageLayout to);

// Per-submit upload memory (host visible, coherent), reset at every submit: data the replayed commands read from
// guest memory is copied here at replay time (vertex, index, uniform, storage data, texture staging).
struct Upload {
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t offset = 0;
    uint8_t* ptr = nullptr;
};
Upload upload_alloc(uint64_t size, uint64_t align);
Upload upload(const void* data, uint64_t size, uint64_t align);

// Descriptor sets for this submit (freed when it completes).
VkDescriptorSet descriptor_set(VkDescriptorSetLayout layout);

// nvn_draw.cpp: replays one draw-related command; ends the render pass it may have open when asked.
struct Targets {
    uint64_t colors[8] = {};
    uint32_t color_count = 0;
    uint64_t depth = 0;
};
void draw_begin_submit();
void draw_log_stats();
void draw_command(VkCommandBuffer cb, const Cmd& c, const CmdList& list, const Targets& targets);
void draw_end_rendering(VkCommandBuffer cb);  // before transfers (clears, copies, present) and at the end

}  // namespace nx::nvn
