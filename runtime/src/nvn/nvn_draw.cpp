// Draws (docs/NVN.md, N6): replays the bind, state and draw commands of nvn_state.cpp into Vulkan. Everything a draw
// reads from guest memory (vertices, indices, uniform and storage buffers, textures) is copied at replay time into
// the submit's upload buffer: the replay runs right when the game submits and is waited for, so the memory holds
// what the GPU would have read. Render targets become a dynamic-rendering pass, begun at the first draw and ended
// before any transfer.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <set>

#include "nvn_draw_state.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

// The replay state shared with nvn_draw_trace.cpp (nvn_draw_state.h).
namespace replay {

State g_state;
Stats g_stats;

const uint8_t* guest_bytes(uint64_t gpu) {
    uint64_t off = 0;
    MemoryPool* p = pool_at_gpu(gpu, &off);
    uint64_t host = p ? pool_host(*p, off) : 0;
    return host ? reinterpret_cast<const uint8_t*>(host) : nullptr;
}

uint32_t handle_texture(uint64_t handle, uint32_t* sampler) {
    if (sampler) *sampler = uint32_t(handle >> 20) & 0xFFF;
    return uint32_t(handle & 0xFFFFF);
}

// The texture and sampler a handle names in the bound pools.
void texture_for(uint64_t handle, uint64_t& texture, SamplerInfo& sampler, ViewInfo* view) {
    uint32_t sid = 0;
    uint32_t tid = handle_texture(handle, &sid);
    texture = 0;
    if (view) *view = {};
    if (auto* tp = get<DescriptorPool>(g_state.texture_pool)) {
        auto it = tp->textures.find(tid);
        if (it != tp->textures.end()) texture = it->second;
        // The slot's texture gone or re-initialized as another kind: the copy made at registration (nvn_objects.cpp).
        // NX_SNAPSHOT_SHAPE=1 also uses the copy when only the memory or size changed (the pool's descriptor copy):
        // it broke the course's foreground flowers (dynamic-resolution targets re-initialized every frame are meant
        // to be followed) and did not light the Goombas (docs/nvn/DEBUG.md #45).
        static const bool kind_only = std::getenv("NX_SNAPSHOT_SHAPE") == nullptr;
        if (auto s = tp->snapshots.find(tid); texture && s != tp->snapshots.end()) {
            Texture* now = get<Texture>(texture);
            Texture* then = get<Texture>(s->second);
            bool moved = now && then && !kind_only &&
                         (now->memory != then->memory || now->width != then->width || now->height != then->height ||
                          now->depth != then->depth || now->levels != then->levels);
            if (then && (!now || moved || now->target != then->target || now->format != then->format)) texture = s->second;
        }
        auto v = tp->views.find(tid);
        if (view && v != tp->views.end()) *view = v->second;
    }
    if (auto* sp = get<DescriptorPool>(g_state.sampler_pool)) {
        auto it = sp->samplers.find(sid);
        if (it != sp->samplers.end()) sampler = it->second;
    }
}

}  // namespace replay

using namespace replay;

