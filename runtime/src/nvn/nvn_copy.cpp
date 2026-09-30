// NVN copies between textures (docs/NVN.md, N6). nvnCommandBufferCopyTextureToTexture(cmd, const NVNtexture* src,
// const NVNtextureView* srcView, const NVNcopyRegion* srcRegion, const NVNtexture* dst, const NVNtextureView* dstView,
// const NVNcopyRegion* dstRegion, int copyFlags) (Wonder's call sites). NVNcopyRegion {int x, y, z, width, height,
// depth}; for array textures z/depth are layers. Flags: 1 linear filter, 4/8/16 mirror x/y/z (2 selects the 2D
// engine: no difference here). Replayed as a vkCmdCopyImage when formats and sizes match, else a vkCmdBlitImage.
// Wonder copies its frame into a texture for screen transitions (the opening's cloud wipe).
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "nvn_pipeline.h"
#include "nvn_vk.h"

namespace nx::nvn {

using namespace vk;

namespace {

struct CopyInfo {
    ViewInfo src_view, dst_view;
    int32_t src[6] = {}, dst[6] = {};
    uint32_t flags = 0;
};

void CopyTextureToTexture(CPU& c) {
    CopyInfo info;
    info.src_view = read_view(c, c.x[2]);
    info.dst_view = read_view(c, c.x[5]);
    for (int k = 0; k < 6; k++) {
        info.src[k] = c.x[3] ? ld<int32_t>(c.x[3] + 4ull * k) : 0;
        info.dst[k] = c.x[6] ? ld<int32_t>(c.x[6] + 4ull * k) : 0;
    }
    info.flags = uint32_t(c.x[7]);
    Cmd cmd{Cmd::CopyTextureToTexture};
    cmd.a[0] = c.x[1], cmd.a[1] = c.x[4];
    record(c.x[0], cmd, &info, sizeof info);
}

bool compressed(VkFormat f) {
    return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
}

}  // namespace

void replay_copy(VkCommandBuffer cb, const Cmd& c, const CmdList& list) {
    static const bool off = std::getenv("NX_NO_COPY") != nullptr;  // debug: skip texture copies
    if (off) return;
    if (c.blob >= list.blobs.size() || list.blobs[c.blob].size() != sizeof(CopyInfo)) return;
    CopyInfo info;
    std::memcpy(&info, list.blobs[c.blob].data(), sizeof info);
    GpuImage* src = ensure_contents(cb, c.a[0]);
    GpuImage* dst = ensure_contents(cb, c.a[1]);
    if (!src || !dst || src == dst) return;
    uint32_t sl = std::min(info.src_view.base_level, src->levels - 1), dl = std::min(info.dst_view.base_level, dst->levels - 1);
    bool s3d = src->view_type == VK_IMAGE_VIEW_TYPE_3D, d3d = dst->view_type == VK_IMAGE_VIEW_TYPE_3D;
    auto sub = [](const GpuImage& img, uint32_t level, int32_t z, int32_t depth, uint32_t view_layer, bool is3d) {
        VkImageAspectFlags aspect = img.aspect;
        uint32_t layer = is3d ? 0 : std::min<uint32_t>(uint32_t(std::max(z, 0)) + view_layer, img.layers - 1);
        uint32_t count = is3d ? 1 : std::clamp<uint32_t>(uint32_t(std::max(depth, 1)), 1, img.layers - layer);
        return VkImageSubresourceLayers{aspect, level, layer, count};
    };
    VkImageSubresourceLayers ss = sub(*src, sl, info.src[2], info.src[5], info.src_view.min_layer, s3d);
    VkImageSubresourceLayers ds = sub(*dst, dl, info.dst[2], info.dst[5], info.dst_view.min_layer, d3d);
    static const bool trace_cubes = std::getenv("NX_TRACE_CUBES") != nullptr;
    static int cube_logs = 0;
    if (trace_cubes && (dst->view_type == VK_IMAGE_VIEW_TYPE_CUBE || dst->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) &&
        cube_logs++ < 400)
        log("cubes: copy %llx -> %llx level %u layer %u+%u (z %d depth %d view layer %u) %dx%d", (unsigned long long)c.a[0],
            (unsigned long long)c.a[1], ds.mipLevel, ds.baseArrayLayer, ds.layerCount, info.dst[2], info.dst[5],
            info.dst_view.min_layer, info.dst[3], info.dst[4]);
    transition(cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cb, *dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    int32_t sw = std::max(info.src[3], 1), sh = std::max(info.src[4], 1);
    int32_t dw = std::max(info.dst[3], 1), dh = std::max(info.dst[4], 1);
    bool mirror = info.flags & (4 | 8 | 16);
    if (src->format == dst->format && sw == dw && sh == dh && !mirror && ss.layerCount == ds.layerCount) {
        VkImageCopy r{};
        r.srcSubresource = ss, r.dstSubresource = ds;
        r.srcOffset = {info.src[0], info.src[1], s3d ? info.src[2] : 0};
        r.dstOffset = {info.dst[0], info.dst[1], d3d ? info.dst[2] : 0};
        r.extent = {uint32_t(sw), uint32_t(sh), s3d ? uint32_t(std::max(info.src[5], 1)) : 1u};
        p_vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    } else if (!compressed(src->format) && !compressed(dst->format) && src->aspect == dst->aspect) {
        VkImageBlit b{};
        b.srcSubresource = ss, b.dstSubresource = ds;
        ds.layerCount = ss.layerCount = std::min(ss.layerCount, ds.layerCount);
        b.srcSubresource.layerCount = b.dstSubresource.layerCount = ss.layerCount;
        b.srcOffsets[0] = {info.src[0], info.src[1], 0};
        b.srcOffsets[1] = {info.src[0] + sw, info.src[1] + sh, 1};
        b.dstOffsets[0] = {info.dst[0], info.dst[1], 0};
        b.dstOffsets[1] = {info.dst[0] + dw, info.dst[1] + dh, 1};
        if (info.flags & 4) std::swap(b.dstOffsets[0].x, b.dstOffsets[1].x);
        if (info.flags & 8) std::swap(b.dstOffsets[0].y, b.dstOffsets[1].y);
        bool linear = (info.flags & 1) && src->aspect == VK_IMAGE_ASPECT_COLOR_BIT;
        p_vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else {
        static int logged = 0;
        if (logged++ < 5)
            log("nvn: copy %llx -> %llx not replayed (formats %d -> %d, %dx%d -> %dx%d)", (unsigned long long)c.a[0],
                (unsigned long long)c.a[1], int(src->format), int(dst->format), sw, sh, dw, dh);
        return;
    }
    dst->rendered = true;  // GPU-written from now on
}

NX_HLE("nvnCommandBufferCopyTextureToTexture", CopyTextureToTexture);

}  // namespace nx::nvn
