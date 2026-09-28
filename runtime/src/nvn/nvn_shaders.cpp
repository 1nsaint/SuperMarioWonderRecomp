// NVN shader stages -> offline-translated SPIR-V (nvn_shaders.h, docs/nvn/TRANSLATOR.md "Runtime side").
//
// The meaning of each binding comes from the translator's own naming and numbering (Ryujinx.Graphics.Shader,
// Translation/ResourceManager.cs): uniform buffers are "<stage>_c<k>" at binding stageSlot * 18 + 1 + k, storage
// buffers "<stage>_s<slot>", textures "<stage>_t_tcb_<word hex>" (handle in the texture bank c[2]) or
// "<stage>_t_cb<k>_<word hex>" (handle in c[k]), images "<stage>_i_..." likewise with a format suffix. The one thing
// neither the names nor the numbers carry is the c[0] offset a storage buffer's address was loaded from; that comes
// from the harness's results.tsv (apply_harness).
#include "nvn_shaders.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef NX_SHADERS_STANDALONE
#include <memory>
#include <mutex>

#include <nx/runtime.h>

#include "platform.h"
#include "vk_bridge.h"
#endif

namespace nx::nvn {

// ---- SHA-1 --------------------------------------------------------------------------------------------------------

namespace {

inline uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

inline uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);  // little-endian host (arm64, x86-64), like the Switch
    return v;
}

}  // namespace

Sha1::Sha1() : h_{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0} {}

void Sha1::block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5a827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ed9eba1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
        else f = b ^ c ^ d, k = 0xca62c1d6;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e;
}

void Sha1::update(const void* data, size_t size) {
    auto* p = static_cast<const uint8_t*>(data);
    bytes_ += size;
    if (used_) {
        size_t n = std::min(size, 64 - used_);
        std::memcpy(buf_ + used_, p, n);
        used_ += n, p += n, size -= n;
        if (used_ < 64) return;
        block(buf_);
        used_ = 0;
    }
    for (; size >= 64; p += 64, size -= 64) block(p);
    std::memcpy(buf_, p, size);
    used_ = size;
}

