// Pipelines and sampled resources (nvn_pipeline.h). Everything is cached for the life of the process: layouts by
// their bindings, pipelines by their full state, samplers by their state, views by (image, type).
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>

#include "nvn_pipeline.h"
#include "nvn_tiling.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

VkShaderStageFlags stage_bit(ShaderStage s) {
    return s == ShaderStage::Vertex     ? VK_SHADER_STAGE_VERTEX_BIT
           : s == ShaderStage::Fragment ? VK_SHADER_STAGE_FRAGMENT_BIT
                                        : VK_SHADER_STAGE_COMPUTE_BIT;
}

VkShaderModule module_of(const ShaderInfo* s) {
    static std::unordered_map<const ShaderInfo*, VkShaderModule> cache;
    auto& m = cache[s];
    if (!m) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = s->spirv.size() * 4;
        ci.pCode = s->spirv.data();
        if (p_vkCreateShaderModule(vk_device(), &ci, nullptr, &m) != VK_SUCCESS) log("nvn/vk: shader %s rejected", s->hash.c_str());
    }
    return m;
}

VkDescriptorSetLayout set_layout(const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
    static std::map<std::string, VkDescriptorSetLayout> cache;
    std::string key(reinterpret_cast<const char*>(bindings.data()), bindings.size() * sizeof(bindings[0]));
    auto& l = cache[key];
    if (!l) {
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = uint32_t(bindings.size());
        ci.pBindings = bindings.data();
        p_vkCreateDescriptorSetLayout(vk_device(), &ci, nullptr, &l);
    }
    return l;
}

// ---- samplers -------------------------------------------------------------------------------------------------------

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

// ---- images ---------------------------------------------------------------------------------------------------------

VkImageViewType view_type(const ImageShape& s) {  // SPIR-V Dim: 0 1D, 1 2D, 2 3D, 3 Cube
    switch (s.dim) {
        case 0: return s.arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case 2: return VK_IMAGE_VIEW_TYPE_3D;
        case 3: return s.arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        default: return s.arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

bool identity(const VkComponentMapping& m) {
    auto id = [](VkComponentSwizzle s, VkComponentSwizzle own) { return s == VK_COMPONENT_SWIZZLE_IDENTITY || s == own; };
    return id(m.r, VK_COMPONENT_SWIZZLE_R) && id(m.g, VK_COMPONENT_SWIZZLE_G) && id(m.b, VK_COMPONENT_SWIZZLE_B) &&
           id(m.a, VK_COMPONENT_SWIZZLE_A);
}

// Views Metal accepts of an image: cubes only of cube images with 6 layers per cube, 3D only of 3D images (a 2D
// view of a 3D image is for rendering a slice), 1D of 1D. A shader declaring another shape than the texture it gets
// (a samplerCube given a 2D texture) reads the dummy instead: Metal aborts on the view.
bool view_compatible(const GpuImage& img, VkImageViewType type, uint32_t layers) {
    bool cube_img = img.view_type == VK_IMAGE_VIEW_TYPE_CUBE || img.view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    bool img3d = img.view_type == VK_IMAGE_VIEW_TYPE_3D;
    bool img1d = img.view_type == VK_IMAGE_VIEW_TYPE_1D || img.view_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    switch (type) {
        case VK_IMAGE_VIEW_TYPE_CUBE:
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return cube_img && layers >= 6;
        case VK_IMAGE_VIEW_TYPE_3D: return img3d;
        case VK_IMAGE_VIEW_TYPE_1D:
        case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return img1d;
        default: return !img3d && !img1d;
    }
}

// A view for sampling: the image's own when it fits, else one of the view type with the texture's swizzle.
VkImageView view_of(GpuImage& img, VkImageViewType type, const ViewInfo& view) {
    uint32_t swizzle = view.swizzle;
    VkComponentMapping m = img.swizzle;
    if (swizzle) {
        constexpr VkComponentSwizzle kSwizzle[] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE,
                                                   VK_COMPONENT_SWIZZLE_R,    VK_COMPONENT_SWIZZLE_G,
                                                   VK_COMPONENT_SWIZZLE_B,    VK_COMPONENT_SWIZZLE_A};
        m = {kSwizzle[swizzle & 0xFF], kSwizzle[(swizzle >> 8) & 0xFF], kSwizzle[(swizzle >> 16) & 0xFF],
             kSwizzle[swizzle >> 24]};
    }
    uint32_t base = std::min(view.base_level, img.levels - 1);
    uint32_t levels = view.levels ? std::min(view.levels, img.levels - base) : img.levels - base;
    uint32_t min_layer = std::min(view.min_layer, img.layers - 1);
    bool whole = base == 0 && levels == img.levels && min_layer == 0 && (!view.layers || view.layers == img.layers);
    if (type == img.view_type && identity(m) && whole) return img.view;
    static std::map<std::tuple<VkImage, VkImageViewType, uint32_t, uint64_t, uint64_t>, VkImageView> cache;
    uint64_t range = uint64_t(base) | uint64_t(levels) << 16 | uint64_t(min_layer) << 32 | uint64_t(view.layers) << 48;
    auto& v = cache[{img.image, type, uint32_t(m.r) | uint32_t(m.g) << 8 | uint32_t(m.b) << 16 | uint32_t(m.a) << 24,
                     range, 0}];
    if (!v) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = img.image;
        ci.viewType = type;
        ci.format = img.format;
        ci.components = m;
        uint32_t layers = std::min(view.layers ? view.layers : img.layers - min_layer, img.layers - min_layer);
        if (type == VK_IMAGE_VIEW_TYPE_2D || type == VK_IMAGE_VIEW_TYPE_1D) layers = 1;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE) layers = 6;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) layers -= layers % 6;
        if (!view_compatible(img, type, layers)) return VK_NULL_HANDLE;
        ci.subresourceRange = {img.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                   ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                   : img.aspect,
                               base, levels, min_layer, layers};
        if (p_vkCreateImageView(vk_device(), &ci, nullptr, &v) != VK_SUCCESS) v = img.view;
    }
    return v;
}

