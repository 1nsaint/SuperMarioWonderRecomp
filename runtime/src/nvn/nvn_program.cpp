// NVN programs (docs/NVN.md, N5): the shader stages the game binds. Each stage arrives as Maxwell code at a GPU
// address in one of its memory pools, plus the NVN control section (docs/nvn/SHADERS.md: magic 0x98761234,
// program offset at +0x708, ...). Here we record them; translation to SPIR-V is keyed by the code bytes.
#include <cstring>
#include <vector>

#include "nvn.h"

namespace nx::nvn {

namespace {

constexpr uint32_t kControlMagic = 0x98761234;

struct Stage {
    uint64_t code_gpu = 0;   // NVNbufferAddress of the code block (0x30-byte header + SPH + program)
    uint64_t code_host = 0;  // the same bytes in guest memory
    uint64_t control = 0;    // guest pointer to the control section
};

struct Program : Object {
    std::vector<Stage> stages;
};

void ProgramInitialize(CPU& c) {  // NVNboolean (NVNprogram*, NVNdevice*)
    make<Program>(c.x[0]);
    c.x[0] = 1;
}

void ProgramFinalize(CPU& c) { destroy(c.x[0]); }

// Finds, in one NVNshaderData(Ext) entry, the code address (a GPU address inside a pool) and the control pointer
// (guest memory starting with the control magic). The Ext entry's layout is learned from the data itself and logged
// once (docs/nvn/API.md).
Stage decode_entry(uint64_t entry, size_t stride) {
    Stage s;
    for (size_t off = 0; off + 8 <= stride; off += 8) {
        uint64_t v = ld<uint64_t>(entry + off);
        uint64_t pool_off = 0;
        if (!s.code_gpu && v)
            if (MemoryPool* p = pool_at_gpu(v, &pool_off)) {
                s.code_gpu = v;
                s.code_host = p->memory + pool_off;
                continue;
            }
        if (!s.control && v >= kModuleBase && v < kKernelBase + kKernelSize && (v & 3) == 0 && ld<uint32_t>(v) == kControlMagic)
            s.control = v;
    }
    return s;
}

// NVNboolean nvnProgramSetShaders(NVNprogram*, int count, const NVNshaderData* stages): {NVNbufferAddress data;
// const void* control} per stage. The Ext variant's entries are larger (learned at run time, see decode_entry).
void set_shaders(CPU& c, size_t stride) {
    auto* p = get<Program>(c.x[0]);
    int count = int(c.x[1]);
    uint64_t entries = c.x[2];
    static bool logged = false;
    if (!logged && entries) {
        logged = true;
        for (size_t k = 0; k < 2 * stride / 8; k++)
            log("nvn: shader data +%02zx: %016llx", k * 8, (unsigned long long)ld<uint64_t>(entries + k * 8));
    }
    if (p) {
        p->stages.clear();
        for (int k = 0; k < count; k++) {
            Stage s = decode_entry(entries + k * stride, stride);
            if (!s.code_gpu || !s.control)
                log("nvn: program %llx stage %d: code %llx control %llx (not found)", (unsigned long long)c.x[0], k,
                    (unsigned long long)s.code_gpu, (unsigned long long)s.control);
            p->stages.push_back(s);
        }
    }
    c.x[0] = 1;
}

void ProgramSetShaders(CPU& c) { set_shaders(c, 0x10); }
void ProgramSetShadersExt(CPU& c) { set_shaders(c, 0x18); }  // {data, control, 0}

}  // namespace

NX_HLE("nvnProgramInitialize", ProgramInitialize);
NX_HLE("nvnProgramFinalize", ProgramFinalize);
NX_HLE("nvnProgramSetShaders", ProgramSetShaders);
NX_HLE("nvnProgramSetShadersExt", ProgramSetShadersExt);

}  // namespace nx::nvn
