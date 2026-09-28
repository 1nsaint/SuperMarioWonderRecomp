// NVN entry points (docs/NVN.md, phase N2).
//
// The game links one NVN symbol, nvnBootstrapLoader, and asks it for nvnDeviceGetProcAddress; every other NVN
// function is then fetched by name, the same shape as vkGetInstanceProcAddr. Both are HLE here, so the SDK's NVN
// driver never runs. A name we implement (an NX_HLE with the NVN name) gets its function; any other name gets a
// tracing stub that records the call and its arguments and returns a neutral value, so the trace goes as far as it
// can. The trace is written to <data root>/nvn_trace.txt: one line per call (the first kFullCalls of each proc),
// then a count per proc at exit.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

namespace {

constexpr uint32_t kFullCalls = 32;  // calls of one proc traced with their arguments

std::mutex g_lock;
FILE* g_trace_file = nullptr;
std::map<std::string, uint64_t> g_calls;  // proc -> calls
std::atomic<uint64_t> g_seq{0};

std::string guest_str(uint64_t addr) {
    if (!addr) return "(null)";
    return std::string(reinterpret_cast<const char*>(addr), strnlen(reinterpret_cast<const char*>(addr), 256));
}

void write_line(const char* line) {
    if (!g_trace_file) {
        g_trace_file = plat::fopen_utf8(plat::data_root() + "/nvn_trace.txt", "w");
        std::atexit([] {
            std::lock_guard lk(g_lock);
            if (!g_trace_file) return;
            std::fprintf(g_trace_file, "\n# calls per proc\n");
            for (auto& [name, n] : g_calls) std::fprintf(g_trace_file, "%10llu %s\n", (unsigned long long)n, name.c_str());
            std::fclose(g_trace_file);
            g_trace_file = nullptr;
        });
    }
    if (g_trace_file) {
        std::fputs(line, g_trace_file);
        std::fputc('\n', g_trace_file);
        std::fflush(g_trace_file);
    }
}

// A proc we do not implement yet: record it, return 0 except for the ones whose 0 means "failed" to the game.
void trace_stub(CPU& c) {
    const char* name = hle_current_name();
    uint64_t n, seq = g_seq++;
    {
        std::lock_guard lk(g_lock);
        n = ++g_calls[name];
        if (n <= kFullCalls) {
            char line[512];
            std::snprintf(line, sizeof line, "%8llu %s(%llx, %llx, %llx, %llx, %llx, %llx) lr %s", (unsigned long long)seq,
                          name, (unsigned long long)c.x[0], (unsigned long long)c.x[1], (unsigned long long)c.x[2],
                          (unsigned long long)c.x[3], (unsigned long long)c.x[4], (unsigned long long)c.x[5],
                          describe(c.x[30]).c_str());
            write_line(line);
        }
    }
    if (n == 1) log("nvn: first call of %s (lr %s)", name, describe(c.x[30]).c_str());
    // Initialize functions return NVNboolean: false would make the engine give up at the first object.
    c.x[0] = std::strstr(name, "Initialize") ? 1 : 0;
}

// Where the SDK's own NVN driver implements `name` (its nvnBootstrapLoader, recompiled, still exported by sdk):
// the reference for signatures and constants, recorded in the trace as "# real <name> = sdk+<offset>".
void note_real_proc(CPU& c, const std::string& name) {
    static uint64_t real_loader = find_symbol("nvnBootstrapLoader");
    static uint64_t scratch = kernel_alloc(0x1000);
    if (!real_loader || name.size() >= 0x1000) return;
    std::memcpy(reinterpret_cast<void*>(scratch), name.c_str(), name.size() + 1);
    uint64_t saved[31];
    std::memcpy(saved, c.x, sizeof saved);
    c.x[0] = scratch;
    call(c, real_loader);
    uint64_t real = c.x[0];
    std::memcpy(c.x, saved, sizeof saved);
    std::lock_guard lk(g_lock);
    write_line(("# real " + name + " = " + describe(real)).c_str());
}

uint64_t proc_address(CPU& c, const std::string& name) {
    if (name.empty()) return 0;
    static std::mutex seen_lock;
    static std::map<std::string, bool> seen;
    bool first;
    {
        std::lock_guard lk(seen_lock);
        first = !seen[name];
        seen[name] = true;
    }
    if (first && std::getenv("NX_NVN_REAL")) note_real_proc(c, name);
    return hle_address_or(name.c_str(), trace_stub);
}

// PFNNVNGENERICFUNCPROC nvnBootstrapLoader(const char* name)
void nvn_bootstrap_loader(CPU& c) {
    std::string name = guest_str(c.x[0]);
    log("nvnBootstrapLoader(%s)", name.c_str());
    {
        std::lock_guard lk(g_lock);
        write_line(("# nvnBootstrapLoader(" + name + ")").c_str());
    }
    c.x[0] = proc_address(c, name);
}

// PFNNVNGENERICFUNCPROC nvnDeviceGetProcAddress(const NVNdevice* device, const char* name)
void nvn_device_get_proc_address(CPU& c) {
    std::string name = guest_str(c.x[1]);
    if (g_trace) log("nvnDeviceGetProcAddress(%llx, %s)", (unsigned long long)c.x[0], name.c_str());
    c.x[0] = proc_address(c, name);
}

}  // namespace

NX_HLE("nvnBootstrapLoader", nvn_bootstrap_loader);
NX_HLE("nvnDeviceGetProcAddress", nvn_device_get_proc_address);

}  // namespace nx
