// The Vulkan side of NVN, shared between nvn_vk.cpp (device, images, submit, present) and nvn_draw.cpp (pipelines,
// resources, draws). Everything here runs under the GPU lock taken by gpu_submit / gpu_present.
#pragma once

#include <string>

#include "nvn.h"
#include "vk_gen.h"

namespace nx::nvn {

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;  // every level and layer
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0, height = 0, levels = 1, layers = 1, depth = 1;  // depth: 3D images only
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool rendered = false;  // written by the GPU: never re-uploaded from guest memory
    bool uploaded = false;  // filled from the texture's pool memory
    VkComponentMapping swizzle{};  // for sampling (attachment views stay identity)
    bool storage = false;          // created with VK_IMAGE_USAGE_STORAGE_BIT (compute shaders write it)
    // An R8 texture always read as (1, 1, 1, R) (a glyph cache): stored as RGBA8 (255, 255, 255, R) and sampled
    // without swizzle. MoltenVK returned alpha 0 for that view swizzle on a 2D-array view, so all text was invisible.
    bool alpha_from_r = false;
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
    TargetViews views;  // level and layer of each (index 8: depth)
};
void draw_begin_submit();
void draw_end_submit();  // after the GPU finished the submit
void draw_log_stats();
void draw_command(VkCommandBuffer cb, const Cmd& c, const CmdList& list, const Targets& targets);
void draw_end_rendering(VkCommandBuffer cb);  // before transfers (clears, copies, present) and at the end

void replay_copy(VkCommandBuffer cb, const Cmd& c, const CmdList& list);  // nvn_copy.cpp: texture copies
void forget_image_views(const GpuImage& img);  // nvn_pipeline.cpp: before an image is freed

// nvn_debug.cpp: GPU time per render pass (NX_GPU_PROFILE=1).
bool gpu_profile_on();
void gpu_profile_begin(VkCommandBuffer cb);
void gpu_profile_mark(VkCommandBuffer cb, const std::string& label);
void gpu_profile_collect();
void gpu_profile_log(uint64_t frames);

}  // namespace nx::nvn
