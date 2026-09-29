// NVN on the host (docs/NVN.md). Shared pieces of runtime/src/nvn/*.
//
// The game allocates every NVN object itself (NVNtexture, NVNmemoryPool, ...) at the real driver's sizes. Builders
// and state objects (NVNtextureBuilder, NVNblendState, ...) are plain structs the SDK's own driver fills and reads:
// their procs go straight to that driver (recompiled in sdk), so their layout and the sizes it computes (texture
// storage size and alignment for block-linear data) are exactly the real ones. Objects that do work (memory pools,
// textures, queues, command buffers, windows, ...) are ours: a host object keyed by the guest object's address,
// built from the builder through the driver's getters.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nx/runtime.h>

#include "vk_bridge.h"  // Vulkan headers without prototypes; the dispatch table is vk_gen.h

namespace nx::nvn {

// The SDK driver's implementation of an NVN proc (0 if it has none). Resolved through its own nvnBootstrapLoader.
uint64_t real_proc(CPU& c, const char* name);

// Calls guest function `fn` with integer arguments, returns x0. Registers are restored afterwards.
uint64_t guest_call(CPU& c, uint64_t fn, std::initializer_list<uint64_t> args);

// Guest memory for out-parameters of driver calls: up to 0x1000 bytes private to the calling thread.
uint64_t scratch(CPU& c, size_t size);

// Same, for a function returning a float (in s0).
float guest_call_f(CPU& c, uint64_t fn, std::initializer_list<uint64_t> args);

// Calls the SDK driver's `name` (a getter of a builder or state object).
inline uint64_t driver(CPU& c, const char* name, std::initializer_list<uint64_t> args) {
    return guest_call(c, real_proc(c, name), args);
}

// ---- host objects ---------------------------------------------------------------------------------------------

struct Object {
    virtual ~Object() = default;
};

std::mutex& objects_lock();
std::map<uint64_t, std::unique_ptr<Object>>& objects();

template <typename T>
T* get(uint64_t guest) {
    std::lock_guard lk(objects_lock());
    auto it = objects().find(guest);
    return it == objects().end() ? nullptr : dynamic_cast<T*>(it->second.get());
}

template <typename T>
T& make(uint64_t guest) {
    std::lock_guard lk(objects_lock());
    auto& slot = objects()[guest];
    slot = std::make_unique<T>();
    return static_cast<T&>(*slot);
}

inline void destroy(uint64_t guest) {
    std::lock_guard lk(objects_lock());
    objects().erase(guest);
}

// ---- memory ---------------------------------------------------------------------------------------------------

struct MemoryPool : Object {
    uint64_t memory = 0;  // guest (= host) address of the storage the game gave; 0 for a virtual pool
    uint64_t size = 0;
    uint32_t flags = 0;
    uint64_t gpu = 0;  // our GPU virtual address of byte 0
    // Virtual pools (nvnMemoryPoolMapVirtual): virtual offset -> (physical pool, its offset, size).
    struct Mapping {
        uint64_t physical = 0, physical_offset = 0, size = 0;
    };
    std::map<uint64_t, Mapping> mappings;
};

// Host address of `offset` in a pool, through its virtual mappings if it has no storage of its own; 0 if unmapped.
uint64_t pool_host(const MemoryPool& pool, uint64_t offset);

// GPU virtual addresses: every pool gets a range; address -> (pool, offset) for command translation.
uint64_t gpu_alloc(uint64_t size);
MemoryPool* pool_at_gpu(uint64_t gpu, uint64_t* offset);
void pool_register(MemoryPool* pool);
void pool_unregister(MemoryPool* pool);

// ---- textures and windows ---------------------------------------------------------------------------------------

struct GpuImage;  // the Vulkan side of a texture (nvn_vk.cpp)

struct Texture : Object {
    uint32_t target = 0, format = 0, flags = 0, levels = 0, samples = 0, depth_stencil_mode = 0;
    uint32_t width = 0, height = 0, depth = 0, stride = 0;
    uint64_t pool = 0, offset = 0, storage_size = 0;
    uint64_t memory = 0, gpu = 0;
    uint32_t block_height_gobs = 0;  // packaged (BNTX) textures carry their own; 0: the layout's default
    uint8_t swizzle[4] = {2, 3, 4, 5};  // NVNtextureSwizzle per channel (0 zero, 1 one, 2 R, 3 G, 4 B, 5 A)
    std::shared_ptr<GpuImage> image;  // made on first use by the GPU
};

struct Window : Object {
    std::vector<uint64_t> textures;  // guest NVNtexture*
    int active = 0, next = 0, interval = 1;
};

// ---- samplers and descriptor pools --------------------------------------------------------------------------------

struct SamplerInfo {  // NVN enums (nvn_sampler.cpp)
    uint32_t min_filter = 0, mag_filter = 0, wrap[3] = {}, compare_mode = 0, compare_func = 0, reduction = 0;
    float lod_min = 0, lod_max = 1000, lod_bias = 0, max_anisotropy = 1, border[4] = {};
};

struct DescriptorPool : Object {  // an NVNtexturePool or NVNsamplerPool
    uint64_t pool = 0, offset = 0;
    uint32_t count = 0;
    std::map<uint32_t, uint64_t> textures;     // id -> guest NVNtexture*
    std::map<uint32_t, uint32_t> swizzles;     // id -> the registering view's swizzle (4 NVNtextureSwizzle bytes)
    std::map<uint32_t, SamplerInfo> samplers;  // id -> sampler state
};

// NVNformat -> VkFormat, and whether it is color (0), depth (1), stencil (2) or both (3) (nvn_format.cpp).
VkFormat vk_format(uint32_t format);
uint32_t format_aspect(uint32_t format);

// NVN enums -> Vulkan (nvn_enums.cpp).
VkBlendFactor vk_blend_factor(uint32_t f);
VkBlendOp vk_blend_op(uint32_t e);
VkCompareOp vk_compare(uint32_t f);
VkStencilOp vk_stencil_op(uint32_t op);
VkCullModeFlags vk_cull(uint32_t face);
VkPrimitiveTopology vk_topology(uint32_t mode);
VkIndexType vk_index_type(uint32_t type);
uint32_t index_size(uint32_t type);
VkFormat vk_vertex_format(uint32_t format);

// ---- recorded commands ----------------------------------------------------------------------------------------
// A command buffer records into a list of these; nvnQueueSubmitCommands replays the lists into Vulkan. State
// objects (blend, depth/stencil, vertex layouts, ...) are decoded when recorded, through the SDK driver's getters,
// and kept in the list's blobs: the game may reuse its structs right after the call.

struct Cmd {
    enum Op : uint8_t {
        SetRenderTargets, ClearColor, ClearDepthStencil, SetViewport, SetScissor, CallList,
        SetDepthRange, BindProgram, BindUniformBuffer, BindStorageBuffer, BindTexture, BindVertexBuffer,
        BindVertexAttribs, BindVertexStreams, BindBlendState, BindColorState, BindChannelMaskState,
        BindDepthStencilState, BindPolygonState, SetBlendColor, UpdateUniformBuffer, SetTexturePool, SetSamplerPool,
        DrawArrays, DrawElements, ReportCounter,
    } op;
    uint32_t n = 0, mask = 0;
    uint64_t a[9] = {};  // textures (render targets: colors then depth), handles, GPU addresses, sizes, ...
    float f[4] = {};
    int32_t i[4] = {};
    uint32_t blob = 0;  // index into CmdList::blobs (decoded state, inline data)
};

struct CmdList {
    std::vector<Cmd> cmds;
    std::vector<std::vector<uint8_t>> blobs;
};

// Decoded state objects (values are NVN enums, see nvn_state.cpp).
struct VertexAttrib {
    uint32_t format = 0, offset = 0, stream = 0;
};
struct VertexStream {
    uint32_t stride = 0, divisor = 0;
};
struct BlendTarget {
    uint32_t target = 0, src_rgb = 0, dst_rgb = 0, src_a = 0, dst_a = 0, eq_rgb = 0, eq_a = 0;
};
struct ColorStateInfo {
    uint8_t blend_enable[8] = {};
    uint32_t alpha_test = 0, logic_op = 0;
};
struct ChannelMaskInfo {
    uint8_t mask[8] = {};  // bit 0 r, 1 g, 2 b, 3 a
};
struct DepthStencilInfo {
    uint8_t depth_test = 0, depth_write = 0, stencil_test = 0;
    uint32_t depth_func = 0;
    uint32_t stencil_func[2] = {}, stencil_fail[2] = {}, depth_fail[2] = {}, depth_pass[2] = {};
};
struct PolygonInfo {
    uint32_t cull = 0, front = 0, mode = 0, offset_enables = 0;
};

std::shared_ptr<CmdList> command_list(uint64_t handle);  // a recorded handle's commands (nvn_cmd.cpp)
// Appends to the command buffer (guest NVNcommandBuffer*) being recorded; `blob` is copied into the list.
void record(uint64_t cmdbuf, Cmd cmd, const void* blob = nullptr, size_t size = 0);

// ---- the GPU (nvn_vk.cpp) -------------------------------------------------------------------------------------

void gpu_submit(CPU& c, const std::vector<std::shared_ptr<CmdList>>& lists);
void gpu_present(uint64_t texture);  // guest NVNtexture* of a window texture

// Programs: the translated stages bound by nvnProgramSetShaders (nvn_program.cpp; ShaderInfo in nvn_shaders.h).
struct ShaderInfo;
const std::vector<const ShaderInfo*>* program_shaders(uint64_t program);

// A format's block (width, height in texels; bytes), from the SDK driver's format table (nvn_texture_layout.cpp).
void format_block(uint32_t format, uint32_t& block_w, uint32_t& block_h, uint32_t& bytes);

// Bytes a texture built from `builder` occupies in its pool (nvn_texture_layout.cpp).
uint64_t texture_storage_size(CPU& c, uint64_t builder);

}  // namespace nx::nvn
