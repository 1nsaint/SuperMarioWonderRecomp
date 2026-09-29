// Draw traces (docs/nvn/DEBUG.md): NX_TRACE_DRAWS=<first draw> logs draws with their shaders, targets, textures and
// state; NX_TRACE_TARGET, NX_TRACE_FS_HASH, NX_TRACE_COUNT select them; NX_TRACE_VS_BLOCKS and NX_TRACE_GLYPHS add
// uniform blocks and glyph records. Moved out of nvn_draw.cpp, which calls trace_draw() after every draw.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "nvn_draw_state.h"

namespace nx::nvn::replay {

using namespace nx::vk;

void trace_draw(const Targets& t, uint32_t mode, bool indexed, const Cmd& c, const ShaderInfo* vs,
                const ShaderInfo* fs, const TexBindings& tex_bindings, const std::vector<uint64_t>& handles) {
    // Always: the first draws with a huge vertex or instance count (a GPU hang candidate).
    static int big = 0;
    uint32_t count = uint32_t(indexed ? c.i[0] : c.i[1]), instances = uint32_t(std::max(1, c.i[3]));
    if ((count > (1u << 18) || instances > 1024 || uint64_t(count) * instances > (1u << 20)) && big++ < 40)
        log("nvn: huge draw %llu: vs %s fs %s %s count %u x%u", (unsigned long long)g_stats.done, vs->hash.c_str(),
            fs ? fs->hash.c_str() : "-", indexed ? "indexed" : "arrays", count, instances);
    // NX_TRACE_DRAWS=<first draw>: logs 80 draws from there.
    static const char* trace_env = std::getenv("NX_TRACE_DRAWS");
    static const uint64_t trace_from = trace_env ? std::strtoull(trace_env, nullptr, 10) : 0;
    // NX_TRACE_TARGET=<hex NVNtexture*>: only draws into that color target.
    static const char* target_env = std::getenv("NX_TRACE_TARGET");
    static const uint64_t trace_target = target_env ? std::strtoull(target_env, nullptr, 16) : 0;
    static uint64_t traced = 0;
    // NX_TRACE_FS_HASH=<hash>: only draws with that fragment shader.
    static const char* fs_env = std::getenv("NX_TRACE_FS_HASH");
    static const uint64_t trace_count = std::getenv("NX_TRACE_COUNT") ? std::strtoull(std::getenv("NX_TRACE_COUNT"), nullptr, 10) : 80;
    if (trace_env && g_stats.done >= trace_from && traced < trace_count && (!trace_target || t.colors[0] == trace_target) &&
        (!fs_env || (fs && fs->hash == fs_env))) {
        traced++;
        std::string tex;
        for (size_t k = 0; k < tex_bindings.size(); k++) {
            uint64_t texture = 0;
            SamplerInfo si;
            texture_for(handles[k], texture, si);
            Texture* tx = texture ? get<Texture>(texture) : nullptr;
            char b[96];
            if (std::getenv("NX_TRACE_GLYPHS"))
                log("  sampler for t%zu: min %u mag %u wrap %u/%u/%u compare %u/%u reduction %u lod %g..%g bias %g aniso %g",
                    k, si.min_filter, si.mag_filter, si.wrap[0], si.wrap[1], si.wrap[2], si.compare_mode, si.compare_func,
                    si.reduction, si.lod_min, si.lod_max, si.lod_bias, si.max_anisotropy);
            std::snprintf(b, sizeof b, " t%zu=%llx:%llx(%ux%u f%x%s)", k, (unsigned long long)handles[k],
                          (unsigned long long)texture, tx ? tx->width : 0, tx ? tx->height : 0, tx ? tx->format : 0,
                          tx && tx->image && tx->image->rendered ? " rt" : "");
            tex += b;
        }
        if (std::getenv("NX_TRACE_VS_BLOCKS"))
            if (const Range& u = g_state.uniforms[0][0]; u.address)
                if (auto* f = reinterpret_cast<const float*>(guest_bytes(u.address)))
                    for (int row = 0; row < std::min<int>(int(u.size / 16), 24); row++)
                        log("  vs c3[%d]: %g %g %g %g", row, f[4 * row], f[4 * row + 1], f[4 * row + 2], f[4 * row + 3]);
        for (int blk = 1; blk <= 3 && std::getenv("NX_TRACE_VS_BLOCKS"); blk++)
            if (const Range& u = g_state.uniforms[0][blk]; u.address)
                if (auto* f = reinterpret_cast<const float*>(guest_bytes(u.address)))
                    log("  vs c%d: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", blk + 3, f[0], f[1], f[2], f[3],
                        f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
        if (const Range& u = g_state.uniforms[0][0]; u.address)
            if (auto* f = reinterpret_cast<const float*>(guest_bytes(u.address)))
                log("  vs c3 @%llx: %g %g %g %g | %g %g %g %g", (unsigned long long)u.address, f[0], f[1], f[2], f[3], f[4],
                    f[5], f[6], f[7]);
        if (const Range& u = g_state.uniforms[1][0]; u.address)
            if (auto* f = reinterpret_cast<const float*>(guest_bytes(u.address)))
                for (uint64_t k = 0; k < std::min<uint64_t>(u.size / 16, 8); k++)
                    log("  fs c3[%llu] @%llx: %g %g %g %g", (unsigned long long)k, (unsigned long long)u.address,
                        f[4 * k], f[4 * k + 1], f[4 * k + 2], f[4 * k + 3]);
        if (const Range& r = g_state.storage[0][0]; r.address && std::getenv("NX_TRACE_GLYPHS"))
            if (auto* g = guest_bytes(r.address))
                for (uint64_t k = 0; k < std::min<uint64_t>(r.size / 64, 3); k++) {
                    const uint32_t* w = reinterpret_cast<const uint32_t*>(g + 64 * k);
                    if (Texture* at = get<Texture>(0x4383061d8ull); at && at->memory && at->stride) {
                        float u0, v0, u1, v1;
                        std::memcpy(&u0, w + 4, 4), std::memcpy(&v0, w + 5, 4), std::memcpy(&u1, w + 6, 4), std::memcpy(&v1, w + 7, 4);
                        uint32_t x = uint32_t((u0 + u1) * 0.5f * at->width), y = uint32_t((v0 + v1) * 0.5f * at->height);
                        const uint8_t* m = reinterpret_cast<const uint8_t*>(at->memory);
                        uint32_t nz = 0, mx = 0;
                        for (int dy = -20; dy <= 20; dy++)
                            for (int dx = -20; dx <= 20; dx++) {
                                uint8_t v = m[uint64_t(y + dy) * at->stride + x + dx];
                                nz += v != 0, mx = std::max<uint32_t>(mx, v);
                            }
                        log("  glyph %llu atlas memory at %u,%u: %u of 1681 non-zero, max %u", (unsigned long long)k, x, y, nz, mx);
                    }
                    log("  glyph %llu: %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
                        (unsigned long long)k, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11],
                        w[12], w[13], w[14], w[15]);
                }
        for (int k = 0; k < 2; k++)
            if (const Range& r = g_state.storage[0][k]; r.address) {
                auto* f = reinterpret_cast<const float*>(guest_bytes(r.address));
                log("  vs s%d @%llx+%llx: %g %g %g %g %g %g %g %g", k, (unsigned long long)r.address,
                    (unsigned long long)r.size, f ? f[0] : -1, f ? f[1] : -1, f ? f[2] : -1, f ? f[3] : -1,
                    f ? f[4] : -1, f ? f[5] : -1, f ? f[6] : -1, f ? f[7] : -1);
            }
        {
            const auto& k = vs->constants;
            std::string w;
            for (size_t i = 0; i + 4 <= std::min<size_t>(k.size(), 256); i += 4) {
                char t[16];
                uint32_t v;
                std::memcpy(&v, k.data() + i, 4);
                std::snprintf(t, sizeof t, " %x", v);
                w += t;
            }
            log("  vs constants %zu bytes:%s", k.size(), w.c_str());
        }
        if (fs) {
            const auto& k = fs->constants;
            const float* f = reinterpret_cast<const float*>(k.data());
            log("  fs constants %zu bytes: %g %g %g %g", k.size(), k.size() >= 4 ? f[0] : -1, k.size() >= 8 ? f[1] : -1,
                k.size() >= 12 ? f[2] : -1, k.size() >= 16 ? f[3] : -1);
        }
        const BlendTarget& b0 = g_state.blend[0];
        log("  blend %u (%u %u %u %u eq %u %u) mask %x logic %x alpha %x fmt %d", g_state.color.blend_enable[0], b0.src_rgb,
            b0.dst_rgb, b0.src_a, b0.dst_a, b0.eq_rgb, b0.eq_a, g_state.mask.mask[0], g_state.color.logic_op,
            g_state.color.alpha_test, t.colors[0] ? int(image_of(t.colors[0])->format) : -1);
        log("  depth func %u write %u", g_state.depth.depth_func, g_state.depth.depth_write);
        log("  viewport %d %d %d %d (%d) scissor %d %d %d %d (%d) cull %u front %u depth %g..%g test %u", g_state.viewport[0],
            g_state.viewport[1], g_state.viewport[2], g_state.viewport[3], g_state.has_viewport, g_state.scissor[0],
            g_state.scissor[1], g_state.scissor[2], g_state.scissor[3], g_state.has_scissor, g_state.polygon.cull,
            g_state.polygon.front, g_state.depth_near, g_state.depth_far, g_state.depth.depth_test);
        log("draw %llu: vs %s fs %s mode %u %s count %d x%d targets %u:%llx depth %llx%s", (unsigned long long)g_stats.done,
            vs->hash.c_str(), fs ? fs->hash.c_str() : "-", mode, indexed ? "indexed" : "arrays",
            indexed ? c.i[0] : c.i[1], c.i[3], t.color_count, (unsigned long long)t.colors[0], (unsigned long long)t.depth, tex.c_str());
    }}

}  // namespace nx::nvn::replay
