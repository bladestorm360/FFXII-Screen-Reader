#include "proxy/dinput8_proxy.h"
#include "core/logger.h"
#include <Windows.h>
#include <atomic>
#include <cstdio>

// dinput8.dll function signatures (subset we proxy).
// We use opaque pointers to avoid pulling in <dinput.h>.
typedef HRESULT (WINAPI* PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT (WINAPI* PFN_DllCanUnloadNow)(void);
typedef HRESULT (WINAPI* PFN_DllGetClassObject)(REFCLSID, REFIID, LPVOID*);
typedef HRESULT (WINAPI* PFN_DllRegisterServer)(void);
typedef HRESULT (WINAPI* PFN_DllUnregisterServer)(void);
typedef void*   (WINAPI* PFN_GetdfDIJoystick)(void);

static HMODULE g_realDInput8 = nullptr;

static PFN_DirectInput8Create   g_DirectInput8Create   = nullptr;
static PFN_DllCanUnloadNow      g_DllCanUnloadNow      = nullptr;
static PFN_DllGetClassObject    g_DllGetClassObject    = nullptr;
static PFN_DllRegisterServer    g_DllRegisterServer    = nullptr;
static PFN_DllUnregisterServer  g_DllUnregisterServer  = nullptr;
static PFN_GetdfDIJoystick      g_GetdfDIJoystick      = nullptr;

// Lazy-init guard: one-shot, non-blocking.
static std::atomic<bool> g_initRan{false};

namespace DInput8Proxy {

bool Init() {
    bool expected = false;
    if (!g_initRan.compare_exchange_strong(expected, true)) {
        // Already attempted; result reflected in g_realDInput8.
        return g_realDInput8 != nullptr;
    }

    char systemDir[MAX_PATH];
    GetSystemDirectoryA(systemDir, MAX_PATH);

    char dllPath[MAX_PATH];
    snprintf(dllPath, MAX_PATH, "%s\\dinput8.dll", systemDir);

    g_realDInput8 = LoadLibraryA(dllPath);
    if (!g_realDInput8) {
        Log::Write("PROXY", "ERROR: failed to load real dinput8.dll from System32");
        return false;
    }

    g_DirectInput8Create  = (PFN_DirectInput8Create)  GetProcAddress(g_realDInput8, "DirectInput8Create");
    g_DllCanUnloadNow     = (PFN_DllCanUnloadNow)     GetProcAddress(g_realDInput8, "DllCanUnloadNow");
    g_DllGetClassObject   = (PFN_DllGetClassObject)   GetProcAddress(g_realDInput8, "DllGetClassObject");
    g_DllRegisterServer   = (PFN_DllRegisterServer)   GetProcAddress(g_realDInput8, "DllRegisterServer");
    g_DllUnregisterServer = (PFN_DllUnregisterServer) GetProcAddress(g_realDInput8, "DllUnregisterServer");
    g_GetdfDIJoystick     = (PFN_GetdfDIJoystick)     GetProcAddress(g_realDInput8, "GetdfDIJoystick");

    if (!g_DirectInput8Create) {
        Log::Write("PROXY", "ERROR: real dinput8.dll missing DirectInput8Create");
        FreeLibrary(g_realDInput8);
        g_realDInput8 = nullptr;
        return false;
    }

    Log::Write("PROXY", "dinput8 proxy initialized (forwarding to System32)");
    return true;
}

void Shutdown() {
    if (g_realDInput8) {
        FreeLibrary(g_realDInput8);
        g_realDInput8 = nullptr;
    }
}

} // namespace DInput8Proxy

// Lazy-init helper for every exported call. The game may call any of these
// from process-wide init paths; ensure the real DLL is loaded before forwarding.
static inline void EnsureLoaded() {
    if (!g_initRan.load(std::memory_order_relaxed)) {
        DInput8Proxy::Init();
    }
}

extern "C" {

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf,
                                  LPVOID* ppvOut, LPUNKNOWN punkOuter) {
    EnsureLoaded();
    if (g_DirectInput8Create)
        return g_DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    return E_FAIL;
}

HRESULT WINAPI DllCanUnloadNow(void) {
    EnsureLoaded();
    if (g_DllCanUnloadNow) return g_DllCanUnloadNow();
    return S_FALSE; // can't unload — be conservative
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    EnsureLoaded();
    if (g_DllGetClassObject) return g_DllGetClassObject(rclsid, riid, ppv);
    return E_FAIL;
}

HRESULT WINAPI DllRegisterServer(void) {
    EnsureLoaded();
    if (g_DllRegisterServer) return g_DllRegisterServer();
    return E_FAIL;
}

HRESULT WINAPI DllUnregisterServer(void) {
    EnsureLoaded();
    if (g_DllUnregisterServer) return g_DllUnregisterServer();
    return E_FAIL;
}

void* WINAPI GetdfDIJoystick(void) {
    EnsureLoaded();
    if (g_GetdfDIJoystick) return g_GetdfDIJoystick();
    return nullptr;
}

} // extern "C"
