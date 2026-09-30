// A Vulkan pipeline cache kept on disk (docs/nvn/ANDROID-PORT.md A3, DEBUG-2.md #73): every pipeline built in a
// session is compiled again in the next one, a hitch at each new scene (Adreno takes tens of ms per pipeline).
// The cache is <data root>/cache/pipelines2_<vendor>_<device>.bin, loaded when the first pipeline is made and saved
// every 32 new pipelines (pipeline_cache() is called once per pipeline made): read under the GPU lock, written to the
// file on a thread of its own. Data from another driver is left out: its header must match this device's vendor,
// device and cache UUID. NX_PIPELINE_CACHE=0: no cache.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

VkPipelineCache g_cache = VK_NULL_HANDLE;
std::string g_path;
std::atomic<uint32_t> g_new{0};
std::atomic<bool> g_saving{false};

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NX_PIPELINE_CACHE");
        return !e || e[0] != '0';
    }();
    return on;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> v;
    if (FILE* f = plat::fopen_utf8(path, "rb")) {
        plat::fseek64(f, 0, SEEK_END);
        int64_t n = plat::ftell64(f);
        plat::fseek64(f, 0, SEEK_SET);
        if (n > 0) v.resize(size_t(n)), v.resize(std::fread(v.data(), 1, v.size(), f));
        std::fclose(f);
    }
    return v;
}

// The header every cache blob starts with (VkPipelineCacheHeaderVersionOne): it must be this device's.
bool matches(const std::vector<uint8_t>& data, const VkPhysicalDeviceProperties& props) {
    if (data.size() < 16 + VK_UUID_SIZE) return false;
    uint32_t h[4];
    std::memcpy(h, data.data(), sizeof h);
    return h[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && h[2] == props.vendorID && h[3] == props.deviceID &&
           std::memcmp(data.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

// The cache's contents are read here, on the replaying thread under the GPU lock, where no pipeline is being made;
// only the file is written on a thread of its own. Read on another thread while pipelines were made (the first
// version), MoltenVK could serialise a half-updated cache: a run that loaded one drew the course's tree trunk as huge
// brown triangles from a point on (DEBUG-2.md #76).
void save() {
    if (g_saving) return;  // the previous file is still being written
    size_t n = 0;
    std::vector<uint8_t> data;
    for (int tries = 0; tries < 4 && data.empty(); tries++) {  // INCOMPLETE: retried
        if (p_vkGetPipelineCacheData(vk_device(), g_cache, &n, nullptr) != VK_SUCCESS || !n) break;
        data.resize(n);
        if (p_vkGetPipelineCacheData(vk_device(), g_cache, &n, data.data()) != VK_SUCCESS) data.clear();
    }
    if (data.empty()) return log("nvn/vk: pipeline cache: the driver returned no data (%u pipelines)", g_new.load());
    data.resize(n);
    g_saving = true;
    std::thread([data = std::move(data)] {
        std::string tmp = g_path + ".tmp";
        if (FILE* f = plat::fopen_utf8(tmp, "wb")) {
            bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
            ok = std::fclose(f) == 0 && ok;
            if (ok && std::rename(tmp.c_str(), g_path.c_str()) == 0)
                log("nvn/vk: pipeline cache saved (%.1f MB)", double(data.size()) / 1e6);
        }
        g_saving = false;
    }).detach();
}

}  // namespace

VkPipelineCache pipeline_cache() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!enabled()) return;
        VkPhysicalDeviceProperties props{};
        p_vkGetPhysicalDeviceProperties(vk_physical(), &props);
        std::string dir = plat::data_root() + "/cache";
        plat::make_dirs(dir);
        char name[64];
        std::snprintf(name, sizeof name, "/pipelines2_%04x_%04x.bin", props.vendorID, props.deviceID);  // 2: #76
        g_path = dir + name;
        std::vector<uint8_t> data = read_file(g_path);
        bool use = matches(data, props);
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = use ? data.size() : 0;
        ci.pInitialData = use ? data.data() : nullptr;
        if (p_vkCreatePipelineCache(vk_device(), &ci, nullptr, &g_cache) != VK_SUCCESS) g_cache = VK_NULL_HANDLE;
        log("nvn/vk: pipeline cache %s: %s", g_path.c_str(),
            use ? "loaded" : data.empty() ? "new" : "from another driver, started over");
    });
    // One call per pipeline made, under the GPU lock (replays make them): every 32 the cache is saved.
    if (g_cache && (g_new.fetch_add(1) + 1) % 32 == 0) save();
    return g_cache;
}

}  // namespace nx::nvn
