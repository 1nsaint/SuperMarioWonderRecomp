// Draws (docs/NVN.md, N6): replays the bind, state and draw commands of nvn_state.cpp into Vulkan. Everything a draw
// reads from guest memory (vertices, indices, uniform and storage buffers, textures) is copied at replay time into
// the submit's upload buffer: the replay runs right when the game submits and is waited for, so the memory holds
// what the GPU would have read. Render targets become a dynamic-rendering pass, begun at the first draw and ended
// before any transfer.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>

#include "nvn_pipeline.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

constexpr int kStages = 6;  // NVN shader stages: 0 vertex, 1 fragment, 2 geometry, 3/4 tessellation, 5 compute

struct Range {
    uint64_t address = 0, size = 0;
};

struct State {
    uint64_t program = 0;
    Range uniforms[kStages][18], storage[kStages][16], vertex_buffers[16];
    uint64_t textures[kStages][32] = {};  // NVN texture handles by sampler binding
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
    bool has_viewport = false, has_scissor = false;
    float depth_near = 0, depth_far = 1;
};

State g_state;
bool g_rendering = false;
Targets g_pass;           // what the open pass renders to
VkExtent2D g_pass_extent{};

int api_stage(ShaderStage s) { return s == ShaderStage::Fragment ? 1 : s == ShaderStage::Compute ? 5 : 0; }

const uint8_t* guest_bytes(uint64_t gpu) {
    uint64_t off = 0;
    MemoryPool* p = pool_at_gpu(gpu, &off);
    uint64_t host = p ? pool_host(*p, off) : 0;
    return host ? reinterpret_cast<const uint8_t*>(host) : nullptr;
}

