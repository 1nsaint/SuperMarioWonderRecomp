// Boot: map and link the recompiled modules (what rtld does), create the main thread and enter
// nn::init::Start(main thread handle, main module base, init, fini) like rtld's _start.
// The platform's entry point (platform_win.cpp / platform_android.cpp) calls nx::boot().
#include <cstdio>
#include <cstdlib>
#include <string>

#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"

extern const nx::ModuleInfo nx_module_main;
extern const nx::ModuleInfo nx_module_sdk;

namespace nx {

void apply_game_settings();  // game_settings.cpp
void apply_widescreen();

uint64_t mrs(CPU& c, uint32_t sysreg) {
    switch (sysreg) {
        case 0xDF02:  // CNTVCT_EL0
        case 0xDF01:  // CNTPCT_EL0
            return system_tick();
        case 0xDF00:  // CNTFRQ_EL0
            return 19200000;
        case 0xD807:  // DCZID_EL0: DC ZVA allowed, 64-byte blocks
            return 4;
        case 0xC000:  // MIDR_EL1 (Cortex-A57)
            return 0x411FD071;
        default:
            log("mrs %#x at lr %s", sysreg, describe(c.x[30]).c_str());
            return 0;
    }
}

void msr(CPU& c, uint32_t sysreg, uint64_t value) {
    log("msr %#x = %llx at lr %s", sysreg, (unsigned long long)value, describe(c.x[30]).c_str());
}

// ---- module initializers --------------------------------------------------------------------------------------
// Each module's DT_INIT (at +0x30 in both main and sdk, see .dynamic) registers the module with the libc
// (__nnmusl_init_dso: TLS, unwind info) and runs its init_array, unless the registration reports 1: then the
// constructors are left to the loader and we run them. Registration only works once the program has given nn::os
// a TLS allocator (SetMemoryAllocatorForThreadLocal, in nninitStartup), so rtld's initializer callback does nothing
// and the modules are initialized right before nnMain, SDK first. Static TLS itself comes from the kernel
// (kernel.cpp: a block per thread at TLS region +0x1F0, where nn::os loads TPIDR_EL0 from).
constexpr uint64_t kDtInit = 0x30;

static void call_preserving(CPU& c, uint64_t target) {
    uint64_t saved[13];
    for (int r = 19; r <= 31; r++) saved[r - 19] = c.x[r];
    call(c, target);
    for (int r = 19; r <= 31; r++) c.x[r] = saved[r - 19];
}

// DT_INIT starts with `adrp x19, page; ldr w8, [x19, #off]`: the word where it keeps __nnmusl_init_dso's result.
static uint64_t init_status_addr(const ModuleInfo& m) {
    uint64_t pc = m.base + kDtInit + 0x10;
    uint32_t adrp = ld<uint32_t>(pc), ldr = ld<uint32_t>(pc + 4);
    int64_t imm = int64_t(((adrp >> 5) & 0x7FFFF) << 2 | ((adrp >> 29) & 3)) << 43 >> 31;  // sign-extended << 12
    return (pc & ~0xFFFull) + uint64_t(imm) + ((ldr >> 10) & 0xFFF) * 4;
}

static void init_module(CPU& c, const ModuleInfo& m) {
    call_preserving(c, m.base + kDtInit);
    uint32_t status = ld<uint32_t>(init_status_addr(m));
    if (status == 1)
        for (size_t k = 0; k < m.init_count; k++) call_preserving(c, m.base + m.init_array[k]);
    log("%s constructors: %zu", m.name, status == 1 ? m.init_count : 0);
}

static void rtld_init(CPU&) {}
static void rtld_fini(CPU&) {}

// nnMain, as called by nn::init::Start: the constructors need the program's allocator, which nninitStartup has
// set up by now, so they run here, right before the real nnMain.
static void nn_main(CPU& c) {
    init_module(c, nx_module_sdk);
    init_module(c, nx_module_main);
    apply_game_settings();
    apply_widescreen();
    log("entering nnMain");
    uint64_t real = 0;
    for (size_t k = 0; k < nx_module_main.symbol_count; k++)
        if (!nx_module_main.symbols[k].imported && std::string(nx_module_main.symbols[k].name) == "nnMain")
            real = nx_module_main.base + nx_module_main.symbols[k].value;
    call(c, real);
}

// ---- render resolution ----------------------------------------------------------------------------------------
// nnMain sets the game's output size to 1280x720 in two globals (main+0x8ccd98 and main+0x8ccec0: {u32 width,
// u32 height}) and then calls nn::time::Initialize before creating the renderer; that import is our hook to put
// the configured resolution there. The game renders natively at any size and aspect (wider than 16:9 it adds its
// own side art).
static bool render_resolution(uint32_t& w, uint32_t& h) {
    if (const char* r = std::getenv("NX_RESOLUTION")) return std::sscanf(r, "%ux%u", &w, &h) == 2 && w && h;
#if defined(__ANDROID__)
    plat::window_size(w, h);  // the phone's own screen, in its own aspect
    return w && h;
#else
    return false;
#endif
}

static void time_initialize(CPU& c) {
    uint32_t w = 0, h = 0;
    if (render_resolution(w, h)) {
        for (uint64_t off : {0x8ccd98ull, 0x8ccec0ull}) {
            uint64_t p = nx_module_main.base + off;
            if (ld<uint32_t>(p) == 1280 && ld<uint32_t>(p + 4) == 720) {
                st<uint32_t>(p, w);
                st<uint32_t>(p + 4, h);
            }
        }
        log("render resolution %ux%u", w, h);
    }
    call(c, find_symbol("_ZN2nn4time10InitializeEv"));
}

// longjmp(buf, val): unwinds the host stack to the recompiled function that called setjmp on buf.
static void long_jmp(CPU& c) { throw LongJmp{c.x[0], uint32_t(c.x[1])}; }

static void process_return(CPU&) {
    log("nn::init::Start returned");
    plat::exit_process(0);
}

NX_HLE("nnMain", nn_main);
NX_HLE("_ZN2nn4time10InitializeEv", time_initialize);
NX_HLE("longjmp", long_jmp);
NX_HLE("_longjmp", long_jmp);
NX_HLE("__rtld_init", rtld_init);
NX_HLE("__rtld_fini", rtld_fini);
NX_HLE("__process_return", process_return);

// ---- boot -----------------------------------------------------------------------------------------------------

void boot() {
    if (const char* t = std::getenv("NX_TRACE")) g_trace = *t == '1';
    if (plat::setting("trace") == "1") g_trace = true;
    memory_init();
    kernel_init();
    const ModuleInfo* mods[] = {&nx_module_main, &nx_module_sdk};
    load_modules(mods, 2, plat::data_root() + "/analysis");

    uint32_t main_thread = create_main_thread(16ull << 20);
    CPU& c = current_cpu();
    uint64_t start = find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (!start) fatal("nn::init::Start not found");
    c.x[0] = main_thread;
    c.x[1] = nx_module_main.base;
    c.x[2] = hle_address("__rtld_init");
    c.x[3] = hle_address("__rtld_fini");
    c.x[30] = hle_address("__process_return");
    log("entering nn::init::Start at %s", describe(start).c_str());
    call(c, start);
    log("returned from nn::init::Start");
}

}  // namespace nx
