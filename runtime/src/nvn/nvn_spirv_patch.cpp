// Vertex inputs with integer formats (docs/nvn/DEBUG.md #20). The offline translator declares every vertex input as a
// float vec4 (TRANSLATOR.md, "What only the runtime knows"): on Maxwell an attribute of an integer format arrives in
// the shader as the integer's bits, and the code reinterprets them (skinning: `floatBitsToInt(blend_index) * 48`).
// With a float input, Vulkan/Metal convert the integer to a float value (3 -> 3.0 = 0x40400000), so every bone index
// was garbage and skinned models (every character) were drawn with matrices read from past the palette.
//
// patch_int_inputs() rewrites a module so the chosen locations are declared as ivec4/uvec4 and every load is bitcast
// back to the float the rest of the code expects: the Maxwell semantics exactly.
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#include "nvn_pipeline.h"

namespace nx::nvn {

namespace {

enum : uint32_t {
    OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpFunction = 54, OpVariable = 59,
    OpLoad = 61, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpBitcast = 124,
    DecorationLocation = 30, StorageInput = 1,
};

struct Inst {
    size_t at;  // word index
    uint32_t op, len;
};

}  // namespace

// 0 float (as is), 1 unsigned integer bits, 2 signed integer bits.
uint32_t vertex_int_kind(uint32_t nvn_format) {
    switch (vk_vertex_format(nvn_format)) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16_UINT:
        case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
            return 1;
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16_SINT:
        case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_A2B10G10R10_SINT_PACK32:
            return 2;
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT:
        case VK_FORMAT_R32G32B32A32_UINT:
            return nvn_format >= 0x55 && nvn_format <= 0x68 ? 0 : 1;  // 32-bit "scaled" wants the value: keep float
        case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32A32_SINT:
            return nvn_format >= 0x55 && nvn_format <= 0x68 ? 0 : 2;
        default: return 0;
    }
}

