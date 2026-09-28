// NVN objects, CPU side (docs/NVN.md, N3): memory pools and GPU addresses, buffers, textures, samplers, texture and
// sampler pools. Signatures come from the game's call sites (nvn_trace.txt); builder state is read back
// through the SDK driver's getters (nvn.h). The Vulkan side hangs off these objects.
#include <atomic>
#include <cstring>

#include "nvn.h"
#include "platform.h"

namespace nx::nvn {

// ---- GPU virtual addresses --------------------------------------------------------------------------------------

namespace {

std::mutex g_gpu_lock;
std::map<uint64_t, MemoryPool*> g_pools_by_gpu;  // gpu start -> pool
uint64_t g_gpu_next = 0x100000000ull;  // NVN GPU addresses are 40-bit; keep 0 and low values invalid

}  // namespace

uint64_t gpu_alloc(uint64_t size) {
    std::lock_guard lk(g_gpu_lock);
    uint64_t a = g_gpu_next;
    g_gpu_next = (a + size + 0x1FFFF) & ~0xFFFFull;  // 64 KiB aligned, a guard gap between pools
    return a;
}

void pool_register(MemoryPool* p) {
    std::lock_guard lk(g_gpu_lock);
    g_pools_by_gpu[p->gpu] = p;
}

void pool_unregister(MemoryPool* p) {
    std::lock_guard lk(g_gpu_lock);
    g_pools_by_gpu.erase(p->gpu);
}

MemoryPool* pool_at_gpu(uint64_t gpu, uint64_t* offset) {
    std::lock_guard lk(g_gpu_lock);
    auto it = g_pools_by_gpu.upper_bound(gpu);
    if (it == g_pools_by_gpu.begin()) return nullptr;
    --it;
    MemoryPool* p = it->second;
    if (gpu - p->gpu >= p->size) return nullptr;
    if (offset) *offset = gpu - p->gpu;
    return p;
}

namespace {

// ---- memory pools -----------------------------------------------------------------------------------------------

// NVNboolean nvnMemoryPoolInitialize(NVNmemoryPool*, const NVNmemoryPoolBuilder*)
void MemoryPoolInitialize(CPU& c) {
    uint64_t guest = c.x[0], builder = c.x[1];
    auto& p = make<MemoryPool>(guest);
    p.memory = driver(c, "nvnMemoryPoolBuilderGetMemory", {builder});
    p.size = driver(c, "nvnMemoryPoolBuilderGetSize", {builder});
    p.flags = uint32_t(driver(c, "nvnMemoryPoolBuilderGetFlags", {builder}));
    p.gpu = gpu_alloc(p.size);
    pool_register(&p);
    if (g_trace)
        log("nvn: pool %llx: memory %llx size %llx flags %x gpu %llx", (unsigned long long)guest,
            (unsigned long long)p.memory, (unsigned long long)p.size, p.flags, (unsigned long long)p.gpu);
    c.x[0] = 1;
}

void MemoryPoolFinalize(CPU& c) {
    if (auto* p = get<MemoryPool>(c.x[0])) pool_unregister(p);
    destroy(c.x[0]);
}

void MemoryPoolMap(CPU& c) {  // void* (const NVNmemoryPool*)
    auto* p = get<MemoryPool>(c.x[0]);
    c.x[0] = p ? p->memory : 0;
}

void MemoryPoolGetBufferAddress(CPU& c) {  // NVNbufferAddress (const NVNmemoryPool*)
    auto* p = get<MemoryPool>(c.x[0]);
    c.x[0] = p ? p->gpu : 0;
}

void MemoryPoolGetSize(CPU& c) {
    auto* p = get<MemoryPool>(c.x[0]);
    c.x[0] = p ? p->size : 0;
}

void MemoryPoolGetFlags(CPU& c) {
    auto* p = get<MemoryPool>(c.x[0]);
    c.x[0] = p ? p->flags : 0;
}

// Guest memory is host memory and the GPU side is coherent with it (imported host memory): nothing to flush.
void MemoryPoolFlushMappedRange(CPU&) {}
void MemoryPoolInvalidateMappedRange(CPU&) {}

// ---- buffers ----------------------------------------------------------------------------------------------------

struct Buffer : Object {
    uint64_t pool = 0;  // guest NVNmemoryPool*
    uint64_t offset = 0, size = 0;
    uint64_t memory = 0, gpu = 0;
};

void BufferInitialize(CPU& c) {  // NVNboolean (NVNbuffer*, const NVNbufferBuilder*)
    uint64_t guest = c.x[0], builder = c.x[1];
    auto& b = make<Buffer>(guest);
    b.pool = driver(c, "nvnBufferBuilderGetMemoryPool", {builder});
    b.offset = driver(c, "nvnBufferBuilderGetMemoryOffset", {builder});
    b.size = driver(c, "nvnBufferBuilderGetSize", {builder});
    if (auto* p = get<MemoryPool>(b.pool)) {
        b.memory = p->memory + b.offset;
        b.gpu = p->gpu + b.offset;
    }
    c.x[0] = 1;
}

void BufferFinalize(CPU& c) { destroy(c.x[0]); }

void BufferMap(CPU& c) {
    auto* b = get<Buffer>(c.x[0]);
    c.x[0] = b ? b->memory : 0;
}

void BufferGetAddress(CPU& c) {
    auto* b = get<Buffer>(c.x[0]);
    c.x[0] = b ? b->gpu : 0;
}

void BufferGetSize(CPU& c) {
    auto* b = get<Buffer>(c.x[0]);
    c.x[0] = b ? b->size : 0;
}

void BufferGetMemoryPool(CPU& c) {
    auto* b = get<Buffer>(c.x[0]);
    c.x[0] = b ? b->pool : 0;
}

void BufferGetMemoryOffset(CPU& c) {
    auto* b = get<Buffer>(c.x[0]);
    c.x[0] = b ? b->offset : 0;
}

void BufferFlushMappedRange(CPU&) {}
void BufferInvalidateMappedRange(CPU&) {}

// ---- textures ---------------------------------------------------------------------------------------------------

struct Texture : Object {
    uint32_t target = 0, format = 0, flags = 0, levels = 0, samples = 0, depth_stencil_mode = 0;
    uint32_t width = 0, height = 0, depth = 0, stride = 0;
    uint64_t pool = 0, offset = 0, storage_size = 0;
    uint64_t memory = 0, gpu = 0;
};

void TextureInitialize(CPU& c) {  // NVNboolean (NVNtexture*, const NVNtextureBuilder*)
    uint64_t guest = c.x[0], builder = c.x[1];
    auto& t = make<Texture>(guest);
    t.target = uint32_t(driver(c, "nvnTextureBuilderGetTarget", {builder}));
    t.format = uint32_t(driver(c, "nvnTextureBuilderGetFormat", {builder}));
    t.flags = uint32_t(driver(c, "nvnTextureBuilderGetFlags", {builder}));
    t.levels = uint32_t(driver(c, "nvnTextureBuilderGetLevels", {builder}));
    t.samples = uint32_t(driver(c, "nvnTextureBuilderGetSamples", {builder}));
    t.width = uint32_t(driver(c, "nvnTextureBuilderGetWidth", {builder}));
    t.height = uint32_t(driver(c, "nvnTextureBuilderGetHeight", {builder}));
    t.depth = uint32_t(driver(c, "nvnTextureBuilderGetDepth", {builder}));
    t.stride = uint32_t(driver(c, "nvnTextureBuilderGetStride", {builder}));
    t.depth_stencil_mode = uint32_t(driver(c, "nvnTextureBuilderGetDepthStencilMode", {builder}));
    t.pool = driver(c, "nvnTextureBuilderGetMemoryPool", {builder});
    t.offset = driver(c, "nvnTextureBuilderGetMemoryOffset", {builder});
    t.storage_size = texture_storage_size(c, builder);
    if (auto* p = get<MemoryPool>(t.pool)) {
        t.memory = p->memory + t.offset;
        t.gpu = p->gpu + t.offset;
    }
    if (g_trace)
        log("nvn: texture %llx: target %u format %u %ux%ux%u levels %u samples %u flags %x size %llx", (unsigned long long)guest,
            t.target, t.format, t.width, t.height, t.depth, t.levels, t.samples, t.flags, (unsigned long long)t.storage_size);
    c.x[0] = 1;
}

void TextureFinalize(CPU& c) { destroy(c.x[0]); }

#define NX_TEXTURE_GETTER(fn, field)          \
    void fn(CPU& c) {                         \
        auto* t = get<Texture>(c.x[0]);       \
        c.x[0] = t ? uint64_t(t->field) : 0;  \
    }
NX_TEXTURE_GETTER(TextureGetWidth, width)
NX_TEXTURE_GETTER(TextureGetHeight, height)
NX_TEXTURE_GETTER(TextureGetDepth, depth)
NX_TEXTURE_GETTER(TextureGetFormat, format)
NX_TEXTURE_GETTER(TextureGetTarget, target)
NX_TEXTURE_GETTER(TextureGetLevels, levels)
NX_TEXTURE_GETTER(TextureGetSamples, samples)
NX_TEXTURE_GETTER(TextureGetFlags, flags)
NX_TEXTURE_GETTER(TextureGetStride, stride)
NX_TEXTURE_GETTER(TextureGetMemoryPool, pool)
NX_TEXTURE_GETTER(TextureGetMemoryOffset, offset)
NX_TEXTURE_GETTER(TextureGetStorageSize, storage_size)
NX_TEXTURE_GETTER(TextureGetDepthStencilMode, depth_stencil_mode)
#undef NX_TEXTURE_GETTER

// ---- samplers and descriptor pools ----------------------------------------------------------------------------

struct Sampler : Object {
    uint64_t builder_copy[16] = {};  // raw builder bytes, decoded when the Vulkan sampler is made
};

void SamplerInitialize(CPU& c) {  // NVNboolean (NVNsampler*, const NVNsamplerBuilder*)
    auto& s = make<Sampler>(c.x[0]);
    std::memcpy(s.builder_copy, reinterpret_cast<const void*>(c.x[1]), sizeof s.builder_copy);
    c.x[0] = 1;
}

void SamplerFinalize(CPU& c) { destroy(c.x[0]); }

struct DescriptorPool : Object {
    uint64_t pool = 0, offset = 0;
    uint32_t count = 0;
    std::map<uint32_t, uint64_t> entries;  // id -> guest texture / sampler (views: later)
};

// NVNboolean nvn{Texture,Sampler}PoolInitialize(pool, const NVNmemoryPool*, ptrdiff_t offset, int numDescriptors)
void DescriptorPoolInitialize(CPU& c) {
    auto& d = make<DescriptorPool>(c.x[0]);
    d.pool = c.x[1];
    d.offset = c.x[2];
    d.count = uint32_t(c.x[3]);
    c.x[0] = 1;
}

void DescriptorPoolFinalize(CPU& c) { destroy(c.x[0]); }

// nvnSamplerPoolRegisterSampler(pool, int id, const NVNsampler*); nvnTexturePoolRegisterTexture(pool, int id,
// const NVNtexture*, const NVNtextureView*)
void DescriptorPoolRegister(CPU& c) {
    if (auto* d = get<DescriptorPool>(c.x[0])) {
        std::lock_guard lk(objects_lock());
        d->entries[uint32_t(c.x[1])] = c.x[2];
    }
}

void DescriptorPoolGetMemoryPool(CPU& c) {
    auto* d = get<DescriptorPool>(c.x[0]);
    c.x[0] = d ? d->pool : 0;
}

void DescriptorPoolGetMemoryOffset(CPU& c) {
    auto* d = get<DescriptorPool>(c.x[0]);
    c.x[0] = d ? d->offset : 0;
}

void DescriptorPoolGetSize(CPU& c) {
    auto* d = get<DescriptorPool>(c.x[0]);
    c.x[0] = d ? d->count : 0;
}

// nvnDeviceGetTextureHandle(device, int textureID, int samplerID): Maxwell's bindless handle, texture id in the
// low 20 bits and sampler id above (what the shaders' TEX instructions decode). Texel fetch: the texture id alone.
void DeviceGetTextureHandle(CPU& c) { c.x[0] = (c.x[1] & 0xFFFFF) | ((c.x[2] & 0xFFF) << 20); }
void DeviceGetTexelFetchHandle(CPU& c) { c.x[0] = c.x[1] & 0xFFFFF; }
void DeviceGetImageHandle(CPU& c) { c.x[0] = c.x[1] & 0xFFFFF; }

}  // namespace

NX_HLE("nvnMemoryPoolInitialize", MemoryPoolInitialize);
NX_HLE("nvnMemoryPoolFinalize", MemoryPoolFinalize);
NX_HLE("nvnMemoryPoolMap", MemoryPoolMap);
NX_HLE("nvnMemoryPoolGetBufferAddress", MemoryPoolGetBufferAddress);
NX_HLE("nvnMemoryPoolGetSize", MemoryPoolGetSize);
NX_HLE("nvnMemoryPoolGetFlags", MemoryPoolGetFlags);
NX_HLE("nvnMemoryPoolFlushMappedRange", MemoryPoolFlushMappedRange);
NX_HLE("nvnMemoryPoolInvalidateMappedRange", MemoryPoolInvalidateMappedRange);
NX_HLE("nvnBufferInitialize", BufferInitialize);
NX_HLE("nvnBufferFinalize", BufferFinalize);
NX_HLE("nvnBufferMap", BufferMap);
NX_HLE("nvnBufferGetAddress", BufferGetAddress);
NX_HLE("nvnBufferGetSize", BufferGetSize);
NX_HLE("nvnBufferGetMemoryPool", BufferGetMemoryPool);
NX_HLE("nvnBufferGetMemoryOffset", BufferGetMemoryOffset);
NX_HLE("nvnBufferFlushMappedRange", BufferFlushMappedRange);
NX_HLE("nvnBufferInvalidateMappedRange", BufferInvalidateMappedRange);
NX_HLE("nvnTextureInitialize", TextureInitialize);
NX_HLE("nvnTextureFinalize", TextureFinalize);
NX_HLE("nvnTextureGetWidth", TextureGetWidth);
NX_HLE("nvnTextureGetHeight", TextureGetHeight);
NX_HLE("nvnTextureGetDepth", TextureGetDepth);
NX_HLE("nvnTextureGetFormat", TextureGetFormat);
NX_HLE("nvnTextureGetTarget", TextureGetTarget);
NX_HLE("nvnTextureGetLevels", TextureGetLevels);
NX_HLE("nvnTextureGetSamples", TextureGetSamples);
NX_HLE("nvnTextureGetFlags", TextureGetFlags);
NX_HLE("nvnTextureGetStride", TextureGetStride);
NX_HLE("nvnTextureGetMemoryPool", TextureGetMemoryPool);
NX_HLE("nvnTextureGetMemoryOffset", TextureGetMemoryOffset);
NX_HLE("nvnTextureGetStorageSize", TextureGetStorageSize);
NX_HLE("nvnTextureGetDepthStencilMode", TextureGetDepthStencilMode);
NX_HLE("nvnSamplerInitialize", SamplerInitialize);
NX_HLE("nvnSamplerFinalize", SamplerFinalize);
NX_HLE("nvnSamplerPoolInitialize", DescriptorPoolInitialize);
NX_HLE("nvnTexturePoolInitialize", DescriptorPoolInitialize);
NX_HLE("nvnSamplerPoolFinalize", DescriptorPoolFinalize);
NX_HLE("nvnTexturePoolFinalize", DescriptorPoolFinalize);
NX_HLE("nvnSamplerPoolRegisterSampler", DescriptorPoolRegister);
NX_HLE("nvnSamplerPoolRegisterSamplerBuilder", DescriptorPoolRegister);
NX_HLE("nvnTexturePoolRegisterTexture", DescriptorPoolRegister);
NX_HLE("nvnTexturePoolRegisterImage", DescriptorPoolRegister);
NX_HLE("nvnSamplerPoolGetMemoryPool", DescriptorPoolGetMemoryPool);
NX_HLE("nvnTexturePoolGetMemoryPool", DescriptorPoolGetMemoryPool);
NX_HLE("nvnSamplerPoolGetMemoryOffset", DescriptorPoolGetMemoryOffset);
NX_HLE("nvnTexturePoolGetMemoryOffset", DescriptorPoolGetMemoryOffset);
NX_HLE("nvnSamplerPoolGetSize", DescriptorPoolGetSize);
NX_HLE("nvnTexturePoolGetSize", DescriptorPoolGetSize);
NX_HLE("nvnDeviceGetTextureHandle", DeviceGetTextureHandle);
NX_HLE("nvnDeviceGetTexelFetchHandle", DeviceGetTexelFetchHandle);
NX_HLE("nvnDeviceGetImageHandle", DeviceGetImageHandle);

}  // namespace nx::nvn
