// Memory guard (Android), as memguard_mac.cpp: the process's proportional set size (/proc/self/smaps_rollup, Pss) is
// checked every 250 ms, logged every 30 s, and past the limit the process ends at once, before the low-memory killer
// takes it (or the phone thrashes). The limit is NX_MEM_LIMIT_GB (config.txt), else 60% of the phone's RAM.
// GPU allocations of some drivers are not in Pss: the log shows what is, the Vulkan budget is for later (A6 in
// docs/nvn/ANDROID-PORT.md).
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <nx/runtime.h>

#include "platform.h"

namespace nx::plat {

namespace {

uint64_t pss_bytes() {
    FILE* f = std::fopen("/proc/self/smaps_rollup", "r");
    if (!f) return 0;
    char line[256];
    uint64_t kb = 0;
    while (std::fgets(line, sizeof line, f))
        if (std::strncmp(line, "Pss:", 4) == 0) {
            kb = std::strtoull(line + 4, nullptr, 10);
            break;
        }
    std::fclose(f);
    return kb << 10;
}

}  // namespace

void start_memory_guard() {
    const char* e = std::getenv("NX_MEM_LIMIT_GB");
    double ram = double(sysconf(_SC_PHYS_PAGES)) * double(sysconf(_SC_PAGESIZE)) / double(1ull << 30);
    double gb = e ? std::atof(e) : 0.0;
    if (gb <= 0) gb = ram > 0 ? ram * 0.6 : 6.0;
    const uint64_t limit = uint64_t(gb * double(1ull << 30));
    log("memory: guard at %.1f GB (the phone has %.1f GB)", gb, ram);
    std::thread([limit, gb] {
        uint64_t ticks = 0, peak = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            uint64_t f = pss_bytes();
            if (f > peak) peak = f;
            if (++ticks % 120 == 0)
                log("memory: %.2f GB (peak %.2f GB, limit %.1f GB)", double(f) / double(1ull << 30),
                    double(peak) / double(1ull << 30), gb);
            if (f > limit) {
                log("memory: %.2f GB is over the %.1f GB limit (NX_MEM_LIMIT_GB): stopping",
                    double(f) / double(1ull << 30), gb);
                _exit(3);
            }
        }
    }).detach();
}

}  // namespace nx::plat