bool patch_int_inputs(const std::vector<uint32_t>& in, const uint8_t kinds[16], std::vector<uint32_t>& out) {
    if (in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = in[3];
    std::map<uint32_t, uint32_t> location;               // id -> Location
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ints;  // (width, signed) -> id
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> vecs;  // (component, count) -> id
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ptrs;  // (storage, pointee) -> id
    std::map<uint32_t, size_t> defined_at;               // type id -> word index
    uint32_t f32 = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationLocation) location[w[1]] = w[3];
        else if (n.op == OpTypeInt && n.len == 4) ints[{w[2], w[3]}] = w[1];
        else if (n.op == OpTypeFloat && n.len == 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4) vecs[{w[2], w[3]}] = w[1];
        else if (n.op == OpTypePointer && n.len == 4) ptrs[{w[2], w[3]}] = w[1];
        if (n.op >= OpTypeInt && n.op <= OpTypePointer) defined_at[w[1]] = n.at;
    }
    if (!f32) return false;
    uint32_t v4f = vecs.count({f32, 4}) ? vecs[{f32, 4}] : 0;
    uint32_t ptr_v4f = v4f && ptrs.count({StorageInput, v4f}) ? ptrs[{StorageInput, v4f}] : 0;
    if (!ptr_v4f) return false;

    // The input variables to change, and the types they need (added before the first function when missing).
    std::map<uint32_t, uint32_t> patched;  // variable or access chain id -> signedness (0 unsigned, 1 signed)
    size_t insert_at = in.size();          // the new types go right before the first patched variable
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op != OpVariable || n.len < 4 || w[3] != StorageInput || w[1] != ptr_v4f) continue;
        auto l = location.find(w[2]);
        if (l == location.end() || l->second >= 16 || !kinds[l->second]) continue;
        patched[w[2]] = kinds[l->second] == 2 ? 1 : 0;
        insert_at = std::min(insert_at, n.at);
    }
    if (patched.empty()) return false;
    std::vector<uint32_t> types;
    struct Types { uint32_t scalar, vec4, ptr_scalar, ptr_vec4; } t[2] = {};
    bool ok = true;  // false if an existing type we reuse is declared after the insertion point
    for (uint32_t s = 0; s < 2; s++) {
        bool used = false;
        for (auto& p : patched) used |= p.second == s;
        if (!used) continue;
        auto get = [&](auto& table, std::pair<uint32_t, uint32_t> key, uint32_t op, uint32_t a, uint32_t b) {
            auto it = table.find(key);
            if (it != table.end()) {
                if (defined_at.count(it->second) && defined_at[it->second] > insert_at) ok = false;
                return it->second;
            }
            uint32_t id = bound++;
            types.insert(types.end(), {(4u << 16) | op, id, a, b});
            table[key] = id;
            return id;
        };
        t[s].scalar = get(ints, {32, s}, OpTypeInt, 32, s);
        t[s].vec4 = get(vecs, {t[s].scalar, 4}, OpTypeVector, t[s].scalar, 4);
        t[s].ptr_scalar = get(ptrs, {StorageInput, t[s].scalar}, OpTypePointer, StorageInput, t[s].scalar);
        t[s].ptr_vec4 = get(ptrs, {StorageInput, t[s].vec4}, OpTypePointer, StorageInput, t[s].vec4);
    }

    if (!ok) return false;
    out.assign(in.begin(), in.begin() + 5);
    for (const Inst& n : insts) {
        if (n.at == insert_at) out.insert(out.end(), types.begin(), types.end());
        std::vector<uint32_t> w(in.begin() + n.at, in.begin() + n.at + n.len);
        if (n.op == OpVariable && patched.count(w[2])) {
            w[1] = t[patched[w[2]]].ptr_vec4;
        } else if ((n.op == OpAccessChain || n.op == OpInBoundsAccessChain) && n.len >= 4 && patched.count(w[3])) {
            uint32_t s = patched[w[3]];
            if (n.len == 4) return false;  // a chain with no index: not produced by the translator
            w[1] = t[s].ptr_scalar;  // one index into a vec4: a scalar
            patched[w[2]] = s;
        } else if (n.op == OpLoad && n.len >= 4 && patched.count(w[3])) {
            uint32_t s = patched[w[3]], type = w[1], result = w[2], loaded = bound++;
            w[1] = type == v4f ? t[s].vec4 : t[s].scalar;
            w[2] = loaded;
            out.insert(out.end(), w.begin(), w.end());
            out.insert(out.end(), {(4u << 16) | OpBitcast, type, result, loaded});
            continue;
        } else if (n.op == 62 || n.op == 63 || n.op == 57 || n.op == 83 || n.op == 67) {
            // OpStore, OpCopyMemory, OpFunctionCall, OpCopyObject, OpPtrAccessChain on a patched pointer would keep
            // the float type (the translator emits none of them for inputs): leave this module as it is.
            for (uint32_t k = 1; k < n.len; k++)
                if (patched.count(w[k])) return false;
        }
        out.insert(out.end(), w.begin(), w.end());
    }
    out[3] = bound;
    return true;
}

