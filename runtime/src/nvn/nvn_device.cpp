// NVN device (docs/NVN.md, phases N2-N3). Values come from what the game checks and from the SDK's own NVN driver
// (recompiled in sdk; NX_NVN_REAL=1 records where each of its procs lives, see nvn_trace.cpp).
#include <map>

#include <nx/runtime.h>

namespace nx {

namespace {

// nvnDeviceGetInteger(const NVNdevice*, NVNdeviceInfo pname, int* value).
// 0, 1: API version. Wonder requires major == 55 and minor >= 15 (main+5a7b48), so we report 55.15.
const std::map<uint32_t, int32_t> kDeviceInfo = {
    {0, 55},
    {1, 15},
};

void nvnDeviceGetInteger(CPU& c) {
    uint32_t pname = uint32_t(c.x[1]);
    auto it = kDeviceInfo.find(pname);
    if (it == kDeviceInfo.end()) log("nvn: nvnDeviceGetInteger(%u) unknown -> 0 (lr %s)", pname, describe(c.x[30]).c_str());
    if (c.x[2]) st<int32_t>(c.x[2], it == kDeviceInfo.end() ? 0 : it->second);
}

}  // namespace

NX_HLE("nvnDeviceGetInteger", nvnDeviceGetInteger);

}  // namespace nx
