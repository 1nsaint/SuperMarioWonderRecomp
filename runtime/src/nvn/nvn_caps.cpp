// What the device offers that the Android performance plan (docs/nvn/android-perf/PLAN.md, phase 0) decides on:
// descriptor and pipeline extensions (R3, P1), dynamic state, present timing (P2), and the memory types (R5: is there
// a host-cached one for reading compute results back). Logged once, when the device is made.
#include <cstdlib>
#include <cstring>
#include <string>

#include "nvn_vk.h"

namespace nx::nvn {

using namespace nx::vk;

void log_device_caps(VkPhysicalDevice physical, const std::vector<VkExtensionProperties>& extensions) {
    static const char* const wanted[] = {
        "VK_KHR_push_descriptor",          "VK_EXT_descriptor_buffer",        "VK_KHR_descriptor_update_template",
        "VK_EXT_graphics_pipeline_library", "VK_EXT_robustness2",              "VK_EXT_extended_dynamic_state",
        "VK_EXT_extended_dynamic_state2",  "VK_EXT_extended_dynamic_state3",  "VK_GOOGLE_display_timing",
        "VK_KHR_present_wait",             "VK_KHR_present_id",               "VK_EXT_attachment_feedback_loop_layout",
        "VK_KHR_dynamic_rendering_local_read", "VK_QCOM_render_pass_store_ops", "VK_EXT_load_store_op_none",
    };
    std::string have, missing;
    for (const char* w : wanted) {
        bool found = false;
        for (const VkExtensionProperties& e : extensions) found = found || std::strcmp(e.extensionName, w) == 0;
        std::string& list = found ? have : missing;
        if (!list.empty()) list += " ";
        list += w;
    }
    log("nvn/vk: extensions present: %s", have.c_str());
    log("nvn/vk: extensions missing: %s", missing.c_str());
    VkPhysicalDeviceMemoryProperties mp{};
    p_vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    std::string types;
    for (uint32_t k = 0; k < mp.memoryTypeCount; k++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[k].propertyFlags;
        types += " " + std::to_string(k) + ":";
        if (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) types += "D";
        if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) types += "V";
        if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) types += "C";
        if (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) types += "$";
        if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) types += "L";
        types += "/h" + std::to_string(mp.memoryTypes[k].heapIndex);
    }
    log("nvn/vk: memory types (D device-local, V host-visible, C coherent, $ cached, L lazy):%s", types.c_str());
}

// The upload ring takes ~11 MB of CPU writes per course frame (copies and zero padding). Host-visible coherent memory
// is write-combined (uncached) on Adreno; NX_RING_CACHED=1 asks for a host-cached coherent type first, where the GPU
// snoops the CPU's caches (PLAN.md R5).
uint32_t upload_memory_type(uint32_t bits) {
    const VkMemoryPropertyFlags plain = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const char* e = std::getenv("NX_RING_CACHED");
    if (e && e[0] == '1') {
        VkPhysicalDeviceMemoryProperties mp{};
        p_vkGetPhysicalDeviceMemoryProperties(vk_physical(), &mp);
        for (uint32_t k = 0; k < mp.memoryTypeCount; k++)
            if (bits >> k & 1 && (mp.memoryTypes[k].propertyFlags & (plain | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
                                     (plain | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
                log("nvn/vk: upload ring in host-cached memory (type %u)", k);
                return k;
            }
    }
    return memory_type(bits, plain);
}

}  // namespace nx::nvn
