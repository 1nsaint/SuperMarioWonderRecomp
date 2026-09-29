// NVN's GPU side on Vulkan (docs/NVN.md, N3): the host device, a VkImage per NVN texture the GPU touches, the
// replay of recorded command lists, and presenting a window texture on the host swapchain.
//
// First version: every submit is recorded into one Vulkan command buffer and waited for; image layouts are tracked
// per image; clears and render targets only (draws come with shaders, N5/N6). On macOS this is MoltenVK: the
// instance enumerates portability drivers and the device enables VK_KHR_portability_subset.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "nvn_vk.h"
#include "platform.h"

#include <nx/game.h>

namespace nx::nvn {

using namespace nx::vk;


namespace {

std::recursive_mutex g_gpu;  // the whole GPU side: one submitting thread at a time
bool g_ready = false, g_failed = false;
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkQueue g_queue = VK_NULL_HANDLE;
uint32_t g_family = 0;
VkPhysicalDeviceMemoryProperties g_memory{};
VkCommandPool g_pool = VK_NULL_HANDLE;
VkCommandBuffer g_cb = VK_NULL_HANDLE;
VkFence g_fence = VK_NULL_HANDLE;

VkSurfaceKHR g_surface = VK_NULL_HANDLE;
VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
std::vector<VkImage> g_sc_images;
VkExtent2D g_sc_extent{};
VkFormat g_sc_format = VK_FORMAT_B8G8R8A8_UNORM;
VkSemaphore g_acquired = VK_NULL_HANDLE, g_rendered = VK_NULL_HANDLE;
VkBool32 g_depth_clip_control = VK_FALSE;
VkDescriptorPool g_descriptors = VK_NULL_HANDLE;

// The per-submit upload buffer (nvn_vk.h): one host-visible buffer, bump-allocated, reset at every submit.
VkBuffer g_upload = VK_NULL_HANDLE;
VkDeviceMemory g_upload_memory = VK_NULL_HANDLE;
uint8_t* g_upload_ptr = nullptr;
uint64_t g_upload_size = 0, g_upload_used = 0;
void make_upload_buffer(uint64_t size);

// Commands the Hades bridge intercepts are not in the generated table: ours are loaded here.
PFN_vkEnumerateInstanceExtensionProperties p_enum_instance_ext = nullptr;
PFN_vkCreateInstance p_create_instance = nullptr;
PFN_vkEnumeratePhysicalDevices p_enum_physical = nullptr;
PFN_vkCreateDevice p_create_device = nullptr;

bool check(VkResult r, const char* what) {
    if (r >= 0) return true;
    log("nvn/vk: %s failed (%d)", what, int(r));
    return false;
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (auto& e : list)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

bool init() {
    if (g_ready || g_failed) return g_ready;
    g_failed = true;  // until everything below succeeds
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(plat::vulkan_get_instance_proc_addr());
    if (!gipa) {
        log("nvn/vk: no Vulkan loader");
        return false;
    }
    for (size_t k = 0; k < g_entry_count; k++) *g_entries[k].pfn = gipa(nullptr, g_entries[k].name);
    p_enum_instance_ext = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    p_create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));

