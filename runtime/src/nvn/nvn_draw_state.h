// The draw replay's bound state (nvn_draw.cpp), shared with its debug traces (nvn_draw_trace.cpp).
#pragma once

#include <utility>
#include <vector>

#include "nvn_pipeline.h"

namespace nx::nvn::replay {

constexpr int kStages = 6;  // NVN shader stages: 0 vertex, 1 fragment, 2 geometry, 3/4 tessellation, 5 compute

struct Range {
    uint64_t address = 0, size = 0;
};

struct State {
    uint64_t program = 0;
    Range uniforms[kStages][18], storage[kStages][16], vertex_buffers[16];
    uint64_t textures[kStages][32] = {};  // NVN texture handles by sampler binding
    uint64_t images[kStages][16] = {};    // NVN image handles by image binding (BindImage)
    uint64_t texture_pool = 0, sampler_pool = 0;
    std::vector<VertexAttrib> attribs;
    std::vector<VertexStream> streams;
    BlendTarget blend[8];
    ColorStateInfo color;
    ChannelMaskInfo mask;
    DepthStencilInfo depth;
    PolygonInfo polygon;
    float blend_color[4] = {};
    int32_t viewport[4] = {}, scissor[4] = {};
    bool rasterizer_discard = false, render_enable = true;  // SetRasterizerDiscard / SetRenderEnable: draws skipped
    bool has_viewport = false, has_scissor = false;
    float depth_near = 0, depth_far = 1;
};

struct Stats {
    uint64_t total = 0, no_program = 0, no_vertex = 0, no_pipeline = 0, no_indices = 0, done = 0, to_window = 0;
    uint64_t dispatches = 0;
};

extern State g_state;
extern Stats g_stats;

const uint8_t* guest_bytes(uint64_t gpu);
uint32_t handle_texture(uint64_t handle, uint32_t* sampler);
// The texture and sampler a handle names in the bound pools.
void texture_for(uint64_t handle, uint64_t& texture, SamplerInfo& sampler, ViewInfo* view = nullptr);

using TexBindings = std::vector<std::pair<const ShaderInfo*, const DescriptorBinding*>>;
// NX_TRACE_DRAWS & co (docs/nvn/DEBUG.md): logs a replayed draw with its state, when the switches select it.
void trace_draw(const Targets& t, uint32_t mode, bool indexed, const Cmd& c, const ShaderInfo* vs,
                const ShaderInfo* fs, const TexBindings& tex_bindings, const std::vector<uint64_t>& handles);

// NX_TRACE_SKIN: skinned draws' inputs, vertex uniform blocks and the bone rows they select.
void trace_skin(bool indexed, const Cmd& c, const ShaderInfo* vs, const ShaderInfo* fs);

}  // namespace nx::nvn::replay
