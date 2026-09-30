// The descriptor sets of a draw or dispatch (split from nvn_draw.cpp): the textures its bindings name, resolved once
// per draw (gather_textures), the sets built from the bound state (build_sets) and bound (bind_sets), and the draw's
// shared inputs. The replay's CPU shortcuts of docs/nvn/PERF-PLAN.md live here and in nvn_draw.cpp; NX_DRAW_SLOW
// turns them off (draw_slow, nvn_draw_state.h). Everything here runs under the GPU lock, one draw at a time.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "nvn_draw_state.h"

namespace nx::nvn::replay {

using namespace nx::vk;

DrawTextures g_textures;

bool draw_slow(uint32_t what) {
    static const uint32_t mask = [] {
        const char* e = std::getenv("NX_DRAW_SLOW");
        if (!e) return 0u;
        uint32_t m = 0;
        for (; *e; e++) {  // only letters of "mehsu": those; anything else (1): all
            const char* letters = "mehsu";
            const char* at = std::strchr(letters, *e);
            if (!at) return ~0u;
            m |= 1u << (at - letters);
        }
        return m ? m : ~0u;
    }();
    return (mask & what) != 0;
}

uint32_t handle_texture(uint64_t handle, uint32_t* sampler) {
    if (sampler) *sampler = uint32_t(handle >> 20) & 0xFFF;
    return uint32_t(handle & 0xFFFFF);
}

namespace {

int api_stage(ShaderStage s) { return s == ShaderStage::Fragment ? 1 : s == ShaderStage::Compute ? 5 : 0; }

// The texture and sampler a handle names in the bound pools, looked up (texture_for without its cache).
// `has_sampler`: the sampler pool had the slot (`sampler` is left alone otherwise).
void lookup_handle(uint64_t handle, uint64_t& texture, SamplerInfo& sampler, ViewInfo* view,
                   bool* has_sampler = nullptr) {
    uint32_t sid = 0;
    uint32_t tid = handle_texture(handle, &sid);
    texture = 0;
    if (view) *view = {};
    // The pools' maps are read under the objects lock, and at the replay's mark (nvn_incarnation.cpp): the game's
    // recording thread registers the next frame's slots meanwhile (98% of the registrations, DEBUG.md #56).
    auto* tp = get<DescriptorPool>(g_state.texture_pool);
    auto* sp = get<DescriptorPool>(g_state.sampler_pool);
    PoolEntry e;
    {
        std::shared_lock lk(objects_lock());
        if (tp && !pool_entry_at(g_state.texture_pool, tid, t_replay_mark, e)) {
            if (auto it = tp->textures.find(tid); it != tp->textures.end()) e.has = true, e.texture = it->second;
            if (auto it = tp->snapshots.find(tid); it != tp->snapshots.end()) e.snapshot = it->second;
            if (auto it = tp->views.find(tid); it != tp->views.end()) e.has_view = true, e.view = it->second;
        }
        if (sp)
            if (auto it = sp->samplers.find(sid); it != sp->samplers.end()) {
                sampler = it->second;
                if (has_sampler) *has_sampler = true;
            }
    }
    if (tp) {
        texture = e.texture;
        // The slot's texture gone or re-initialized as another kind: the copy made at registration (nvn_objects.cpp).
        // NX_SNAPSHOT_SHAPE=1 also uses the copy when only the memory or size changed (the pool's descriptor copy):
        // it broke the course's foreground flowers (dynamic-resolution targets re-initialized every frame are meant
        // to be followed) and did not light the Goombas (docs/nvn/DEBUG.md #45).
        static const bool kind_only = std::getenv("NX_SNAPSHOT_SHAPE") == nullptr;
        if (texture && e.snapshot) {
            Texture* now = get<Texture>(texture);
            Texture* then = get<Texture>(e.snapshot);
            bool moved = now && then && !kind_only &&
                         (now->memory != then->memory || now->width != then->width || now->height != then->height ||
                          now->depth != then->depth || now->levels != then->levels);
            if (then && (!now || moved || now->target != then->target || now->format != then->format)) texture = e.snapshot;
        }
        if (view && e.has_view) *view = e.view;
    }
}

// What a handle resolves to, once per submit (docs/nvn/PERF-PLAN.md step 4). The replay's mark (nvn_incarnation.cpp,
// DEBUG.md #55-#57) makes (texture pool, sampler pool, handle) -> (texture, its object, view) constant within a
// submit: every lookup answers as of the mark, and what the mark resolves to is freed only by drain_objects, between
// replays. Sampler slots have no such history; one registered again during a replay keeps its first value here. The
// image's state is not cached (sampled_ready and sampled_view run for every draw). Open addressing, no removal;
// emptied by bumping the generation.
struct HandleSlot {
    uint64_t generation = 0;  // g_handle_generation when filled
    uint64_t handle = 0, texture_pool = 0, sampler_pool = 0;  // the key (handle: its 32 bits)
    uint64_t texture = 0;       // as lookup_handle gives it
    Texture* object = nullptr;  // get<Texture>(texture)
    bool has_sampler = false;
    SamplerInfo sampler;
    ViewInfo view;
    VkSampler vk_sampler[2] = {};  // sampler_for(sampler, shadow), once a binding asked
};
constexpr uint32_t kHandleSlots = 2048;  // a course frame names a few hundred handles
HandleSlot g_handle_slots[kHandleSlots];
uint64_t g_handle_generation = 1, g_handle_mark = 0;
uint32_t g_handle_used = 0;
uint64_t g_handle_hits = 0, g_handle_misses = 0, g_sets_skipped = 0;

void handles_reset() { g_handle_generation++, g_handle_used = 0; }

// The slot of `handle` in the bound pools, filled on first use; null without a replay mark (NX_TEXTURE_RACE_OLD: a
// lookup then answers as of now) or with NX_DRAW_SLOW. Valid until the next call.
HandleSlot* handle_slot(uint64_t handle) {
    uint64_t mark = t_replay_mark;
    if (!mark || draw_slow(kSlowHandles)) return nullptr;
    if (mark != g_handle_mark || g_handle_used > kHandleSlots / 2) handles_reset(), g_handle_mark = mark;
    handle &= 0xFFFFFFFFull;  // 20 bits of texture id, 12 of sampler id
    uint64_t tp = g_state.texture_pool, sp = g_state.sampler_pool;
    uint64_t h = (handle + 1) * 0x9E3779B97F4A7C15ull ^ tp * 0xC2B2AE3D27D4EB4Full ^ sp * 0x165667B19E3779F9ull;
    for (uint32_t k = uint32_t(h >> 40);; k++) {
        HandleSlot& s = g_handle_slots[k & (kHandleSlots - 1)];
        if (s.generation == g_handle_generation) {
            if (s.handle != handle || s.texture_pool != tp || s.sampler_pool != sp) continue;
            g_handle_hits++;
            return &s;
        }
        s = HandleSlot{};
        s.generation = g_handle_generation, s.handle = handle, s.texture_pool = tp, s.sampler_pool = sp;
        lookup_handle(handle, s.texture, s.sampler, &s.view, &s.has_sampler);
        s.object = s.texture ? get<Texture>(s.texture) : nullptr;
        g_handle_used++, g_handle_misses++;
        return &s;
    }
}

// Host data every draw binds (the support buffer, c[0]) is the same for most draws of a submit, and was built and
// copied for each stage of each draw (~11 KB per draw). DEBUG.md #30's rule stands: dedup shares bytes by address, so
// only bytes that do not change during the submit may go through it, and the stack copies these were built in did.
// Here each distinct content gets a block of its own for the submit, found by what it is made from, and dedup keeps
// one upload of it. The blocks are reused from sets_begin_submit on, where dedup starts over too.
struct SupportBlock {
    float width = 0, height = 0;  // the key: support_buffer_set_viewport's arguments
    SupportBuffer sb;
};
struct ConstantsBlock {
    int hw = 0;         // the key: the Maxwell stage (vertex 0, fragment 4) and the stage's storage bindings,
    Range storage[16];  // all driver_constants reads outside compute
    std::array<uint8_t, 0x1000> bytes;
};
constexpr size_t kBlocks = 32;  // more distinct ones in a submit: copied per draw, as before
std::vector<std::unique_ptr<SupportBlock>> g_support;
std::vector<std::unique_ptr<ConstantsBlock>> g_constants;
size_t g_support_used = 0, g_constants_used = 0;

const SupportBuffer* support_block(float width, float height) {
    for (size_t k = 0; k < g_support_used; k++)
        if (std::memcmp(&g_support[k]->width, &width, 4) == 0 && std::memcmp(&g_support[k]->height, &height, 4) == 0)
            return &g_support[k]->sb;
    if (g_support_used == kBlocks) return nullptr;
    if (g_support_used == g_support.size()) g_support.push_back(std::make_unique<SupportBlock>());
    SupportBlock& b = *g_support[g_support_used++];
    b.width = width, b.height = height;
    b.sb = support_buffer_defaults();
    support_buffer_set_viewport(b.sb, width, height);
    return &b.sb;
}

const uint8_t* constants_block(ShaderStage stage, int st) {
    if (stage == ShaderStage::Compute) return nullptr;  // its table also lists the uniform blocks; a few per frame
    int hw = stage == ShaderStage::Fragment ? 4 : 0;
    const Range* key = g_state.storage[st];
    for (size_t k = 0; k < g_constants_used; k++)
        if (g_constants[k]->hw == hw && std::memcmp(g_constants[k]->storage, key, sizeof g_constants[k]->storage) == 0)
            return g_constants[k]->bytes.data();
    if (g_constants_used == kBlocks) return nullptr;
    if (g_constants_used == g_constants.size()) g_constants.push_back(std::make_unique<ConstantsBlock>());
    ConstantsBlock& b = *g_constants[g_constants_used++];
    b.hw = hw;
    std::memcpy(b.storage, key, sizeof b.storage);
    b.bytes = driver_constants(stage, st);
    return b.bytes.data();
}

// The buffer a descriptor points to: `bytes` copied into the upload buffer (at least 16 bytes, zero padded).
// Storage buffers written by compute shaders go back to guest memory after the submit (the GPU's writes are what the
// game and later work read; WriteBack in nvn_vk.h). One copy per written range, shared by the later work of the
// submit (nvn_written.cpp); g_write_backs: NX_COMPUTE_SEPARATE=1, a copy per binding.
std::vector<WriteBack> g_write_backs;
bool all_write_backs() {  // NX_WRITE_BACK_ALL=1: every compute storage binding, written or not (bisecting)
    static const bool on = std::getenv("NX_WRITE_BACK_ALL") != nullptr;
    return on;
}

// guest: the bytes are guest memory (a copy is shared by later draws of the submit); host scratch data (the support
// buffer, c[0]: on the stack, at the same address for every draw) is copied every time (docs/nvn/DEBUG.md #30).
VkDescriptorBufferInfo buffer_of(VkCommandBuffer cb, const void* bytes, uint64_t size, uint64_t min_size = 16,
                                 bool write_back = false, bool guest = true) {
    uint64_t n = std::max<uint64_t>((size + 15) & ~15ull, min_size);
    const auto* p = static_cast<const uint8_t*>(bytes);
    if (!write_back && guest) {
        Upload u = read_upload(cb, p, size, n, 256);
        return {u.buffer, u.offset, n};
    }
    if (write_back && p && size && !written_separate()) {
        Upload u = written_bind(cb, p, size, n);
        return {u.buffer, u.offset, n};
    }
    Upload u = upload_alloc(n, 256);
    std::memset(u.ptr, 0, n);
    if (bytes && size) std::memcpy(u.ptr, bytes, size);
    // Only what a compute shader may have written goes back. Host scratch data came here too and was "written back"
    // after the submit to the stack address it had been built at (the support buffer and c[0] of every stage of every
    // draw, into a dead frame; with NX_ASYNC_GPU from whichever thread retired the submit). NX_DRAW_SLOW keeps that.
    if (bytes && size && (write_back || draw_slow(kSlowUploads)))
        g_write_backs.push_back({u.ptr, const_cast<uint8_t*>(static_cast<const uint8_t*>(bytes)), size});
    return {u.buffer, u.offset, n};
}

// The sets being built: scratch kept from draw to draw (three vectors, one of them ~130 KB, were allocated per draw).
struct Descriptors {
    VkDescriptorSet sets[4] = {};
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> buffers;
    std::vector<VkDescriptorImageInfo> images;
};
Descriptors g_sets;

}  // namespace

std::vector<WriteBack>& separate_write_backs() { return g_write_backs; }

void texture_for(uint64_t handle, uint64_t& texture, SamplerInfo& sampler, ViewInfo* view) {
    HandleSlot* s = handle_slot(handle);
    if (!s) return lookup_handle(handle, texture, sampler, view);
    texture = s->texture;
    if (s->has_sampler) sampler = s->sampler;
    if (view) *view = s->view;
}

// Descriptor sets for a program's stages from the bound state: buffers (sets 0/1), textures (set 2, g_textures as
// gather_textures left them), storage images (set 3). Records uploads and layout changes, so it runs outside render
// passes when textures or images need them. A set is allocated when its first binding is written: the sets no stage
// declares anything in (usually two of the four) are neither allocated nor bound.
void build_sets(VkCommandBuffer cb, const std::vector<const ShaderInfo*>& shaders, const ProgramLayout& layout) {
    Descriptors& d = g_sets;
    if (draw_slow(kSlowScratch))
        d = Descriptors{};
    else
        d.writes.clear(), d.buffers.clear(), d.images.clear();
    const bool every_set = draw_slow(kSlowEmptySets), shared = !draw_slow(kSlowUploads);
    for (int k = 0; k < 4; k++) d.sets[k] = every_set ? descriptor_set(layout.sets[k]) : VK_NULL_HANDLE;
    auto set_of = [&](uint32_t k) {
        if (!d.sets[k]) d.sets[k] = descriptor_set(layout.sets[k]);
        return d.sets[k];
    };
    auto& writes = d.writes;
    auto& buffers = d.buffers;
    auto& images = d.images;
    buffers.reserve(1024), images.reserve(1024), writes.reserve(2048);  // pointers into them are kept
    float vw = g_state.has_viewport ? float(g_state.viewport[2]) : 1280.0f;
    float vh = g_state.has_viewport ? float(g_state.viewport[3]) : 720.0f;
    SupportBuffer sb;  // built when a binding gets a copy of its own
    bool sb_built = false;
    auto support = [&]() -> VkDescriptorBufferInfo {
        const SupportBuffer* block = shared ? support_block(vw, vh) : nullptr;
        if (block) {
            Upload u = dedup_upload(reinterpret_cast<const uint8_t*>(block), sizeof sb, sizeof sb, 256);
            return {u.buffer, u.offset, sizeof sb};
        }
        if (!sb_built) {
            sb = support_buffer_defaults();
            support_buffer_set_viewport(sb, vw, vh);
            sb_built = true;
        }
        return buffer_of(cb, &sb, sizeof sb, 16, false, false);
    };
    // A storage binding's bytes, and whether this compute stage writes them.
    auto storage = [](const ShaderInfo* s, const DescriptorBinding& b, int st, const uint8_t*& p, uint64_t& size) {
        const Range& r = b.nvn.index >= 0 && b.nvn.index < 16 ? g_state.storage[st][b.nvn.index] : Range{};
        p = r.address ? guest_bytes(r.address) : nullptr;
        size = p ? std::min<uint64_t>(r.size, 16ull << 20) : 0;
        return s->stage == ShaderStage::Compute &&
               (all_write_backs() || (b.binding < 64 && written_storage_bindings(s) >> b.binding & 1));
    };
    // The written ranges first (nvn_written.cpp): two bindings of one dispatch that meet get one copy, and bindings
    // that read what the dispatch writes see it, as on the GPU.
    for (const ShaderInfo* s : shaders)
        for (const DescriptorBinding& b : s->refl.bindings) {
            const uint8_t* p;
            uint64_t size;
            if (b.set < 2 && b.nvn.source == NvnSource::StorageBuffer && storage(s, b, api_stage(s->stage), p, size) &&
                p && size && !written_separate())
                written_bind(cb, p, size, std::max<uint64_t>((size + 15) & ~15ull, 0x10000));
        }
    for (const ShaderInfo* s : shaders) {
        int st = api_stage(s->stage);
        for (const DescriptorBinding& b : s->refl.bindings) {
            if (b.set >= 2) continue;
            VkDescriptorBufferInfo info{};
            switch (b.nvn.source) {
                case NvnSource::SupportBuffer: info = support(); break;
                case NvnSource::ConstantData: info = buffer_of(cb, s->constants.data(), s->constants.size()); break;
                case NvnSource::UniformBlock:
                case NvnSource::UniformAsStorage: {
                    const Range& r = b.nvn.index >= 0 && b.nvn.index < 18 ? g_state.uniforms[st][b.nvn.index] : Range{};
                    const uint8_t* p = r.address ? guest_bytes(r.address) : nullptr;
                    // Reads past the bound size return 0 on Maxwell and garbage on Metal (a UI shader read c3[10]
                    // of a 0x90-byte block and wrote Inf): the bound data goes in a zero-padded 4 KB at least.
                    // Banks the shader indexes with a computed value (clamped to 64 KB, nvn_spirv_patch.cpp): all
                    // 64 KB, so every read past the data returns 0 as on Maxwell.
                    bool indexed = b.set == 0 && b.binding < 64 && (dynamic_uniform_bindings(s) >> b.binding & 1);
                    info = buffer_of(cb, p, p ? std::min<uint64_t>(r.size, 0x10000) : 0, indexed ? 0x10000 : 0x1000);
                    break;
                }
                case NvnSource::StorageBuffer: {
                    const uint8_t* p;
                    uint64_t size;
                    bool written = storage(s, b, st, p, size);
                    // At least 64 KB: Metal does not bound-check, and a shader writing past a small or missing
                    // buffer overwrote the next data in the upload buffer (garbage everywhere after a compute).
                    info = buffer_of(cb, p, size, 0x10000, written);
                    break;
                }
                case NvnSource::DriverConstants: {
                    if (const uint8_t* block = shared ? constants_block(s->stage, st) : nullptr) {
                        Upload u = dedup_upload(block, 0x1000, 0x1000, 256);
                        info = {u.buffer, u.offset, 0x1000};
                        break;
                    }
                    auto c0 = driver_constants(s->stage, st);
                    info = buffer_of(cb, c0.data(), c0.size(), 16, false, false);
                    break;
                }
                default: info = buffer_of(cb, nullptr, 0x1000, 16, false, false); break;
            }
            buffers.push_back(info);
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set_of(b.set);
            w.dstBinding = b.binding;
            w.descriptorCount = 1;
            w.descriptorType = VkDescriptorType(b.type);
            w.pBufferInfo = &buffers.back();
            writes.push_back(w);
        }
    }
    const DrawTextures& t = g_textures;
    for (size_t k = 0; k < t.bindings.size(); k++) {
        auto [s, b] = t.bindings[k];
        const ResolvedTexture r = k < t.resolved.size() ? t.resolved[k] : ResolvedTexture{};
        VkDescriptorImageInfo ii{};
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii.imageView = r.texture ? sampled_view(cb, r.texture, b->image, &r.view, &ii.imageLayout)
                                 : dummy_view(cb, b->image);
        ii.sampler = r.vk_sampler ? r.vk_sampler : sampler_for(r.sampler, b->image.depth);
        images.push_back(ii);
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set_of(2);
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
            w.dstSet = set_of(3);
            w.dstBinding = b.binding;
            w.descriptorCount = 1;
            w.descriptorType = VkDescriptorType(b.type);
            w.pImageInfo = &images.back();
            writes.push_back(w);
        }
    }
}