    uint32_t n = 0;
    p_enum_instance_ext(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> iext(n);
    p_enum_instance_ext(nullptr, &n, iext.data());
    std::vector<const char*> exts = {VK_KHR_SURFACE_EXTENSION_NAME, plat::vulkan_surface_extension()};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (has_extension(iext, "VK_KHR_portability_enumeration")) {
        exts.push_back("VK_KHR_portability_enumeration");
        ici.flags |= 0x00000001;  // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = nx::game::kInfo.title;
    app.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    if (!check(p_create_instance(&ici, nullptr, &g_instance), "vkCreateInstance")) return false;
    for (size_t k = 0; k < g_entry_count; k++)
        if (auto p = gipa(g_instance, g_entries[k].name)) *g_entries[k].pfn = p;
    p_enum_physical = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(g_instance, "vkEnumeratePhysicalDevices"));
    p_create_device = reinterpret_cast<PFN_vkCreateDevice>(gipa(g_instance, "vkCreateDevice"));

    n = 1;
    p_enum_physical(g_instance, &n, &g_physical);
    if (!n || !g_physical) {
        log("nvn/vk: no GPU");
        return false;
    }
    VkPhysicalDeviceProperties props{};
    p_vkGetPhysicalDeviceProperties(g_physical, &props);
    p_vkGetPhysicalDeviceMemoryProperties(g_physical, &g_memory);

    uint64_t surface = 0;
    void* layer = plat::create_window();
    if (!check(VkResult(plat::vulkan_create_surface(g_instance, layer, &surface, reinterpret_cast<void*>(gipa))),
               "surface"))
        return false;
    g_surface = reinterpret_cast<VkSurfaceKHR>(surface);

    p_vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    p_vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &n, families.data());
    g_family = UINT32_MAX;
    for (uint32_t k = 0; k < n; k++) {
        VkBool32 present = VK_FALSE;
        p_vkGetPhysicalDeviceSurfaceSupportKHR(g_physical, k, g_surface, &present);
        if ((families[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            g_family = k;
            break;
        }
    }
    if (g_family == UINT32_MAX) {
        log("nvn/vk: no graphics + present queue");
        return false;
    }

    p_vkEnumerateDeviceExtensionProperties(g_physical, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> dext(n);
    p_vkEnumerateDeviceExtensionProperties(g_physical, nullptr, &n, dext.data());
    std::vector<const char*> dexts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    // Features: everything core the GPU has, dynamic rendering (1.3), 8-bit indices (NVN has them), clip-space
    // depth -1..1 (NVN's default depth mode, what Wonder uses), and on MoltenVK the portability subset's own
    // features, which must be enabled to be used.
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT fu8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT fclip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    struct {  // VkPhysicalDevicePortabilitySubsetFeaturesKHR, without its header (beta extensions)
        VkStructureType sType = VkStructureType(1000163000);
        void* pNext = nullptr;
        VkBool32 fields[15] = {};
    } fport;
    features.pNext = &f13;
    void** tail = &f13.pNext;
    if (has_extension(dext, "VK_EXT_index_type_uint8")) {
        dexts.push_back("VK_EXT_index_type_uint8");
        *tail = &fu8, tail = &fu8.pNext;
    }
    if (has_extension(dext, "VK_EXT_depth_clip_control")) {
        dexts.push_back("VK_EXT_depth_clip_control");
        *tail = &fclip, tail = &fclip.pNext;
    }
    if (has_extension(dext, "VK_KHR_portability_subset")) {
        dexts.push_back("VK_KHR_portability_subset");
        *tail = &fport, tail = &fport.pNext;
    }
    p_vkGetPhysicalDeviceFeatures2(g_physical, &features);
    g_depth_clip_control = fclip.depthClipControl;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dexts.size());
    dci.ppEnabledExtensionNames = dexts.data();
    dci.pNext = &features;
    if (!check(p_create_device(g_physical, &dci, nullptr, &g_device), "vkCreateDevice")) return false;
    p_vkGetDeviceQueue(g_device, g_family, 0, &g_queue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_family;
    p_vkCreateCommandPool(g_device, &pci, nullptr, &g_pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    p_vkAllocateCommandBuffers(g_device, &cai, &g_cb);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    p_vkCreateFence(g_device, &fci, nullptr, &g_fence);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    p_vkCreateSemaphore(g_device, &sci, nullptr, &g_acquired);
    p_vkCreateSemaphore(g_device, &sci, nullptr, &g_rendered);

    make_upload_buffer(256ull << 20);  // a busy submit reached 67 MB (uniforms padded to 4 KB, storage to 64 KB)
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 65536}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 32768;
    dpi.poolSizeCount = uint32_t(sizeof sizes / sizeof sizes[0]);
    dpi.pPoolSizes = sizes;
    p_vkCreateDescriptorPool(g_device, &dpi, nullptr, &g_descriptors);
    log("nvn/vk: %s, Vulkan %u.%u, queue family %u", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
        VK_API_VERSION_MINOR(props.apiVersion), g_family);
    g_failed = false;
    g_ready = true;
    return true;
}

bool make_swapchain() {
    if (g_swapchain) {
        p_vkDeviceWaitIdle(g_device);
        p_vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
        g_swapchain = VK_NULL_HANDLE;
    }
    VkSurfaceCapabilitiesKHR caps{};
    p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physical, g_surface, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        uint32_t w = 0, h = 0;
        plat::window_size(w, h);
        extent = {w ? w : 1280, h ? h : 720};
    }
    if (!extent.width || !extent.height) return false;  // minimized
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surface;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = g_sc_format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    if (!check(p_vkCreateSwapchainKHR(g_device, &ci, nullptr, &g_swapchain), "vkCreateSwapchainKHR")) return false;
    uint32_t n = 0;
    p_vkGetSwapchainImagesKHR(g_device, g_swapchain, &n, nullptr);
    g_sc_images.resize(n);
    p_vkGetSwapchainImagesKHR(g_device, g_swapchain, &n, g_sc_images.data());
    g_sc_extent = extent;
    log("nvn/vk: swapchain %ux%u, %u images", extent.width, extent.height, n);
    return true;
}

}  // namespace

VkDevice vk_device() { return g_device; }
VkPhysicalDevice vk_physical() { return g_physical; }
VkQueue vk_queue() { return g_queue; }
uint32_t vk_queue_family() { return g_family; }
bool vk_depth_clip_control() { return g_depth_clip_control; }

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t k = 0; k < g_memory.memoryTypeCount; k++)
        if ((bits >> k) & 1 && (g_memory.memoryTypes[k].propertyFlags & want) == want) return k;
    return 0;
}