// ---- constant buffer reads past the bound size ------------------------------------------------------------------
// On Maxwell a constant buffer read past the bound size returns 0. The translator declares every bank as vec4[4096] and
// Metal checks nothing, so a computed index past the data reads whatever follows in the upload buffer. Wonder's lit
// materials (uber/agl fragment shaders) loop over a screen tile's light list in c[8]: the tile comes from gl_FragCoord
// scaled for a 384x216 grid (c[6][62].zw = 1/384, 1/216), so a 1920x1080 pass computes tiles up to ~800 while the bound
// list (0xb000 bytes) holds 144. The game relies on those reads returning 0 (no lights); here they returned garbage
// light counts, the loop ran for billions of iterations and Metal killed the command buffer (GPU hang ->
// VK_ERROR_DEVICE_LOST): the picture froze at the first talking-flower balloon of the prologue course while the game
// (and its sound) ran on (docs/nvn/DEBUG.md #21). Fix: the array index of such reads is clamped to 4095 (UMin: negative
// indices clamp too) and those bindings are bound zero-padded to 64 KB, so every read past the data returns 0.
bool clamp_uniform_indices(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, uint64_t* bindings) {
    enum : uint32_t {
        OpExtInstImport = 11, OpExtInst = 12, OpConstant = 43, OpStore = 62, OpDecorate = 71, OpLabel = 248,
        DecorationBinding = 33, DecorationDescriptorSet = 34, StorageUniform = 2, GlslUMin = 38,
    };
    if (bindings) *bindings = 0;
    if (in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t glsl = 0, bound = in[3];
    std::map<uint32_t, uint32_t> binding, set, type_of;  // id -> Binding, DescriptorSet, value type
    std::map<uint32_t, uint32_t> ints;                    // int type id -> its 4095 constant (0: none yet)
    std::map<uint32_t, bool> uniform_var, constant;
    size_t first_function = in.size();
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpExtInstImport && n.len >= 3 &&
            std::strcmp(reinterpret_cast<const char*>(w + 2), "GLSL.std.450") == 0)
            glsl = w[1];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationBinding) binding[w[1]] = w[3];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationDescriptorSet) set[w[1]] = w[3];
        else if (n.op == OpTypeInt && n.len == 4 && w[2] == 32) ints.emplace(w[1], 0);
        else if (n.op == OpVariable && n.len >= 4 && w[3] == StorageUniform) uniform_var[w[2]] = true;
        else if (n.op == OpConstant && n.len >= 4) {
            constant[w[2]] = true;
            if (ints.count(w[1]) && w[3] == 4095) ints[w[1]] = w[2];
        } else if (n.op == OpFunction) first_function = std::min(first_function, n.at);
        // Values inside functions: <op> <type> <id> (the few instructions without a result are skipped).
        if (n.at > first_function && n.len >= 3 && n.op != OpStore && n.op != OpDecorate && n.op != OpLabel &&
            n.op != OpFunction && (n.op < 246 || n.op > 256))
            type_of.emplace(w[2], w[1]);
    }
    if (!glsl) return false;
    // The chains to clamp: base a set 0 uniform variable, then member 0, then a computed array index.
    std::map<size_t, uint32_t> clamp;  // word index of the chain -> index type
    uint64_t mask = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if ((n.op != OpAccessChain && n.op != OpInBoundsAccessChain) || n.len < 6 || !uniform_var.count(w[3])) continue;
        if (constant.count(w[5]) || set[w[3]] != 0 || !binding.count(w[3]) || binding[w[3]] >= 64) continue;
        auto t = type_of.find(w[5]);
        if (t == type_of.end() || !ints.count(t->second)) continue;
        clamp[n.at] = t->second;
        mask |= 1ull << binding[w[3]];
    }
    if (bindings) *bindings = mask;
    if (clamp.empty()) return false;
    std::vector<uint32_t> constants;
    for (auto& [type, id] : ints)
        if (!id) {
            id = bound++;
            constants.insert(constants.end(), {(4u << 16) | OpConstant, type, id, 4095});
        }
    out.assign(in.begin(), in.begin() + 5);
    for (const Inst& n : insts) {
        if (n.at == first_function) out.insert(out.end(), constants.begin(), constants.end());
        std::vector<uint32_t> w(in.begin() + n.at, in.begin() + n.at + n.len);
        if (auto c = clamp.find(n.at); c != clamp.end()) {
            uint32_t clamped = bound++;
            out.insert(out.end(), {(7u << 16) | OpExtInst, c->second, clamped, glsl, GlslUMin, w[5], ints[c->second]});
            w[5] = clamped;
        }
        out.insert(out.end(), w.begin(), w.end());
    }
    out[3] = bound;
    return true;
}

uint64_t dynamic_uniform_bindings(const ShaderInfo* s) {
    static std::mutex lock;
    static std::map<const ShaderInfo*, uint64_t> cache;
    std::lock_guard lk(lock);
    auto it = cache.find(s);
    if (it != cache.end()) return it->second;
    std::vector<uint32_t> unused;
    uint64_t mask = 0;
    clamp_uniform_indices(s->spirv, unused, &mask);
    return cache[s] = mask;
}

}  // namespace nx::nvn