// A set with no binding in the layout is statically used by no shader, so nothing need be bound there: the runs of
// sets build_sets allocated are bound, in ascending order (each bind leaves the lower sets, bound with this same
// layout, undisturbed).
void bind_sets(VkCommandBuffer cb, VkPipelineBindPoint point, const ProgramLayout& layout) {
    Descriptors& d = g_sets;
    p_vkUpdateDescriptorSets(vk_device(), uint32_t(d.writes.size()), d.writes.data(), 0, nullptr);
    if (draw_slow(kSlowEmptySets)) {
        p_vkCmdBindDescriptorSets(cb, point, layout.layout, 0, 4, d.sets, 0, nullptr);
        return;
    }
    for (uint32_t k = 0; k < 4;) {
        if (!d.sets[k]) {
            k++, g_sets_skipped++;
            continue;
        }
        uint32_t n = 1;
        while (k + n < 4 && d.sets[k + n]) n++;
        p_vkCmdBindDescriptorSets(cb, point, layout.layout, k, n, d.sets + k, 0, nullptr);
        k += n;
    }
}

// Texture bindings of a program's stages and their handles, into g_textures. kNeedBreak: one needs a transfer or a
// layout change (so no render pass may be open while the sets are built); kSampledPending: one has a pending clear.
uint32_t gather_textures(const std::vector<const ShaderInfo*>& shaders) {
    DrawTextures& t = g_textures;
    if (draw_slow(kSlowScratch))
        t = DrawTextures{};
    else
        t.bindings.clear();
    uint32_t out = 0;
    for (const ShaderInfo* s : shaders)
        for (const DescriptorBinding& b : s->refl.bindings) {
            if (b.set == 2) t.bindings.push_back({s, &b});
            if (b.set == 3) out |= kNeedBreak;  // storage images change layout
        }
    t.handles.assign(t.bindings.size(), 0);
    t.resolved.assign(t.bindings.size(), ResolvedTexture{});
    const uint64_t submit_ns = submit_start_ns();
    for (size_t k = 0; k < t.bindings.size(); k++) {
        auto [s, b] = t.bindings[k];
        int st = api_stage(s->stage);
        uint64_t handle = 0;
        if (b->nvn.source == NvnSource::SamplerBinding && b->nvn.index >= 0 && b->nvn.index < 32)
            handle = g_state.textures[st][b->nvn.index];
        else if (b->nvn.source == NvnSource::HandleInUniform && b->nvn.bank >= 3) {
            const Range& r = g_state.uniforms[st][b->nvn.bank - 3];
            const uint8_t* p = r.address ? guest_bytes(r.address) : nullptr;
            if (p && uint64_t(b->nvn.word) * 4 + 4 <= r.size) handle = *reinterpret_cast<const uint32_t*>(p + b->nvn.word * 4);
        }
        t.handles[k] = handle;
        ResolvedTexture& r = t.resolved[k];
        uint64_t texture = 0;
        // A pool entry naming a texture that no longer exists samples the dummy, like no texture: it ended the
        // render pass at every draw (~1.4M times in a run, docs/nvn/DEBUG.md #34).
        if (HandleSlot* slot = handle_slot(handle)) {
            if (slot->object) texture = slot->texture, slot->object->last_sampled = submit_ns;
            r.sampler = slot->sampler, r.view = slot->view;
            VkSampler& vk = slot->vk_sampler[b->image.depth ? 1 : 0];  // the same for every draw: a cache key built
            if (!vk) vk = sampler_for(slot->sampler, b->image.depth);  // per binding otherwise (nvn_sampler.cpp)
            r.vk_sampler = vk;
        } else {
            lookup_handle(handle, texture, r.sampler, &r.view);
            if (Texture* sampled = texture ? get<Texture>(texture) : nullptr)
                sampled->last_sampled = submit_ns;
            else
                texture = 0;
        }
        r.texture = texture;
        if (texture ? !sampled_ready(texture) : !dummy_ready(b->image)) out |= kNeedBreak;
        if (!texture) continue;
        GpuImage* img = image_of(texture);
        // Sampling one of the open pass's targets (a depth buffer read back by the character lighting) needs what
        // was rendered stored first: a fresh pass. SetRenderTargets used to end every pass, which did this by chance
        // (docs/nvn/DEBUG.md #35). Only when a draw of this pass wrote it (pass_wrote, nvn_draw.cpp).
        if (pass_wrote(img)) out |= kNeedBreak;
        if (clears_pending_image(img)) out |= kNeedBreak | kSampledPending;
    }
    return out;
}