// The VkImage behind an NVN texture, made on first use. NVN texture targets (GL order): 0 1D, 1 2D, 2 3D, 3 1D array,
// 4 2D array, 5 2D multisample, 6 2D multisample array, 7 rectangle, 8 cube map, 9 cube map array, 10 buffer.
// Arrays and cube maps keep their layer count in the builder's depth.
GpuImage* image_of(uint64_t guest) {
    Texture* t = get<Texture>(guest);
    if (!t) return nullptr;
    if (t->image) return t->image.get();
    auto img = std::make_shared<GpuImage>();
    img->format = vk_format(t->format);
    uint32_t aspect = format_aspect(t->format);
    img->aspect = aspect == 0 ? VK_IMAGE_ASPECT_COLOR_BIT
                  : aspect == 1 ? VK_IMAGE_ASPECT_DEPTH_BIT
                  : aspect == 2 ? VK_IMAGE_ASPECT_STENCIL_BIT
                                : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    img->width = std::max(1u, t->width);
    img->height = std::max(1u, t->height);
    img->levels = std::max(1u, t->levels);
    uint32_t depth = std::max(1u, t->depth);
    bool is3d = t->target == 2, cube = t->target == 8 || t->target == 9;
    bool array = t->target == 3 || t->target == 4 || t->target == 6 || t->target == 9;
    img->layers = is3d ? 1 : (array || cube) ? depth : 1;
    img->depth = is3d ? depth : 1;
    if (cube && img->layers < 6) img->layers = 6;
    img->view_type = is3d                              ? VK_IMAGE_VIEW_TYPE_3D
                     : t->target == 9                  ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                     : cube                            ? VK_IMAGE_VIEW_TYPE_CUBE
                     : t->target == 0                  ? VK_IMAGE_VIEW_TYPE_1D
                     : t->target == 3                  ? VK_IMAGE_VIEW_TYPE_1D_ARRAY
                     : (t->target == 4 || t->target == 6) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                         : VK_IMAGE_VIEW_TYPE_2D;
    if (img->format == VK_FORMAT_UNDEFINED) {
        log("nvn/vk: texture %llx has format %#x with no Vulkan equivalent", (unsigned long long)guest, t->format);
        img->format = VK_FORMAT_R8G8B8A8_UNORM;
        img->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        aspect = 0;
    }
    bool compressed = false;
    {
        uint32_t bw = 1, bh = 1, bytes = 4;
        format_block(t->format, bw, bh, bytes);
        compressed = bw > 1 || bh > 1;
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    // 3D textures are rendered one slice at a time (a colour-grading LUT): 2D views of a slice need this flag
    // (Metal otherwise refuses a 2D view of a 3D texture).
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : is3d ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0;
    ci.imageType = is3d ? VK_IMAGE_TYPE_3D : t->target == 0 || t->target == 3 ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
    ci.format = img->format;
    ci.extent = {img->width, ci.imageType == VK_IMAGE_TYPE_1D ? 1 : img->height, is3d ? depth : 1};
    ci.mipLevels = img->levels;
    ci.arrayLayers = img->layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!compressed) ci.usage |= aspect ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    {
        VkFormatProperties fp{};
        p_vkGetPhysicalDeviceFormatProperties(g_physical, ci.format, &fp);
        img->storage = !compressed && !aspect && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
        if (img->storage) ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!check(p_vkCreateImage(g_device, &ci, nullptr, &img->image), "vkCreateImage")) return nullptr;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(g_device, img->image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!check(p_vkAllocateMemory(g_device, &ai, nullptr, &img->memory), "vkAllocateMemory")) return nullptr;
    p_vkBindImageMemory(g_device, img->image, img->memory, 0);
    constexpr VkComponentSwizzle kSwizzle[] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R,
                                               VK_COMPONENT_SWIZZLE_G,    VK_COMPONENT_SWIZZLE_B,   VK_COMPONENT_SWIZZLE_A};
    img->swizzle = {kSwizzle[t->swizzle[0]], kSwizzle[t->swizzle[1]], kSwizzle[t->swizzle[2]], kSwizzle[t->swizzle[3]]};
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img->image;
    vi.viewType = img->view_type;
    vi.format = img->format;
    // A sampled depth/stencil view reads depth.
    vi.subresourceRange = {aspect == 3 ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : img->aspect, 0, img->levels, 0,
                           img->layers};
    check(p_vkCreateImageView(g_device, &vi, nullptr, &img->view), "vkCreateImageView");
    if (g_trace)
        log("nvn/vk: image for texture %llx: %ux%u x%u layers, %u levels, target %u format %#x -> %d",
            (unsigned long long)guest, img->width, img->height, img->layers, img->levels, t->target, t->format,
            int(img->format));
    t->image = img;
    return img.get();
}

