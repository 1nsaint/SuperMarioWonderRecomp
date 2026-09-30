// Pipelines and sampled resources (nvn_pipeline.h). Everything is cached for the life of the process: layouts by
// their bindings, pipelines by their full state, samplers by their state, views by (image, type).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

#include "nvn_pipeline.h"
#include "nvn_tiling.h"

namespace nx::nvn {

using namespace nx::vk;

// NVN views carry a format: the game renders into RGBA8 textures through sRGB views (the GPU encodes on write and
// blends in linear) and samples them the same way. Ignoring it lost one conversion per draw: far too much contrast
// and saturation (docs/nvn/DEBUG.md #29).
VkFormat view_format(const GpuImage& img, uint32_t nvn) {
    if (!nvn || nvn == img.nvn_format || img.alpha_from_r || img.aspect != VK_IMAGE_ASPECT_COLOR_BIT) return img.format;
    VkFormat f = vk_format(nvn);
    uint32_t bw = 1, bh = 1, bytes = 0, iw = 1, ih = 1, ibytes = 0;
    format_block(nvn, bw, bh, bytes);
    format_block(img.nvn_format, iw, ih, ibytes);
    bool ok = f != VK_FORMAT_UNDEFINED && format_aspect(nvn) == 0 && bw == iw && bh == ih && bytes == ibytes;
    static std::set<std::pair<uint32_t, uint32_t>> logged;
    if (logged.insert({img.nvn_format, nvn}).second && logged.size() <= 40)
        log("nvn/vk: texture format %#x viewed as %#x%s", img.nvn_format, nvn, ok ? "" : " (not compatible: ignored)");
    return ok ? f : img.format;
}

namespace {

VkShaderStageFlags stage_bit(ShaderStage s) {
    return s == ShaderStage::Vertex     ? VK_SHADER_STAGE_VERTEX_BIT
           : s == ShaderStage::Fragment ? VK_SHADER_STAGE_FRAGMENT_BIT
                                        : VK_SHADER_STAGE_COMPUTE_BIT;
}

// `kinds`: per vertex input location, 0 float or the integer kind of its attribute format (vertex_int_kind).
// `unwritten`: fragment input locations the vertex stage does not write (default_unwritten_inputs).
VkShaderModule module_of(const ShaderInfo* s, const uint8_t* kinds = nullptr, uint32_t unwritten = 0) {
    static std::map<std::tuple<const ShaderInfo*, std::string, uint32_t>, VkShaderModule> cache;
    // The integer-vertex-input patch (skinned models' bone indices; without it Metal refuses every character's
    // pipeline, DEBUG.md #25/#26). NX_INT_ATTRIBS=0 turns it off.
    static const char* int_env = std::getenv("NX_INT_ATTRIBS");
    static const bool off = int_env && int_env[0] == '0';
    std::string k = kinds && !off ? std::string(reinterpret_cast<const char*>(kinds), 16) : std::string(16, '\0');
    auto& m = cache[{s, k, unwritten}];
    if (!m) {
        bool is_patched = false;
        const uint8_t* int_kinds = k != std::string(16, '\0') ? reinterpret_cast<const uint8_t*>(k.data()) : nullptr;
        std::vector<uint32_t> code = patched_spirv(s, int_kinds, unwritten, &is_patched);
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size() * 4;
        ci.pCode = code.data();
        if (p_vkCreateShaderModule(vk_device(), &ci, nullptr, &m) != VK_SUCCESS) log("nvn/vk: shader %s rejected", s->hash.c_str());
        static int logged = 0;
        if (is_patched && logged++ < 20) log("nvn/vk: shader %s: integer vertex inputs", s->hash.c_str());
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

// ---- images ---------------------------------------------------------------------------------------------------------

VkImageViewType view_type(const ImageShape& s) {  // SPIR-V Dim: 0 1D, 1 2D, 2 3D, 3 Cube
    switch (s.dim) {
        case 0: return s.arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case 2: return VK_IMAGE_VIEW_TYPE_3D;
        case 3: return s.arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        default: return s.arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

// Views made for sampling and for rendering, by image; forget_image_views() drops an image's before it is freed.
std::map<std::tuple<VkImage, VkImageViewType, uint32_t, uint64_t, uint64_t>, VkImageView> g_sampled_views;
std::map<std::tuple<VkImage, uint32_t, uint32_t, VkFormat>, VkImageView> g_attachment_views;

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
    uint32_t swizzle = img.alpha_from_r ? 0 : view.swizzle;  // materialized at upload
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
    VkFormat format = view_format(img, view.format);
    if (type == img.view_type && identity(m) && whole && format == img.format) return img.view;
    auto& cache = g_sampled_views;
    uint64_t range = uint64_t(base) | uint64_t(levels) << 16 | uint64_t(min_layer) << 32 | uint64_t(view.layers) << 48;
    auto& v = cache[{img.image, type, uint32_t(m.r) | uint32_t(m.g) << 8 | uint32_t(m.b) << 16 | uint32_t(m.a) << 24,
                     range, uint64_t(format)}];
    if (!v) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = img.image;
        ci.viewType = type;
        ci.format = format;
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
    if (gpu_profile_on()) gpu_profile_mark(cb, "texture uploads");
    if (!t.memory || !t.storage_size) return;
    if (std::getenv("NX_TRACE_UPLOADS") && t.storage_size >= (1u << 20)) {
        const uint8_t* m = reinterpret_cast<const uint8_t*>(t.memory);
        size_t nz = 0;
        for (uint64_t k = 0; k < t.storage_size; k += 64) nz += m[k] != 0;
        log("nvn: upload %ux%u fmt %#x stride %u: %zu of %llu sampled bytes non-zero", t.width, t.height, t.format,
            t.stride, nz, (unsigned long long)(t.storage_size / 64));
    }
    uint32_t bw = 1, bh = 1, bytes = 4;
    format_block(t.format, bw, bh, bytes);
    std::vector<uint8_t> linear;
    std::vector<VkBufferImageCopy> copies;
    uint32_t layers = img.view_type == VK_IMAGE_VIEW_TYPE_3D ? std::max(1u, t.depth) : img.layers;
    if (img.alpha_from_r && t.stride) {  // R8 pitch-linear -> RGBA8 (255, 255, 255, R), level 0
        const uint8_t* src = reinterpret_cast<const uint8_t*>(t.memory);
        uint64_t rows = uint64_t(img.height) * layers;
        if (uint64_t(t.stride) * rows > t.storage_size) rows = t.storage_size / t.stride;
        Upload u = upload_alloc(rows * img.width * 4, 16);
        for (uint64_t y = 0; y < rows; y++) {
            const uint8_t* s = src + y * t.stride;
            uint32_t* d = reinterpret_cast<uint32_t*>(u.ptr + y * img.width * 4);
            for (uint32_t x = 0; x < img.width; x++) d[x] = 0x00FFFFFFu | (uint32_t(s[x]) << 24);
        }
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset;
        r.imageSubresource = {img.aspect, 0, 0, img.layers};
        r.imageExtent = {img.width, uint32_t(std::min<uint64_t>(rows, img.height)), 1};
        transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        return;
    }
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
    uint8_t int_kinds[16] = {};
    for (uint32_t a = 0; a < key.attrib_count && a < 16; a++) int_kinds[a] = uint8_t(vertex_int_kind(key.attribs[a].format));
    // NX_TRACE_PIPELINES=1: every new pipeline's shaders, and each vertex shader's inputs once.
    static std::set<const ShaderInfo*> logged_vs;
    static const bool trace = std::getenv("NX_TRACE_PIPELINES") != nullptr;
    if (trace) log("nvn/vk: new pipeline vs %s fs %s", key.vs ? key.vs->hash.c_str() : "-", key.fs ? key.fs->hash.c_str() : "-");
    if (trace && key.vs && logged_vs.insert(key.vs).second) {
        std::string line;
        for (const ShaderIo& in : key.vs->refl.inputs) {
            char b[64];
            const VertexAttrib* va = in.location < key.attrib_count ? &key.attribs[in.location] : nullptr;
            std::snprintf(b, sizeof b, " %u:%x/s%u+%u", in.location, va ? va->format : 0, va ? va->stream : 0,
                          va ? va->offset : 0);
            line += b;
        }
        log("nvn/vk: vs %s inputs (location:format/stream+offset)%s", key.vs->hash.c_str(), line.c_str());
    }
    uint32_t unwritten = unwritten_inputs(key.vs, key.fs);
    for (const ShaderInfo* s : {key.vs, key.fs}) {
        if (!s) continue;
        auto& st = stages[nstages++];
        st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st.stage = VkShaderStageFlagBits(stage_bit(s->stage));
        st.module = module_of(s, s == key.vs ? int_kinds : nullptr, s == key.fs ? unwritten : 0);
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
    // NVNfrontFace CW 0 / CCW 1, taken as is: the draws use a negative-height viewport (nvn_draw.cpp) that gives NVN's
    // upper-left-origin mapping, so windings match NVN's.
    rs.frontFace = key.polygon.front == 0 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    // SetDepthClamp: clamps instead of clipping at the near and far planes, like depthClampEnable (a device feature;
    // nvn_vk.cpp enables every core feature the GPU has, and MoltenVK has it through Metal's depth clip mode).
    rs.depthClampEnable = key.depth_clamp && vk_features().depthClamp ? VK_TRUE : VK_FALSE;
    // Polygon offset: NVNpolygonOffsetEnable has a bit per polygon mode (POINT 1, LINE 2, FILL 4; modes are POINT 0,
    // LINE 1, FILL 2), and the bit of the mode drawn with decides. Factor, units and clamp are dynamic (SetDepthBias
    // at each draw, nvn_raster.cpp): the game sets them on the command buffer, not in the state object.
    uint32_t offset_bit = key.polygon.mode <= 2 ? 1u << key.polygon.mode : 4u;
    rs.depthBiasEnable = key.polygon.offset_enables & offset_bit ? VK_TRUE : VK_FALSE;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    bool has_depth = key.depth_format != VK_FORMAT_UNDEFINED;
    ds.depthTestEnable = has_depth && key.depth.depth_test;
    ds.depthWriteEnable = has_depth && key.depth.depth_write;
    ds.depthCompareOp = vk_compare(key.depth.depth_func);
    // EQUAL after a depth pre-pass (the colour pass does not write depth) needs both passes to compute bit-identical
    // depth; two translated shaders compiled separately by Metal do not, and Wonder's whole course colour pass was
    // discarded (only the sky showed). LESS_OR_EQUAL keeps what EQUAL keeps for opaque geometry after a pre-pass.
    if (key.depth.depth_func == 3 && !key.depth.depth_write) ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ds.stencilTestEnable = has_depth && key.depth.stencil_test;
    VkStencilOpState* faces[2] = {&ds.front, &ds.back};
    for (int f = 0; f < 2; f++) {
        faces[f]->failOp = vk_stencil_op(key.depth.stencil_fail[f]);
        faces[f]->depthFailOp = vk_stencil_op(key.depth.depth_fail[f]);
        faces[f]->passOp = vk_stencil_op(key.depth.depth_pass[f]);
        faces[f]->compareOp = vk_compare(key.depth.stencil_func[f]);
        faces[f]->compareMask = faces[f]->writeMask = 0xFF;  // dynamic: SetStencilValueMask / SetStencilMask
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

    // Stencil reference and masks and depth bias values come from command-buffer calls (nvn_raster.cpp), so they are
    // dynamic: one pipeline for every value the game uses.
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_DEPTH_BIAS};
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

// ---- sampled textures -------------------------------------------------------------------------------------------------

// Pending nvnTextureWriteTexels regions go to the image (a transfer: outside render passes).
void apply_texel_writes(VkCommandBuffer cb, Texture& t, GpuImage& img) {
    std::vector<Texture::TexelWrite> writes;
    {
        std::lock_guard lk(objects_lock());
        writes.swap(t.writes);
    }
    if (writes.empty()) return;
    VkImageLayout back = img.layout;
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    bool is3d = img.view_type == VK_IMAGE_VIEW_TYPE_3D;
    for (const auto& w : writes) {
        if (w.level >= img.levels) continue;
        Upload u = upload(w.data.data(), w.data.size(), 16);
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset;
        r.imageSubresource = {img.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                  ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                  : img.aspect,
                              w.level, is3d ? 0 : std::min(w.z, img.layers - 1), is3d ? 1 : std::min(w.d, img.layers)};
        r.imageOffset = {int32_t(w.x), int32_t(w.y), is3d ? int32_t(w.z) : 0};
        r.imageExtent = {w.w, w.h, is3d ? w.d : 1};
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
    if (back != VK_IMAGE_LAYOUT_UNDEFINED) transition(cb, img, back);
}

uint64_t g_submit_serial = 0;

void pipeline_begin_submit() { g_submit_serial++; }

uint64_t g_not_ready[6];  // why sampled_ready said no: no image, dirty, texel writes, rendered, not uploaded, layout

bool sampled_ready(uint64_t texture) {
    Texture* t = get<Texture>(texture);
    if (!t || !t->image) return g_not_ready[0]++, false;  // no image yet (missing textures are the caller's)
    check_linear_content(*t);
    if (t->dirty && !t->image->rendered) return g_not_ready[1]++, false;
    {
        std::shared_lock lk(objects_lock());  // readers only: an exclusive lock here contended with the game's threads
        if (!t->writes.empty()) return g_not_ready[2]++, false;
    }
    const GpuImage& img = *t->image;
    bool ok = img.rendered ? img.layout == VK_IMAGE_LAYOUT_GENERAL
                           : img.uploaded && img.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!ok) g_not_ready[img.rendered ? 3 : !img.uploaded ? 4 : 5]++;
    return ok;
}

void sampled_ready_log() {
    log("nvn passes: textures not ready: %llu no image, %llu dirty, %llu texel writes, %llu rendered not GENERAL, "
        "%llu not uploaded, %llu uploaded in another layout",
        (unsigned long long)g_not_ready[0], (unsigned long long)g_not_ready[1], (unsigned long long)g_not_ready[2],
        (unsigned long long)g_not_ready[3], (unsigned long long)g_not_ready[4], (unsigned long long)g_not_ready[5]);
}

VkImageView attachment_view(GpuImage& img, uint32_t level, uint32_t layer, VkFormat format) {
    if (format == VK_FORMAT_UNDEFINED) format = img.format;
    level = std::min(level, img.levels - 1);
    // A 3D image's slice is a layer of its 2D-array-compatible views, counted at this level.
    layer = std::min(layer, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? std::max(1u, img.depth >> level) - 1 : img.layers - 1);
    if (level == 0 && layer == 0 && img.levels == 1 && img.layers == 1 && img.view_type == VK_IMAGE_VIEW_TYPE_2D &&
        format == img.format)
        return img.view;
    auto& cache = g_attachment_views;
    auto& v = cache[{img.image, level, layer, format}];
    if (!v) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = img.image;
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = format;
        ci.subresourceRange = {img.aspect, level, 1, layer, 1};
        if (p_vkCreateImageView(vk_device(), &ci, nullptr, &v) != VK_SUCCESS) v = img.view;
    }
    return v;
}

GpuImage* ensure_contents(VkCommandBuffer cb, uint64_t texture) {
    Texture* t = get<Texture>(texture);
    GpuImage* img = t ? image_of(texture) : nullptr;
    if (!img) return nullptr;
    bool dirty = t->dirty.exchange(false);
    if (!img->rendered && (!img->uploaded || dirty)) upload_texture(cb, *t, *img);
    apply_texel_writes(cb, *t, *img);
    if (img->layout == VK_IMAGE_LAYOUT_UNDEFINED) {  // never written: black, not garbage
        transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange r{img->aspect, 0, img->levels, 0, img->layers};
        if (img->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
            VkClearColorValue v{};
            p_vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
        }
    }
    return img;
}

VkImageView sampled_view(VkCommandBuffer cb, uint64_t texture, const ImageShape& shape, const ViewInfo* view,
                         VkImageLayout* layout) {
    *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    GpuImage* img = ensure_contents(cb, texture);
    static uint64_t no_image = 0, incompatible = 0;
    if (!img) {
        if (++no_image % 100000 == 1) log("nvn/vk: dummy sampled: %llu with no image, %llu with an incompatible view",
                                          (unsigned long long)no_image, (unsigned long long)incompatible);
        return dummy_view(cb, shape);
    }
    *layout = img->rendered ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    transition(cb, *img, *layout);
    // The swizzle belongs to the texture (its builder, or its pool view), not to the image: an image may be shared
    // with or inherited from another texture over the same memory (#36, #42), whose swizzle differs. With the
    // image's, the UI's single-channel masks lost their alpha and drew as rectangles (docs/nvn/DEBUG.md #44).
    ViewInfo vi = view ? *view : ViewInfo{};
    if (!vi.swizzle && !img->alpha_from_r)
        if (Texture* t = get<Texture>(texture)) {
            uint32_t s = t->pool_swizzle.load();
            if (!s)
                for (int k = 0; k < 4; k++) s |= uint32_t(std::min<uint8_t>(t->swizzle[k], 5)) << (8 * k);
            vi.swizzle = s;
        }
    VkImageView v = view_of(*img, view_type(shape), vi);
    if (!v) {
        *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (++incompatible % 1000 == 1)
            log("nvn/vk: texture %llx (%ux%u x%u, view type %d) sampled as view type %d: dummy", (unsigned long long)texture,
                img->width, img->height, img->layers, int(img->view_type), int(view_type(shape)));
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
        if (std::getenv("NX_DUMMY_MAGENTA")) v = {{1.0f, 0.0f, 1.0f, 1.0f}};  // debug: where the dummy is sampled
        p_vkCmdClearColorImage(cb, d.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    }
    transition(cb, d, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return d.view;
}

VkPipeline compute_pipeline(const ShaderInfo* cs, const ProgramLayout& layout) {
    static std::map<std::pair<const ShaderInfo*, VkPipelineLayout>, VkPipeline> cache;
    auto& p = cache[{cs, layout.layout}];
    if (!p) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module_of(cs);
        ci.stage.pName = "main";
        ci.layout = layout.layout;
        if (p_vkCreateComputePipelines(vk_device(), VK_NULL_HANDLE, 1, &ci, nullptr, &p) != VK_SUCCESS) {
            log("nvn/vk: compute pipeline for %s failed", cs->hash.c_str());
            p = VK_NULL_HANDLE;
        }
    }
    return p;
}

VkImageView storage_view(VkCommandBuffer cb, uint64_t texture, const ImageShape& shape, const ViewInfo* view) {
    GpuImage* img = texture ? ensure_contents(cb, texture) : nullptr;
    if (img && img->storage) {
        transition(cb, *img, VK_IMAGE_LAYOUT_GENERAL);
        img->rendered = true;  // the shader may write it
        ViewInfo v = view ? *view : ViewInfo{};
        v.levels = 1, v.swizzle = 0;  // storage views: one level, identity
        if (VkImageView iv = view_of(*img, view_type(shape), v)) return iv;
    }
    static std::map<VkImageViewType, GpuImage> dummies;
    VkImageViewType type = view_type(shape);
    auto& d = dummies[type];
    if (!d.image) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R32_UINT;
        ci.extent = {1, 1, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        p_vkCreateImage(vk_device(), &ci, nullptr, &d.image);
        VkMemoryRequirements req{};
        p_vkGetImageMemoryRequirements(vk_device(), d.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        p_vkAllocateMemory(vk_device(), &ai, nullptr, &d.memory);
        p_vkBindImageMemory(vk_device(), d.image, d.memory, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = d.image;
        vi.viewType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                      : (type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
        vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        p_vkCreateImageView(vk_device(), &vi, nullptr, &d.view);
        d.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        d.format = ci.format;
    }
    transition(cb, d, VK_IMAGE_LAYOUT_GENERAL);
    return d.view;
}

void forget_image_views(const GpuImage& img) {
    // The caches are ordered by image first: its views are one run from the smallest key with that image (this was
    // a scan of every cached view per destroyed texture, a visible share of the replay time).
    auto drop = [&](auto& cache, auto lowest) {
        for (auto it = cache.lower_bound(lowest); it != cache.end() && std::get<0>(it->first) == img.image;) {
            if (it->second && it->second != img.view) p_vkDestroyImageView(vk_device(), it->second, nullptr);
            it = cache.erase(it);
        }
    };
    drop(g_sampled_views, std::make_tuple(img.image, VkImageViewType(0), 0u, uint64_t(0), uint64_t(0)));
    drop(g_attachment_views, std::make_tuple(img.image, 0u, 0u, VkFormat(0)));
}

}  // namespace nx::nvn
