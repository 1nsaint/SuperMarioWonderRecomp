// Boot: map and link the recompiled modules (what rtld does), create the main thread and enter
// nn::init::Start(main thread handle, main module base, init, fini) like rtld's _start.
#include <cstdlib>
#include <string>

#include <windows.h>

#include <nx/runtime.h>

#include "kernel.h"

extern const nx::ModuleInfo nx_module_main;
extern const nx::ModuleInfo nx_module_sdk;

namespace nx {

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

// Module initializers. Each module's DT_INIT (at +0x30 in both main and sdk, see .dynamic) registers the module's
// TLS and unwind info with __nnmusl_init_dso and runs its init_array.
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

// Run a module's initializers the way rtld does: DT_INIT registers the module with the libc (TLS, unwind info);
// when that registration reports 1 the constructors are left to the loader, so we run the init_array.
// The main thread's static TLS. New threads get theirs from nn::os's thread entry, which calls the libc's
// thread-start hook (it allocates the TLS block and stores it at TLS region +0x1F0) and then loads TPIDR_EL0 from
// there; on Horizon rtld does the same for the main thread. Hook variable: sdk GOT slot 0x11e8628.
constexpr uint64_t kThreadStartHookGot = 0x11e8628;

static void setup_main_thread_tls(CPU& c) {
    uint64_t var = ld<uint64_t>(nx_module_sdk.base + kThreadStartHookGot);
    uint64_t hook = var ? ld<uint64_t>(var) : 0;
    if (hook) {
        uint64_t saved0 = c.x[0];
        c.x[0] = ld<uint64_t>(c.tpidrro + 0x1F8);  // ThreadType*
        call_preserving(c, hook);
        c.x[0] = saved0;
    }
    c.tpidr = ld<uint64_t>(c.tpidrro + 0x1F0);
    log("main thread TLS: hook %s, TPIDR_EL0 = %llx", hook ? describe(hook).c_str() : "none",
        (unsigned long long)c.tpidr);
}

// rtld's initializer callback. Like rtld, register the modules with the libc in load order: main first (it
// becomes the head of the libc's module list; its constructors stay pending, init_dso reports 1), the SDK last
// (the libc itself: it then lays out static TLS for every module, sets TPIDR_EL0 for the main thread and installs
// the thread-pointer hooks nn::os uses for new threads).
// libc internals (sdk): the head of the libc's module list, and the function that starts a module (for the head:
// allocates static TLS for all modules, copies .tdata, sets TPIDR_EL0 and installs the thread-pointer hooks).
constexpr uint64_t kMuslDsoHead = 0x1294ed0;
constexpr uint64_t kMuslStartDso = 0x51dd38;

// Module initializers can only run once the program has given nn::os a TLS allocator
// (SetMemoryAllocatorForThreadLocal, done in nninitStartup): __nnmusl_init_dso bails out before that. So rtld's
// callback does nothing and the modules are initialized right before nnMain, SDK first.
static void rtld_init(CPU&) {}

// The program's pending constructors, once nninitStartup has set up its allocator.
static void init_module(CPU& c, const ModuleInfo& m) {
    call_preserving(c, m.base + kDtInit);
    uint32_t status = ld<uint32_t>(init_status_addr(m));
    if (status == 1)
        for (size_t k = 0; k < m.init_count; k++) call_preserving(c, m.base + m.init_array[k]);
    log("%s constructors: %zu", m.name, status == 1 ? m.init_count : 0);
}

// nnMain, as called by nn::init::Start: the program's constructors need its allocator, which nninitStartup has
// set up by now, so they run here, right before the real nnMain.
static void nn_main(CPU& c) {
    log("TLS allocator set: %llx", (unsigned long long)ld<uint64_t>(nx_module_sdk.base + 0x11f5008));
    init_module(c, nx_module_sdk);
    init_module(c, nx_module_main);
    log("TPIDR_EL0 = %llx, musl head %llx", (unsigned long long)c.tpidr,
        (unsigned long long)ld<uint64_t>(nx_module_sdk.base + kMuslDsoHead));
    uint64_t list = ld<uint64_t>(nx_module_sdk.base + 0x1294ec8), count = ld<uint64_t>(nx_module_sdk.base + 0x1294ed8);
    log("musl modules: list %llx count %llu", (unsigned long long)list, (unsigned long long)count);
    for (uint64_t k = 0; list && k < 8; k++) {
        uint64_t d = ld<uint64_t>(list + 8 * k);
        if (!d) continue;
        log("  [%llu] dso %llx: %llx %llx %llx %llx %llx %llx flags %x", (unsigned long long)k, (unsigned long long)d,
            (unsigned long long)ld<uint64_t>(d), (unsigned long long)ld<uint64_t>(d + 8),
            (unsigned long long)ld<uint64_t>(d + 0x10), (unsigned long long)ld<uint64_t>(d + 0x18),
            (unsigned long long)ld<uint64_t>(d + 0x20), (unsigned long long)ld<uint64_t>(d + 0x30),
            ld<uint8_t>(d + 0x38));
    }
    log("entering nnMain");
    uint64_t real = 0;
    for (size_t k = 0; k < nx_module_main.symbol_count; k++)
        if (!nx_module_main.symbols[k].imported && std::string(nx_module_main.symbols[k].name) == "nnMain")
            real = nx_module_main.base + nx_module_main.symbols[k].value;
    call(c, real);
}
NX_HLE("nnMain", nn_main);
static void rtld_fini(CPU&) {}
static void process_return(CPU&) {
    log("nn::init::Start returned");
    ExitProcess(0);
}
NX_HLE("__rtld_init", rtld_init);
NX_HLE("__rtld_fini", rtld_fini);
NX_HLE("__process_return", process_return);

}  // namespace nx