void transition(VkCommandBuffer cb, GpuImage& img, VkImageLayout to) {
    if (img.layout == to) return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = img.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
    img.layout = to;
}

Upload upload_alloc(uint64_t size, uint64_t align) {
    uint64_t at = (g_upload_used + align - 1) & ~(align - 1);
    if (at + size > g_upload_size) {
        log("nvn/vk: upload buffer full (%llu + %llu bytes)", (unsigned long long)at, (unsigned long long)size);
        at = 0;  // wraps: this submit's earlier data may be overwritten (grow the buffer when this shows)
    }
    g_upload_used = at + size;
    return {g_upload, at, g_upload_ptr + at};
}

Upload upload(const void* data, uint64_t size, uint64_t align) {
    Upload u = upload_alloc(size, align);
    if (data && size) std::memcpy(u.ptr, data, size);
    return u;
}

VkDescriptorSet descriptor_set(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (p_vkAllocateDescriptorSets(g_device, &ai, &set) != VK_SUCCESS) log("nvn/vk: descriptor pool exhausted");
    return set;
}

namespace {

void make_upload_buffer(uint64_t size) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    p_vkCreateBuffer(g_device, &bi, nullptr, &g_upload);
    VkMemoryRequirements req{};
    p_vkGetBufferMemoryRequirements(g_device, g_upload, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    p_vkAllocateMemory(g_device, &ai, nullptr, &g_upload_memory);
    p_vkBindBufferMemory(g_device, g_upload, g_upload_memory, 0);
    void* p = nullptr;
    p_vkMapMemory(g_device, g_upload_memory, 0, VK_WHOLE_SIZE, 0, &p);
    g_upload_ptr = static_cast<uint8_t*>(p);
    g_upload_size = size;
}

void replay(VkCommandBuffer cb, Targets& st, const CmdList& list, int depth) {
    for (const Cmd& c : list.cmds) {
        switch (c.op) {
            case Cmd::SetRenderTargets:
                draw_end_rendering(cb);
                st.color_count = c.n;
                for (uint32_t k = 0; k < 8; k++) st.colors[k] = k < c.n ? c.a[k] : 0;
                st.depth = c.a[8];
                st.views = {};
                if (c.blob < list.blobs.size() && list.blobs[c.blob].size() == sizeof(TargetViews))
                    std::memcpy(&st.views, list.blobs[c.blob].data(), sizeof st.views);
                break;
            case Cmd::ClearColor: {
                draw_end_rendering(cb);
                if (c.n >= 8 || !st.colors[c.n]) break;
                GpuImage* img = image_of(st.colors[c.n]);
                if (!img || !(img->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) break;
                transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                img->rendered = true;  // GPU-written from now on: never re-read from guest memory
                VkClearColorValue v{};
                std::memcpy(v.float32, c.f, 16);
                if (std::getenv("NX_TRACE_DRAWS")) {
                    static std::set<std::pair<uint64_t, std::string>> seen;
                    char col[64];
                    std::snprintf(col, sizeof col, "%g %g %g %g", c.f[0], c.f[1], c.f[2], c.f[3]);
                    if (seen.size() < 64 && seen.insert({st.colors[c.n], col}).second)
                        log("clear %llx (%ux%u) to %s", (unsigned long long)st.colors[c.n], img->width, img->height, col);
                }
                VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, std::min<uint32_t>(st.views.level[c.n], img->levels - 1), 1,
                                          std::min<uint32_t>(st.views.layer[c.n], img->layers - 1), 1};
                p_vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
                break;
            }
            case Cmd::ClearDepthStencil: {
                draw_end_rendering(cb);
                GpuImage* img = st.depth ? image_of(st.depth) : nullptr;
                if (!img || img->aspect == VK_IMAGE_ASPECT_COLOR_BIT) break;
                transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                img->rendered = true;
                VkClearDepthStencilValue v{c.f[0], uint32_t(c.i[0])};
                VkImageSubresourceRange r{img->aspect, std::min<uint32_t>(st.views.level[8], img->levels - 1), 1,
                                          std::min<uint32_t>(st.views.layer[8], img->layers - 1), 1};
                p_vkCmdClearDepthStencilImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
                break;
            }
            case Cmd::CallList:
                if (depth < 8) {
                    if (auto l = command_list(c.a[0]))
                        replay(cb, st, *l, depth + 1);
                    else {
                        static int missing = 0;
                        if (++missing <= 5) log("nvn: called handle %llx is unknown", (unsigned long long)c.a[0]);
                    }
                }
                break;
            case Cmd::CopyTextureToTexture:
                draw_end_rendering(cb);
                replay_copy(cb, c, list);
                break;
            default: draw_command(cb, c, list, st); break;
        }
    }
}

bool begin() {
    p_vkResetCommandBuffer(g_cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return check(p_vkBeginCommandBuffer(g_cb, &bi), "vkBeginCommandBuffer");
}

// Where GPU time goes, logged with the draw stats: submits, host time replaying them, time waiting for the GPU,
// bytes through the upload buffer.
struct Timing {
    uint64_t submits = 0, replay_ns = 0, wait_ns = 0, present_wait_ns = 0, upload_bytes = 0;
} g_timing;

void end_and_wait(VkSemaphore wait, VkSemaphore signal) {
    uint64_t t0 = plat::monotonic_ns();
    p_vkEndCommandBuffer(g_cb);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cb;
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
        si.pWaitDstStageMask = &stage;
    }
    if (signal) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal;
    }
    p_vkResetFences(g_device, 1, &g_fence);
    if (check(p_vkQueueSubmit(g_queue, 1, &si, g_fence), "vkQueueSubmit"))
        p_vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, UINT64_MAX);
    (signal ? g_timing.present_wait_ns : g_timing.wait_ns) += plat::monotonic_ns() - t0;
}

