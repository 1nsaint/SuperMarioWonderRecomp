// Pipelines and the resources draws read (docs/NVN.md, N4-N6): shader modules, descriptor-set and pipeline
// layouts, graphics pipelines keyed by their state, samplers, and sampled textures (untiled and uploaded on first
// use). Used by nvn_draw.cpp, under the GPU lock.
#pragma once

#include <vector>

#include "nvn_shaders.h"
#include "nvn_vk.h"

namespace nx::nvn {

bool vk_depth_clip_control();

// The four descriptor sets of the translator's scheme (0 uniform buffers, 1 storage buffers, 2 textures, 3 images)
// for a set of stages, and the pipeline layout over them.
struct ProgramLayout {
    VkDescriptorSetLayout sets[4] = {};
    VkPipelineLayout layout = VK_NULL_HANDLE;
};
const ProgramLayout& program_layout(const std::vector<const ShaderInfo*>& stages);

// Everything a graphics pipeline is built from (NVN enums, decoded state).
struct PipelineKey {
    const ShaderInfo* vs = nullptr;
    const ShaderInfo* fs = nullptr;
    uint32_t topology = 4;
    VertexAttrib attribs[16] = {};
    VertexStream streams[16] = {};
    uint32_t attrib_count = 0, stream_count = 0;
    BlendTarget blend[8] = {};
    ColorStateInfo color;
    ChannelMaskInfo mask;
    DepthStencilInfo depth;
    PolygonInfo polygon;
    VkFormat color_formats[8] = {};
    uint32_t color_count = 0;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
};
VkPipeline graphics_pipeline(const PipelineKey& key, const ProgramLayout& layout);

VkSampler sampler_for(const SamplerInfo& info, bool shadow);

// A sampled texture ready for the shaders (uploaded from guest memory the first time, unless the GPU renders it),
// and a view of it matching what the shader declares. Transitions the image to SHADER_READ_ONLY on cb.
// `view`: the texture pool entry's view (levels, layers, swizzle), if any. `layout` gets the layout to sample in:
// images the GPU renders stay GENERAL (a bloom chain samples one level while rendering another).
VkImageView sampled_view(VkCommandBuffer cb, uint64_t texture, const ImageShape& shape, const ViewInfo* view,
                         VkImageLayout* layout);
// A single level and layer of an image, for rendering to.
VkImageView attachment_view(GpuImage& img, uint32_t level, uint32_t layer);
// True when sampled_view() would need no transfer or layout change (it can then be called inside a render pass).
bool sampled_ready(uint64_t texture);
void pipeline_begin_submit();  // per-submit state (the content checks of pitch-linear textures)
bool dummy_ready(const ImageShape& shape);
// A 1x1 black texture of the shape, for bindings with nothing behind them.
VkImageView dummy_view(VkCommandBuffer cb, const ImageShape& shape);

}  // namespace nx::nvn