// Untiles the texture's pool memory and copies it into the image (every level and layer).
void upload_texture(VkCommandBuffer cb, const Texture& t, GpuImage& img) {
    img.uploaded = true;
    if (!t.memory || !t.storage_size) return;
    uint32_t bw = 1, bh = 1, bytes = 4;
    format_block(t.format, bw, bh, bytes);
    std::vector<uint8_t> linear;
    std::vector<VkBufferImageCopy> copies;
    uint32_t layers = img.view_type == VK_IMAGE_VIEW_TYPE_3D ? std::max(1u, t.depth) : img.layers;
    if (t.stride) {  // pitch-linear: rows as they are
        uint64_t rows = (img.height + bh - 1) / bh;
        Upload u = upload(reinterpret_cast<const void*>(t.memory), uint64_t(t.stride) * rows * layers, 16);
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset;
        r.bufferRowLength = t.stride / bytes * bw;
        r.imageSubresource = {img.aspect, 0, 0, img.layers};
        r.imageExtent = {img.width, img.height, 1};
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        return;
    }
    auto levels = deswizzle_texture(reinterpret_cast<const uint8_t*>(t.memory), t.storage_size, img.width,
                                    img.height, layers, img.levels, bw, bh, bytes, linear, t.block_height_gobs);
    if (levels.empty()) {
        log("nvn/vk: texture %ux%u format %#x: could not untile (%llu bytes)", img.width, img.height, t.format,
            (unsigned long long)t.storage_size);
        return;
    }
    Upload u = upload(linear.data(), linear.size(), 16);
    for (uint32_t k = 0; k < levels.size(); k++) {
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset + levels[k].offset;
        r.imageSubresource = {img.aspect, k, 0, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? 1u : img.layers};
        r.imageExtent = {levels[k].width, levels[k].height, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? layers : 1u};
        copies.push_back(r);
    }
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
                             copies.data());
}

}  // namespace

// ---- layouts ----------------------------------------------------------------------------------------------------------

const ProgramLayout& program_layout(const std::vector<const ShaderInfo*>& stages) {
    static std::map<std::vector<const ShaderInfo*>, ProgramLayout> cache;
    auto it = cache.find(stages);
    if (it != cache.end()) return it->second;
    std::map<uint32_t, VkDescriptorSetLayoutBinding> sets[4];
    for (const ShaderInfo* s : stages)
        for (const DescriptorBinding& b : s->refl.bindings) {
            if (b.set >= 4) continue;
            auto& vb = sets[b.set][b.binding];
            vb.binding = b.binding;
            vb.descriptorType = VkDescriptorType(b.type);
            vb.descriptorCount = std::max(1u, b.count);
            vb.stageFlags |= stage_bit(s->stage);
        }
    ProgramLayout pl;
    for (int k = 0; k < 4; k++) {
        std::vector<VkDescriptorSetLayoutBinding> v;
        for (auto& [n, b] : sets[k]) v.push_back(b);
        pl.sets[k] = set_layout(v);
    }
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 4;
    ci.pSetLayouts = pl.sets;
    p_vkCreatePipelineLayout(vk_device(), &ci, nullptr, &pl.layout);
    return cache[stages] = pl;
}

// ---- graphics pipelines ---------------------------------------------------------------------------------------------