void begin_rendering(VkCommandBuffer cb, const Targets& t) {
    VkRenderingAttachmentInfo colors[8]{};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkExtent2D extent{UINT32_MAX, UINT32_MAX};
    for (uint32_t k = 0; k < t.color_count; k++) {
        colors[k].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        GpuImage* img = t.colors[k] ? image_of(t.colors[k]) : nullptr;
        if (!img) continue;
        transition(cb, *img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        img->rendered = true;
        colors[k].imageView = img->view;
        colors[k].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[k].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = {std::min(extent.width, img->width), std::min(extent.height, img->height)};
    }
    GpuImage* dimg = t.depth ? image_of(t.depth) : nullptr;
    if (dimg && dimg->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
        transition(cb, *dimg, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        dimg->rendered = true;
        depth.imageView = dimg->view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = {std::min(extent.width, dimg->width), std::min(extent.height, dimg->height)};
    } else {
        dimg = nullptr;
    }
    if (extent.width == UINT32_MAX) extent = {1, 1};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = t.color_count;
    ri.pColorAttachments = colors;
    if (dimg) {
        if (dimg->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &depth;
        if (dimg->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &depth;
    }
    p_vkCmdBeginRendering(cb, &ri);
    g_rendering = true;
    g_pass = t;
    g_pass_extent = extent;
}

// The buffer a descriptor points to: `bytes` copied into the upload buffer (at least 16 bytes, zero padded).
VkDescriptorBufferInfo buffer_of(const void* bytes, uint64_t size) {
    uint64_t n = std::max<uint64_t>((size + 15) & ~15ull, 16);
    Upload u = upload_alloc(n, 256);
    std::memset(u.ptr, 0, n);
    if (bytes && size) std::memcpy(u.ptr, bytes, size);
    return {u.buffer, u.offset, n};
}

uint32_t handle_texture(uint64_t handle, uint32_t* sampler) {
    if (sampler) *sampler = uint32_t(handle >> 20) & 0xFFF;
    return uint32_t(handle & 0xFFFFF);
}

struct Resolved {
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> buffers;
    std::vector<VkDescriptorImageInfo> images;
    std::vector<uint64_t> textures;  // guest textures sampled (for the render-pass check)
};

// The texture and sampler a handle names in the bound pools.
void texture_for(uint64_t handle, uint64_t& texture, SamplerInfo& sampler) {
    uint32_t sid = 0;
    uint32_t tid = handle_texture(handle, &sid);
    texture = 0;
    if (auto* tp = get<DescriptorPool>(g_state.texture_pool)) {
        auto it = tp->textures.find(tid);
        if (it != tp->textures.end()) texture = it->second;
    }
    if (auto* sp = get<DescriptorPool>(g_state.sampler_pool)) {
        auto it = sp->samplers.find(sid);
        if (it != sp->samplers.end()) sampler = it->second;
    }
}

struct Stats {
    uint64_t total = 0, no_program = 0, no_vertex = 0, no_pipeline = 0, no_indices = 0, done = 0, to_window = 0;
} g_stats;

bool draw(VkCommandBuffer cb, const Targets& t, uint32_t mode, bool indexed, const Cmd& c) {
    g_stats.total++;
    const auto* shaders = program_shaders(g_state.program);
    if (!shaders || shaders->empty()) {
        g_stats.no_program++;
        return false;
    }
    const ShaderInfo* vs = nullptr;
    const ShaderInfo* fs = nullptr;
    for (const ShaderInfo* s : *shaders) {
        if (s->stage == ShaderStage::Vertex) vs = s;
        if (s->stage == ShaderStage::Fragment) fs = s;
    }
    if (!vs) {
        g_stats.no_vertex++;
        return false;
    }
    const ProgramLayout& layout = program_layout(*shaders);

    // Textures first: an upload or a layout change cannot happen inside the render pass.
    std::vector<std::pair<const ShaderInfo*, const DescriptorBinding*>> tex_bindings;
    for (const ShaderInfo* s : *shaders)
        for (const DescriptorBinding& b : s->refl.bindings)
            if (b.set == 2) tex_bindings.push_back({s, &b});
    std::vector<uint64_t> handles(tex_bindings.size());
    bool need_break = false;
    for (size_t k = 0; k < tex_bindings.size(); k++) {
        auto [s, b] = tex_bindings[k];
        int st = api_stage(s->stage);
        uint64_t handle = 0;
        if (b->nvn.source == NvnSource::SamplerBinding && b->nvn.index >= 0 && b->nvn.index < 32)
            handle = g_state.textures[st][b->nvn.index];
        else if (b->nvn.source == NvnSource::HandleInUniform && b->nvn.bank >= 3) {
            const Range& r = g_state.uniforms[st][b->nvn.bank - 3];
            const uint8_t* p = r.address ? guest_bytes(r.address) : nullptr;
            if (p && uint64_t(b->nvn.word) * 4 + 4 <= r.size) handle = *reinterpret_cast<const uint32_t*>(p + b->nvn.word * 4);
        }
        handles[k] = handle;
        uint64_t texture = 0;
        SamplerInfo si;
        texture_for(handle, texture, si);
        if (texture ? !sampled_ready(texture) : !dummy_ready(b->image)) need_break = true;
    }
    bool targets_changed = !g_rendering || std::memcmp(&g_pass, &t, sizeof t) != 0;
    if (g_rendering && (need_break || targets_changed)) draw_end_rendering(cb);

    // Descriptor sets.
    VkDescriptorSet sets[4];
    for (int k = 0; k < 4; k++) sets[k] = descriptor_set(layout.sets[k]);
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> buffers;
    std::vector<VkDescriptorImageInfo> images;
    buffers.reserve(256), images.reserve(256), writes.reserve(512);
    SupportBuffer sb = support_buffer_defaults();
    float vw = g_state.has_viewport ? float(g_state.viewport[2]) : 1280.0f;
    float vh = g_state.has_viewport ? float(g_state.viewport[3]) : 720.0f;
    support_buffer_set_viewport(sb, vw, vh);
    for (const ShaderInfo* s : *shaders) {
        int st = api_stage(s->stage);
        for (const DescriptorBinding& b : s->refl.bindings) {
            if (b.set >= 2) continue;
            VkDescriptorBufferInfo info{};
            switch (b.nvn.source) {
                case NvnSource::SupportBuffer: info = buffer_of(&sb, sizeof sb); break;
                case NvnSource::ConstantData: info = buffer_of(s->constants.data(), s->constants.size()); break;
                case NvnSource::UniformBlock:
                case NvnSource::UniformAsStorage: {
                    const Range& r = b.nvn.index >= 0 && b.nvn.index < 18 ? g_state.uniforms[st][b.nvn.index] : Range{};
                    const uint8_t* p = r.address ? guest_bytes(r.address) : nullptr;
                    info = buffer_of(p, p ? std::min<uint64_t>(r.size, 0x10000) : 0);
                    break;
                }
                case NvnSource::StorageBuffer: {
                    const Range& r = b.nvn.index >= 0 && b.nvn.index < 16 ? g_state.storage[st][b.nvn.index] : Range{};
                    const uint8_t* p = r.address ? guest_bytes(r.address) : nullptr;
                    info = buffer_of(p, p ? std::min<uint64_t>(r.size, 16ull << 20) : 0);
                    break;
                }
                default: info = buffer_of(nullptr, 0x1000); break;  // c[0] driver constants: not filled yet
            }
            buffers.push_back(info);
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = sets[b.set];
            w.dstBinding = b.binding;
            w.descriptorCount = 1;
            w.descriptorType = VkDescriptorType(b.type);
            w.pBufferInfo = &buffers.back();
            writes.push_back(w);
        }
    }
    for (size_t k = 0; k < tex_bindings.size(); k++) {
        auto [s, b] = tex_bindings[k];
        uint64_t texture = 0;
        SamplerInfo si;
        texture_for(handles[k], texture, si);
        VkDescriptorImageInfo ii{};
        ii.imageView = texture ? sampled_view(cb, texture, b->image) : dummy_view(cb, b->image);
        ii.sampler = sampler_for(si, b->image.depth);
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        images.push_back(ii);
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets[2];
        w.dstBinding = b->binding;
        w.descriptorCount = 1;
        w.descriptorType = VkDescriptorType(b->type);
        w.pImageInfo = &images.back();
        writes.push_back(w);
    }
    if (!g_rendering) begin_rendering(cb, t);

    // Pipeline.
    PipelineKey key;
    std::memset(&key, 0, sizeof key);  // the key is compared byte for byte
    key.vs = vs, key.fs = fs;
    key.topology = mode;
    key.attrib_count = uint32_t(std::min<size_t>(g_state.attribs.size(), 16));
    std::copy_n(g_state.attribs.begin(), key.attrib_count, key.attribs);
    key.stream_count = uint32_t(std::min<size_t>(g_state.streams.size(), 16));
    std::copy_n(g_state.streams.begin(), key.stream_count, key.streams);
    std::copy_n(g_state.blend, 8, key.blend);
    key.color = g_state.color, key.mask = g_state.mask, key.depth = g_state.depth, key.polygon = g_state.polygon;
    key.color_count = t.color_count;
    for (uint32_t k = 0; k < t.color_count; k++) {
        GpuImage* img = t.colors[k] ? image_of(t.colors[k]) : nullptr;
        key.color_formats[k] = img ? img->format : VK_FORMAT_R8G8B8A8_UNORM;
    }
    GpuImage* dimg = t.depth ? image_of(t.depth) : nullptr;
    key.depth_format = dimg && dimg->aspect != VK_IMAGE_ASPECT_COLOR_BIT ? dimg->format : VK_FORMAT_UNDEFINED;
    VkPipeline pipeline = graphics_pipeline(key, layout);
    if (!pipeline) {
        g_stats.no_pipeline++;
        return false;
    }

    p_vkUpdateDescriptorSets(vk_device(), uint32_t(writes.size()), writes.data(), 0, nullptr);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.layout, 0, 4, sets, 0, nullptr);

    // Vertex buffers: the bound ranges of the streams in use, plus the zero stream (binding 15).
    uint32_t max_vertex = 0;
    for (uint32_t s = 0; s < 15; s++) {
        const Range& r = g_state.vertex_buffers[s];
        const uint8_t* p = r.address ? guest_bytes(r.address) : nullptr;
        Upload u = upload(p, p ? std::min<uint64_t>(r.size, 64ull << 20) : 0, 16);
        VkDeviceSize off = u.offset;
        p_vkCmdBindVertexBuffers(cb, s, 1, &u.buffer, &off);
    }
    {
        Upload z = upload_alloc(64, 16);
        std::memset(z.ptr, 0, 64);
        VkDeviceSize off = z.offset;
        p_vkCmdBindVertexBuffers(cb, 15, 1, &z.buffer, &off);
    }
    (void)max_vertex;

    VkViewport vp{0, 0, float(g_pass_extent.width), float(g_pass_extent.height), 0, 1};
    if (g_state.has_viewport)
        vp = {float(g_state.viewport[0]), float(g_state.viewport[1]), float(g_state.viewport[2]),
              float(g_state.viewport[3]), g_state.depth_near, g_state.depth_far};
    VkRect2D sc{{0, 0}, g_pass_extent};
    if (g_state.has_scissor)
        sc = {{std::max(0, g_state.scissor[0]), std::max(0, g_state.scissor[1])},
              {uint32_t(std::max(0, g_state.scissor[2])), uint32_t(std::max(0, g_state.scissor[3]))}};
    p_vkCmdSetViewport(cb, 0, 1, &vp);
    p_vkCmdSetScissor(cb, 0, 1, &sc);
    p_vkCmdSetBlendConstants(cb, g_state.blend_color);
    p_vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);

    if (indexed) {
        uint32_t count = uint32_t(c.i[0]);
        const uint8_t* p = guest_bytes(c.a[0]);
        if (!p) {
            g_stats.no_indices++;
            return false;
        }
        Upload u = upload(p, uint64_t(count) * index_size(c.mask), 16);
        p_vkCmdBindIndexBuffer(cb, u.buffer, u.offset, vk_index_type(c.mask));
        p_vkCmdDrawIndexed(cb, count, uint32_t(std::max(1, c.i[3])), 0, c.i[1], uint32_t(c.i[2]));
    } else {
        p_vkCmdDraw(cb, uint32_t(c.i[1]), uint32_t(std::max(1, c.i[3])), uint32_t(c.i[0]), uint32_t(c.i[2]));
    }
    g_stats.done++;
    static const bool trace = std::getenv("NX_TRACE_DRAWS") != nullptr;
    if (trace && g_stats.done <= 40) {
        std::string tex;
        for (size_t k = 0; k < tex_bindings.size(); k++) {
            uint64_t texture = 0;
            SamplerInfo si;
            texture_for(handles[k], texture, si);
            Texture* tx = texture ? get<Texture>(texture) : nullptr;
            char b[96];
            std::snprintf(b, sizeof b, " t%zu=%llx:%llx(%ux%u f%x%s)", k, (unsigned long long)handles[k],
                          (unsigned long long)texture, tx ? tx->width : 0, tx ? tx->height : 0, tx ? tx->format : 0,
                          tx && tx->image && tx->image->rendered ? " rt" : "");
            tex += b;
        }
        log("draw %llu: vs %s fs %s mode %u %s count %d targets %u:%llx depth %llx%s", (unsigned long long)g_stats.done,
            vs->hash.c_str(), fs ? fs->hash.c_str() : "-", mode, indexed ? "indexed" : "arrays",
            indexed ? c.i[0] : c.i[1], t.color_count, (unsigned long long)t.colors[0], (unsigned long long)t.depth, tex.c_str());
    }
    return true;
}

}  // namespace

void draw_begin_submit() { g_rendering = false; }

uint64_t recorded_draws();

void draw_log_stats() {
    Stats& t = g_stats;
    log("nvn draws: %llu recorded, %llu replayed:", (unsigned long long)recorded_draws(), (unsigned long long)t.total);
    log("nvn draws: %llu total, %llu done, %llu no program, %llu no vertex shader, %llu no pipeline, %llu no indices",
        (unsigned long long)t.total, (unsigned long long)t.done, (unsigned long long)t.no_program,
        (unsigned long long)t.no_vertex, (unsigned long long)t.no_pipeline, (unsigned long long)t.no_indices);
}

void draw_end_rendering(VkCommandBuffer cb) {
    if (!g_rendering) return;
    p_vkCmdEndRendering(cb);
    g_rendering = false;
}

void draw_command(VkCommandBuffer cb, const Cmd& c, const CmdList& list, const Targets& targets) {
    State& s = g_state;
    auto blob = [&](size_t size) -> const uint8_t* {
        return c.blob < list.blobs.size() && list.blobs[c.blob].size() >= size ? list.blobs[c.blob].data() : nullptr;
    };
    switch (c.op) {
        case Cmd::SetViewport:
            std::memcpy(s.viewport, c.i, sizeof s.viewport), s.has_viewport = true;
            break;
        case Cmd::SetScissor:
            std::memcpy(s.scissor, c.i, sizeof s.scissor), s.has_scissor = true;
            break;
        case Cmd::SetDepthRange:
            s.depth_near = c.f[0], s.depth_far = c.f[1];
            break;
        case Cmd::BindProgram: s.program = c.a[0]; break;
        case Cmd::BindUniformBuffer:
            if (c.i[0] >= 0 && c.i[0] < kStages && c.i[1] >= 0 && c.i[1] < 18) s.uniforms[c.i[0]][c.i[1]] = {c.a[0], c.a[1]};
            break;
        case Cmd::BindStorageBuffer:
            if (c.i[0] >= 0 && c.i[0] < kStages && c.i[1] >= 0 && c.i[1] < 16) s.storage[c.i[0]][c.i[1]] = {c.a[0], c.a[1]};
            break;
        case Cmd::BindTexture:
            if (c.i[0] >= 0 && c.i[0] < kStages && c.i[1] >= 0 && c.i[1] < 32) s.textures[c.i[0]][c.i[1]] = c.a[0];
            break;
        case Cmd::BindVertexBuffer:
            if (c.i[0] >= 0 && c.i[0] < 16) s.vertex_buffers[c.i[0]] = {c.a[0], c.a[1]};
            break;
        case Cmd::SetTexturePool: s.texture_pool = c.a[0]; break;
        case Cmd::SetSamplerPool: s.sampler_pool = c.a[0]; break;
        case Cmd::BindVertexAttribs:
            if (auto* p = blob(c.n * sizeof(VertexAttrib)))
                s.attribs.assign(reinterpret_cast<const VertexAttrib*>(p), reinterpret_cast<const VertexAttrib*>(p) + c.n);
            break;
        case Cmd::BindVertexStreams:
            if (auto* p = blob(c.n * sizeof(VertexStream)))
                s.streams.assign(reinterpret_cast<const VertexStream*>(p), reinterpret_cast<const VertexStream*>(p) + c.n);
            break;
        case Cmd::BindBlendState:
            if (auto* p = blob(sizeof(BlendTarget))) {
                BlendTarget b;
                std::memcpy(&b, p, sizeof b);
                if (b.target < 8) s.blend[b.target] = b;
            }
            break;
        case Cmd::BindColorState:
            if (auto* p = blob(sizeof(ColorStateInfo))) std::memcpy(&s.color, p, sizeof s.color);
            break;
        case Cmd::BindChannelMaskState:
            if (auto* p = blob(sizeof(ChannelMaskInfo))) std::memcpy(&s.mask, p, sizeof s.mask);
            break;
        case Cmd::BindDepthStencilState:
            if (auto* p = blob(sizeof(DepthStencilInfo))) std::memcpy(&s.depth, p, sizeof s.depth);
            break;
        case Cmd::BindPolygonState:
            if (auto* p = blob(sizeof(PolygonInfo))) std::memcpy(&s.polygon, p, sizeof s.polygon);
            break;
        case Cmd::SetBlendColor: std::memcpy(s.blend_color, c.f, sizeof s.blend_color); break;
        case Cmd::UpdateUniformBuffer: {  // (buffer address, size, offset, data): written in order, like the GPU
            uint64_t off = c.a[2];
            if (c.blob < list.blobs.size())
                if (uint8_t* p = const_cast<uint8_t*>(guest_bytes(c.a[0] + off)))
                    std::memcpy(p, list.blobs[c.blob].data(), list.blobs[c.blob].size());
            break;
        }
        case Cmd::ReportCounter: {  // {value, timestamp}: the work before it is done when the replay gets here
            static uint64_t reports = 0;
            if (uint8_t* p = const_cast<uint8_t*>(guest_bytes(c.a[0]))) {
                uint64_t v[2] = {++reports, plat::monotonic_ns()};
                std::memcpy(p, v, sizeof v);
            }
            break;
        }
        case Cmd::DrawArrays: draw(cb, targets, c.n, false, c); break;
        case Cmd::DrawElements: draw(cb, targets, c.n, true, c); break;
        default: break;
    }
}

}  // namespace nx::nvn
