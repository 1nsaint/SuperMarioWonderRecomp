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
    bool rasterizer_discard = false, render_enable = true;  // SetRasterizerDiscard / SetRenderEnable: draws skipped
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
        transition(cb, *img, VK_IMAGE_LAYOUT_GENERAL);  // rendered images stay GENERAL (sampled while rendered)
        img->rendered = true;
        uint32_t level = std::min<uint32_t>(t.views.level[k], img->levels - 1);
        colors[k].imageView = attachment_view(*img, level, t.views.layer[k]);
        colors[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        colors[k].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = {std::min(extent.width, std::max(1u, img->width >> level)),
                  std::min(extent.height, std::max(1u, img->height >> level))};
    }
    GpuImage* dimg = t.depth ? image_of(t.depth) : nullptr;
    if (dimg && dimg->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
        transition(cb, *dimg, VK_IMAGE_LAYOUT_GENERAL);
        dimg->rendered = true;
        uint32_t level = std::min<uint32_t>(t.views.level[8], dimg->levels - 1);
        depth.imageView = attachment_view(*dimg, level, t.views.layer[8]);
        depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = {std::min(extent.width, std::max(1u, dimg->width >> level)),
                  std::min(extent.height, std::max(1u, dimg->height >> level))};
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
VkDescriptorBufferInfo buffer_of(const void* bytes, uint64_t size, uint64_t min_size = 16) {
    uint64_t n = std::max<uint64_t>((size + 15) & ~15ull, min_size);
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
void texture_for(uint64_t handle, uint64_t& texture, SamplerInfo& sampler, ViewInfo* view = nullptr) {
    uint32_t sid = 0;
    uint32_t tid = handle_texture(handle, &sid);
    texture = 0;
    if (view) *view = {};
    if (auto* tp = get<DescriptorPool>(g_state.texture_pool)) {
        auto it = tp->textures.find(tid);
        if (it != tp->textures.end()) texture = it->second;
        auto v = tp->views.find(tid);
        if (view && v != tp->views.end()) *view = v->second;
    }
    if (auto* sp = get<DescriptorPool>(g_state.sampler_pool)) {
        auto it = sp->samplers.find(sid);
        if (it != sp->samplers.end()) sampler = it->second;
    }
}

struct Stats {
    uint64_t total = 0, no_program = 0, no_vertex = 0, no_pipeline = 0, no_indices = 0, done = 0, to_window = 0;
} g_stats;

uint64_t g_dump_draws = 0, g_dump_request = 0;

bool draw(VkCommandBuffer cb, const Targets& t, uint32_t mode, bool indexed, const Cmd& c) {
    g_stats.total++;
    if (g_state.rasterizer_discard || !g_state.render_enable) return false;  // nothing reaches the targets
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
    // NX_SKIP_FS=<hash>,<hash>: draws with those fragment shaders are dropped (to measure what they cost).
    static const std::string skip = std::getenv("NX_SKIP_FS") ? std::getenv("NX_SKIP_FS") : "";
    if (!skip.empty() && fs && skip.find(fs->hash) != std::string::npos) return false;
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
                    // Reads past the bound size return 0 on Maxwell and garbage on Metal (a UI shader read c3[10]
                    // of a 0x90-byte block and wrote Inf): the bound data goes in a zero-padded 4 KB at least.
                    info = buffer_of(p, p ? std::min<uint64_t>(r.size, 0x10000) : 0, 0x1000);
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
        ViewInfo view;
        texture_for(handles[k], texture, si, &view);
        VkDescriptorImageInfo ii{};
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii.imageView = texture ? sampled_view(cb, texture, b->image, &view, &ii.imageLayout) : dummy_view(cb, b->image);
        ii.sampler = sampler_for(si, b->image.depth);
        images.push_back(ii);
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = sets[2];
        w.dstBinding = b->binding;
        w.descriptorCount = 1;
        w.descriptorType = VkDescriptorType(b->type);
        w.pImageInfo = &images.back();
        writes.push_back(w);
    }
    if (!g_rendering) {
        if (gpu_profile_on()) {
            GpuImage* img = t.color_count && t.colors[0] ? image_of(t.colors[0]) : nullptr;
            char label[128];
            std::snprintf(label, sizeof label, "%ux%u L%u fs %s", img ? img->width : 0, img ? img->height : 0,
                          t.views.level[0], fs ? fs->hash.c_str() : "-");
            gpu_profile_mark(cb, label);
        }
        begin_rendering(cb, t);
    }

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
        // 8-bit indices: Metal has none, and MoltenVK converts them on the GPU at every draw (that was most of the
        // frame's GPU time), so they are widened here.
        Upload u;
        VkIndexType type = vk_index_type(c.mask);
        if (type == VK_INDEX_TYPE_UINT8_EXT) {
            u = upload_alloc(uint64_t(count) * 2, 16);
            auto* d = reinterpret_cast<uint16_t*>(u.ptr);
            for (uint32_t k = 0; k < uint32_t(count); k++) d[k] = p[k];
            type = VK_INDEX_TYPE_UINT16;
        } else {
            u = upload(p, uint64_t(count) * index_size(c.mask), 16);
        }
        p_vkCmdBindIndexBuffer(cb, u.buffer, u.offset, type);
        p_vkCmdDrawIndexed(cb, count, uint32_t(std::max(1, c.i[3])), 0, c.i[1], uint32_t(c.i[2]));
    } else {
        p_vkCmdDraw(cb, uint32_t(c.i[1]), uint32_t(std::max(1, c.i[3])), uint32_t(c.i[0]), uint32_t(c.i[2]));
    }
    g_stats.done++;
    // NX_DUMP_ON_TARGET=<hex NVNtexture*>: every NX_DUMP_FRAMES-th draw into it asks for NX_DUMP_TEXTURES to be read
    // back right after this submit (before later frames change them).
    static const char* dump_env = std::getenv("NX_DUMP_ON_TARGET");
    static const uint64_t dump_target = dump_env ? std::strtoull(dump_env, nullptr, 16) : 0;
    if (dump_target && t.colors[0] == dump_target) g_dump_request = ++g_dump_draws;
    // NX_TRACE_DRAWS=<first draw>: logs 80 draws from there.
    static const char* trace_env = std::getenv("NX_TRACE_DRAWS");
    static const uint64_t trace_from = trace_env ? std::strtoull(trace_env, nullptr, 10) : 0;
    // NX_TRACE_TARGET=<hex NVNtexture*>: only draws into that color target.
    static const char* target_env = std::getenv("NX_TRACE_TARGET");
    static const uint64_t trace_target = target_env ? std::strtoull(target_env, nullptr, 16) : 0;
    static uint64_t traced = 0;
    // NX_TRACE_FS_HASH=<hash>: only draws with that fragment shader.
    static const char* fs_env = std::getenv("NX_TRACE_FS_HASH");
    if (trace_env && g_stats.done >= trace_from && traced < 80 && (!trace_target || t.colors[0] == trace_target) &&
        (!fs_env || (fs && fs->hash == fs_env))) {
        traced++;
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
        if (const Range& u = g_state.uniforms[1][0]; u.address)
            if (auto* f = reinterpret_cast<const float*>(guest_bytes(u.address)))
                for (uint64_t k = 0; k < std::min<uint64_t>(u.size / 16, 8); k++)
                    log("  fs c3[%llu] @%llx: %g %g %g %g", (unsigned long long)k, (unsigned long long)u.address,
                        f[4 * k], f[4 * k + 1], f[4 * k + 2], f[4 * k + 3]);
        for (int k = 0; k < 2; k++)
            if (const Range& r = g_state.storage[0][k]; r.address) {
                auto* f = reinterpret_cast<const float*>(guest_bytes(r.address));
                log("  vs s%d @%llx+%llx: %g %g %g %g %g %g %g %g", k, (unsigned long long)r.address,
                    (unsigned long long)r.size, f ? f[0] : -1, f ? f[1] : -1, f ? f[2] : -1, f ? f[3] : -1,
                    f ? f[4] : -1, f ? f[5] : -1, f ? f[6] : -1, f ? f[7] : -1);
            }
        if (fs) {
            const auto& k = fs->constants;
            const float* f = reinterpret_cast<const float*>(k.data());
            log("  fs constants %zu bytes: %g %g %g %g", k.size(), k.size() >= 4 ? f[0] : -1, k.size() >= 8 ? f[1] : -1,
                k.size() >= 12 ? f[2] : -1, k.size() >= 16 ? f[3] : -1);
        }
        const BlendTarget& b0 = g_state.blend[0];
        log("  blend %u (%u %u %u %u eq %u %u) mask %x logic %x alpha %x fmt %d", g_state.color.blend_enable[0], b0.src_rgb,
            b0.dst_rgb, b0.src_a, b0.dst_a, b0.eq_rgb, b0.eq_a, g_state.mask.mask[0], g_state.color.logic_op,
            g_state.color.alpha_test, t.colors[0] ? int(image_of(t.colors[0])->format) : -1);
        log("  viewport %d %d %d %d (%d) scissor %d %d %d %d (%d) cull %u front %u depth %g..%g test %u", g_state.viewport[0],
            g_state.viewport[1], g_state.viewport[2], g_state.viewport[3], g_state.has_viewport, g_state.scissor[0],
            g_state.scissor[1], g_state.scissor[2], g_state.scissor[3], g_state.has_scissor, g_state.polygon.cull,
            g_state.polygon.front, g_state.depth_near, g_state.depth_far, g_state.depth.depth_test);
        log("draw %llu: vs %s fs %s mode %u %s count %d x%d targets %u:%llx depth %llx%s", (unsigned long long)g_stats.done,
            vs->hash.c_str(), fs ? fs->hash.c_str() : "-", mode, indexed ? "indexed" : "arrays",
            indexed ? c.i[0] : c.i[1], c.i[3], t.color_count, (unsigned long long)t.colors[0], (unsigned long long)t.depth, tex.c_str());
    }
    return true;
}

}  // namespace

void draw_begin_submit() {
    g_rendering = false;
    pipeline_begin_submit();
}

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
    gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
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
        case Cmd::SetRasterizerDiscard: s.rasterizer_discard = c.n != 0; break;
        case Cmd::SetRenderEnable: s.render_enable = c.n != 0; break;
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

uint64_t draw_take_dump_request() {
    uint64_t r = g_dump_request;
    g_dump_request = 0;
    return r;
}

}  // namespace nx::nvn
