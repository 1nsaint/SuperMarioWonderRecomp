// Host runtime: memory map, module loader/linker, dispatch, HLE registry, logging.
#pragma once

#include <cstdint>
#include <string>

#include <nx/cpu.h>
#include <nx/module.h>

namespace nx {

// ---- logging --------------------------------------------------------------------------------------------------

void log(const char* fmt, ...);
[[noreturn]] void fatal(const char* fmt, ...);
extern bool g_trace;  // verbose tracing (NX_TRACE=1)

// ---- address space --------------------------------------------------------------------------------------------
// Guest pointers are host pointers. Everything the guest can see lives in these reserved ranges.

constexpr uint64_t kAddressSpaceBase = 0x0008000000ull;
constexpr uint64_t kAddressSpaceSize = 0x7FF8000000ull;  // 39-bit, like an application on Horizon
constexpr uint64_t kModuleBase = 0x4000000000ull;        // main; sdk at +0x10000000 (baked into the code)
constexpr uint64_t kHeapBase = 0x5000000000ull;
constexpr uint64_t kHeapMax = 0x180000000ull;  // 6 GiB
constexpr uint64_t kAliasBase = 0x5800000000ull;
constexpr uint64_t kAliasSize = 0x1000000000ull;
constexpr uint64_t kStackBase = 0x6800000000ull;
constexpr uint64_t kStackSize = 0x100000000ull;
constexpr uint64_t kKernelBase = 0x7000000000ull;  // TLS pages, main thread stack: memory we hand out
constexpr uint64_t kKernelSize = 0x100000000ull;
constexpr uint64_t kThunkBase = 0x7F00000000ull;  // HLE function addresses (never mapped)
constexpr uint64_t kThunkSize = 0x100000ull;

void memory_init();
bool commit(uint64_t addr, uint64_t size);   // make [addr, addr+size) read/write
void decommit(uint64_t addr, uint64_t size);
uint64_t kernel_alloc(uint64_t size, uint64_t align = 0x1000);  // from the kernel region

// Horizon memory states for QueryMemory.
enum MemState : uint32_t {
    kMemFree = 0x00,
    kMemCode = 0x03,
    kMemCodeData = 0x04,
    kMemHeap = 0x05,
    kMemStatic = 0x0C,  // module ro/data seen as static
    kMemStack = 0x0B,
    kMemThreadLocal = 0x0C,
    kMemAlias = 0x0E,
    kMemInaccessible = 0x10,
};
void vm_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm);
struct VmInfo {
    uint64_t addr, size;
    uint32_t state, perm;
};
VmInfo vm_query(uint64_t addr);

// ---- modules --------------------------------------------------------------------------------------------------

struct LoadedModule {
    const ModuleInfo* info;
    uint64_t base, size;
    Fn* table;  // text offset / 4 -> function
};

void load_modules(const ModuleInfo* const* mods, size_t count, const std::string& data_dir);
uint64_t find_symbol(const char* name);  // 0 when not found
const LoadedModule* module_at(uint64_t addr);
std::string describe(uint64_t addr);  // "sdk+0x1234 (symbol+0x10)"

// ---- HLE ------------------------------------------------------------------------------------------------------

using HleFn = void (*)(CPU&);
struct HleRegistrar {
    HleRegistrar(const char* name, HleFn fn);
};
#define NX_HLE_CAT2(a, b) a##b
#define NX_HLE_CAT(a, b) NX_HLE_CAT2(a, b)
#define NX_HLE(sym, fn) static ::nx::HleRegistrar NX_HLE_CAT(hle_reg_, __LINE__)(sym, fn)

uint64_t hle_address(const char* name);  // thunk address; unknown names get a logging stub
HleFn hle_lookup(const char* name);

// Call guest code at `addr` on context c (args already in x0..).
inline void guest_call(CPU& c, uint64_t addr) { call(c, addr); }

// ---- kernel ---------------------------------------------------------------------------------------------------

void kernel_init();
uint32_t create_main_thread(uint64_t stack_size);  // the calling host thread becomes it
CPU& current_cpu();
uint64_t system_tick();

}  // namespace nx