// Debugging aid: NX_WATCH=<hex address> puts a hardware write watchpoint (DR0, 8 bytes) on every thread the
// kernel starts and on the main thread; the first write logs the host RIP (module) and the guest context.
static uint64_t g_watch = 0;

static void arm_watch(HANDLE thread) {
    if (!g_watch) return;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(thread, &ctx);
    ctx.Dr0 = g_watch;
    ctx.Dr7 = (1ull << 0) | (1ull << 16) | (3ull << 18);  // L0, R/W0 = write, LEN0 = 8 bytes
    SetThreadContext(thread, &ctx);
}

static LONG WINAPI watch_handler(EXCEPTION_POINTERS* e) {
    using namespace nx;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_watch) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(e->ExceptionRecord->ExceptionAddress), &mod);
    if (mod) GetModuleFileNameA(mod, name, sizeof name);
    log("WATCH: %llx written (now %llx) by host %p in %s (+%llx)", (unsigned long long)g_watch,
        (unsigned long long)ld<uint64_t>(g_watch), e->ExceptionRecord->ExceptionAddress, name,
        (unsigned long long)(uint64_t(e->ExceptionRecord->ExceptionAddress) - uint64_t(mod)));
    CPU& c = current_cpu();
    log("  guest lr %s", describe(c.x[30]).c_str());
    for (int k = 0; k < 32; k += 4)
        log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
            (unsigned long long)c.x[k + 2], (unsigned long long)c.x[k + 3]);
    backtrace(c);
    static int hits = 0;
    if (++hits >= 30) e->ContextRecord->Dr7 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void nx_arm_watch_current() { arm_watch(GetCurrentThread()); }

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* e) {
    using namespace nx;
    auto* r = e->ExceptionRecord;
    uint64_t info = r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0;
    log("CRASH: exception %08lx at host %p, data address %llx", r->ExceptionCode, r->ExceptionAddress,
        (unsigned long long)info);
    CPU& c = current_cpu();
    log("  guest lr %s  sp %llx  tpidr %llx  tpidrro %llx", describe(c.x[30]).c_str(), (unsigned long long)c.x[31],
        (unsigned long long)c.tpidr, (unsigned long long)c.tpidrro);
    backtrace(c);
    for (int k = 0; k < 31; k += 4)
        log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
            (unsigned long long)c.x[k + 2], (unsigned long long)(k + 3 < 32 ? c.x[k + 3] : 0));
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char** argv) {
    using namespace nx;
    SetUnhandledExceptionFilter(crash_handler);
    if (const char* w = std::getenv("NX_WATCH")) {
        g_watch = std::strtoull(w, nullptr, 16);
        AddVectoredExceptionHandler(1, watch_handler);
        arm_watch(GetCurrentThread());
        log("watching writes to %llx", (unsigned long long)g_watch);
    }
    if (const char* t = std::getenv("NX_TRACE")) g_trace = *t == '1';
    std::string data = argc > 1 ? argv[1] : "private/analysis";

    memory_init();
    kernel_init();
    const ModuleInfo* mods[] = {&nx_module_main, &nx_module_sdk};
    load_modules(mods, 2, data);

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
    return 0;
}
