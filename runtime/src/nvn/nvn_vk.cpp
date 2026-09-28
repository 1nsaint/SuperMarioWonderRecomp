// NVN's GPU side on Vulkan (docs/NVN.md, N3): the host device, a VkImage per NVN texture the GPU touches, the
// replay of recorded command lists, and presenting a window texture on the host swapchain.
//
// First version: every submit is recorded into one Vulkan command buffer and waited for; image layouts are tracked
// per image; clears and render targets only (draws come with shaders, N5/N6). On macOS this is MoltenVK: the
// instance enumerates portability drivers and the device enables VK_KHR_portability_subset.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nvn.h"
#include "platform.h"
#include "vk_gen.h"

#include <nx/game.h>

namespace nx::nvn {

using namespace nx::vk;

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0, height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

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
    if (has_extension(dext, "VK_KHR_portability_subset")) dexts.push_back("VK_KHR_portability_subset");
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

uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t k = 0; k < g_memory.memoryTypeCount; k++)
        if ((bits >> k) & 1 && (g_memory.memoryTypes[k].propertyFlags & want) == want) return k;
    return 0;
}

// The VkImage behind an NVN texture, made on first use (render target, clear, present).
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
    if (img->format == VK_FORMAT_UNDEFINED) {
        log("nvn/vk: texture %llx has format %#x with no Vulkan equivalent", (unsigned long long)guest, t->format);
        img->format = VK_FORMAT_R8G8B8A8_UNORM;
        img->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = img->format;
    ci.extent = {img->width, img->height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               (aspect ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!check(p_vkCreateImage(g_device, &ci, nullptr, &img->image), "vkCreateImage")) return nullptr;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(g_device, img->image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!check(p_vkAllocateMemory(g_device, &ai, nullptr, &img->memory), "vkAllocateMemory")) return nullptr;
    p_vkBindImageMemory(g_device, img->image, img->memory, 0);
    if (g_trace)
        log("nvn/vk: image for texture %llx: %ux%u format %#x -> %d", (unsigned long long)guest, img->width,
            img->height, t->format, int(img->format));
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
    b.subresourceRange = {img.aspect, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
    img.layout = to;
}

struct Exec {
    uint64_t colors[8] = {};
    uint32_t color_count = 0;
    uint64_t depth = 0;
};

void replay(VkCommandBuffer cb, Exec& st, const CmdList& list, int depth) {
    for (const Cmd& c : list.cmds) {
        switch (c.op) {
            case Cmd::SetRenderTargets:
                st.color_count = c.n;
                for (uint32_t k = 0; k < 8; k++) st.colors[k] = k < c.n ? c.a[k] : 0;
                st.depth = c.a[8];
                break;
            case Cmd::ClearColor: {
                if (c.n >= 8 || !st.colors[c.n]) break;
                GpuImage* img = image_of(st.colors[c.n]);
                if (!img || !(img->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) break;
                transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkClearColorValue v{};
                std::memcpy(v.float32, c.f, 16);
                VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                p_vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
                break;
            }
            case Cmd::ClearDepthStencil: {
                GpuImage* img = st.depth ? image_of(st.depth) : nullptr;
                if (!img || img->aspect == VK_IMAGE_ASPECT_COLOR_BIT) break;
                transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkClearDepthStencilValue v{c.f[0], uint32_t(c.i[0])};
                VkImageSubresourceRange r{img->aspect, 0, 1, 0, 1};
                p_vkCmdClearDepthStencilImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
                break;
            }
            case Cmd::CallList:
                if (depth < 8)
                    if (auto l = command_list(c.a[0])) replay(cb, st, *l, depth + 1);
                break;
            case Cmd::SetViewport:
            case Cmd::SetScissor:
                break;
        }
    }
}

bool begin() {
    p_vkResetCommandBuffer(g_cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return check(p_vkBeginCommandBuffer(g_cb, &bi), "vkBeginCommandBuffer");
}

void end_and_wait(VkSemaphore wait, VkSemaphore signal) {
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

void gpu_submit(CPU&, const std::vector<std::shared_ptr<CmdList>>& lists) {
    std::lock_guard lk(g_gpu);
    if (!init() || !begin()) return;
    Exec st;
    for (auto& l : lists) replay(g_cb, st, *l, 0);
    end_and_wait(VK_NULL_HANDLE, VK_NULL_HANDLE);
}

void gpu_present(uint64_t texture) {
    std::lock_guard lk(g_gpu);
    if (!init()) return;
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
    bool dump = src && dump_every() && presents % dump_every() == 0 && readback_record(*src);
    end_and_wait(g_acquired, g_rendered);
    if (dump) readback_write(*src, presents);
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
