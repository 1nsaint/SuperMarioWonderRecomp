// NVN's GPU side on Vulkan (docs/NVN.md, N3): the host device, a VkImage per NVN texture the GPU touches, the
// replay of recorded command lists, and presenting a window texture on the host swapchain.
//
// First version: every submit is recorded into one Vulkan command buffer and waited for (NX_ASYNC_GPU: into a
// slot's, not waited for, nvn_frames.cpp); image layouts are tracked per image, in submission order; clears and render
// targets only (draws come with shaders, N5/N6). On macOS this is MoltenVK: the instance enumerates portability
// drivers and the device enables VK_KHR_portability_subset.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
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
VkPhysicalDeviceFeatures g_features{};  // the core features enabled (vk_features())
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

}  // namespace

// A lost device means the GPU hung (a shader looping for ever): the process ends at once. Submitting more work, or
// waiting, keeps the GPU stuck, and on macOS that froze WindowServer and restarted the machine.
[[noreturn]] void gpu_hung(const char* why) {
    log("nvn/vk: GPU hang (%s): stopping", why);
    std::fprintf(stderr, "nvn/vk: GPU hang (%s): stopping\n", why);
    _exit(4);
}

namespace {

bool check(VkResult r, const char* what) {
    if (r >= 0) return true;
    log("nvn/vk: %s failed (%d)", what, int(r));
    if (r == VK_ERROR_DEVICE_LOST) gpu_hung(what);
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
    g_features = features.features;
    log("nvn/vk: depthClamp %u, depthBiasClamp %u, depthBounds %u", g_features.depthClamp, g_features.depthBiasClamp,
        g_features.depthBounds);
    log("nvn/vk: portability: format reinterpretation %u, format swizzle %u", fport.fields[2], fport.fields[3]);
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
    if (async_gpu())
        frame_init(g_pool, g_upload, g_upload_ptr, g_upload_size);  // the upload buffer becomes the slots' ring
    else
        g_descriptors = make_descriptor_pool();
    log("nvn/vk: %s, Vulkan %u.%u, queue family %u", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
        VK_API_VERSION_MINOR(props.apiVersion), g_family);
    g_failed = false;
    g_ready = true;
    return true;
}

bool make_swapchain() {
    if (g_swapchain) {
        if (async_gpu()) frame_wait_all();
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
    // The game paces itself on its vsync event (hle_vi.cpp, 60 Hz): a FIFO present also waited for the display's own
    // refresh, two clocks drifting against each other (stutter near 60 fps, docs/nvn/DEBUG.md #33). MAILBOX never
    // blocks and shows the newest frame at each refresh; FIFO when the driver has no mailbox.
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t modes_n = 0;
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(g_physical, g_surface, &modes_n, nullptr);
    std::vector<VkPresentModeKHR> modes(modes_n);
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(g_physical, g_surface, &modes_n, modes.data());
    for (VkPresentModeKHR m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR && !std::getenv("NX_FIFO")) ci.presentMode = m;
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

std::recursive_mutex& gpu_lock() { return g_gpu; }
VkDevice vk_device() { return g_device; }
VkPhysicalDevice vk_physical() { return g_physical; }
VkQueue vk_queue() { return g_queue; }
uint32_t vk_queue_family() { return g_family; }
bool vk_depth_clip_control() { return g_depth_clip_control; }
const VkPhysicalDeviceFeatures& vk_features() { return g_features; }

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t k = 0; k < g_memory.memoryTypeCount; k++)
        if ((bits >> k) & 1 && (g_memory.memoryTypes[k].propertyFlags & want) == want) return k;
    return 0;
}

// The VkImage behind an NVN texture, made on first use. NVN texture targets (GL order): 0 1D, 1 2D, 2 3D, 3 1D array,
// 4 2D array, 5 2D multisample, 6 2D multisample array, 7 rectangle, 8 cube map, 9 cube map array, 10 buffer.
// Arrays and cube maps keep their layer count in the builder's depth.
// Live textures over the same storage share one image: the game renders through one NVNtexture and samples the same
// memory through another (UI capture and blur chains, agl filters). With an image per object the sampled one was a
// stale upload of guest memory: no blur behind the menus, missing UI masks (docs/nvn/DEBUG.md #36).
struct StorageKey {
    uint64_t memory = 0;
    uint32_t format = 0, width = 0, height = 0, depth = 0, levels = 0, target = 0, samples = 0, swizzle = 0;
    bool operator<(const StorageKey& o) const {
        return std::tie(memory, format, width, height, depth, levels, target, samples, swizzle) <
               std::tie(o.memory, o.format, o.width, o.height, o.depth, o.levels, o.target, o.samples, o.swizzle);
    }
};
std::map<StorageKey, std::weak_ptr<GpuImage>> g_live_images;

GpuImage* image_of(uint64_t guest) {
    Texture* t = get<Texture>(guest);
    if (!t) return nullptr;
    if (t->image) return t->image.get();
    uint32_t swizzle = uint32_t(t->swizzle[0]) | uint32_t(t->swizzle[1]) << 8 | uint32_t(t->swizzle[2]) << 16 |
                  uint32_t(t->swizzle[3]) << 24;
    // Swizzles apply when sampling (sampled_view, #44): neither the key's nor a pool view's keeps textures from sharing.
    // With them, the shadow map and the UI's text strips were sampled as stale uploads (#46). Env: bisecting.
    static const bool key_swizzle = std::getenv("NX_KEY_SWIZZLE") != nullptr;
    if (!key_swizzle) swizzle = 0;
    StorageKey key{t->memory, t->format, t->width, t->height, t->depth, t->levels, t->target, t->samples, swizzle};
    static const bool share = std::getenv("NX_NO_IMAGE_SHARE") == nullptr;  // bisecting
    // An R8 image widened for alpha-from-red keeps its own.
    static const bool old_rule = std::getenv("NX_SHARE_NO_POOL_SWIZZLE") != nullptr;  // bisecting
    bool widened = t->format == 0x1 && t->pool_swizzle == 0x02010101u;
    if (share && t->memory && (old_rule ? t->pool_swizzle == 0 : !widened))
        if (auto it = g_live_images.find(key); it != g_live_images.end())
            if (auto shared = it->second.lock(); shared && shared->image && !shared->alpha_from_r) {
                static uint64_t shares = 0;
                if (++shares % 1000 == 1) log("nvn/vk: %llu textures shared an image with another", (unsigned long long)shares);
                t->image = shared;
                return shared.get();
            }
    if ((t->image = reuse_image(*t))) return t->image.get();
    auto img = std::make_shared<GpuImage>();
    img->format = vk_format(t->format);
    img->nvn_format = t->format;
    if (t->format == 0x1 && t->pool_swizzle == 0x02010101u) {
        img->format = VK_FORMAT_R8G8B8A8_UNORM;
        img->alpha_from_r = true;
    }
    uint32_t aspect = format_aspect(t->format);
    img->aspect = aspect == 0 ? VK_IMAGE_ASPECT_COLOR_BIT
                  : aspect == 1 ? VK_IMAGE_ASPECT_DEPTH_BIT
                  : aspect == 2 ? VK_IMAGE_ASPECT_STENCIL_BIT
                                : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    bool one_d = vk_1d_images() && (t->target == 0 || t->target == 3);  // else 2D images of height 1 (G8)
    img->width = std::max(1u, t->width);
    img->height = (t->target == 0 || t->target == 3) && !one_d ? 1u : std::max(1u, t->height);
    img->levels = std::max(1u, t->levels);
    uint32_t depth = std::max(1u, t->target == 3 && !one_d && t->depth <= 1 ? t->height : t->depth);  // GL-style layers
    bool is3d = t->target == 2, cube = t->target == 8 || t->target == 9;
    bool array = t->target == 3 || t->target == 4 || t->target == 6 || t->target == 9;
    img->layers = is3d ? 1 : (array || cube) ? depth : 1;
    img->depth = is3d ? depth : 1;
    if (cube && img->layers < 6) img->layers = 6;
    // NVN takes more levels than the size allows (a 1x1 texture with 5), Metal aborts on it (DEBUG.md #27).
    uint32_t largest = std::max({img->width, img->height, img->depth}), fit = 1;
    while (largest >>= 1) fit++;
    img->levels = std::min(img->levels, fit);
    img->view_type = is3d             ? VK_IMAGE_VIEW_TYPE_3D
                     : t->target == 9 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                     : cube           ? VK_IMAGE_VIEW_TYPE_CUBE
                     : one_d          ? (t->target == 0 ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_1D_ARRAY)
                     : array          ? VK_IMAGE_VIEW_TYPE_2D_ARRAY  // 1D array (G8), 2D array, 2D MS array
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
    // Colour images may be viewed in another format of the same size (an sRGB view of RGBA8, docs/nvn/DEBUG.md #29).
    if (aspect == 0 && !img->alpha_from_r) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ci.imageType = is3d ? VK_IMAGE_TYPE_3D : one_d ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
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
    static uint64_t created = 0;
    if (g_trace || ++created % 1000 == 0)  // a sample of what keeps being created
        log("nvn/vk: image %llu for texture %llx (memory %llx): %ux%u x%u layers, %u levels, target %u format %#x -> %d",
            (unsigned long long)created, (unsigned long long)guest, (unsigned long long)t->memory,
            img->width, img->height, img->layers, img->levels, t->target, t->format, int(img->format));
    t->image = img;
    if (t->memory) g_live_images[key] = img;
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
    if (async_gpu()) return frame_upload_alloc(size, align);
    uint64_t at = (g_upload_used + align - 1) & ~(align - 1);
    if (at + size > g_upload_size) {
        log("nvn/vk: upload buffer full (%llu + %llu bytes)", (unsigned long long)at, (unsigned long long)size);
        at = 0;  // wraps: this submit's earlier data may be overwritten (grow the buffer when this shows)
        replay::dedup_reset();  // shared copies may be among them
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
    if (async_gpu()) return frame_descriptor_set(layout);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_descriptors;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (p_vkAllocateDescriptorSets(g_device, &ai, &set) != VK_SUCCESS) log("nvn/vk: descriptor pool exhausted");
    return set;
}

VkDescriptorPool make_descriptor_pool() {
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 65536}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 32768;
    dpi.poolSizeCount = uint32_t(sizeof sizes / sizeof sizes[0]);
    dpi.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    p_vkCreateDescriptorPool(g_device, &dpi, nullptr, &pool);
    return pool;
}

namespace {

void make_upload_buffer(uint64_t size) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = kUploadUsage;
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

bool begin(bool replay = false) {
    if (async_gpu()) return (g_cb = frame_begin(replay)) != VK_NULL_HANDLE;
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
    // 2 s at most: a frame's work takes milliseconds; longer is a shader that does not end.
    if (check(p_vkQueueSubmit(g_queue, 1, &si, g_fence), "vkQueueSubmit")) {
        VkResult w = p_vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, 2'000'000'000ull);
        if (w == VK_TIMEOUT) gpu_hung("a submit did not finish in 2 s");
        check(w, "vkWaitForFences");
    }
    (signal ? g_timing.present_wait_ns : g_timing.wait_ns) += plat::monotonic_ns() - t0;
}

void log_timing(uint64_t frames) {
    auto ms = [&](uint64_t ns) { return double(ns) / 1e6 / double(frames); };
    if (async_gpu()) g_timing.wait_ns += frame_take_wait_ns();  // fences waited for (slot reuse, ring, syncs)
    static uint64_t last = 0;  // wall clock: the real frame rate
    uint64_t now = plat::monotonic_ns();
    double fps = last ? double(frames) * 1e9 / double(now - last) : 0.0;
    last = now;
    log("nvn/vk: per frame over %llu: %.1f fps, %.1f submits, replay %.2f ms, GPU wait %.2f ms, present wait %.2f ms, "
        "upload %.2f MB",
        (unsigned long long)frames, fps, double(g_timing.submits) / double(frames), ms(g_timing.replay_ns),
        ms(g_timing.wait_ns), ms(g_timing.present_wait_ns), double(g_timing.upload_bytes) / 1e6 / double(frames));
    g_timing = {};
}

}  // namespace

uint64_t draw_take_dump_request();          // nvn_draw.cpp

namespace {
std::shared_ptr<const DeferredValues> g_deferred;  // the submit being replayed (replay thread only)
}
const DeferredValues* current_deferred() { return g_deferred.get(); }

void gpu_submit(const std::vector<std::shared_ptr<CmdList>>& lists, std::shared_ptr<const DeferredValues> deferred) {
    g_deferred = std::move(deferred);
    std::lock_guard lk(g_gpu);
    ReplayScope replay;  // drain_objects (nothing replays now, under the GPU lock, #50), then the texture mark (#55)
    if (!init()) return;
    bool async = async_gpu();
    if (!async) {  // the previous submit was waited for: all of it is free
        free_retired_images(UINT64_MAX);
        p_vkResetDescriptorPool(g_device, g_descriptors, 0);
        g_upload_used = 0;
    }
    if (!begin(true)) return;
    draw_begin_submit();
    gpu_profile_begin(g_cb);
    zero_draws_begin_submit(g_cb);
    uint64_t t0 = plat::monotonic_ns();
    Targets st;
    for (auto& l : lists) replay_list(g_cb, st, *l, 0);
    draw_end_rendering(g_cb);
    g_timing.submits++, g_timing.replay_ns += plat::monotonic_ns() - t0;
    g_timing.upload_bytes += async ? frame_upload_used() : g_upload_used;
    gpu_profile_mark(g_cb, "end");
    if (async)  // the profile, zero-draw queries and snapshots read this submit's results right below: waited for
        frame_submit(VK_NULL_HANDLE, VK_NULL_HANDLE, draw_take_write_backs(),
                     gpu_profile_on() || zero_draws_on() || debug_snapshots_pending());
    else
        end_and_wait(VK_NULL_HANDLE, VK_NULL_HANDLE);
    draw_end_submit();  // asynchronous: snapshots only (the write-backs went with the submit)
    gpu_profile_collect();
    zero_draws_collect();
    debug_dump_requested(draw_take_dump_request());
}

// Headless: the presented texture is only counted, logged and dumped (no swapchain).
void present_headless(uint64_t texture) {
    if (!begin()) return;
    GpuImage* src = image_of(texture);
    static uint64_t presents = 0;
    presents++;
    if (presents % (presents < 300 ? 60 : 300) == 0) draw_log_stats();
    if (presents % 60 == 0) log_timing(60), gpu_profile_log(60);
    if (presents % 300 == 0) zero_draws_log();
    bool dump = false;
    if (src && src->layout != VK_IMAGE_LAYOUT_UNDEFINED && frame_dump_every() && presents % frame_dump_every() == 0) {
        transition(g_cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        dump = frame_dump_record(g_cb, *src);
    }
    if (async_gpu())
        frame_submit(VK_NULL_HANDLE, VK_NULL_HANDLE, {}, dump);  // a dumped frame is waited for
    else
        end_and_wait(VK_NULL_HANDLE, VK_NULL_HANDLE);
    if (dump) frame_dump_write(*src, presents);
    if (frame_dump_every() && presents % frame_dump_every() == 0) debug_dump_textures(presents);
}

void gpu_present(uint64_t texture, const int32_t* crop) {
    std::lock_guard lk(g_gpu);
    drain_objects();  // under the GPU lock, as in gpu_submit
    if (!init()) return;
    if (!async_gpu()) free_retired_images(UINT64_MAX);
    if (plat::headless()) return present_headless(texture);
    if (plat::window_minimized()) return;
    if (!g_swapchain && !make_swapchain()) return;
    uint32_t index = 0;
    VkSemaphore acquired = async_gpu() ? frame_acquire_semaphore() : g_acquired;
    VkResult r = p_vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
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
        if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] + crop[2] <= int32_t(src->width) &&
            crop[1] + crop[3] <= int32_t(src->height))  // nvnWindowSetCrop
            blit.srcOffsets[0] = {crop[0], crop[1], 0}, blit.srcOffsets[1] = {crop[0] + crop[2], crop[1] + crop[3], 1};
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
    if (presents % 300 == 0) zero_draws_log();
    bool dump = src && frame_dump_every() && presents % frame_dump_every() == 0 && frame_dump_record(g_cb, *src);
    VkSemaphore rendered = async_gpu() ? frame_rendered_semaphore(index) : g_rendered;
    if (async_gpu())
        frame_submit(acquired, rendered, {}, dump);  // a dumped frame is waited for
    else
        end_and_wait(acquired, rendered);
    if (dump) frame_dump_write(*src, presents);
    if (frame_dump_every() && presents % frame_dump_every() == 0) debug_dump_textures(presents);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swapchain;
    pi.pImageIndices = &index;
    r = p_vkQueuePresentKHR(g_queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) make_swapchain();
}

}  // namespace nx::nvn