VkPipeline graphics_pipeline(const PipelineKey& key, const ProgramLayout& layout) {
    static std::map<std::string, VkPipeline> cache;
    std::string k(reinterpret_cast<const char*>(&key), sizeof key);
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2]{};
    uint32_t nstages = 0;
    for (const ShaderInfo* s : {key.vs, key.fs}) {
        if (!s) continue;
        auto& st = stages[nstages++];
        st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st.stage = VkShaderStageFlagBits(stage_bit(s->stage));
        st.module = module_of(s);
        st.pName = "main";
    }

    // Vertex input: each shader input location reads the attribute of the same index. Locations with no attribute
    // (or an unknown format) read a zero vec4 from binding 15.
    std::vector<VkVertexInputAttributeDescription> attrs;
    std::vector<VkVertexInputBindingDescription> binds;
    bool streams_used[16] = {}, zero_used = false;
    if (key.vs)
        for (const ShaderIo& in : key.vs->refl.inputs) {
            VkVertexInputAttributeDescription a{in.location, 15, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
            if (in.location < key.attrib_count) {
                const VertexAttrib& va = key.attribs[in.location];
                VkFormat f = vk_vertex_format(va.format);
                if (f != VK_FORMAT_UNDEFINED && va.stream < 15) {
                    a.binding = va.stream, a.format = f, a.offset = va.offset;
                    streams_used[va.stream] = true;
                }
            }
            if (a.binding == 15) zero_used = true;
            attrs.push_back(a);
        }
    for (uint32_t s = 0; s < 15; s++)
        if (streams_used[s]) {
            const VertexStream& vs = key.streams[s];
            binds.push_back({s, vs.stride, vs.divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
        }
    if (zero_used) binds.push_back({15, 0, VK_VERTEX_INPUT_RATE_INSTANCE});
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = uint32_t(binds.size());
    vi.pVertexBindingDescriptions = binds.data();
    vi.vertexAttributeDescriptionCount = uint32_t(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = vk_topology(key.topology);

    VkPipelineViewportDepthClipControlCreateInfoEXT clip{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    clip.negativeOneToOne = VK_TRUE;  // NVN depth mode 0 (Wonder): clip-space z in -1..1
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    if (vk_depth_clip_control()) vp.pNext = &clip;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = key.polygon.mode == 0 ? VK_POLYGON_MODE_POINT : key.polygon.mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = vk_cull(key.polygon.cull);
    // NVNfrontFace CW 0 / CCW 1 is in the GL sense (y up). Wonder sets the upper-left window origin, where the
    // hardware negates y like Vulkan does, so a face the game calls CCW is wound CW in Vulkan's framebuffer: with
    // the enum taken as is, the fullscreen composite was culled and the window kept its guest memory (magenta).
    rs.frontFace = key.polygon.front == 0 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    bool has_depth = key.depth_format != VK_FORMAT_UNDEFINED;
    ds.depthTestEnable = has_depth && key.depth.depth_test;
    ds.depthWriteEnable = has_depth && key.depth.depth_write;
    ds.depthCompareOp = vk_compare(key.depth.depth_func);
    ds.stencilTestEnable = has_depth && key.depth.stencil_test;
    VkStencilOpState* faces[2] = {&ds.front, &ds.back};
    for (int f = 0; f < 2; f++) {
        faces[f]->failOp = vk_stencil_op(key.depth.stencil_fail[f]);
        faces[f]->depthFailOp = vk_stencil_op(key.depth.depth_fail[f]);
        faces[f]->passOp = vk_stencil_op(key.depth.depth_pass[f]);
        faces[f]->compareOp = vk_compare(key.depth.stencil_func[f]);
        faces[f]->compareMask = faces[f]->writeMask = 0xFF;
    }

    VkPipelineColorBlendAttachmentState att[8]{};
    for (uint32_t k = 0; k < key.color_count; k++) {
        const BlendTarget& b = key.blend[k];
        att[k].blendEnable = key.color.blend_enable[k] ? VK_TRUE : VK_FALSE;
        att[k].srcColorBlendFactor = vk_blend_factor(b.src_rgb);
        att[k].dstColorBlendFactor = vk_blend_factor(b.dst_rgb);
        att[k].colorBlendOp = vk_blend_op(b.eq_rgb);
        att[k].srcAlphaBlendFactor = vk_blend_factor(b.src_a);
        att[k].dstAlphaBlendFactor = vk_blend_factor(b.dst_a);
        att[k].alphaBlendOp = vk_blend_op(b.eq_a);
        att[k].colorWriteMask = key.mask.mask[k] & 0xF;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = key.color_count;
    cb.pAttachments = att;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = key.color_count;
    ri.pColorAttachmentFormats = key.color_formats;
    bool stencil = key.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT || key.depth_format == VK_FORMAT_S8_UINT;
    ri.depthAttachmentFormat = key.depth_format == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : key.depth_format;
    ri.stencilAttachmentFormat = stencil ? key.depth_format : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &ri;
    ci.stageCount = nstages;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = layout.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (p_vkCreateGraphicsPipelines(vk_device(), VK_NULL_HANDLE, 1, &ci, nullptr, &p) != VK_SUCCESS)
        log("nvn/vk: pipeline for %s + %s failed", key.vs ? key.vs->hash.c_str() : "-", key.fs ? key.fs->hash.c_str() : "-");
    else if (g_trace)
        log("nvn/vk: pipeline %zu: %s + %s", cache.size() + 1, key.vs ? key.vs->hash.c_str() : "-", key.fs ? key.fs->hash.c_str() : "-");
    return cache[k] = p;
}

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
    ci.minLod = m <= 1 ? 0.0f : s.lod_min;
    ci.maxLod = m <= 1 ? 0.25f : s.lod_max;  // no mipmapping: level 0 only
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

// ---- sampled textures -------------------------------------------------------------------------------------------------

bool sampled_ready(uint64_t texture) {
    Texture* t = get<Texture>(texture);
    if (!t || !t->image) return false;
    const GpuImage& img = *t->image;
    return img.rendered ? img.layout == VK_IMAGE_LAYOUT_GENERAL
                        : img.uploaded && img.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

VkImageView attachment_view(GpuImage& img, uint32_t level, uint32_t layer) {
    level = std::min(level, img.levels - 1);
    // A 3D image's slice is a layer of its 2D-array-compatible views, counted at this level.
    layer = std::min(layer, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? std::max(1u, img.depth >> level) - 1 : img.layers - 1);
    if (level == 0 && layer == 0 && img.levels == 1 && img.layers == 1 && img.view_type == VK_IMAGE_VIEW_TYPE_2D)
        return img.view;
    static std::map<std::tuple<VkImage, uint32_t, uint32_t>, VkImageView> cache;
    auto& v = cache[{img.image, level, layer}];
    if (!v) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = img.image;
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = img.format;
        ci.subresourceRange = {img.aspect, level, 1, layer, 1};
        if (p_vkCreateImageView(vk_device(), &ci, nullptr, &v) != VK_SUCCESS) v = img.view;
    }
    return v;
}

VkImageView sampled_view(VkCommandBuffer cb, uint64_t texture, const ImageShape& shape, const ViewInfo* view,
                         VkImageLayout* layout) {
    Texture* t = get<Texture>(texture);
    GpuImage* img = t ? image_of(texture) : nullptr;
    *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!img) return dummy_view(cb, shape);
    if (!img->rendered && !img->uploaded) upload_texture(cb, *t, *img);
    if (img->layout == VK_IMAGE_LAYOUT_UNDEFINED) {  // never written: black, not garbage
        transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange r{img->aspect, 0, img->levels, 0, img->layers};
        if (img->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
            VkClearColorValue v{};
            p_vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
        }
    }
    *layout = img->rendered ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    transition(cb, *img, *layout);
    VkImageView v = view_of(*img, view_type(shape), view ? *view : ViewInfo{});
    if (!v) {
        *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return dummy_view(cb, shape);
    }
    return v;
}

std::map<VkImageViewType, GpuImage>& dummies() {
    static std::map<VkImageViewType, GpuImage> d;
    return d;
}

bool dummy_ready(const ImageShape& shape) {
    auto it = dummies().find(view_type(shape));
    return it != dummies().end() && it->second.image;
}

VkImageView dummy_view(VkCommandBuffer cb, const ImageShape& shape) {
    VkImageViewType type = view_type(shape);
    auto& d = dummies()[type];
    if (d.image) return d.view;
    bool cube = type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                   : (type == VK_IMAGE_VIEW_TYPE_1D || type == VK_IMAGE_VIEW_TYPE_1D_ARRAY) ? VK_IMAGE_TYPE_1D
                                                                                            : VK_IMAGE_TYPE_2D;
    ci.format = shape.depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {1, 1, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = cube ? 6 : 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    p_vkCreateImage(vk_device(), &ci, nullptr, &d.image);
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(vk_device(), d.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    p_vkAllocateMemory(vk_device(), &ai, nullptr, &d.memory);
    p_vkBindImageMemory(vk_device(), d.image, d.memory, 0);
    d.format = ci.format;
    d.aspect = shape.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    d.layers = ci.arrayLayers;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = d.image;
    vi.viewType = type;
    vi.format = ci.format;
    vi.subresourceRange = {d.aspect, 0, 1, 0, ci.arrayLayers};
    p_vkCreateImageView(vk_device(), &vi, nullptr, &d.view);
    transition(cb, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange r{d.aspect, 0, 1, 0, ci.arrayLayers};
    if (shape.depth) {
        VkClearDepthStencilValue v{1.0f, 0};
        p_vkCmdClearDepthStencilImage(cb, d.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    } else {
        VkClearColorValue v{};
        p_vkCmdClearColorImage(cb, d.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    }
    transition(cb, d, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return d.view;
}

}  // namespace nx::nvn