void Sha1::finish(uint8_t digest[20]) {
    uint64_t bits = bytes_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    pad = 0;
    while (used_ != 56) update(&pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
    update(len, 8);
    for (int i = 0; i < 20; i++) digest[i] = uint8_t(h_[i / 4] >> (24 - 8 * (i % 4)));
}

std::string sha1_hex(const void* data, size_t size) {
    Sha1 s;
    s.update(data, size);
    uint8_t d[20];
    s.finish(d);
    static const char* kHex = "0123456789abcdef";
    std::string out(40, '0');
    for (int i = 0; i < 20; i++) out[2 * i] = kHex[d[i] >> 4], out[2 * i + 1] = kHex[d[i] & 15];
    return out;
}

std::string shader_hash(const uint8_t* code, uint32_t size) { return sha1_hex(code, size).substr(0, 16); }

// ---- control section ------------------------------------------------------------------------------------------------

ShaderControl read_control(const uint8_t* control) {
    ShaderControl c;
    c.valid = rd32(control) == kShaderControlMagic;
    c.program_size = rd32(control + 0x6f8);
    c.const_size = rd32(control + 0x6fc);
    c.const_offset = rd32(control + 0x700);
    c.code_size = rd32(control + 0x704);
    c.program_start = rd32(control + 0x708);
    for (int i = 0; i < 3; i++) c.local_size[i] = rd32(control + 0x748 + 4 * i);
    return c;
}

// ---- SPIR-V reflection ----------------------------------------------------------------------------------------------

namespace {

// The handful of SPIR-V opcodes, decorations and storage classes reflection needs (SPIR-V 1.x unified spec).
enum : uint32_t {
    OpName = 5, OpEntryPoint = 15, OpExecutionMode = 16, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22,
    OpTypeVector = 23, OpTypeMatrix = 24, OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27,
    OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43,
    OpVariable = 59, OpDecorate = 71, OpMemberDecorate = 72,
};
enum : uint32_t {
    DecBlock = 2, DecBufferBlock = 3, DecBuiltIn = 11, DecLocation = 30, DecComponent = 31, DecBinding = 33,
    DecDescriptorSet = 34,
};
enum : uint32_t {
    ScUniformConstant = 0, ScInput = 1, ScUniform = 2, ScOutput = 3, ScStorageBuffer = 12,
};
constexpr uint32_t kExecutionModeLocalSize = 17;
constexpr uint32_t kDimBuffer = 5;

struct Decorations {
    int set = -1, binding = -1, location = -1, component = 0, builtin = -1;
    bool block = false, buffer_block = false;
    std::vector<uint32_t> member_builtins;
};

struct Type {
    uint32_t op = 0;
    std::vector<uint32_t> w;  // operands after the result id
};

std::string spirv_string(const uint32_t* w, size_t n, size_t* used) {
    std::string s;
    size_t i = 0;
    for (; i < n; i++) {
        bool end = false;
        for (int b = 0; b < 4; b++) {
            char ch = char((w[i] >> (8 * b)) & 0xff);
            if (!ch) {
                end = true;
                break;
            }
            s.push_back(ch);
        }
        if (end) {
            i++;
            break;
        }
    }
    if (used) *used = i;
    return s;
}

bool parse_uint(std::string_view s, int base, int& out) {
    if (s.empty()) return false;
    long v = 0;
    for (char ch : s) {
        int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : 99;
        if (d >= base) return false;
        v = v * base + d;
        if (v > 0x7fffffff) return false;
    }
    out = int(v);
    return true;
}

// Ryujinx's stage slot inside each set's binding range: vertex and compute 0, fragment 1 (GetStageIndex remap;
// the harness's StaticGpuAccessor gives the same numbers).
int stage_slot(ShaderStage s) { return s == ShaderStage::Fragment ? 1 : 0; }

// The storage buffer descriptors NVN keeps in c[0] (TRANSLATOR.md, bank table): graphics stage i at
// 0x110 + 0x100 * i + 0x10 * n (vertex i = 0, fragment i = 4); compute uniform blocks at 0x210 + 0x10 * n and
// storage buffers at 0x310 + 0x10 * n.
void set_c0_offset(NvnMeaning& m, ShaderStage stage, int byte_offset) {
    m.c0_offset = byte_offset;
    m.source = NvnSource::Unknown;
    m.index = -1;
    auto in = [&](int base, int count) { return byte_offset >= base && byte_offset < base + 0x10 * count && (byte_offset - base) % 0x10 == 0; };
    if (stage == ShaderStage::Compute) {
        if (in(0x210, 16)) m.source = NvnSource::UniformAsStorage, m.index = (byte_offset - 0x210) / 0x10;
        else if (in(0x310, 16)) m.source = NvnSource::StorageBuffer, m.index = (byte_offset - 0x310) / 0x10;
    } else if (stage == ShaderStage::Vertex || stage == ShaderStage::Fragment) {
        int base = 0x110 + 0x100 * (stage == ShaderStage::Vertex ? 0 : 4);
        if (in(base, 16)) m.source = NvnSource::StorageBuffer, m.index = (byte_offset - base) / 0x10;
    }
}

void set_texture_handle(NvnMeaning& m, bool image, int bank, int word) {
    m.default_bank = bank < 0;
    m.bank = bank < 0 ? 2 : bank;
    m.word = word;
    m.index = -1;
    m.source = NvnSource::Unknown;
    if (m.bank == 2) {
        // c[2]: texture handle of sampler binding n at byte 0x20 + 8n (verified on every stage), images after 32
        // texture slots at 0x120 + 8n (inferred from the agl compute shaders: words 72, 74, 76).
        int first = image ? 72 : 8;
        if (word >= first && (word - first) % 2 == 0 && (word - first) / 2 < 32) {
            m.source = image ? NvnSource::ImageBinding : NvnSource::SamplerBinding;
            m.index = (word - first) / 2;
        }
    } else if (m.bank >= 3 && !image) {
        m.source = NvnSource::HandleInUniform;
        m.index = m.bank - 3;
    }
}

// The NVN meaning from the variable's name and binding. Names first; the binding number alone fixes uniform banks.
void derive_meaning(DescriptorBinding& b, ShaderStage stage) {
    NvnMeaning& m = b.nvn;
    std::string_view n = b.name;
    if (n == "support_buffer") {
        m.source = NvnSource::SupportBuffer;
        return;
    }
    if (size_t us = n.find('_'); us != std::string_view::npos) {
        std::string_view prefix = n.substr(0, us);  // vp fp cp (tcp tep gp): Ryujinx GetShaderStagePrefix
        if (prefix == "vp" || prefix == "fp" || prefix == "cp" || prefix == "tcp" || prefix == "tep" || prefix == "gp")
            n.remove_prefix(us + 1);
    }
    int v = 0;
    if (b.type == DescriptorType::UniformBuffer) {
        int bank = -1;
        if (n.size() > 1 && n[0] == 'c' && parse_uint(n.substr(1), 10, v)) bank = v;
        else if (b.set == 0 && b.binding >= 1) bank = int(b.binding) - 1 - stage_slot(stage) * 18;  // fallback
        m.bank = bank;
        m.source = bank == 0 ? NvnSource::DriverConstants : bank == 1 ? NvnSource::ConstantData
                   : bank >= 3 ? NvnSource::UniformBlock : NvnSource::Unknown;
        m.index = bank >= 3 ? bank - 3 : -1;
        return;
    }
    if (b.type == DescriptorType::StorageBuffer) {
        if (n.size() > 1 && n[0] == 's' && parse_uint(n.substr(1), 10, v)) m.sb_slot = v;
        else if (b.set == 1) m.sb_slot = int(b.binding) - stage_slot(stage) * 16;
        m.source = NvnSource::Unknown;  // until apply_harness gives the c[0] offset
        return;
    }
    // Textures and images: "<t|i|s>[type]_tcb_<hex>[_fmt]" or "<t|i|s>[type]_cb<k>_<hex>[_fmt]".
    bool image = b.type == DescriptorType::StorageImage || b.type == DescriptorType::StorageTexelBuffer;
    size_t us = n.find('_');
    if (us == std::string_view::npos) return;
    std::string_view rest = n.substr(us + 1);
    int bank = -2;
    if (rest.substr(0, 4) == "tcb_") {
        bank = -1;
        rest.remove_prefix(4);
    } else if (rest.substr(0, 2) == "cb") {
        size_t us2 = rest.find('_');
        if (us2 == std::string_view::npos || !parse_uint(rest.substr(2, us2 - 2), 10, bank)) return;
        rest.remove_prefix(us2 + 1);
    } else {
        return;
    }
    std::string_view hex = rest.substr(0, rest.find('_'));
    int word = 0;
    if (!parse_uint(hex, 16, word)) return;
    set_texture_handle(m, image, bank, word);
}

}  // namespace

const DescriptorBinding* ShaderReflection::find(uint32_t set, uint32_t binding) const {
    for (const auto& b : bindings)
        if (b.set == set && b.binding == binding) return &b;
    return nullptr;
}

bool reflect_spirv(const uint32_t* words, size_t count, ShaderReflection& out, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    out = ShaderReflection{};
    if (count < 5 || words[0] != 0x07230203) return fail("not a SPIR-V module");

    std::unordered_map<uint32_t, std::string> names;
    std::unordered_map<uint32_t, Decorations> dec;
    std::unordered_map<uint32_t, Type> types;
    std::unordered_map<uint32_t, uint32_t> constants;
    struct Var {
        uint32_t id, ptr_type, storage;
    };
    std::vector<Var> vars;
    uint32_t entry_id = 0;
    bool have_entry = false;

    for (size_t i = 5; i < count;) {
        uint32_t wc = words[i] >> 16, op = words[i] & 0xffff;
        if (wc == 0 || i + wc > count) return fail("truncated instruction");
        const uint32_t* w = words + i + 1;  // operands
        size_t n = wc - 1;
        switch (op) {
        case OpEntryPoint:
            if (!have_entry && n >= 3) {
                have_entry = true;
                uint32_t model = w[0];
                entry_id = w[1];
                out.entry = spirv_string(w + 2, n - 2, nullptr);
                out.stage = model == 0 ? ShaderStage::Vertex : model == 4 ? ShaderStage::Fragment
                            : model == 5 ? ShaderStage::Compute : ShaderStage::Unknown;
            }
            break;
        case OpExecutionMode:
            if (n >= 5 && w[0] == entry_id && w[1] == kExecutionModeLocalSize)
                out.local_size[0] = w[2], out.local_size[1] = w[3], out.local_size[2] = w[4];
            break;
        case OpName:
            if (n >= 2) names[w[0]] = spirv_string(w + 1, n - 1, nullptr);
            break;
        case OpDecorate:
            if (n >= 2) {
                Decorations& d = dec[w[0]];
                uint32_t lit = n >= 3 ? w[2] : 0;
                switch (w[1]) {
                case DecDescriptorSet: d.set = int(lit); break;
                case DecBinding: d.binding = int(lit); break;
                case DecLocation: d.location = int(lit); break;
                case DecComponent: d.component = int(lit); break;
                case DecBuiltIn: d.builtin = int(lit); break;
                case DecBlock: d.block = true; break;
                case DecBufferBlock: d.buffer_block = true; break;
                }
            }
            break;
        case OpMemberDecorate:
            if (n >= 4 && w[2] == DecBuiltIn) dec[w[0]].member_builtins.push_back(w[3]);
            break;
        case OpTypeBool: case OpTypeInt: case OpTypeFloat: case OpTypeVector: case OpTypeMatrix: case OpTypeImage:
        case OpTypeSampler: case OpTypeSampledImage: case OpTypeArray: case OpTypeRuntimeArray: case OpTypeStruct:
        case OpTypePointer:
            if (n >= 1) types[w[0]] = Type{op, std::vector<uint32_t>(w + 1, w + n)};
            break;
        case OpConstant:
            if (n >= 3) constants[w[1]] = w[2];  // low word is enough for array lengths
            break;
        case OpVariable:
            if (n >= 3) vars.push_back({w[1], w[0], w[2]});
            break;
        }
        i += wc;
    }
    if (!have_entry) return fail("no OpEntryPoint");

    auto type = [&](uint32_t id) -> const Type* {
        auto it = types.find(id);
        return it == types.end() ? nullptr : &it->second;
    };
    auto scalar = [&](uint32_t id, uint32_t& comps) -> ScalarType {
        const Type* t = type(id);
        comps = 1;
        if (t && t->op == OpTypeVector && t->w.size() >= 2) comps = t->w[1], t = type(t->w[0]);
        if (!t) return ScalarType::Other;
        if (t->op == OpTypeFloat) return ScalarType::Float;
        if (t->op == OpTypeBool) return ScalarType::Bool;
        if (t->op == OpTypeInt && t->w.size() >= 2) return t->w[1] ? ScalarType::Sint : ScalarType::Uint;
        return ScalarType::Other;
    };
    auto builtins_of = [&](uint32_t var_id, uint32_t pointee, std::vector<uint32_t>& list) {
        auto add = [&](uint32_t b) {
            if (std::find(list.begin(), list.end(), b) == list.end()) list.push_back(b);
        };
        if (auto it = dec.find(var_id); it != dec.end() && it->second.builtin >= 0) add(uint32_t(it->second.builtin));
        // gl_PerVertex-style blocks, possibly arrays of them
        for (const Type* t = type(pointee); t;) {
            if (t->op == OpTypeArray && !t->w.empty()) {
                pointee = t->w[0];
                t = type(pointee);
                continue;
            }
            if (t->op == OpTypeStruct)
                if (auto it = dec.find(pointee); it != dec.end())
                    for (uint32_t b : it->second.member_builtins) add(b);
            break;
        }
    };

    for (const Var& v : vars) {
        const Type* ptr = type(v.ptr_type);
        if (!ptr || ptr->op != OpTypePointer || ptr->w.size() < 2) continue;
        uint32_t pointee = ptr->w[1];
        const Decorations* d = nullptr;
        if (auto it = dec.find(v.id); it != dec.end()) d = &it->second;

        if (v.storage == ScInput || v.storage == ScOutput) {
            auto& list = v.storage == ScInput ? out.builtins_in : out.builtins_out;
            builtins_of(v.id, pointee, list);
            if (!d || d->location < 0 || d->builtin >= 0) continue;
            ShaderIo io;
            io.location = uint32_t(d->location);
            io.component = uint32_t(d->component);
            uint32_t elem = pointee;
            if (const Type* t = type(elem); t && t->op == OpTypeArray && t->w.size() >= 2) {
                io.array = constants.count(t->w[1]) ? constants[t->w[1]] : 1;
                elem = t->w[0];
            }
            io.type = scalar(elem, io.components);
            (v.storage == ScInput ? out.inputs : out.outputs).push_back(io);
            continue;
        }
        if (v.storage != ScUniformConstant && v.storage != ScUniform && v.storage != ScStorageBuffer) continue;
        if (!d || d->set < 0 || d->binding < 0) continue;

        DescriptorBinding b;
        b.set = uint32_t(d->set);
        b.binding = uint32_t(d->binding);
        if (auto it = names.find(v.id); it != names.end()) b.name = it->second;
        uint32_t inner = pointee;
        for (const Type* t = type(inner); t && (t->op == OpTypeArray || t->op == OpTypeRuntimeArray); t = type(inner)) {
            if (t->op == OpTypeRuntimeArray) b.count = 0;
            else if (t->w.size() >= 2) b.count *= constants.count(t->w[1]) ? constants[t->w[1]] : 1;
            inner = t->w.empty() ? 0 : t->w[0];
        }
        const Type* t = type(inner);
        if (!t) return fail("descriptor of unknown type");
        auto image_shape = [&](const Type* img) {
            ImageShape s;
            if (img && img->op == OpTypeImage && img->w.size() >= 7) {
                uint32_t c;
                s.component = scalar(img->w[0], c);
                s.dim = uint8_t(img->w[1]);
                s.depth = img->w[2] == 1, s.arrayed = img->w[3] != 0, s.ms = img->w[4] != 0;
                s.sampled = uint8_t(img->w[5]);
                s.format = img->w[6];
            }
            return s;
        };
        if (t->op == OpTypeStruct) {
            const Decorations* sd = nullptr;
            if (auto it = dec.find(inner); it != dec.end()) sd = &it->second;
            bool storage = v.storage == ScStorageBuffer || (sd && sd->buffer_block);
            b.type = storage ? DescriptorType::StorageBuffer : DescriptorType::UniformBuffer;
        } else if (t->op == OpTypeSampledImage) {
            b.image = image_shape(t->w.empty() ? nullptr : type(t->w[0]));
            b.type = b.image.dim == kDimBuffer ? DescriptorType::UniformTexelBuffer : DescriptorType::CombinedImageSampler;
        } else if (t->op == OpTypeImage) {
            b.image = image_shape(t);
            bool storage = b.image.sampled == 2;
            b.type = b.image.dim == kDimBuffer ? (storage ? DescriptorType::StorageTexelBuffer : DescriptorType::UniformTexelBuffer)
                                               : (storage ? DescriptorType::StorageImage : DescriptorType::SampledImage);
        } else if (t->op == OpTypeSampler) {
            b.type = DescriptorType::Sampler;
        } else {
            return fail("descriptor of unexpected type");
        }
        derive_meaning(b, out.stage);
        if (b.nvn.source == NvnSource::SupportBuffer) out.uses_support_buffer = true;
        out.bindings.push_back(std::move(b));
    }
    std::sort(out.bindings.begin(), out.bindings.end(), [](const DescriptorBinding& a, const DescriptorBinding& b) {
        return a.set != b.set ? a.set < b.set : a.binding < b.binding;
    });
    auto by_location = [](const ShaderIo& a, const ShaderIo& b) {
        return a.location != b.location ? a.location < b.location : a.component < b.component;
    };
    std::sort(out.inputs.begin(), out.inputs.end(), by_location);
    std::sort(out.outputs.begin(), out.outputs.end(), by_location);
    return true;
}

// ---- harness records --------------------------------------------------------------------------------------------------

bool parse_harness_resources(std::string_view text, std::vector<HarnessResource>& out) {
    out.clear();
    while (!text.empty()) {
        size_t sp = text.find(' ');
        std::string_view tok = text.substr(0, sp);
        text = sp == std::string_view::npos ? std::string_view{} : text.substr(sp + 1);
        if (tok.empty()) continue;
        // "...=s<set>b<binding>"
        size_t eq = tok.rfind('=');
        if (eq == std::string_view::npos || eq + 1 >= tok.size() || tok[eq + 1] != 's') return false;
        std::string_view sb = tok.substr(eq + 2);
        size_t bpos = sb.find('b');
        HarnessResource r;
        if (bpos == std::string_view::npos || !parse_uint(sb.substr(0, bpos), 10, r.set) ||
            !parse_uint(sb.substr(bpos + 1), 10, r.binding))
            return false;
        std::string_view lhs = tok.substr(0, eq);
        if (lhs.substr(0, 2) == "cb") {
            r.kind = HarnessResource::Uniform;
            if (!parse_uint(lhs.substr(2), 10, r.bank)) return false;
        } else if (lhs.substr(0, 4) == "sb[c") {
            // sb[c<slot>+0x<off>]
            r.kind = HarnessResource::Storage;
            size_t plus = lhs.find("+0x"), close = lhs.find(']');
            if (plus == std::string_view::npos || close == std::string_view::npos) return false;
            if (!parse_uint(lhs.substr(4, plus - 4), 10, r.bank) || !parse_uint(lhs.substr(plus + 3, close - plus - 3), 16, r.word))
                return false;
        } else if (lhs.substr(0, 3) == "tex" || lhs.substr(0, 3) == "img") {
            // tex<type>[c<slot>:<handle>(x<n>)]
            r.kind = lhs[0] == 't' ? HarnessResource::Texture : HarnessResource::Image;
            size_t open = lhs.find("[c"), colon = lhs.find(':'), close = lhs.find(']');
            if (open == std::string_view::npos || colon == std::string_view::npos || close == std::string_view::npos)
                return false;
            r.type = std::string(lhs.substr(3, open - 3));
            std::string_view bank = lhs.substr(open + 2, colon - open - 2);
            bool neg = !bank.empty() && bank[0] == '-';
            if (!parse_uint(neg ? bank.substr(1) : bank, 10, r.bank)) return false;
            if (neg) r.bank = -r.bank;
            std::string_view handle = lhs.substr(colon + 1, close - colon - 1);
            size_t x = handle.find('x');
            if (x != std::string_view::npos) {
                if (!parse_uint(handle.substr(x + 1), 10, r.array_length)) return false;
                handle = handle.substr(0, x);
            }
            if (!parse_uint(handle, 10, r.word)) return false;
        } else {
            return false;
        }
        out.push_back(std::move(r));
    }
    return true;
}

size_t parse_results_tsv(std::string_view text, std::unordered_map<std::string, std::string>& out) {
    size_t lines = 0;
    int resources_col = -1;
    while (!text.empty()) {
        size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        std::vector<std::string_view> cols;
        for (size_t start = 0;;) {
            size_t tab = line.find('\t', start);
            cols.push_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
            if (tab == std::string_view::npos) break;
            start = tab + 1;
        }
        if (resources_col < 0) {  // header line names the columns
            for (size_t i = 0; i < cols.size(); i++)
                if (cols[i] == "resources") resources_col = int(i);
            if (resources_col < 0) return 0;
            continue;
        }
        if (cols.size() <= size_t(resources_col) || cols[0].empty()) continue;
        out[std::string(cols[0])] = std::string(cols[resources_col]);
        lines++;
    }
    return lines;
}

bool apply_harness(ShaderReflection& refl, const std::vector<HarnessResource>& resources) {
    bool ok = true;
    for (const HarnessResource& r : resources) {
        DescriptorBinding* b = nullptr;
        for (auto& x : refl.bindings)
            if (int(x.set) == r.set && int(x.binding) == r.binding) b = &x;
        if (!b) {
            ok = false;
            continue;
        }
        switch (r.kind) {
        case HarnessResource::Storage:
            if (b->type != DescriptorType::StorageBuffer || r.bank != 0) {
                ok = false;
                break;
            }
            set_c0_offset(b->nvn, refl.stage, r.word * 4);  // the harness prints 32-bit words
            break;
        case HarnessResource::Texture:
        case HarnessResource::Image:
            if (b->nvn.word < 0) set_texture_handle(b->nvn, r.kind == HarnessResource::Image, r.bank, r.word);
            break;
        case HarnessResource::Uniform:
            break;  // name and binding already say it
        }
    }
    return ok;
}

std::string sampler_type_text(const ImageShape& s) {
    static const char* kDims[] = {"Texture1D", "Texture2D", "Texture3D", "TextureCube", "?", "TextureBuffer"};
    std::string t = s.dim < 6 ? kDims[s.dim] : "?";
    if (s.arrayed) t += "|Array";
    if (s.ms) t += "|Multisample";
    if (s.depth) t += "|Shadow";
    return t;
}

std::string describe_like_harness(const ShaderReflection& refl) {
    std::string out;
    char tmp[160];
    for (const auto& b : refl.bindings) {
        const NvnMeaning& m = b.nvn;
        tmp[0] = 0;
        switch (b.type) {
        case DescriptorType::UniformBuffer:
            if (m.source != NvnSource::SupportBuffer) std::snprintf(tmp, sizeof tmp, "cb%d=s%ub%u", m.bank, b.set, b.binding);
            break;
        case DescriptorType::StorageBuffer:
            if (m.c0_offset >= 0) std::snprintf(tmp, sizeof tmp, "sb[c0+0x%x]=s%ub%u", m.c0_offset / 4, b.set, b.binding);
            else std::snprintf(tmp, sizeof tmp, "sb[?]=s%ub%u", b.set, b.binding);
            break;
        default: {
            bool image = b.type == DescriptorType::StorageImage || b.type == DescriptorType::StorageTexelBuffer;
            std::string arr = b.count != 1 ? "x" + std::to_string(b.count) : "";
            std::snprintf(tmp, sizeof tmp, "%s%s[c%d:%d%s]=s%ub%u", image ? "img" : "tex", sampler_type_text(b.image).c_str(),
                          m.default_bank ? -1 : m.bank, m.word, arr.c_str(), b.set, b.binding);
        }
        }
        if (!tmp[0]) continue;
        if (!out.empty()) out += ' ';
        out += tmp;
    }
    return out;
}

// ---- support buffer --------------------------------------------------------------------------------------------------

SupportBuffer support_buffer_defaults() {
    SupportBuffer sb;
    std::memset(&sb, 0, sizeof sb);
    for (auto& s : sb.render_scale) s[0] = 1.0f;  // Ryujinx's SupportBufferUpdater: (1, 0, 0, 0) for all 73
    return sb;
}

void support_buffer_set_viewport(SupportBuffer& sb, float width, float height, bool transform_disabled) {
    const float scale = 1.0f;  // no resolution scaling
    sb.viewport_inverse[0] = width != 0 ? scale * 2.0f / width : 0.0f;
    sb.viewport_inverse[1] = height != 0 ? scale * 2.0f / height : 0.0f;
    sb.viewport_inverse[2] = 1.0f;
    sb.viewport_inverse[3] = transform_disabled ? 1.0f : 0.0f;
    sb.viewport_size[0] = width;
    sb.viewport_size[1] = height < 0 ? -height : height;
    sb.viewport_size[2] = 1.0f;
    sb.viewport_size[3] = 0.0f;
}

// ---- one stage ------------------------------------------------------------------------------------------------------

bool make_shader_info(const uint8_t* code, const uint8_t* control, std::vector<uint32_t> spirv,
                      const std::string* resources, ShaderInfo& out, std::string* error) {
    auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    out = ShaderInfo{};
    out.control = read_control(control);
    const ShaderControl& c = out.control;
    if (!c.valid) return fail("control section without magic 0x98761234");
    if (c.code_size == 0 || c.code_size > 16u << 20) return fail("implausible code block size");
    out.hash = shader_hash(code, c.code_size);
    if (!reflect_spirv(spirv.data(), spirv.size(), out.refl, error)) return false;
    out.stage = out.refl.stage;
    out.spirv = std::move(spirv);
    if (c.const_size) {
        if (uint64_t(c.const_offset) + c.const_size > c.code_size) return fail("constant data outside the code block");
        out.constants.assign(code + c.const_offset, code + c.const_offset + c.const_size);
    }
    if (out.stage == ShaderStage::Compute)
        for (int i = 0; i < 3; i++) out.local_size[i] = std::max<uint32_t>(1, c.local_size[i]);
    if (resources) {
        std::vector<HarnessResource> list;
        if (!parse_harness_resources(*resources, list)) return fail("unreadable resources record");
        if (!apply_harness(out.refl, list)) return fail("resources record does not match the SPIR-V");
    }
    return true;
}

const char* stage_name(ShaderStage s) {
    switch (s) {
    case ShaderStage::Vertex: return "vertex";
    case ShaderStage::Fragment: return "fragment";
    case ShaderStage::Compute: return "compute";
    default: return "unknown";
    }
}

const char* source_name(NvnSource s) {
    switch (s) {
    case NvnSource::SupportBuffer: return "support buffer";
    case NvnSource::DriverConstants: return "c[0] driver constants";
    case NvnSource::ConstantData: return "c[1] constant data";
    case NvnSource::UniformBlock: return "uniform block";
    case NvnSource::StorageBuffer: return "storage buffer";
    case NvnSource::UniformAsStorage: return "uniform block (as storage)";
    case NvnSource::SamplerBinding: return "sampler binding";
    case NvnSource::ImageBinding: return "image binding";
    case NvnSource::HandleInUniform: return "handle in uniform block";
    default: return "unknown";
    }
}

// ---- the runtime cache ------------------------------------------------------------------------------------------------
#ifndef NX_SHADERS_STANDALONE

static_assert(uint32_t(DescriptorType::Sampler) == VK_DESCRIPTOR_TYPE_SAMPLER &&
                  uint32_t(DescriptorType::CombinedImageSampler) == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                  uint32_t(DescriptorType::SampledImage) == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
                  uint32_t(DescriptorType::StorageImage) == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
                  uint32_t(DescriptorType::UniformTexelBuffer) == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
                  uint32_t(DescriptorType::StorageTexelBuffer) == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER &&
                  uint32_t(DescriptorType::UniformBuffer) == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                  uint32_t(DescriptorType::StorageBuffer) == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
              "DescriptorType mirrors VkDescriptorType");

namespace {

bool read_whole(const std::string& path, std::string& out) {
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return false;
    plat::fseek64(f, 0, SEEK_END);
    int64_t size = plat::ftell64(f);
    out.resize(size > 0 ? size_t(size) : 0);
    bool ok = size >= 0 && plat::read_at(f, 0, out.data(), out.size()) == out.size();
    std::fclose(f);
    return ok;
}

struct Cache {
    std::mutex lock;
    std::unordered_map<std::string, std::unique_ptr<ShaderInfo>> by_hash;  // null: no usable .spv (logged once)
    std::unordered_map<std::string, std::string> resources;                // results.tsv, hash -> resources
    bool resources_loaded = false;
};

Cache& cache() {
    static Cache c;
    return c;
}

}  // namespace

const ShaderInfo* shader_for(uint64_t code_host, uint64_t control) {
    if (!code_host || !control) return nullptr;
    auto* ctrl = reinterpret_cast<const uint8_t*>(control);
    ShaderControl c = read_control(ctrl);
    if (!c.valid || c.code_size == 0 || c.code_size > 16u << 20) {
        log("nvn: shader control %llx: bad section (magic %08x, code size %#x)", (unsigned long long)control,
            rd32(ctrl), c.code_size);
        return nullptr;
    }
    auto* code = reinterpret_cast<const uint8_t*>(code_host);
    std::string hash = shader_hash(code, c.code_size);

    Cache& k = cache();
    std::lock_guard lk(k.lock);
    if (auto it = k.by_hash.find(hash); it != k.by_hash.end()) return it->second.get();
    std::string dir = plat::data_root() + "/spirv/";
    if (!k.resources_loaded) {
        k.resources_loaded = true;
        std::string text;
        if (read_whole(dir + "results.tsv", text))
            log("nvn: %zu shader records from %sresults.tsv", parse_results_tsv(text, k.resources), dir.c_str());
        else
            log("nvn: no %sresults.tsv: storage buffers of translated shaders cannot be bound", dir.c_str());
    }
    std::unique_ptr<ShaderInfo>& slot = k.by_hash[hash];
    std::string bytes;
    if (!read_whole(dir + hash + ".spv", bytes) || bytes.size() % 4 || bytes.empty()) {
        log("nvn: shader %s (code %#x bytes, control %llx): no %s%s.spv", hash.c_str(), c.code_size,
            (unsigned long long)control, dir.c_str(), hash.c_str());
        return nullptr;
    }
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    auto res = k.resources.find(hash);
    auto info = std::make_unique<ShaderInfo>();
    std::string error;
    if (!make_shader_info(code, ctrl, std::move(words), res == k.resources.end() ? nullptr : &res->second, *info, &error)) {
        log("nvn: shader %s: %s", hash.c_str(), error.c_str());
        return nullptr;
    }
    if (g_trace)
        log("nvn: shader %s %s: %zu bindings, %zu inputs, %zu outputs, c[1] %u bytes", hash.c_str(),
            stage_name(info->stage), info->refl.bindings.size(), info->refl.inputs.size(), info->refl.outputs.size(),
            uint32_t(info->constants.size()));
    slot = std::move(info);
    return slot.get();
}

#endif  // NX_SHADERS_STANDALONE

}  // namespace nx::nvn