// The zero stream (vertex binding 15): one per submit instead of 64 bytes per draw. Host bytes that never change, so
// dedup may share them.
Upload zero_stream() {
    static const uint8_t zeros[64] = {};
    if (!draw_slow(kSlowUploads)) return dedup_upload(zeros, sizeof zeros, sizeof zeros, 16);
    Upload z = upload_alloc(64, 16);
    std::memset(z.ptr, 0, 64);
    return z;
}

// draw_inputs (nvn_written.cpp) with the indices shared: a mesh drawn in several passes of a submit (depth, shadows,
// colour) had its index buffer copied each time. Guest bytes, like the vertex streams, so dedup's rule holds; 8-bit
// indices are widened per draw as before.
bool draw_inputs_shared(VkCommandBuffer cb, const ShaderInfo* vs, bool indexed, const Cmd& c, DrawInputs& in) {
    if (!indexed || draw_slow(kSlowUploads) || vk_index_type(c.mask) == VK_INDEX_TYPE_UINT8_EXT)
        return draw_inputs(cb, vs, indexed, c, in);
    draw_inputs(cb, vs, false, c, in);  // the vertex streams
    const uint8_t* p = guest_bytes(c.a[0]);
    if (!p) return false;
    in.index_type = vk_index_type(c.mask);
    uint64_t bytes = uint64_t(uint32_t(c.i[0])) * index_size(c.mask);
    if (!written_read(cb, p, bytes, bytes, index_size(c.mask), in.index)) in.index = dedup_upload(p, bytes, bytes, 16);
    return true;
}

void sets_begin_submit() {
    handles_reset();
    g_support_used = g_constants_used = 0;
}

void sets_log() {
    if (g_handle_hits + g_handle_misses + g_sets_skipped)
        log("nvn: texture handles: %llu from the submit's cache, %llu looked up; %llu empty descriptor sets skipped",
            (unsigned long long)g_handle_hits, (unsigned long long)g_handle_misses, (unsigned long long)g_sets_skipped);
    g_handle_hits = g_handle_misses = g_sets_skipped = 0;
}

}  // namespace nx::nvn::replay
