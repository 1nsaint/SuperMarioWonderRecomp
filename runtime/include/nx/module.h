// Tables the recompiler emits for a module.
#pragma once

#include <cstddef>
#include <cstdint>

#include <nx/cpu.h>

namespace nx {

struct FuncEntry {
    uint64_t offset;  // module-relative guest address
    Fn fn;
};

struct ImportSlot {
    uint64_t got;  // module-relative address of the GOT slot
    const char* name;
};

}  // namespace nx

extern const nx::FuncEntry nx_functions[];
extern const size_t nx_function_count;
extern const nx::ImportSlot nx_imports[];
extern const size_t nx_import_count;
