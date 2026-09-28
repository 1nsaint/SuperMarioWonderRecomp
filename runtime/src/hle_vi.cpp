// nn::vi (display/layers) and nv:: (graphics driver setup) at the API level. The layer is a Win32 window; its
// HWND is the "native window" the game hands to vkCreateViSurfaceNN, which the Vulkan layer turns into a
// VK_KHR_win32_surface.
#include <atomic>
#include <thread>

#include <windows.h>

#include <nx/runtime.h>

namespace nx {

HWND g_window = nullptr;
std::atomic<bool> g_quit{false};

namespace {

constexpr uint64_t kFakeDisplay = kKernelBase - 0x1000;  // opaque pointers, never dereferenced by the game
constexpr uint64_t kFakeLayer = kKernelBase - 0x2000;

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_CLOSE) {
        g_quit = true;
        log("window closed");
        ExitProcess(0);
    }
    return DefWindowProcW(h, msg, w, l);
}

// The window lives on its own thread so its message loop keeps running whatever the game threads do.
void window_thread(std::atomic<bool>* ready) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HadesRecomp";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{0, 0, 1920, 1080};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_window = CreateWindowW(wc.lpszClassName, L"Hades (Switch recomp)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                             wc.hInstance, nullptr);
    *ready = true;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

void ok(CPU& c) { c.x[0] = 0; }

void OpenDefaultDisplay(CPU& c) {
    st<uint64_t>(c.x[0], kFakeDisplay);
    c.x[0] = 0;
}

void CreateLayer(CPU& c) {
    if (!g_window) {
        static std::atomic<bool> ready{false};
        std::thread(window_thread, &ready).detach();
        while (!ready) Sleep(1);
        log("vi: layer created -> window %p", (void*)g_window);
    }
    st<uint64_t>(c.x[0], kFakeLayer);
    c.x[0] = 0;
}

void GetNativeWindow(CPU& c) {  // (void** out, Layer*)
    st<uint64_t>(c.x[0], uint64_t(g_window));
    c.x[0] = 0;
}

void nv_noop(CPU& c) {}

}  // namespace

NX_HLE("_ZN2nn2vi10InitializeEv", ok);
NX_HLE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", OpenDefaultDisplay);
NX_HLE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", CreateLayer);
NX_HLE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", GetNativeWindow);
NX_HLE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_noop);
NX_HLE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_noop);
NX_HLE("_ZN2nv18InitializeGraphicsEPvm", nv_noop);

}  // namespace nx