namespace {

bool g_rendering = false;
bool g_sampled_pending = false;  // gather_textures: the draw samples an image with a pending clear
Targets g_pass;           // what the open pass renders to
GpuImage* g_pass_images[9];  // their images (8: depth), resolved once per pass for gather_textures
uint32_t g_pass_written = 0;
uint64_t g_pass_ends[8];  // passes ended, by draw_end_rendering reason
uint64_t g_submit_ns = 0;  // when the submit being replayed started
// gather_textures' resolution of each texture binding, used by build_sets for the same draw: pool lookups and object
// lookups (each under the object lock) were done twice or three times per binding (a large share of the replay).
struct ResolvedTexture {
    uint64_t texture = 0;
    SamplerInfo sampler;
    ViewInfo view;
};
std::vector<ResolvedTexture> g_resolved;  // targets the open pass's draws wrote: bit c colour c, bit 8 depth
VkExtent2D g_pass_extent{};

int api_stage(ShaderStage s) { return s == ShaderStage::Fragment ? 1 : s == ShaderStage::Compute ? 5 : 0; }

void begin_rendering(VkCommandBuffer cb, const Targets& t) {
    if (clears_pending_other(t)) clears_flush(cb);  // other targets' clears first, in order
    // An image never written (layout UNDEFINED) has nothing to load: loadOp CLEAR to zero, not a LOAD of whatever
    // the memory held (G3, docs/nvn/DEBUG.md #50); a pending clear still wins (clears_apply). Env: bisecting.
    static const bool load_undefined = std::getenv("NX_UNDEFINED_LOAD") != nullptr;
    uint32_t fresh = 0;  // bit c colour c, bit 8 depth: begun cleared to 0 (depth 0, stencil 0)
    VkRenderingAttachmentInfo colors[8]{};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkExtent2D extent{UINT32_MAX, UINT32_MAX};
    for (uint32_t k = 0; k < t.color_count; k++) {
        colors[k].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        GpuImage* img = t.colors[k] ? image_of(t.colors[k]) : nullptr;
        g_pass_images[k] = img;
        if (!img) continue;
        if (img->layout == VK_IMAGE_LAYOUT_UNDEFINED && !load_undefined) fresh |= 1u << k;
        transition(cb, *img, VK_IMAGE_LAYOUT_GENERAL);  // rendered images stay GENERAL (sampled while rendered)
        img->rendered = true;
        uint32_t level = std::min<uint32_t>(t.views.level[k], img->levels - 1);
        colors[k].imageView = attachment_view(*img, level, t.views.layer[k], view_format(*img, t.views.format[k]));
        // NX_TRACE_CUBES=<first frame>: writes into cube images from that frame on. With 400 from the start, the log
        // ran out at the first 3D scene (frame ~171) and never reached a course load.
        static const char* trace_cubes = std::getenv("NX_TRACE_CUBES");
        static const uint64_t cube_from = trace_cubes ? std::strtoull(trace_cubes, nullptr, 10) : 0;
        static int cube_logs = 0;
        if (trace_cubes && (img->view_type == VK_IMAGE_VIEW_TYPE_CUBE || img->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
            g_frame >= cube_from && cube_logs++ < 2000)
            log("cubes: render into %llx level %u layer %u (%ux%u, %u levels, %u layers) view %s",
                (unsigned long long)t.colors[k], level, t.views.layer[k], img->width, img->height, img->levels,
                img->layers, colors[k].imageView == img->view ? "WHOLE IMAGE (fallback)" : "ok");
        colors[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        colors[k].loadOp = fresh >> k & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = {std::min(extent.width, std::max(1u, img->width >> level)),
                  std::min(extent.height, std::max(1u, img->height >> level))};
    }
    GpuImage* dimg = t.depth ? image_of(t.depth) : nullptr;
    g_pass_images[8] = dimg;
    if (dimg && dimg->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
        if (dimg->layout == VK_IMAGE_LAYOUT_UNDEFINED && !load_undefined) fresh |= 1u << 8;
        transition(cb, *dimg, VK_IMAGE_LAYOUT_GENERAL);
        dimg->rendered = true;
        uint32_t level = std::min<uint32_t>(t.views.level[8], dimg->levels - 1);
        depth.imageView = attachment_view(*dimg, level, t.views.layer[8]);
        depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth.loadOp = fresh >> 8 & 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.clearValue.depthStencil = {1.0f, 0};  // far (a fresh depth buffer; the course passes test LESS)
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        extent = {std::min(extent.width, std::max(1u, dimg->width >> level)),
                  std::min(extent.height, std::max(1u, dimg->height >> level))};
    } else {
        dimg = nullptr;
    }
    if (extent.width == UINT32_MAX) extent = {1, 1};
    VkRenderingAttachmentInfo stencil = depth;  // same view, its own loadOp
    uint32_t cleared = clears_apply(t, colors, depth, stencil);  // pending clears of these targets: loadOp CLEAR
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = t.color_count;
    ri.pColorAttachments = colors;
    if (dimg) {
        if (dimg->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &depth;
        if (dimg->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &stencil;
    }
    p_vkCmdBeginRendering(cb, &ri);
    g_rendering = true;
    g_pass_written = cleared | fresh;  // as a clear inside the pass: sampling it needs it stored first
    g_pass = t;
    g_pass_extent = extent;
}

// The buffer a descriptor points to: `bytes` copied into the upload buffer (at least 16 bytes, zero padded).
// Storage buffers written by compute shaders go back to guest memory after the submit (the GPU's writes are what the
// game and later work read; WriteBack in nvn_vk.h).
std::vector<WriteBack> g_write_backs;
bool all_write_backs() {  // NX_WRITE_BACK_ALL=1: every compute storage binding, written or not (bisecting)
    static const bool on = std::getenv("NX_WRITE_BACK_ALL") != nullptr;
    return on;
}

// guest: the bytes are guest memory (a copy is shared by later draws of the submit); host scratch data (the support
// buffer, c[0]: on the stack, at the same address for every draw) is copied every time (docs/nvn/DEBUG.md #30).
VkDescriptorBufferInfo buffer_of(const void* bytes, uint64_t size, uint64_t min_size = 16, bool write_back = false,
                                 bool guest = true) {
    uint64_t n = std::max<uint64_t>((size + 15) & ~15ull, min_size);
    if (!write_back && guest) {
        Upload u = dedup_upload(static_cast<const uint8_t*>(bytes), size, n, 256);
        return {u.buffer, u.offset, n};
    }
    Upload u = upload_alloc(n, 256);
    std::memset(u.ptr, 0, n);
    if (bytes && size) std::memcpy(u.ptr, bytes, size);
    if (bytes && size) g_write_backs.push_back({u.ptr, const_cast<uint8_t*>(static_cast<const uint8_t*>(bytes)), size});
    return {u.buffer, u.offset, n};
}

struct Resolved {
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> buffers;
    std::vector<VkDescriptorImageInfo> images;
    std::vector<uint64_t> textures;  // guest textures sampled (for the render-pass check)
};



uint64_t g_dump_draws = 0, g_dump_request = 0;

// Descriptor sets for a program's stages from the bound state: buffers (sets 0/1), textures (set 2, the bindings
// `tex_bindings` with their resolved `handles`), storage images (set 3). Records uploads and layout changes, so it
// runs outside render passes when textures or images need them.
struct Descriptors {
    VkDescriptorSet sets[4] = {};
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> buffers;
    std::vector<VkDescriptorImageInfo> images;
};

void build_sets(VkCommandBuffer cb, const std::vector<const ShaderInfo*>& shaders, const ProgramLayout& layout,
                const std::vector<std::pair<const ShaderInfo*, const DescriptorBinding*>>& tex_bindings,
                const std::vector<uint64_t>& handles, Descriptors& d) {
    for (int k = 0; k < 4; k++) d.sets[k] = descriptor_set(layout.sets[k]);
    auto& writes = d.writes;
    auto& buffers = d.buffers;
    auto& images = d.images;
    buffers.reserve(1024), images.reserve(1024), writes.reserve(2048);  // pointers into them are kept
    VkDescriptorSet* sets = d.sets;
    SupportBuffer sb = support_buffer_defaults();
    float vw = g_state.has_viewport ? float(g_state.viewport[2]) : 1280.0f;
    float vh = g_state.has_viewport ? float(g_state.viewport[3]) : 720.0f;
    support_buffer_set_viewport(sb, vw, vh);
    for (const ShaderInfo* s : shaders) {
        int st = api_stage(s->stage);
        for (const DescriptorBinding& b : s->refl.bindings) {
            if (b.set >= 2) continue;
            VkDescriptorBufferInfo info{};
            switch (b.nvn.source) {
                case NvnSource::SupportBuffer: info = buffer_of(&sb, sizeof sb, 16, false, false); break;
                case NvnSource::ConstantData: info = buffer_of(s->constants.data(), s->constants.size()); break;
                case NvnSource::UniformBlock:
                case NvnSource::UniformAsStorage: {
                    const Range& r = b.nvn.index >= 0 && b.nvn.index < 18 ? g_state.uniforms[st][b.nvn.index] : Range{};
                    const uint8_t* p = r.address ? guest_bytes(r.address) : nullptr;
                    // Reads past the bound size return 0 on Maxwell and garbage on Metal (a UI shader read c3[10]
                    // of a 0x90-byte block and wrote Inf): the bound data goes in a zero-padded 4 KB at least.
                    // Banks the shader indexes with a computed value (clamped to 64 KB, nvn_spirv_patch.cpp): all
                    // 64 KB, so every read past the data returns 0 as on Maxwell.
                    bool indexed = b.set == 0 && b.binding < 64 && (dynamic_uniform_bindings(s) >> b.binding & 1);
                    info = buffer_of(p, p ? std::min<uint64_t>(r.size, 0x10000) : 0, indexed ? 0x10000 : 0x1000);
                    break;
                }
                case NvnSource::StorageBuffer: {
                    const Range& r = b.nvn.index >= 0 && b.nvn.index < 16 ? g_state.storage[st][b.nvn.index] : Range{};
                    const uint8_t* p = r.address ? guest_bytes(r.address) : nullptr;
                    // At least 64 KB: Metal does not bound-check, and a shader writing past a small or missing
                    // buffer overwrote the next data in the upload buffer (garbage everywhere after a compute).
                    info = buffer_of(p, p ? std::min<uint64_t>(r.size, 16ull << 20) : 0, 0x10000,
                                     s->stage == ShaderStage::Compute &&
                                         (all_write_backs() || (b.binding < 64 && written_storage_bindings(s) >> b.binding & 1)));
                    break;
                }
                case NvnSource::DriverConstants: {
                    auto c0 = driver_constants(s->stage, st);
                    info = buffer_of(c0.data(), c0.size(), 16, false, false);
                    break;
                }
                default: info = buffer_of(nullptr, 0x1000, 16, false, false); break;
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
        uint64_t texture = k < g_resolved.size() ? g_resolved[k].texture : 0;
        SamplerInfo si = k < g_resolved.size() ? g_resolved[k].sampler : SamplerInfo{};
        ViewInfo view = k < g_resolved.size() ? g_resolved[k].view : ViewInfo{};
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
    for (const ShaderInfo* s : shaders) {
        int st = api_stage(s->stage);
        for (const DescriptorBinding& b : s->refl.bindings) {
            if (b.set != 3) continue;
            uint64_t handle = b.nvn.source == NvnSource::ImageBinding && b.nvn.index >= 0 && b.nvn.index < 16
                                  ? g_state.images[st][b.nvn.index]
                                  : 0;
            uint64_t texture = 0;
            SamplerInfo si;
            ViewInfo view;
            if (handle) texture_for(handle & 0xFFFFF, texture, si, &view);
            VkDescriptorImageInfo ii{};
            ii.imageView = storage_view(cb, texture, b.image, texture ? &view : nullptr);
            ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            images.push_back(ii);
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = sets[3];
            w.dstBinding = b.binding;
            w.descriptorCount = 1;
            w.descriptorType = VkDescriptorType(b.type);
            w.pImageInfo = &images.back();
            writes.push_back(w);
        }
    }
}

// Texture bindings of a program's stages and their handles; true when one needs a transfer or a layout change (so
// no render pass may be open while the sets are built).
bool gather_textures(const std::vector<const ShaderInfo*>& shaders,
                     std::vector<std::pair<const ShaderInfo*, const DescriptorBinding*>>& tex_bindings,
                     std::vector<uint64_t>& handles) {
    bool need_break = false;
    for (const ShaderInfo* s : shaders)
        for (const DescriptorBinding& b : s->refl.bindings) {
            if (b.set == 2) tex_bindings.push_back({s, &b});
            if (b.set == 3) need_break = true;  // storage images change layout
        }
    handles.assign(tex_bindings.size(), 0);
    g_resolved.assign(tex_bindings.size(), ResolvedTexture{});
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
        ViewInfo view;
        texture_for(handle, texture, si, &view);
        // A pool entry naming a texture that no longer exists samples the dummy, like no texture: it ended the
        // render pass at every draw (~1.4M times in a run, docs/nvn/DEBUG.md #34).
        if (Texture* sampled = texture ? get<Texture>(texture) : nullptr)
            sampled->last_sampled = g_submit_ns;
        else
            texture = 0;
        g_resolved[k] = {texture, si, view};
        if (texture ? !sampled_ready(texture) : !dummy_ready(b->image)) need_break = true;
        // Sampling one of the open pass's targets (a depth buffer read back by the character lighting) needs what
        // was rendered stored first: a fresh pass. SetRenderTargets used to end every pass, which did this by chance
        // (docs/nvn/DEBUG.md #35).
        // Only when a draw of this pass wrote it (g_pass_written: bit c colour target c, bit 8 depth).
        if (texture && g_rendering) {
            GpuImage* img = image_of(texture);
            uint32_t hit = img && g_pass.depth && g_pass_images[8] == img ? 1u << 8 : 0;
            for (uint32_t c = 0; c < g_pass.color_count && img; c++)
                if (g_pass.colors[c] && g_pass_images[c] == img) hit |= 1u << c;
            if (hit & g_pass_written) need_break = true;
        }
        if (texture && clears_pending_image(image_of(texture))) g_sampled_pending = need_break = true;
    }
    return need_break;
}

// DispatchCompute: the bound program's compute stage over (x, y, z) groups, outside render passes, then a barrier
// so what it wrote is visible to what follows.
void dispatch(VkCommandBuffer cb, const Cmd& c) {
    static const bool off = std::getenv("NX_NO_COMPUTE") != nullptr;  // debug: skip compute dispatches
    if (off) return;
    const auto* shaders = program_shaders(g_state.program);
    const ShaderInfo* cs = nullptr;
    if (shaders)
        for (const ShaderInfo* s : *shaders)
            if (s->stage == ShaderStage::Compute) cs = s;
    if (!cs) return;
    uint32_t x = uint32_t(c.i[0]), y = uint32_t(c.i[1]), z = uint32_t(c.i[2]);
    if (c.n == 1) {  // indirect
        const uint8_t* p = guest_bytes(c.a[0]);
        if (!p) return;
        std::memcpy(&x, p, 4), std::memcpy(&y, p + 4, 4), std::memcpy(&z, p + 8, 4);
    }
    if (!x || !y || !z) return;
    trace_compute_once(cs, x, y, z);
    std::vector<const ShaderInfo*> stages{cs};
    const ProgramLayout& layout = program_layout(stages);
    VkPipeline pipeline = compute_pipeline(cs, layout);
    if (!pipeline) return;
    draw_end_rendering(cb, 2);
    std::vector<std::pair<const ShaderInfo*, const DescriptorBinding*>> tex_bindings;
    std::vector<uint64_t> handles;
    gather_textures(stages, tex_bindings, handles);
    Descriptors d;
    build_sets(cb, stages, layout, tex_bindings, handles, d);
    p_vkUpdateDescriptorSets(vk_device(), uint32_t(d.writes.size()), d.writes.data(), 0, nullptr);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout.layout, 0, 4, d.sets, 0, nullptr);
    if (gpu_profile_on()) gpu_profile_mark(cb, "compute " + cs->hash);
    p_vkCmdDispatch(cb, x, y, z);
    if (gpu_profile_on()) gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                           nullptr, 0, nullptr);
    g_stats.dispatches++;
}

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
    std::vector<const ShaderInfo*> swapped;  // NX_SWAP_FS / NX_SKIP_FS (nvn_draw_trace.cpp)
    if (!debug_shader_overrides(shaders, fs, swapped)) return false;
    const ProgramLayout& layout = program_layout(*shaders);

    // NX_SNAP_FS_BEFORE=<hash>[@<first draw>]: snapshot colour target 0 before draws with that fragment shader
    // (what a feedback pass like a tonemap reads), 8 at most.
    {
        static const std::string before = std::getenv("NX_SNAP_FS_BEFORE") ? std::getenv("NX_SNAP_FS_BEFORE") : "";
        static const uint64_t from = before.find('@') != std::string::npos
                                         ? std::strtoull(before.c_str() + before.find('@') + 1, nullptr, 10) : 0;
        static int taken = 0;
        bool match = (fs && before.compare(0, 16, fs->hash) == 0) || before.compare(0, 16, vs->hash) == 0;
        if (!before.empty() && match && g_stats.total >= from && taken < 8 &&
            t.color_count && t.colors[0])
            if (GpuImage* img = image_of(t.colors[0])) {
                taken++;
                draw_end_rendering(cb, 6);
                debug_snapshot(cb, *img, 0xB0000000ull + g_stats.total);
            }
    }
    // Textures first: an upload or a layout change cannot happen inside the render pass.
    std::vector<std::pair<const ShaderInfo*, const DescriptorBinding*>> tex_bindings;
    std::vector<uint64_t> handles;
    g_sampled_pending = false;
    bool need_break = gather_textures(*shaders, tex_bindings, handles);
    if (g_sampled_pending) clears_flush(cb);  // cleared (and stored) before it is sampled
    static const bool bytes = std::getenv("NX_TARGETS_MEMCMP") != nullptr;  // bisecting
    bool targets_changed = !g_rendering || (bytes ? std::memcmp(&g_pass, &t, sizeof t) != 0 : !targets_same(g_pass, t));
    if (g_rendering && (need_break || targets_changed)) {
        (targets_changed ? g_stats.breaks_targets : g_stats.breaks_textures)++;
        draw_end_rendering(cb, 1);
    }

    Descriptors d;
    build_sets(cb, *shaders, layout, tex_bindings, handles, d);
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
    key.color_count = t.color_count, key.depth_clamp = g_state.depth_clamp;
    for (uint32_t k = 0; k < t.color_count; k++) {
        GpuImage* img = t.colors[k] ? image_of(t.colors[k]) : nullptr;
        key.color_formats[k] = img ? view_format(*img, t.views.format[k]) : VK_FORMAT_R8G8B8A8_UNORM;
        if (img && (img->nvn_format == 0x3f || t.views.format[k] == 0x3f)) key.ufloat_colors |= 1u << k;  // G4
    }
    GpuImage* dimg = t.depth ? image_of(t.depth) : nullptr;
    key.depth_format = dimg && dimg->aspect != VK_IMAGE_ASPECT_COLOR_BIT ? dimg->format : VK_FORMAT_UNDEFINED;
    VkPipeline pipeline = graphics_pipeline(key, layout);
    if (!pipeline) {
        g_stats.no_pipeline++;
        return false;
    }

    p_vkUpdateDescriptorSets(vk_device(), uint32_t(d.writes.size()), d.writes.data(), 0, nullptr);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.layout, 0, 4, d.sets, 0, nullptr);

    // Vertex buffers: the bound ranges of the streams the vertex shader reads (the pipeline declares no others),
    // plus the zero stream (binding 15).
    uint32_t max_vertex = 0, used = 0;
    for (const ShaderIo& in : vs->refl.inputs)
        if (in.location < g_state.attribs.size() && g_state.attribs[in.location].stream < 15)
            used |= 1u << g_state.attribs[in.location].stream;
    for (uint32_t s = 0; s < 15; s++) {
        if (!(used >> s & 1)) continue;
        const Range& r = g_state.vertex_buffers[s];
        const uint8_t* p = r.address ? guest_bytes(r.address) : nullptr;
        uint64_t size = p ? std::min<uint64_t>(r.size, 64ull << 20) : 0;
        Upload u = dedup_upload(p, size, std::max<uint64_t>((size + 15) & ~15ull, 16), 16);
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
    // NVN's upper-left window origin (what Wonder sets) maps NDC +Y to the top of the render target, like D3D;
    // Vulkan maps it to the bottom. A negative-height viewport gives NVN's mapping, and with it NVN's front faces.
    // (Before this, the front face was inverted instead: triangles were culled right, but every render target was
    // upside down, which the post chain undid in some scenes and not in others: the first course showed a
    // mirrored sky.)
    vp.y += vp.height, vp.height = -vp.height;
    VkRect2D sc{{0, 0}, g_pass_extent};
    if (g_state.has_scissor)
        sc = {{std::max(0, g_state.scissor[0]), std::max(0, g_state.scissor[1])},
              {uint32_t(std::max(0, g_state.scissor[2])), uint32_t(std::max(0, g_state.scissor[3]))}};
    p_vkCmdSetViewport(cb, 0, 1, &vp);
    p_vkCmdSetScissor(cb, 0, 1, &sc);
    p_vkCmdSetBlendConstants(cb, g_state.blend_color);
    raster_dynamic(cb);  // stencil reference and masks, depth bias (nvn_raster.cpp)

    trace_skin(indexed, c, vs, fs);
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
        bool q = zero_draws_on() && zero_draws_begin(cb, vs->hash + " " + (fs ? fs->hash : std::string("-")));
        p_vkCmdDrawIndexed(cb, count, uint32_t(std::max(1, c.i[3])), 0, c.i[1], uint32_t(c.i[2]));
        if (q) zero_draws_end(cb);
    } else {
        bool q = zero_draws_on() && zero_draws_begin(cb, vs->hash + " " + (fs ? fs->hash : std::string("-")));
        p_vkCmdDraw(cb, uint32_t(c.i[1]), uint32_t(std::max(1, c.i[3])), uint32_t(c.i[0]), uint32_t(c.i[2]));
        if (q) zero_draws_end(cb);
    }
    g_stats.done++;
    g_pass_written |= ((1u << t.color_count) - 1) | (g_state.depth.depth_write || g_state.depth.stencil_test ? 1u << 8 : 0);
    // NX_SNAP_FS=<hash>,<hash>[@<first draw>]: also snapshot after draws with those fragment shaders (8 at most).
    static const std::string snap_fs = std::getenv("NX_SNAP_FS") ? std::getenv("NX_SNAP_FS") : "";
    static const uint64_t snap_from = snap_fs.find('@') != std::string::npos
                                          ? std::strtoull(snap_fs.c_str() + snap_fs.find('@') + 1, nullptr, 10) : 0;
    static int snap_count = 0;
    bool snap_by_fs = !snap_fs.empty() && g_stats.done >= snap_from && snap_count < 8 &&
                      ((fs && snap_fs.find(fs->hash) != std::string::npos) ||
                       snap_fs.find(vs->hash) != std::string::npos) &&
                      ++snap_count;  // the vertex shader's hash matches too
    if ((snap_by_fs || snap_follow(t, vs, fs, handles) || debug_snap_wanted(g_stats.done)) && t.color_count && t.colors[0])
        if (GpuImage* snap = image_of(t.colors[0])) draw_end_rendering(cb, 6), debug_snapshot(cb, *snap, g_stats.done);
    // NX_DUMP_ON_TARGET=<hex NVNtexture*>: every NX_DUMP_FRAMES-th draw into it asks for NX_DUMP_TEXTURES to be read
    // back right after this submit (before later frames change them).
    static const char* dump_env = std::getenv("NX_DUMP_ON_TARGET");
    static const uint64_t dump_target = dump_env ? std::strtoull(dump_env, nullptr, 16) : 0;
    if (dump_target && t.colors[0] == dump_target) g_dump_request = ++g_dump_draws;
    // NX_DUMP_ON_FS=<fs hash>[,...]: a draw with one of these fragment shaders dumps its target 0 and what it samples.
    static const std::string dump_fs = [] { const char* e = std::getenv("NX_DUMP_ON_FS"); return std::string(e ? e : ""); }();
    static const uint64_t dump_from = [] { const char* e = std::getenv("NX_DUMP_FROM"); return e ? std::strtoull(e, nullptr, 10) : 0; }();
    if (!dump_fs.empty() && fs && g_frame >= dump_from && dump_fs.find(fs->hash) != std::string::npos && g_dump_draws < 400 &&
        t.colors[0] && image_of(t.colors[0]) && (std::getenv("NX_DUMP_ANY") || image_of(t.colors[0])->view_type == VK_IMAGE_VIEW_TYPE_CUBE ||
                                                           image_of(t.colors[0])->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) {
        debug_dump_add(t.colors[0]);
        for (const ResolvedTexture& r : g_resolved) debug_dump_add(r.texture);
        g_dump_request = ++g_dump_draws;
    }
    trace_draw(t, mode, indexed, c, vs, fs, tex_bindings, handles);
    return true;
}

}  // namespace

void draw_end_submit() {  // after the submit's fence: compute results to guest memory, debug snapshots
    debug_snapshots_write();
    for (const WriteBack& w : g_write_backs) std::memcpy(w.to, w.from, w.size);
    g_write_backs.clear();
}

std::vector<WriteBack> draw_take_write_backs() {  // an asynchronous submit copies them when it is retired
    std::vector<WriteBack> v;
    v.swap(g_write_backs);
    return v;
}

void draw_begin_submit() {
    g_rendering = false;
    clears_begin_submit();
    g_submit_ns = plat::monotonic_ns();
    dedup_reset();
    pipeline_begin_submit();
}

uint64_t recorded_draws();

void draw_log_stats() {
    dedup_log();
    sampled_ready_log();
    clears_log();
    log("nvn passes ended: %llu submit end, %llu draw, %llu dispatch, %llu clear of other targets, %llu clear larger than "
        "the pass, %llu copy, %llu snapshot, %llu SetRenderTargets", (unsigned long long)g_pass_ends[0],
        (unsigned long long)g_pass_ends[1], (unsigned long long)g_pass_ends[2], (unsigned long long)g_pass_ends[3],
        (unsigned long long)g_pass_ends[4], (unsigned long long)g_pass_ends[5], (unsigned long long)g_pass_ends[6],
        (unsigned long long)g_pass_ends[7]);
    Stats& t = g_stats;
    log("nvn draws: %llu recorded, %llu replayed:", (unsigned long long)recorded_draws(), (unsigned long long)t.total);
    log("nvn draws: %llu total, %llu done, %llu no program, %llu no vertex shader, %llu no pipeline, %llu no indices",
        (unsigned long long)t.total, (unsigned long long)t.done, (unsigned long long)t.no_program,
        (unsigned long long)t.no_vertex, (unsigned long long)t.no_pipeline, (unsigned long long)t.no_indices);
    log("nvn passes: %llu ended by a draw's texture uploads or layout changes, %llu by new targets, %llu dispatches",
        (unsigned long long)t.breaks_textures, (unsigned long long)t.breaks_targets, (unsigned long long)t.dispatches);
}

void pass_end_only(VkCommandBuffer cb, int reason) {
    if (!g_rendering) return;
    g_pass_ends[reason & 7]++;
    p_vkCmdEndRendering(cb);
    g_rendering = false;
    gpu_profile_mark(cb, "outside passes (clears, uploads, copies)");
}
void draw_end_rendering(VkCommandBuffer cb, int reason) {  // and the pending clears (nvn_clears.cpp)
    pass_end_only(cb, reason);
    clears_flush(cb);
}
bool pass_open_on(const Targets& t) { return g_rendering && targets_same(g_pass, t); }
VkExtent2D pass_extent() { return g_pass_extent; }
void pass_begin(VkCommandBuffer cb, const Targets& t) { begin_rendering(cb, t); }
void pass_mark_written(uint32_t bits) { g_pass_written |= bits; }

uint64_t submit_start_ns() { return g_submit_ns; }

// Entry points of the draws and dispatches for draw_command (nvn_draw_state.cpp).
bool draw_call(VkCommandBuffer cb, const Targets& t, uint32_t mode, bool indexed, const Cmd& c) {
    return draw(cb, t, mode, indexed, c);
}
void dispatch_call(VkCommandBuffer cb, const Cmd& c) { dispatch(cb, c); }

uint64_t draw_take_dump_request() {
    uint64_t r = g_dump_request;
    g_dump_request = 0;
    return r;
}

}  // namespace nx::nvn
