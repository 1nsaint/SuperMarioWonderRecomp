// Per-submit upload reuse (docs/nvn/DEBUG.md #30). A draw's buffers are copied from guest memory into the upload
// ring when it is replayed; the same camera block, scene constants and vertex buffers were copied again for every
// draw that bound them (~130 KB per draw, 50-70 MB per frame). Within one submit guest memory only changes through
// inline uniform updates (UpdateUniformBuffer, which invalidates what it overlaps) and compute results (written back
// after the submit, so every draw of the submit reads the same pre-dispatch bytes either way): a copy made once can
// serve every later draw of the submit that binds the same bytes.
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>

#include "nvn_draw_state.h"

namespace nx::nvn::replay {

namespace {

struct Entry {
    uint64_t size = 0, padded = 0;
    Upload upload;
};
// By host address; several sizes of one address are rare, so the latest one is kept.
std::map<const uint8_t*, Entry> g_copies;
uint64_t g_saved = 0, g_copied = 0;

}  // namespace

Upload dedup_upload(const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align) {
    if (!p || !size) {
        Upload u = upload_alloc(padded, align);
        std::memset(u.ptr, 0, padded);
        return u;
    }
    static const bool off = std::getenv("NX_NO_DEDUP") != nullptr;  // bisecting: a fresh copy every time
    auto it = off ? g_copies.end() : g_copies.find(p);
    if (it != g_copies.end() && it->second.size == size && it->second.padded == padded &&
        it->second.upload.offset % align == 0) {
        g_saved += padded;
        return it->second.upload;
    }
    Upload u = upload_alloc(padded, align);
    std::memcpy(u.ptr, p, size);
    if (padded > size) std::memset(u.ptr + size, 0, padded - size);
    g_copies[p] = {size, padded, u};
    g_copied += padded;
    return u;
}

void dedup_reset() { g_copies.clear(); }

void dedup_invalidate(const uint8_t* p, uint64_t size) {
    // Entries starting before p may still reach into it: look back over the few that can.
    auto it = g_copies.lower_bound(p + size);
    while (it != g_copies.begin()) {
        --it;
        if (it->first + it->second.size <= p) {
            if (p - it->first > (64ll << 20)) break;  // nothing larger than 64 MB is copied
            continue;
        }
        it = g_copies.erase(it);
    }
}

// c[0], the NVN driver's constants, as far as shaders read them (docs/nvn/TRANSLATOR.md): buffer descriptors
// {u64 GPU address; u32 size} of what is bound, which compute shaders search to find the buffer an address falls in
// (they wrote garbage with a zero table), and the maximum point size.
std::array<uint8_t, 0x1000> driver_constants(ShaderStage stage, int st) {
    std::array<uint8_t, 0x1000> c0{};
    auto put = [&](uint32_t at, const Range& r) {
        if (at + 12 > c0.size()) return;
        std::memcpy(c0.data() + at, &r.address, 8);
        uint32_t size = uint32_t(std::min<uint64_t>(r.size, 0xFFFFFFFFu));
        std::memcpy(c0.data() + at + 8, &size, 4);
    };
    if (stage == ShaderStage::Compute) {
        for (int n = 0; n < 16; n++) put(0x210 + 0x10 * n, g_state.uniforms[st][n]);
        for (int n = 0; n < 16; n++) put(0x310 + 0x10 * n, g_state.storage[st][n]);
    } else {
        int hw = stage == ShaderStage::Fragment ? 4 : 0;  // Maxwell stage numbers: vertex 0, fragment 4
        for (int n = 0; n < 16; n++) put(0x110 + 0x100 * hw + 0x10 * n, g_state.storage[st][n]);
        float max_point = 64.0f;
        std::memcpy(c0.data() + 0x610, &max_point, 4);
    }
    return c0;
}

void dedup_log() {
    if (g_copied + g_saved)
        log("nvn: uploads copied %.1f MB, reused %.1f MB", double(g_copied) / 1e6, double(g_saved) / 1e6);
    g_copied = g_saved = 0;
}

}  // namespace nx::nvn::replay
