#include "proxy/dinput8_proxy.h"
#include "core/logger.h"
#include "input/input_tracker.h"
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

// ============================================================================
// Read the mod's hotkeys through the game's OWN DirectInput keyboard poll.
//
// FFXII acquires the keyboard via DirectInput (exclusive), which starves external
// keyboard hooks. Rather than change the game's mode, we ride its own input path:
// the game polls the keyboard every frame via IDirectInputDevice8::GetDeviceState
// (a 256-byte DIK scan-code state buffer). We hook that, read the same buffer the
// game just read, and feed our hotkeys to InputTracker. The mod's keys thus travel
// the exact channel the game uses, so exclusive acquisition is irrelevant and the
// game's behavior is unchanged. (We wrap the COM vtables to install the hook.)
// ============================================================================

// GUID_SysKeyboard (dinput.h) — the standard system keyboard device.
static const GUID kGuidSysKeyboard =
    { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateDevice)(void*, const GUID&, void**, LPUNKNOWN);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetDeviceState)(void*, DWORD, void*);

static PFN_CreateDevice    g_origCreateDevice   = nullptr;
static PFN_GetDeviceState  g_origGetDeviceState = nullptr;
static std::atomic<bool> g_di8Patched{false};
static std::atomic<bool> g_devPatched{false};
static void*             g_kbDevices[8] = {};
static std::atomic<int>  g_kbDeviceCount{0};

static bool IsKeyboardDev(void* dev) {
    int n = g_kbDeviceCount.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < 8; ++i) if (g_kbDevices[i] == dev) return true;
    return false;
}

// Swap one COM vtable entry (aligned pointer write = atomic on x64).
static void PatchVtableEntry(void* comObj, int index, void* detour, void** origOut) {
    void** vtbl = *reinterpret_cast<void***>(comObj);
    DWORD oldProt = 0;
    if (VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
        if (origOut && !*origOut) *origOut = vtbl[index];
        vtbl[index] = detour;
        VirtualProtect(&vtbl[index], sizeof(void*), oldProt, &oldProt);
    }
}

// IDirectInputDevice8 vtable: GetDeviceState = index 9. Runs on the game's input
// thread each frame; we read the freshly-filled DIK buffer and feed the mod's keys.
static HRESULT STDMETHODCALLTYPE HookedGetDeviceState(void* self, DWORD cbData, void* lpvData) {
    HRESULT hr = g_origGetDeviceState(self, cbData, lpvData);
    if (SUCCEEDED(hr) && lpvData && cbData >= 256 && IsKeyboardDev(self))
        InputTracker::FeedDInputKeyboard(reinterpret_cast<const unsigned char*>(lpvData));
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateDevice(void* self, const GUID& rguid,
                                                    void** lplpDev, LPUNKNOWN outer) {
    HRESULT hr = g_origCreateDevice(self, rguid, lplpDev, outer);
    if (SUCCEEDED(hr) && lplpDev && *lplpDev) {
        void* dev = *lplpDev;
        // All device instances share one vtable — patch GetDeviceState (9) once;
        // the hook itself only acts on the keyboard device(s).
        bool expected = false;
        if (g_devPatched.compare_exchange_strong(expected, true))
            PatchVtableEntry(dev, 9, reinterpret_cast<void*>(&HookedGetDeviceState),
                             reinterpret_cast<void**>(&g_origGetDeviceState));
        if (IsEqualGUID(rguid, kGuidSysKeyboard)) {
            int idx = g_kbDeviceCount.fetch_add(1);
            if (idx < 8) g_kbDevices[idx] = dev;
            Log::Write("PROXY", "keyboard device created (hotkeys via GetDeviceState hook)");
        }
    }
    return hr;
}

extern "C" {

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf,
                                  LPVOID* ppvOut, LPUNKNOWN punkOuter) {
    EnsureLoaded();
    if (!g_DirectInput8Create) return E_FAIL;
    HRESULT hr = g_DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    // Wrap the returned IDirectInput8 so we can intercept device creation and force
    // the keyboard non-exclusive. Patch CreateDevice (vtable index 3) once.
    if (SUCCEEDED(hr) && ppvOut && *ppvOut) {
        bool expected = false;
        if (g_di8Patched.compare_exchange_strong(expected, true))
            PatchVtableEntry(*ppvOut, 3, reinterpret_cast<void*>(&HookedCreateDevice),
                             reinterpret_cast<void**>(&g_origCreateDevice));
    }
    return hr;
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
