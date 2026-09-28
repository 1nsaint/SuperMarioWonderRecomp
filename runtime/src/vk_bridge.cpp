// Hand-written Vulkan bridges: loader, proc-address lookups (they hand out guest-callable thunks), instance and
// device creation (extension filtering), the Switch surface (VK_NN_vi_surface -> VK_KHR_win32_surface), physical
// device order (discrete GPU first) and debug callbacks (guest function pointers the host must never call).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vk_gen.h"

namespace nx {
void log_game_settings();  // game_settings.cpp
void apply_game_settings();
}

namespace nx::vk {

namespace {

PFN_vkGetInstanceProcAddr host_gipa = nullptr;
PFN_vkCreateInstance host_create_instance = nullptr;
PFN_vkEnumerateInstanceExtensionProperties host_enum_inst_ext = nullptr;
PFN_vkEnumerateInstanceLayerProperties host_enum_inst_layers = nullptr;
PFN_vkDestroyInstance host_destroy_instance = nullptr;
PFN_vkCreateDevice host_create_device = nullptr;
PFN_vkGetDeviceProcAddr host_gdpa = nullptr;
PFN_vkEnumeratePhysicalDevices host_enum_phys = nullptr;
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical = VK_NULL_HANDLE;

const std::unordered_set<std::string> kSpecial = {
    "vkGetInstanceProcAddr", "vkGetDeviceProcAddr", "vkCreateInstance", "vkEnumerateInstanceExtensionProperties",
    "vkEnumerateInstanceLayerProperties", "vkCreateDevice", "vkCreateViSurfaceNN", "vkCreateDebugUtilsMessengerEXT",
    "vkCreateDebugReportCallbackEXT", "vkDestroyInstance", "vkEnumeratePhysicalDevices",
    "vkDestroyDebugUtilsMessengerEXT", "vkDestroyDebugReportCallbackEXT", "vkGetPhysicalDeviceQueueFamilyProperties",
    "vkGetPhysicalDeviceQueueFamilyProperties2", "vkGetPhysicalDeviceQueueFamilyProperties2KHR", "vkQueuePresentKHR", "vkGetDeviceQueue", "vkCreateSwapchainKHR",
    "vkAcquireNextImageKHR", "vkGetSwapchainImagesKHR", "vkDestroySwapchainKHR", "vkWaitForFences",
    "vkGetPhysicalDeviceSurfaceSupportKHR", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", "vkGetPhysicalDeviceSurfaceFormatsKHR", "vkGetPhysicalDeviceSurfacePresentModesKHR", "vkDestroySurfaceKHR",
};

void load_loader() {
    if (host_gipa) return;
    host_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(plat::vulkan_get_instance_proc_addr());
    if (!host_gipa) fatal("no Vulkan loader");
    host_create_instance = reinterpret_cast<PFN_vkCreateInstance>(host_gipa(nullptr, "vkCreateInstance"));
    host_enum_inst_ext = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        host_gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    host_enum_inst_layers = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
        host_gipa(nullptr, "vkEnumerateInstanceLayerProperties"));
    // Global commands of the generated table.
    for (size_t k = 0; k < g_entry_count; k++) *g_entries[k].pfn = host_gipa(nullptr, g_entries[k].name);
}

void load_instance(VkInstance inst) {
    for (size_t k = 0; k < g_entry_count; k++)
        if (auto p = host_gipa(inst, g_entries[k].name)) *g_entries[k].pfn = p;
    host_destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(host_gipa(inst, "vkDestroyInstance"));
    host_create_device = reinterpret_cast<PFN_vkCreateDevice>(host_gipa(inst, "vkCreateDevice"));
    host_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(host_gipa(inst, "vkGetDeviceProcAddr"));
    host_enum_phys = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(host_gipa(inst, "vkEnumeratePhysicalDevices"));
}

std::string guest_str(uint64_t p) { return p ? std::string(reinterpret_cast<const char*>(p)) : std::string(); }

// Guest-callable address for a command we bridge, or 0.
uint64_t proc_address(const std::string& name, bool host_has) {
    if (kSpecial.count(name) || (host_has && hle_lookup(name.c_str()))) return hle_address(name.c_str());
    return 0;
}

// ---- instance -------------------------------------------------------------------------------------------------

void w_vkGetInstanceProcAddr(CPU& c) {
    load_loader();
    std::string name = guest_str(c.x[1]);
    VkInstance inst = arg<VkInstance>(c.x[0]);
    bool host_has = host_gipa(inst ? inst : g_instance, name.c_str()) != nullptr;
    c.x[0] = proc_address(name, host_has);
    if (g_trace || !c.x[0]) log("vkGetInstanceProcAddr(%s) -> %s", name.c_str(), c.x[0] ? "bridged" : "null");
}

void w_vkGetDeviceProcAddr(CPU& c) {
    std::string name = guest_str(c.x[1]);
    bool host_has = host_gdpa && host_gdpa(arg<VkDevice>(c.x[0]), name.c_str()) != nullptr;
    c.x[0] = proc_address(name, host_has);
    if (g_trace || !c.x[0]) log("vkGetDeviceProcAddr(%s) -> %s", name.c_str(), c.x[0] ? "bridged" : "null");
}

// Two-call enumeration helper: fills the guest array from `all`.
template <typename T>
VkResult enumerate(const std::vector<T>& all, uint64_t count_ptr, uint64_t out) {
    uint32_t n = ld<uint32_t>(count_ptr);
    if (!out) {
        st<uint32_t>(count_ptr, uint32_t(all.size()));
        return VK_SUCCESS;
    }
    uint32_t w = std::min<uint32_t>(n, uint32_t(all.size()));
    std::memcpy(reinterpret_cast<void*>(out), all.data(), sizeof(T) * w);
    st<uint32_t>(count_ptr, w);
    return w < all.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

void w_vkEnumerateInstanceExtensionProperties(CPU& c) {  // (layer, uint32_t* count, props*)
    load_loader();
    uint32_t n = 0;
    host_enum_inst_ext(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> all(n);
    host_enum_inst_ext(nullptr, &n, all.data());
    all.resize(n);
    VkExtensionProperties vi{};
    std::strcpy(vi.extensionName, VK_NN_VI_SURFACE_EXTENSION_NAME);
    vi.specVersion = 1;
    all.push_back(vi);  // the game asks for the Switch surface; we provide it on top of win32
    c.x[0] = ret_u64(enumerate(all, c.x[1], c.x[2]));
}

void w_vkEnumerateInstanceLayerProperties(CPU& c) {
    load_loader();
    st<uint32_t>(c.x[0], 0);  // the game runs without layers; validation is added by the host side on request
    c.x[0] = VK_SUCCESS;
}

void w_vkCreateInstance(CPU& c) {  // (const VkInstanceCreateInfo*, alloc, VkInstance*)
    load_loader();
    // The engine resets its settings to their registered defaults when it loads its configuration (early in
    // nnMain); the renderer, starting here, is what reads the display ones. Apply the player's settings again.
    apply_game_settings();
    VkInstanceCreateInfo ci = *reinterpret_cast<const VkInstanceCreateInfo*>(c.x[0]);
    uint32_t nhave = 0;
    host_enum_inst_ext(nullptr, &nhave, nullptr);
    std::vector<VkExtensionProperties> have(nhave);
    host_enum_inst_ext(nullptr, &nhave, have.data());
    auto host_has = [&](const char* e) {
        return std::any_of(have.begin(), have.end(), [&](const VkExtensionProperties& h) { return std::strcmp(h.extensionName, e) == 0; });
    };
    std::vector<const char*> exts;
    for (uint32_t k = 0; k < ci.enabledExtensionCount; k++) {
        const char* e = ci.ppEnabledExtensionNames[k];
        bool ok = std::strcmp(e, VK_NN_VI_SURFACE_EXTENSION_NAME) != 0 && host_has(e);
        log("vkCreateInstance: game wants %s%s", e, ok ? "" : " (not on this host, dropped)");
        if (ok) exts.push_back(e);
    }
    auto add = [&](const char* e) {
        if (std::none_of(exts.begin(), exts.end(), [&](const char* x) { return std::strcmp(x, e) == 0; })) exts.push_back(e);
    };
    add(VK_KHR_SURFACE_EXTENSION_NAME);
    add(plat::vulkan_surface_extension());
    std::vector<const char*> layers;
    if (const char* v = std::getenv("NX_VK_VALIDATION"); v && *v == '1') layers.push_back("VK_LAYER_KHRONOS_validation");
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = uint32_t(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    ci.pNext = nullptr;  // debug messenger infos carry guest callbacks
    // With validation on, report through a host messenger.
    static VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    if (!layers.empty()) {
        add(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        ci.enabledExtensionCount = uint32_t(exts.size());
        ci.ppEnabledExtensionNames = exts.data();
        dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dbg.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                                 const VkDebugUtilsMessengerCallbackDataEXT* d, void*) -> VkBool32 {
            log("[vk] %s", d->pMessage);
            return VK_FALSE;
        };
        ci.pNext = &dbg;
    }
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = host_create_instance(&ci, nullptr, &inst);
    log("vkCreateInstance -> %d (%zu extensions, %zu layers)", r, exts.size(), layers.size());
    if (r == VK_SUCCESS) {
        g_instance = inst;
        load_instance(inst);
        st<uint64_t>(c.x[2], reinterpret_cast<uint64_t>(inst));
    }
    c.x[0] = ret_u64(r);
}

void w_vkDestroyInstance(CPU& c) {
    if (host_destroy_instance) host_destroy_instance(arg<VkInstance>(c.x[0]), nullptr);
}

void w_vkEnumeratePhysicalDevices(CPU& c) {  // (instance, uint32_t* count, VkPhysicalDevice*)
    VkInstance inst = arg<VkInstance>(c.x[0]);
    uint32_t n = 0;
    host_enum_phys(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n);
    host_enum_phys(inst, &n, all.data());
    // Discrete GPUs first: the game takes the first device, like the single GPU of a Switch.
    std::stable_sort(all.begin(), all.end(), [](VkPhysicalDevice a, VkPhysicalDevice b) {
        VkPhysicalDeviceProperties pa, pb;
        p_vkGetPhysicalDeviceProperties(a, &pa);
        p_vkGetPhysicalDeviceProperties(b, &pb);
        return (pa.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) > (pb.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
    });
    // Only one: a Switch has one GPU, and The Forge's Switch path leaves per-GPU arrays unset for the others.
    if (all.size() > 1) all.resize(1);
    if (!all.empty()) {
        g_physical = all[0];
        VkPhysicalDeviceProperties p;
        p_vkGetPhysicalDeviceProperties(all[0], &p);
        log("vk: using %s (api %u.%u.%u)", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
    }
    c.x[0] = ret_u64(enumerate(all, c.x[1], c.x[2]));
}

// The Forge's Switch build keeps queue create infos in a 4-entry array; desktop GPUs expose more families
// (video decode/encode, optical flow). Report the first three: indices stay valid, family 0 does everything.
constexpr uint32_t kMaxQueueFamilies = 3;

void w_vkGetPhysicalDeviceQueueFamilyProperties(CPU& c) {  // (physical, uint32_t* count, props*)
    uint32_t n = 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties(arg<VkPhysicalDevice>(c.x[0]), &n, nullptr);
    std::vector<VkQueueFamilyProperties> all(n);
    p_vkGetPhysicalDeviceQueueFamilyProperties(arg<VkPhysicalDevice>(c.x[0]), &n, all.data());
    all.resize(std::min(n, kMaxQueueFamilies));
    enumerate(all, c.x[1], c.x[2]);
}

void w_vkGetPhysicalDeviceQueueFamilyProperties2(CPU& c) {  // (physical, uint32_t* count, VkQueueFamilyProperties2*)
    uint32_t n = 0;
    p_vkGetPhysicalDeviceQueueFamilyProperties2(arg<VkPhysicalDevice>(c.x[0]), &n, nullptr);
    uint32_t want = ld<uint32_t>(c.x[1]);
    n = std::min(n, kMaxQueueFamilies);
    if (!c.x[2]) {
        st<uint32_t>(c.x[1], n);
        return;
    }
    uint32_t w = std::min(want, n);
    p_vkGetPhysicalDeviceQueueFamilyProperties2(arg<VkPhysicalDevice>(c.x[0]), &w,
                                                reinterpret_cast<VkQueueFamilyProperties2*>(c.x[2]));
    st<uint32_t>(c.x[1], w);
}

// ---- device ---------------------------------------------------------------------------------------------------

void w_vkCreateDevice(CPU& c) {  // (physical, const VkDeviceCreateInfo*, alloc, VkDevice*)
    VkPhysicalDevice pd = arg<VkPhysicalDevice>(c.x[0]);
    VkDeviceCreateInfo ci = *reinterpret_cast<const VkDeviceCreateInfo*>(c.x[1]);
    uint32_t n = 0;
    p_vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> have(n);
    p_vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, have.data());
    std::vector<const char*> exts;
    for (uint32_t k = 0; k < ci.enabledExtensionCount; k++) {
        const char* e = ci.ppEnabledExtensionNames[k];
        bool ok = std::any_of(have.begin(), have.end(), [&](const VkExtensionProperties& h) { return std::strcmp(h.extensionName, e) == 0; });
        log("vkCreateDevice: game wants %s%s", e, ok ? "" : " (not on this GPU, dropped)");
        if (ok) exts.push_back(e);
    }
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = 0;
    ci.ppEnabledLayerNames = nullptr;
    VkDevice dev = VK_NULL_HANDLE;
    VkResult r = host_create_device(pd, &ci, nullptr, &dev);
    log("vkCreateDevice -> %d", r);
    if (r == VK_SUCCESS) st<uint64_t>(c.x[3], reinterpret_cast<uint64_t>(dev));
    c.x[0] = ret_u64(r);
}

// ---- surface and debug ----------------------------------------------------------------------------------------

// The OS can take the window away (Android: home, app switcher, screen lock) and hand out a new one; the Switch
// never loses its screen, so the game keeps its surface and swapchain for good. The handles it holds stay the
// same (the first host handles, used as names from then on) and the host surface and swapchain behind them are
// replaced when the window changes or the driver says they are lost.
struct SurfaceInfo {
    VkInstance instance;
    void* window;  // a reference we hold (plat::create_window)
    uint64_t generation;
    VkSurfaceKHR host;
};
std::unordered_map<uint64_t, SurfaceInfo> g_surfaces;  // game handle -> current

// The host surface behind a game handle, re-made on the current window if the OS replaced it. False: no window.
bool refresh_surface(SurfaceInfo& s) {
    uint64_t gen = plat::window_generation();
    if (s.host && s.generation == gen) return true;
    if (s.host) {
        p_vkDestroySurfaceKHR(s.instance, s.host, nullptr);
        plat::release_window(s.window);
        s.host = VK_NULL_HANDLE;
    }
    s.window = plat::create_window();
    s.generation = gen;
    uint64_t h = 0;
    VkResult r = VkResult(plat::vulkan_create_surface(s.instance, s.window, &h, reinterpret_cast<void*>(host_gipa)));
    log("vk: surface re-made on window %p (generation %llu): %d", s.window, (unsigned long long)gen, r);
    s.host = reinterpret_cast<VkSurfaceKHR>(h);
    return r == VK_SUCCESS;
}

VkSurfaceKHR host_surface(uint64_t game) {
    auto it = g_surfaces.find(game);
    if (it == g_surfaces.end()) return reinterpret_cast<VkSurfaceKHR>(game);
    refresh_surface(it->second);
    return it->second.host;
}

void w_vkCreateViSurfaceNN(CPU& c) {  // (instance, const VkViSurfaceCreateInfoNN*, alloc, VkSurfaceKHR*)
    auto* vi = reinterpret_cast<const VkViSurfaceCreateInfoNN*>(c.x[1]);
    uint64_t s = 0;
    auto r = VkResult(plat::vulkan_create_surface(arg<VkInstance>(c.x[0]), vi->window, &s,
                                                  reinterpret_cast<void*>(host_gipa)));
    log("vkCreateViSurfaceNN -> %s surface on %p: %d", plat::vulkan_surface_extension(), vi->window, r);
    if (r == VK_SUCCESS) {
        st<uint64_t>(c.x[3], s);
        // The window came from nn::vi::GetNativeWindow (a reference the game never gives back): ours now.
        g_surfaces[s] = {arg<VkInstance>(c.x[0]), vi->window, plat::window_generation(), reinterpret_cast<VkSurfaceKHR>(s)};
    }
    c.x[0] = ret_u64(r);
}

void w_vkDestroySurfaceKHR(CPU& c) {  // (instance, surface, alloc)
    auto it = g_surfaces.find(c.x[1]);
    if (it == g_surfaces.end()) {
        p_vkDestroySurfaceKHR(arg<VkInstance>(c.x[0]), arg<VkSurfaceKHR>(c.x[1]), nullptr);
        return;
    }
    if (it->second.host) p_vkDestroySurfaceKHR(it->second.instance, it->second.host, nullptr);
    plat::release_window(it->second.window);
    g_surfaces.erase(it);
}

void w_vkGetPhysicalDeviceSurfaceSupportKHR(CPU& c) {  // (physical, family, surface, VkBool32*)
    c.x[0] = ret_u64(p_vkGetPhysicalDeviceSurfaceSupportKHR(arg<VkPhysicalDevice>(c.x[0]), uint32_t(c.x[1]),
                                                            host_surface(c.x[2]), arg<VkBool32*>(c.x[3])));
}

void w_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(CPU& c) {  // (physical, surface, caps*)
    c.x[0] = ret_u64(p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(arg<VkPhysicalDevice>(c.x[0]), host_surface(c.x[1]),
                                                                 arg<VkSurfaceCapabilitiesKHR*>(c.x[2])));
}

void w_vkGetPhysicalDeviceSurfaceFormatsKHR(CPU& c) {  // (physical, surface, count*, formats*)
    c.x[0] = ret_u64(p_vkGetPhysicalDeviceSurfaceFormatsKHR(arg<VkPhysicalDevice>(c.x[0]), host_surface(c.x[1]),
                                                            arg<uint32_t*>(c.x[2]), arg<VkSurfaceFormatKHR*>(c.x[3])));
}

void w_vkGetPhysicalDeviceSurfacePresentModesKHR(CPU& c) {  // (physical, surface, count*, modes*)
    c.x[0] = ret_u64(p_vkGetPhysicalDeviceSurfacePresentModesKHR(
        arg<VkPhysicalDevice>(c.x[0]), host_surface(c.x[1]), arg<uint32_t*>(c.x[2]), arg<VkPresentModeKHR*>(c.x[3])));
}

// ---- screenshots: NX_SCREENSHOT=<frame>[,<frame>...] writes private/captures/frame_<n>.bmp ---------------------

// Hades keeps its per-frame command pools and buffers in fixed arrays of 3 (main+26e428), indexed by the swapchain
// image index. Hosts that hand out more images (Android: 4+) get a virtual swapchain: the game sees 3 plain images
// and each present copies the rendered one into the host image acquired for it.
constexpr uint32_t kMaxGuestImages = 3;

// Where the frame time goes, reported with the fps line: host time spent blocked in fence waits (GPU behind),
// acquire and present, summed over the report interval.
std::atomic<uint64_t> g_wait_fence_ns{0}, g_wait_acquire_ns{0}, g_wait_present_ns{0};

struct Timed {
    std::atomic<uint64_t>& sum;
    uint64_t t0 = plat::monotonic_ns();
    ~Timed() { sum += plat::monotonic_ns() - t0; }
};

struct SwapchainInfo {
    VkDevice device;
    VkFormat format;
    VkExtent2D extent;
    uint32_t layers = 1;
    VkSwapchainKHR host_sc = VK_NULL_HANDLE;  // the current host swapchain (the game's handle is its first one)
    uint64_t surface = 0;                     // the game's surface handle
    VkSwapchainCreateInfoKHR ci{};            // as created (host-adjusted), to re-make it
    std::vector<uint32_t> families;
    uint64_t generation = 0;  // window generation it was made for
    bool lost = false;        // the host said out of date / surface lost: re-make before the next acquire
    VkQueue queue = VK_NULL_HANDLE;  // where the game presents
    std::vector<VkImage> images;  // what the game sees (the virtual images when `virt`)
    // Virtual swapchain.
    bool virt = false;
    std::vector<VkImage> host;           // the host swapchain's images
    std::vector<VkDeviceMemory> memory;  // of the virtual images
    std::vector<uint32_t> host_of;       // virtual index -> host index acquired for it
    uint32_t next = 0;                   // next virtual index to hand out
    std::vector<VkSemaphore> copied;     // per host image: its copy is done, present may start
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmds;  // ring of copy command buffers
    std::vector<VkFence> fences;
    uint32_t slot = 0;
};
std::unordered_map<uint64_t, SwapchainInfo> g_swapchains;
std::unordered_map<uint64_t, std::pair<VkDevice, uint32_t>> g_queues;  // queue -> (device, family)

uint32_t find_memory_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    p_vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}

void destroy_virtual(SwapchainInfo& s) {
    if (!s.virt) return;
    p_vkDeviceWaitIdle(s.device);
    for (VkImage i : s.images) p_vkDestroyImage(s.device, i, nullptr);
    for (VkDeviceMemory m : s.memory) p_vkFreeMemory(s.device, m, nullptr);
    for (VkSemaphore x : s.copied) p_vkDestroySemaphore(s.device, x, nullptr);
    for (VkFence f : s.fences) p_vkDestroyFence(s.device, f, nullptr);
    if (s.pool) p_vkDestroyCommandPool(s.device, s.pool, nullptr);
}

void make_copied_semaphores(SwapchainInfo& s);

// Plain images with the swapchain's format, size and usage, standing in for the host swapchain's images.
bool make_virtual(SwapchainInfo& s, const VkSwapchainCreateInfoKHR& ci) {
    s.virt = true;
    s.images.assign(kMaxGuestImages, VK_NULL_HANDLE);
    s.memory.assign(kMaxGuestImages, VK_NULL_HANDLE);
    s.host_of.assign(kMaxGuestImages, 0);
    for (uint32_t k = 0; k < kMaxGuestImages; k++) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = ci.imageFormat;
        ii.extent = {ci.imageExtent.width, ci.imageExtent.height, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = ci.imageArrayLayers;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = ci.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (p_vkCreateImage(s.device, &ii, nullptr, &s.images[k]) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        p_vkGetImageMemoryRequirements(s.device, s.images[k], &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = find_memory_type(g_physical, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (p_vkAllocateMemory(s.device, &mai, nullptr, &s.memory[k]) != VK_SUCCESS) return false;
        p_vkBindImageMemory(s.device, s.images[k], s.memory[k], 0);
    }
    make_copied_semaphores(s);
    return true;
}

void w_vkGetDeviceQueue(CPU& c) {  // (device, family, index, VkQueue*)
    VkQueue q = VK_NULL_HANDLE;
    p_vkGetDeviceQueue(arg<VkDevice>(c.x[0]), uint32_t(c.x[1]), uint32_t(c.x[2]), &q);
    st<uint64_t>(c.x[3], reinterpret_cast<uint64_t>(q));
    g_queues[reinterpret_cast<uint64_t>(q)] = {arg<VkDevice>(c.x[0]), uint32_t(c.x[1])};
}

// The host's surface capabilities applied to the game's create info (what the Switch driver accepts is not what
// every host surface supports, Android/Adreno especially): keep the game's choices where the surface allows them.
void adapt_to_surface(VkSwapchainCreateInfoKHR& ci) {
    if (!g_physical) return;
    VkSurfaceCapabilitiesKHR caps{};
    p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physical, ci.surface, &caps);
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // for screenshots
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // for a virtual swapchain's copies
    if (!(caps.supportedCompositeAlpha & ci.compositeAlpha))
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                               VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
            if (caps.supportedCompositeAlpha & a) {
                ci.compositeAlpha = a;
                break;
            }
    if (!(caps.supportedTransforms & ci.preTransform)) ci.preTransform = caps.currentTransform;
    ci.minImageCount = std::max(ci.minImageCount, caps.minImageCount);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    uint32_t nm = 0;
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(g_physical, ci.surface, &nm, nullptr);
    std::vector<VkPresentModeKHR> modes(nm);
    p_vkGetPhysicalDeviceSurfacePresentModesKHR(g_physical, ci.surface, &nm, modes.data());
    if (std::find(modes.begin(), modes.end(), ci.presentMode) == modes.end()) ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
}

void get_host_images(SwapchainInfo& s) {
    uint32_t n = 0;
    p_vkGetSwapchainImagesKHR(s.device, s.host_sc, &n, nullptr);
    s.host.resize(n);
    p_vkGetSwapchainImagesKHR(s.device, s.host_sc, &n, s.host.data());
}

void make_copied_semaphores(SwapchainInfo& s) {
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    s.copied.assign(s.host.size(), VK_NULL_HANDLE);
    for (VkSemaphore& x : s.copied) p_vkCreateSemaphore(s.device, &sci, nullptr, &x);
}

// A new host surface/swapchain behind the game's handles (virtual swapchains only: the game's images stay).
bool remake(SwapchainInfo& s) {
    p_vkDeviceWaitIdle(s.device);
    for (VkSemaphore x : s.copied) p_vkDestroySemaphore(s.device, x, nullptr);
    s.copied.clear();
    if (s.host_sc) p_vkDestroySwapchainKHR(s.device, s.host_sc, nullptr);
    s.host_sc = VK_NULL_HANDLE;
    auto it = g_surfaces.find(s.surface);
    if (it == g_surfaces.end() || !refresh_surface(it->second)) return false;
    s.ci.surface = it->second.host;
    s.ci.pQueueFamilyIndices = s.families.empty() ? nullptr : s.families.data();
    adapt_to_surface(s.ci);
    VkResult r = p_vkCreateSwapchainKHR(s.device, &s.ci, nullptr, &s.host_sc);
    s.generation = it->second.generation;
    s.lost = r != VK_SUCCESS;
    if (r != VK_SUCCESS) {
        log("vk: swapchain re-make failed: %d", r);
        s.host_sc = VK_NULL_HANDLE;
        return false;
    }
    get_host_images(s);
    make_copied_semaphores(s);
    s.next = 0;
    log("vk: swapchain re-made on the new window (%zu host images)", s.host.size());
    return true;
}

void w_vkCreateSwapchainKHR(CPU& c) {  // (device, const VkSwapchainCreateInfoKHR*, alloc, VkSwapchainKHR*)
    VkSwapchainCreateInfoKHR ci = *reinterpret_cast<const VkSwapchainCreateInfoKHR*>(c.x[1]);
    uint64_t game_surface = reinterpret_cast<uint64_t>(ci.surface);
    ci.surface = host_surface(game_surface);
    ci.pNext = nullptr;
    if (ci.oldSwapchain)
        if (auto it = g_swapchains.find(reinterpret_cast<uint64_t>(ci.oldSwapchain)); it != g_swapchains.end())
            ci.oldSwapchain = it->second.host_sc;
    // What the Switch driver accepts is not what every host surface supports (Android/Adreno especially): keep
    // the game's choices where the surface allows them, otherwise fall back to what it does.
    adapt_to_surface(ci);
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = p_vkCreateSwapchainKHR(arg<VkDevice>(c.x[0]), &ci, nullptr, &sc);
    log("vkCreateSwapchainKHR %ux%u format %d, %u images, present mode %d -> %d", ci.imageExtent.width,
        ci.imageExtent.height, ci.imageFormat, ci.minImageCount, ci.presentMode, r);
    if (r == VK_SUCCESS) {
        st<uint64_t>(c.x[3], reinterpret_cast<uint64_t>(sc));
        SwapchainInfo& s = g_swapchains[reinterpret_cast<uint64_t>(sc)];
        s.device = arg<VkDevice>(c.x[0]);
        s.format = ci.imageFormat;
        s.extent = ci.imageExtent;
        s.layers = ci.imageArrayLayers;
        s.host_sc = sc;
        s.surface = game_surface;
        s.ci = ci;
        if (ci.imageSharingMode == VK_SHARING_MODE_CONCURRENT && ci.pQueueFamilyIndices)
            s.families.assign(ci.pQueueFamilyIndices, ci.pQueueFamilyIndices + ci.queueFamilyIndexCount);
        s.generation = plat::window_generation();
        get_host_images(s);
        uint32_t n = uint32_t(s.host.size());
        s.images = s.host;
        // Virtual (the game's own images, copied on present) when the host has more images than the game handles,
        // and wherever the window can be replaced: the host swapchain can then be re-made under the game.
        if (n > kMaxGuestImages || std::strcmp(plat::vulkan_surface_extension(), "VK_KHR_android_surface") == 0) {
            if (!(ci.imageUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) || !make_virtual(s, ci))
                fatal("swapchain has %u images, the game handles %u, and no virtual swapchain could be made", n,
                      kMaxGuestImages);
            log("vk: %u host swapchain images, the game sees %u (copied on present)", n, kMaxGuestImages);
        }
    }
    c.x[0] = ret_u64(r);
}

void w_vkGetSwapchainImagesKHR(CPU& c) {  // (device, swapchain, uint32_t* count, VkImage*)
    auto it = g_swapchains.find(c.x[1]);
    if (it == g_swapchains.end()) {
        c.x[0] = ret_u64(p_vkGetSwapchainImagesKHR(arg<VkDevice>(c.x[0]), arg<VkSwapchainKHR>(c.x[1]),
                                                   arg<uint32_t*>(c.x[2]), arg<VkImage*>(c.x[3])));
        return;
    }
    if (!it->second.virt) get_host_images(it->second), it->second.images = it->second.host;
    c.x[0] = ret_u64(enumerate(it->second.images, c.x[2], c.x[3]));
}

void w_vkDestroySwapchainKHR(CPU& c) {  // (device, swapchain, alloc)
    auto it = g_swapchains.find(c.x[1]);
    if (it == g_swapchains.end()) {
        p_vkDestroySwapchainKHR(arg<VkDevice>(c.x[0]), arg<VkSwapchainKHR>(c.x[1]), nullptr);
        return;
    }
    destroy_virtual(it->second);
    if (it->second.host_sc) p_vkDestroySwapchainKHR(it->second.device, it->second.host_sc, nullptr);
    g_swapchains.erase(it);
}

std::vector<uint64_t> screenshot_frames() {
    std::vector<uint64_t> v;
    if (const char* e = std::getenv("NX_SCREENSHOT"))
        for (const char* p = e; *p;) {
            v.push_back(std::strtoull(p, const_cast<char**>(&p), 10));
            if (*p == ',') p++;
        }
    return v;
}

// Copies the image about to be presented into a host buffer (waiting on the game's semaphores and signalling a
// new one the present waits on instead), then writes a BMP.
void capture(VkQueue queue, VkPresentInfoKHR& pi, uint64_t frame, VkSemaphore& out_sem) {
    auto sit = g_swapchains.find(reinterpret_cast<uint64_t>(pi.pSwapchains[0]));
    auto qit = g_queues.find(reinterpret_cast<uint64_t>(queue));
    if (sit == g_swapchains.end() || qit == g_queues.end() || !g_physical) return;
    SwapchainInfo& s = sit->second;
    VkDevice dev = s.device;
    VkImage img = s.images[pi.pImageIndices[0]];
    uint32_t w = s.extent.width, h = s.extent.height;
    VkDeviceSize size = VkDeviceSize(w) * h * 4;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    p_vkCreateBuffer(dev, &bci, nullptr, &buf);
    VkMemoryRequirements mr;
    p_vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_memory_type(g_physical, mr.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem;
    p_vkAllocateMemory(dev, &mai, nullptr, &mem);
    p_vkBindBufferMemory(dev, buf, mem, 0);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = qit->second.second;
    VkCommandPool pool;
    p_vkCreateCommandPool(dev, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb;
    p_vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    p_vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    p_vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    std::swap(b.oldLayout, b.newLayout);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
    p_vkEndCommandBuffer(cb);

    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    p_vkCreateSemaphore(dev, &sci, nullptr, &out_sem);
    std::vector<VkPipelineStageFlags> stages(pi.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = pi.waitSemaphoreCount;
    si.pWaitSemaphores = pi.pWaitSemaphores;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &out_sem;
    p_vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    p_vkQueueWaitIdle(queue);
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &out_sem;

    void* data = nullptr;
    p_vkMapMemory(dev, mem, 0, size, 0, &data);
    std::string dir = plat::data_root() + "/captures";
    plat::make_dirs(dir);
    std::string path = dir + "/frame_" + std::to_string(frame) + ".bmp";
    if (FILE* f = plat::fopen_utf8(path, "wb")) {
        // BITMAPFILEHEADER (14 bytes) + BITMAPINFOHEADER (40 bytes), little endian, top-down 32-bit.
        uint8_t hdr[54] = {'B', 'M'};
        auto put32 = [&](int at, uint32_t v) { std::memcpy(hdr + at, &v, 4); };
        put32(2, uint32_t(54 + size));
        put32(10, 54);
        put32(14, 40);
        put32(18, w);
        put32(22, uint32_t(-int32_t(h)));
        hdr[26] = 1;
        hdr[28] = 32;
        std::fwrite(hdr, 1, sizeof hdr, f);
        const uint8_t* px = static_cast<const uint8_t*>(data);
        bool rgba = s.format == VK_FORMAT_R8G8B8A8_UNORM || s.format == VK_FORMAT_R8G8B8A8_SRGB;
        std::vector<uint8_t> row(size_t(w) * 4);
        for (uint32_t y = 0; y < h; y++) {
            std::memcpy(row.data(), px + size_t(y) * w * 4, row.size());
            if (rgba)
                for (uint32_t x = 0; x < w; x++) std::swap(row[x * 4], row[x * 4 + 2]);  // BMP wants BGRA
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        log("screenshot: %s (%ux%u, format %d)", path.c_str(), w, h, s.format);
    }
    p_vkUnmapMemory(dev, mem);
    p_vkDestroyCommandPool(dev, pool, nullptr);
    p_vkDestroyBuffer(dev, buf, nullptr);
    p_vkFreeMemory(dev, mem, nullptr);
}

// A minimized window has no surface to draw to; the Switch never loses its screen, so the game does not handle
// VK_ERROR_OUT_OF_DATE_KHR. Hold the render thread while minimized (the game pauses, like a console asleep).
void wait_while_minimized() {
    while (plat::window_minimized()) plat::sleep_ns(50'000'000);
}

void w_vkWaitForFences(CPU& c) {  // (device, count, const VkFence*, waitAll, timeout)
    Timed t{g_wait_fence_ns};
    c.x[0] = ret_u64(p_vkWaitForFences(arg<VkDevice>(c.x[0]), uint32_t(c.x[1]), arg<const VkFence*>(c.x[2]),
                                       VkBool32(c.x[3]), c.x[4]));
}

void w_vkAcquireNextImageKHR(CPU& c) {  // (device, swapchain, timeout, semaphore, fence, uint32_t* index)
    wait_while_minimized();
    Timed timed{g_wait_acquire_ns};
    // The game's semaphore and fence go to the host acquire: they signal when the host image is free, which is
    // what the copy on present writes. The game gets the next virtual image.
    auto it = g_swapchains.find(c.x[1]);
    SwapchainInfo* s = it != g_swapchains.end() && it->second.virt ? &it->second : nullptr;
    uint32_t host = 0;
    VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
    if (!s) {
        r = p_vkAcquireNextImageKHR(arg<VkDevice>(c.x[0]), arg<VkSwapchainKHR>(c.x[1]), c.x[2], arg<VkSemaphore>(c.x[3]),
                                    arg<VkFence>(c.x[4]), arg<uint32_t*>(c.x[5]));
    } else {
        // Window replaced or swapchain lost: re-make it under the game (waiting for a window as long as it takes).
        for (int attempt = 0; attempt < 50; attempt++) {
            wait_while_minimized();
            if (s->lost || !s->host_sc || s->generation != plat::window_generation()) {
                if (!remake(*s)) {
                    plat::sleep_ns(100'000'000);
                    continue;
                }
            }
            r = p_vkAcquireNextImageKHR(s->device, s->host_sc, c.x[2], arg<VkSemaphore>(c.x[3]), arg<VkFence>(c.x[4]), &host);
            if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR || r == VK_TIMEOUT || r == VK_NOT_READY) break;
            log("vkAcquireNextImageKHR -> %d: re-making the swapchain", r);
            s->lost = true;
        }
    }
    if (s && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) {
        uint32_t v = s->next;
        s->next = (v + 1) % kMaxGuestImages;
        s->host_of[v] = host;
        st<uint32_t>(c.x[5], v);
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) log("vkAcquireNextImageKHR -> %d", r);
    // The game does not handle out-of-date (a console never loses its screen): suboptimal is success for it.
    c.x[0] = ret_u64(r == VK_SUBOPTIMAL_KHR ? VK_SUCCESS : r);
}

// Records and submits the copies of a present's virtual images into their host images: waits on the game's
// semaphores, signals the per-host-image `copied` semaphores the host present waits on instead, and rewrites the
// image indices to the host ones.
void copy_virtual(VkQueue queue, VkPresentInfoKHR& pi, std::vector<uint32_t>& indices, std::vector<VkSemaphore>& waits) {
    SwapchainInfo* first = nullptr;
    for (uint32_t k = 0; k < pi.swapchainCount; k++) {
        auto it = g_swapchains.find(reinterpret_cast<uint64_t>(pi.pSwapchains[k]));
        if (it != g_swapchains.end() && it->second.virt) {
            first = &it->second;
            break;
        }
    }
    if (!first) return;
    VkDevice dev = first->device;
    if (!first->pool) {
        auto q = g_queues.find(reinterpret_cast<uint64_t>(queue));
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = q != g_queues.end() ? q->second.second : 0;
        p_vkCreateCommandPool(dev, &pci, nullptr, &first->pool);
        first->cmds.resize(kMaxGuestImages);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = first->pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = kMaxGuestImages;
        p_vkAllocateCommandBuffers(dev, &cai, first->cmds.data());
        first->fences.resize(kMaxGuestImages);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (VkFence& f : first->fences) p_vkCreateFence(dev, &fci, nullptr, &f);
    }
    uint32_t slot = first->slot;
    first->slot = (slot + 1) % kMaxGuestImages;
    VkFence fence = first->fences[slot];
    VkCommandBuffer cb = first->cmds[slot];
    p_vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    p_vkResetFences(dev, 1, &fence);
    p_vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p_vkBeginCommandBuffer(cb, &bi);

    indices.assign(pi.pImageIndices, pi.pImageIndices + pi.swapchainCount);
    std::vector<VkSemaphore> signals;
    for (uint32_t k = 0; k < pi.swapchainCount; k++) {
        auto it = g_swapchains.find(reinterpret_cast<uint64_t>(pi.pSwapchains[k]));
        if (it == g_swapchains.end() || !it->second.virt) continue;
        SwapchainInfo& s = it->second;
        uint32_t v = indices[k], h = s.host_of[v];
        VkImage src = s.images[v], dst = s.host[h];
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, s.layers};
        auto barrier = [&](VkImage img, VkAccessFlags sa, VkAccessFlags da, VkImageLayout from, VkImageLayout to) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = sa;
            b.dstAccessMask = da;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img;
            b.subresourceRange = range;
            return b;
        };
        VkImageMemoryBarrier pre[2] = {
            barrier(src, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
            barrier(dst, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        };
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                               nullptr, 2, pre);
        VkImageCopy region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, s.layers};
        region.extent = {s.extent.width, s.extent.height, 1};
        p_vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                         &region);
        // The virtual image goes back to the layout the game left it in; later work on it (next frames) waits
        // for the copy through this barrier.
        VkImageMemoryBarrier post[2] = {
            barrier(src, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
            barrier(dst, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
        };
        p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                               nullptr, 2, post);
        indices[k] = h;
        signals.push_back(s.copied[h]);
    }
    p_vkEndCommandBuffer(cb);

    std::vector<VkPipelineStageFlags> stages(pi.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = pi.waitSemaphoreCount;
    si.pWaitSemaphores = pi.pWaitSemaphores;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = uint32_t(signals.size());
    si.pSignalSemaphores = signals.data();
    p_vkQueueSubmit(queue, 1, &si, fence);
    waits = std::move(signals);
    pi.waitSemaphoreCount = uint32_t(waits.size());
    pi.pWaitSemaphores = waits.data();
    pi.pImageIndices = indices.data();
}

// The present info with the host swapchains behind the game's handles; records which queue presents.
void to_host_swapchains(VkQueue queue, VkPresentInfoKHR& pi, std::vector<VkSwapchainKHR>& out) {
    out.assign(pi.pSwapchains, pi.pSwapchains + pi.swapchainCount);
    for (VkSwapchainKHR& sc : out)
        if (auto it = g_swapchains.find(reinterpret_cast<uint64_t>(sc)); it != g_swapchains.end()) {
            sc = it->second.host_sc;
            it->second.queue = queue;
        }
    pi.pSwapchains = out.data();
}

void w_vkQueuePresentKHR(CPU& c) {
    wait_while_minimized();
    static uint64_t frames = 0;
    static auto t0 = std::chrono::steady_clock::now();
    static const std::vector<uint64_t> shots = screenshot_frames();
    VkQueue queue = arg<VkQueue>(c.x[0]);
    VkPresentInfoKHR pi = *arg<const VkPresentInfoKHR*>(c.x[1]);
    VkSemaphore extra = VK_NULL_HANDLE;
    if (std::find(shots.begin(), shots.end(), frames + 1) != shots.end()) capture(queue, pi, frames + 1, extra);
    std::vector<uint32_t> host_indices;
    std::vector<VkSemaphore> copied;
    copy_virtual(queue, pi, host_indices, copied);
    const VkSwapchainKHR* game_swapchains = pi.pSwapchains;
    std::vector<VkSwapchainKHR> host_swapchains;
    to_host_swapchains(queue, pi, host_swapchains);
    uint64_t tp = plat::monotonic_ns();
    VkResult r = p_vkQueuePresentKHR(queue, &pi);
    g_wait_present_ns += plat::monotonic_ns() - tp;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
        // The window went away under us (or changed): re-make at the next acquire; the game never sees it.
        for (uint32_t k = 0; k < pi.swapchainCount; k++)
            if (auto it = g_swapchains.find(reinterpret_cast<uint64_t>(game_swapchains[k])); it != g_swapchains.end())
                it->second.lost = true;
        log("vkQueuePresentKHR -> %d: swapchain will be re-made", r);
        r = VK_SUCCESS;
    } else if (r == VK_SUBOPTIMAL_KHR) {
        r = VK_SUCCESS;
    }
    nx::g_frame = frames + 1;
    if (extra) {
        p_vkQueueWaitIdle(queue);
        p_vkDestroySemaphore(g_swapchains.begin()->second.device, extra, nullptr);
    }
    // Worst frame (time between presents) of the interval: what is felt as stutter.
    static uint64_t prev_present = 0, worst = 0;
    uint64_t now_ns = plat::monotonic_ns();
    if (prev_present) worst = std::max(worst, now_ns - prev_present);
    prev_present = now_ns;
    if (frames == 0) log_game_settings();
    if (++frames == 1 || frames % 300 == 0) {
        static auto last = t0;
        auto now = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(now - t0).count(), ds = std::chrono::duration<double>(now - last).count();
        auto per_frame = [](std::atomic<uint64_t>& a) { return a.exchange(0) / 300.0 / 1e6; };
        log("vk: frame %llu presented (%d), %.1f fps (last %.1f s), %.1f average; worst %.1f ms; per frame: "
            "fences %.2f ms, acquire %.2f ms, present %.2f ms",
            (unsigned long long)frames, r, frames == 1 ? 0.0 : 300 / ds, ds, frames / s, worst / 1e6,
            per_frame(g_wait_fence_ns), per_frame(g_wait_acquire_ns), per_frame(g_wait_present_ns));
        last = now;
        worst = 0;
    }
    c.x[0] = ret_u64(r);
}

void w_create_debug_callback(CPU& c) {  // a handle that is never used: the host must not call guest callbacks
    st<uint64_t>(c.x[3], 0xDEB6);
    c.x[0] = VK_SUCCESS;
}
void w_destroy_debug_callback(CPU&) {}

}  // namespace

NX_HLE("vkGetInstanceProcAddr", w_vkGetInstanceProcAddr);
NX_HLE("vkGetDeviceProcAddr", w_vkGetDeviceProcAddr);
NX_HLE("vkEnumerateInstanceExtensionProperties", w_vkEnumerateInstanceExtensionProperties);
NX_HLE("vkEnumerateInstanceLayerProperties", w_vkEnumerateInstanceLayerProperties);
NX_HLE("vkCreateInstance", w_vkCreateInstance);
NX_HLE("vkDestroyInstance", w_vkDestroyInstance);
NX_HLE("vkEnumeratePhysicalDevices", w_vkEnumeratePhysicalDevices);
NX_HLE("vkCreateDevice", w_vkCreateDevice);
NX_HLE("vkGetPhysicalDeviceQueueFamilyProperties", w_vkGetPhysicalDeviceQueueFamilyProperties);
NX_HLE("vkGetPhysicalDeviceQueueFamilyProperties2", w_vkGetPhysicalDeviceQueueFamilyProperties2);
NX_HLE("vkGetPhysicalDeviceQueueFamilyProperties2KHR", w_vkGetPhysicalDeviceQueueFamilyProperties2);
NX_HLE("vkCreateViSurfaceNN", w_vkCreateViSurfaceNN);
NX_HLE("vkQueuePresentKHR", w_vkQueuePresentKHR);
NX_HLE("vkAcquireNextImageKHR", w_vkAcquireNextImageKHR);
NX_HLE("vkWaitForFences", w_vkWaitForFences);
NX_HLE("vkGetDeviceQueue", w_vkGetDeviceQueue);
NX_HLE("vkCreateSwapchainKHR", w_vkCreateSwapchainKHR);
NX_HLE("vkGetSwapchainImagesKHR", w_vkGetSwapchainImagesKHR);
NX_HLE("vkDestroySwapchainKHR", w_vkDestroySwapchainKHR);
NX_HLE("vkDestroySurfaceKHR", w_vkDestroySurfaceKHR);
NX_HLE("vkGetPhysicalDeviceSurfaceSupportKHR", w_vkGetPhysicalDeviceSurfaceSupportKHR);
NX_HLE("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", w_vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
NX_HLE("vkGetPhysicalDeviceSurfaceFormatsKHR", w_vkGetPhysicalDeviceSurfaceFormatsKHR);
NX_HLE("vkGetPhysicalDeviceSurfacePresentModesKHR", w_vkGetPhysicalDeviceSurfacePresentModesKHR);
NX_HLE("vkCreateDebugUtilsMessengerEXT", w_create_debug_callback);
NX_HLE("vkCreateDebugReportCallbackEXT", w_create_debug_callback);
NX_HLE("vkDestroyDebugUtilsMessengerEXT", w_destroy_debug_callback);
NX_HLE("vkDestroyDebugReportCallbackEXT", w_destroy_debug_callback);

}  // namespace nx::vk