void log_timing(uint64_t frames) {
    auto ms = [&](uint64_t ns) { return double(ns) / 1e6 / double(frames); };
    log("nvn/vk: per frame over %llu: %.1f submits, replay %.2f ms, GPU wait %.2f ms, present wait %.2f ms, "
        "upload %.2f MB",
        (unsigned long long)frames, double(g_timing.submits) / double(frames), ms(g_timing.replay_ns),
        ms(g_timing.wait_ns), ms(g_timing.present_wait_ns), double(g_timing.upload_bytes) / 1e6 / double(frames));
    g_timing = {};
}

// NX_DUMP_FRAMES=N: every Nth presented texture is read back and written to <data root>/frames/NNNNN.ppm
// (what the game drew, independent of the window).
uint32_t dump_every() {
    static uint32_t n = [] {
        const char* e = std::getenv("NX_DUMP_FRAMES");
        return e ? uint32_t(std::strtoul(e, nullptr, 10)) : 0u;
    }();
    return n;
}

struct Readback {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t size = 0;
};

Readback g_readback;

bool readback_record(GpuImage& img) {
    uint64_t size = uint64_t(img.width) * img.height * 4;
    if (img.format != VK_FORMAT_R8G8B8A8_UNORM && img.format != VK_FORMAT_R8G8B8A8_SRGB &&
        img.format != VK_FORMAT_B8G8R8A8_UNORM)
        return false;
    if (g_readback.size < size) {
        if (g_readback.buffer) p_vkDestroyBuffer(g_device, g_readback.buffer, nullptr);
        if (g_readback.memory) p_vkFreeMemory(g_device, g_readback.memory, nullptr);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        p_vkCreateBuffer(g_device, &bi, nullptr, &g_readback.buffer);
        VkMemoryRequirements req{};
        p_vkGetBufferMemoryRequirements(g_device, g_readback.buffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =
            memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        p_vkAllocateMemory(g_device, &ai, nullptr, &g_readback.memory);
        p_vkBindBufferMemory(g_device, g_readback.buffer, g_readback.memory, 0);
        g_readback.size = size;
    }
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {img.width, img.height, 1};
    p_vkCmdCopyImageToBuffer(g_cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_readback.buffer, 1, &r);
    return true;
}

void readback_write(const GpuImage& img, uint64_t frame) {
    void* p = nullptr;
    if (p_vkMapMemory(g_device, g_readback.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return;
    std::string dir = plat::data_root() + "/frames";
    plat::make_dirs(dir);
    char name[64];
    std::snprintf(name, sizeof name, "/%05llu.ppm", (unsigned long long)frame);
    if (FILE* f = plat::fopen_utf8(dir + name, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", img.width, img.height);
        const uint8_t* px = static_cast<const uint8_t*>(p);
        bool bgra = img.format == VK_FORMAT_B8G8R8A8_UNORM;
        std::vector<uint8_t> row(size_t(img.width) * 3);
        for (uint32_t y = 0; y < img.height; y++) {
            for (uint32_t x = 0; x < img.width; x++) {
                const uint8_t* s = px + (size_t(y) * img.width + x) * 4;
                row[x * 3 + 0] = bgra ? s[2] : s[0];
                row[x * 3 + 1] = s[1];
                row[x * 3 + 2] = bgra ? s[0] : s[2];
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
    }
    p_vkUnmapMemory(g_device, g_readback.memory);
}

}  // namespace

void debug_dump_textures(uint64_t frame);  // nvn_debug.cpp
uint64_t draw_take_dump_request();          // nvn_draw.cpp

void gpu_submit(CPU&, const std::vector<std::shared_ptr<CmdList>>& lists) {
    std::lock_guard lk(g_gpu);
    if (!init()) return;
    p_vkResetDescriptorPool(g_device, g_descriptors, 0);
    g_upload_used = 0;
    if (!begin()) return;
    draw_begin_submit();
    gpu_profile_begin(g_cb);
    uint64_t t0 = plat::monotonic_ns();
    Targets st;
    for (auto& l : lists) replay(g_cb, st, *l, 0);
    draw_end_rendering(g_cb);
    g_timing.submits++, g_timing.replay_ns += plat::monotonic_ns() - t0, g_timing.upload_bytes += g_upload_used;
    gpu_profile_mark(g_cb, "end");
    end_and_wait(VK_NULL_HANDLE, VK_NULL_HANDLE);
    draw_end_submit();
    gpu_profile_collect();
    if (uint64_t n = draw_take_dump_request(); n && dump_every() && n % dump_every() == 0) debug_dump_textures(n);
}

// Headless: the presented texture is only counted, logged and dumped (no swapchain).
void present_headless(uint64_t texture) {
    if (!begin()) return;
    GpuImage* src = image_of(texture);
    static uint64_t presents = 0;
    presents++;
    if (presents % (presents < 300 ? 60 : 300) == 0) draw_log_stats();
    if (presents % 60 == 0) log_timing(60), gpu_profile_log(60);
    bool dump = false;
    if (src && src->layout != VK_IMAGE_LAYOUT_UNDEFINED && dump_every() && presents % dump_every() == 0) {
        transition(g_cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        dump = readback_record(*src);
    }
    end_and_wait(VK_NULL_HANDLE, VK_NULL_HANDLE);
    if (dump) readback_write(*src, presents);
    if (dump_every() && presents % dump_every() == 0) debug_dump_textures(presents);
}

void gpu_present(uint64_t texture) {
    std::lock_guard lk(g_gpu);
    if (!init()) return;
    if (plat::headless()) return present_headless(texture);
    if (plat::window_minimized()) return;
    if (!g_swapchain && !make_swapchain()) return;
    uint32_t index = 0;
    VkResult r = p_vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, g_acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        make_swapchain();
        return;
    }
    if (!check(r, "vkAcquireNextImageKHR") || !begin()) return;
    GpuImage* src = image_of(texture);
    GpuImage dst;
    dst.image = g_sc_images[index];
    dst.format = g_sc_format;
    if (src) {
        if (src->layout == VK_IMAGE_LAYOUT_UNDEFINED) {  // never rendered: dark grey, to tell it from a failure
            transition(g_cb, *src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue v{{0.08f, 0.08f, 0.1f, 1.0f}};
            VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            p_vkCmdClearColorImage(g_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rr);
        }
        transition(g_cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    transition(g_cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (src) {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(src->width), int32_t(src->height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {int32_t(g_sc_extent.width), int32_t(g_sc_extent.height), 1};
        p_vkCmdBlitImage(g_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    transition(g_cb, dst, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    static uint64_t presents = 0;
    presents++;
    if (presents % (presents < 300 ? 60 : 300) == 0) draw_log_stats();
    if (presents % 60 == 0) log_timing(60), gpu_profile_log(60);
    bool dump = src && dump_every() && presents % dump_every() == 0 && readback_record(*src);
    end_and_wait(g_acquired, g_rendered);
    if (dump) readback_write(*src, presents);
    if (dump_every() && presents % dump_every() == 0) debug_dump_textures(presents);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swapchain;
    pi.pImageIndices = &index;
    r = p_vkQueuePresentKHR(g_queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) make_swapchain();
}

}  // namespace nx::nvn
