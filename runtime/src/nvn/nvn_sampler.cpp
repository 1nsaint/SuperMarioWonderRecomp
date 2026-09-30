// NVN sampler states as Vulkan samplers (split from nvn_pipeline.cpp).
#include <algorithm>
#include <map>
#include <string>

#include "nvn_pipeline.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

VkSamplerAddressMode vk_wrap(uint32_t w) {  // NVN: 0 clamp, 1 repeat, 2 mirror clamp, 3 mirror clamp to edge,
    switch (w) {                            // 4 mirror clamp to border, 5 clamp to border, 6 mirrored repeat, 7 edge
        case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 2:
        case 3:
        case 4: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

}  // namespace

// ---- samplers -------------------------------------------------------------------------------------------------------

VkSampler sampler_for(const SamplerInfo& s, bool shadow) {
    static std::map<std::string, VkSampler> cache;
    std::string key(reinterpret_cast<const char*>(&s), sizeof s);
    key += shadow ? '1' : '0';
    auto& v = cache[key];
    if (v) return v;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    // NVN min filter: 0 nearest, 1 linear, 2 nearest mip nearest, 3 linear mip nearest, 4 nearest mip linear,
    // 5 linear mip linear; mag: 0 nearest, 1 linear.
    uint32_t m = s.min_filter;
    ci.minFilter = (m == 1 || m == 3 || m == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.magFilter = s.mag_filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = (m == 4 || m == 5) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = vk_wrap(s.wrap[0]);
    ci.addressModeV = vk_wrap(s.wrap[1]);
    ci.addressModeW = vk_wrap(s.wrap[2]);
    ci.mipLodBias = s.lod_bias;
    // Games pass lod_min -1000 ("no limit"); Metal wants lodMinClamp >= 0, and a negative one made the glyph
    // sampler return 0 (all text invisible).
    ci.minLod = m <= 1 ? 0.0f : std::max(0.0f, s.lod_min);
    ci.maxLod = m <= 1 ? 0.25f : std::max(ci.minLod, s.lod_max);  // no mipmapping: level 0 only
    ci.anisotropyEnable = s.max_anisotropy > 1.0f;
    ci.maxAnisotropy = std::min(s.max_anisotropy, 16.0f);
    ci.compareEnable = shadow;
    ci.compareOp = shadow ? vk_compare(s.compare_func) : VK_COMPARE_OP_ALWAYS;
    bool white = s.border[3] > 0.5f && s.border[0] > 0.5f;
    ci.borderColor = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                     : s.border[3] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                          : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    p_vkCreateSampler(vk_device(), &ci, nullptr, &v);
    return v;
}

}  // namespace nx::nvn
