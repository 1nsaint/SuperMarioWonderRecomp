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

#include <nx/runtime.h>

namespace nx::nvn {

// The SDK driver's implementation of an NVN proc (0 if it has none). Resolved through its own nvnBootstrapLoader.
uint64_t real_proc(CPU& c, const char* name);

// Calls guest function `fn` with integer arguments, returns x0. Registers are restored afterwards.
uint64_t guest_call(CPU& c, uint64_t fn, std::initializer_list<uint64_t> args);

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
    uint64_t memory = 0;  // guest (= host) address of the storage the game gave
    uint64_t size = 0;
    uint32_t flags = 0;
    uint64_t gpu = 0;  // our GPU virtual address of byte 0
};

// GPU virtual addresses: every pool gets a range; address -> (pool, offset) for command translation.
uint64_t gpu_alloc(uint64_t size);
MemoryPool* pool_at_gpu(uint64_t gpu, uint64_t* offset);
void pool_register(MemoryPool* pool);
void pool_unregister(MemoryPool* pool);

// Bytes a texture built from `builder` occupies in its pool (nvn_texture_layout.cpp).
uint64_t texture_storage_size(CPU& c, uint64_t builder);

}  // namespace nx::nvn
