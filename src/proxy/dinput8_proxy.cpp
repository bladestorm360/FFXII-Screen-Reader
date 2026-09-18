#include "proxy/dinput8_proxy.h"
#include "core/logger.h"
#include "core/stall_probe.h"
#include "core/frame_probe.h"
#include "input/input_tracker.h"
#include "input/gamepad_sdl.h"
#include "input/pad_hook.h"
#include "navigation/auto_walk.h"
#include "navigation/soundscape.h"   // OnInputPoll -- the stall watchdog; this poll outlives the field tick
#include <Windows.h>
#include <atomic>
#include <cstring>
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

// NON-KEYBOARD DirectInput devices -- and as of S186 this is A LIVE PAD PATH, not a probe.
//
// The engine carries a `PInputDevicePadDirectInput` class alongside `PInputDevicePadXInput`
// (rtti_classes), so a DirectInput pad path EXISTS in PhyreEngine. This logger was added to find out
// whether FFXII ever uses it, because "the exe imports XInput" does not answer that -- a DInput pad
// arrives through the very CreateDevice call below, not through XInput at all.
//
// IT DOES USE IT, AND THAT IS WHY V1.0's CONTROLLER SUPPORT WAS BROKEN. A tester's DualSense logged
// `cbData=272` here (DIJOYSTATE2) for a whole session while the XInput side never saw a pad at all.
// XInput is the Xbox protocol; a PlayStation pad does not speak it without Steam Input or DS4Windows
// in the way. So this is the surface every non-Xbox controller reaches the game through, and it is
// where the router's consume mask has to be applied for those players.
//
// 80 bytes = DIJOYSTATE, 272 = DIJOYSTATE2; a mouse is 16/20, which is what the `guid=6F1D2B60`
// entries in older logs were.
static void*             g_otherDevices[8] = {};
static std::atomic<int>  g_otherDeviceCount{0};
static std::atomic<bool> g_otherLogged[8]{};

static int OtherDevIndex(void* dev) {
    int n = g_otherDeviceCount.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < 8; ++i) if (g_otherDevices[i] == dev) return i;
    return -1;
}

static bool IsKeyboardDev(void* dev) {
    int n = g_kbDeviceCount.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < 8; ++i) if (g_kbDevices[i] == dev) return true;
    return false;
}

// ---- BLANKING THE GAME'S DIRECTINPUT JOYSTICK ---------------------------------------------------
//
// THE FFXII ANSWER TO FFPR's `DisableUnityGamepad()`. The mod reads the controller through SDL3 and
// hands the game a pad built from that reading (pad_hook.cpp). For that to be the WHOLE story, the
// physical device must not also reach the game down a second road: on a DualSense or any other
// non-XInput pad, FFXII polls the stick directly through DirectInput, and anything arriving that way
// has been routed by nobody and consumed by nobody.
//
// So when the mod is driving, this makes the game's joystick read look like a controller sitting
// perfectly still. It is not a mapping and there is nothing device-specific in it -- no
// `rgbButtons[]` index tables, no SDL binding lookups, no per-button confirmation. Those existed only
// because the old design had to remove SOME inputs and keep others in a format it did not own. This
// one removes all of them, and the inputs come back to the game through SDL3 like every other pad's.
//
// `DIJOYSTATE` and `DIJOYSTATE2` share the layout we touch:
//     +0   lX lY lZ lRx lRy lRz      (LONG each)     +32  rgdwPOV[4]   (0xFFFFFFFF = centred)
//     +24  rglSlider[2]                              +48  rgbButtons[] (high bit set = down)
//
// AN AXIS'S NEUTRAL IS NOT 0. A DirectInput axis carries whatever range the game asked for via
// DIPROP_RANGE -- often 0..65535, centre 32768 -- so writing 0 would be a hard deflection, which on
// the left stick would walk the player into a wall. The device is asked instead:
// `IDirectInputDevice8::GetProperty(DIPROP_RANGE)`, vtable slot 5, once per axis per session.
struct DIPropHeader { DWORD dwSize, dwHeaderSize, dwObj, dwHow; };
struct DIPropRange  { DIPropHeader diph; LONG lMin, lMax; };
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetProperty)(void*, const GUID*, DIPropHeader*);
static const GUID* const kDIPropRange = reinterpret_cast<const GUID*>(static_cast<uintptr_t>(4));
constexpr DWORD kDIPH_ByOffset = 1;

// The eight axis slots of DIJOYSTATE2, by byte offset.
constexpr int kAxisOffsets[8] = { 0, 4, 8, 12, 16, 20, 24, 28 };

struct AxisNeutral { LONG centre = 0; bool known = false; bool absent = false; };
static AxisNeutral g_axisNeutral[8];
static std::atomic<bool> g_blankLogged{false};

// Ask the device where this axis's centre is. `absent` latches for an axis the data format has no
// object at, which is normal -- most pads do not use all eight slots.
static void LearnAxisNeutral(void* dev, int i) {
    AxisNeutral& a = g_axisNeutral[i];
    if (a.known || a.absent || !dev) return;

    void** vtbl = *reinterpret_cast<void***>(dev);
    auto getProp = reinterpret_cast<PFN_GetProperty>(vtbl[5]);
    if (!getProp) { a.absent = true; return; }

    DIPropRange r{};
    r.diph.dwSize       = sizeof(DIPropRange);
    r.diph.dwHeaderSize = sizeof(DIPropHeader);
    r.diph.dwObj        = static_cast<DWORD>(kAxisOffsets[i]);
    r.diph.dwHow        = kDIPH_ByOffset;
    if (FAILED(getProp(dev, kDIPropRange, &r.diph)) || r.lMax <= r.lMin) { a.absent = true; return; }

    a.centre = r.lMin + (r.lMax - r.lMin) / 2;
    a.known  = true;
    char m[160];
    snprintf(m, sizeof(m), "DInput axis +%d: range %ld..%ld, neutral %ld (read from the device)",
             kAxisOffsets[i], (long)r.lMin, (long)r.lMax, (long)a.centre);
    Log::Write("PAD", m);
}

static void BlankDInputPad(void* dev, void* buf, DWORD cbData) {
    if (!buf || (cbData != 80 && cbData != 272)) return;
    if (!GamepadSDL::DriveGame()) return;          // off means byte-identical: touch nothing

    if (!g_blankLogged.exchange(true))
        Log::Write("PAD", "this pad also reaches the game through DirectInput -- blanking that road; "
                          "the mod feeds the game from SDL3 instead");

    auto* bytes = static_cast<uint8_t*>(buf);

    for (int i = 0; i < 8; ++i) {
        LearnAxisNeutral(dev, i);
        const AxisNeutral& a = g_axisNeutral[i];
        // An axis whose neutral we could not read is LEFT ALONE. Guessing one risks writing a hard
        // deflection, and a stick that drifts is worse than one the game still sees.
        if (a.known) *reinterpret_cast<LONG*>(bytes + kAxisOffsets[i]) = a.centre;
    }

    auto* povs = reinterpret_cast<uint32_t*>(bytes + 32);
    for (int i = 0; i < 4; ++i) povs[i] = 0xFFFFFFFFu;

    const int nButtons = (cbData == 272) ? 128 : 32;
    memset(bytes + 48, 0, static_cast<size_t>(nButtons));
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

// __try AND A SCOPED OBJECT CANNOT SHARE A FUNCTION (MSVC C2712), so the guard and the measurement
// live in two functions rather than one block. `PollPadGuarded` holds the __try and owns no object
// with a destructor; `PollPadMeasured` holds the STALL_SCOPE and owns no __try.
static void PollPadGuarded() {
    __try {
        GamepadSDL::PollIfStale();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void PollPadMeasured() {
    STALL_SCOPE("GamepadSDL::PollIfStale");
    PollPadGuarded();
}

// Same split, for the pad-poll path: refresh the claim, then take away what was claimed.
static void PollAndBlankGuarded(void* dev, void* lpvData, DWORD cbData) {
    __try {
        GamepadSDL::PollIfStale();
        BlankDInputPad(dev, lpvData, cbData);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// IDirectInputDevice8 vtable: GetDeviceState = index 9. Runs on the game's input
// thread each frame; we read the freshly-filled DIK buffer and feed the mod's keys.
static HRESULT STDMETHODCALLTYPE HookedGetDeviceState(void* self, DWORD cbData, void* lpvData) {
    HRESULT hr = g_origGetDeviceState(self, cbData, lpvData);
    // Frame heartbeat: the game polls this every frame, menus included, so it is the one place that
    // can see a stall from outside our own hook bodies.
    if (cbData >= 256 && IsKeyboardDev(self)) {
        // WHICH THREAD is this? Never recorded before, and without it a clean FrameTick result is
        // ambiguous: "the game thread never stalled" and "my anchor was on a different thread that
        // kept running" look identical. That ambiguity is why the party-menu freeze went unexplained.
        StallProbe::NoteThread("DInput::GetDeviceState");
        StallProbe::FrameTick(/*gapWarnMs=*/100.0);
        // Cadence report, one line per 10 s. This anchor keeps running in menus and loads where the
        // field tick does not, which is what makes "the field tick stopped" and "the game stalled"
        // tell apart. StallProbe above only speaks above 100 ms and so can never report the normal
        // rate — the gap that left every tier-C constant unmeasurable. See core/frame_probe.h.
        FrameProbe::OnInputPoll();

        // THE PAD READ, and this is the site that guarantees one happens at all: the game polls the
        // keyboard every frame, menus and loads included, whereas it polls a pad only when one is
        // attached. Kept OUTSIDE the `SUCCEEDED(hr)` gate below on purpose -- an unacquired keyboard
        // (which this very log has caught happening) must not take the controller down with it.
        //
        // MEASURED, NOT ASSUMED (`L-88`). It is an event drain plus ~27 cached reads and it collapses
        // to one read per frame however many hooks ask, but the budget report is what proves that.
        PollPadMeasured();

        // S192: the soundscape's stall watchdog. THIS POLL IS THE POINT -- it keeps running while
        // the field tick is stopped by a pause or a load, which is exactly when a soundscape voice
        // would otherwise keep playing over a paused game. Same reasoning as the pad read above:
        // this is the one tick that does not stop. O(1) unless something is actually sounding.
        Soundscape::OnInputPoll();
    }
    // One line per non-keyboard device, the first time the game polls it. See g_otherDevices above.
    if (!IsKeyboardDev(self)) {
        const int oi = OtherDevIndex(self);
        if (oi >= 0 && !g_otherLogged[oi].exchange(true)) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "non-keyboard DirectInput device polled: idx=%d cbData=%lu%s",
                     oi, (unsigned long)cbData,
                     (cbData == 80 || cbData == 272) ? " (JOYSTICK-SIZED -- a DInput pad path is live)"
                                                     : "");
            Log::Write("PAD", msg);
        }
        // ONLY THE GAME'S OWN DEVICES ARE SUPPRESSED, and `oi >= 0` is what says so.
        //
        // WHY THIS GATE IS LOAD-BEARING. `g_otherDevices` is filled by HookedCreateDevice, which
        // only runs for devices created through OUR `DirectInput8Create` export -- that is, the
        // game's. SDL reaches DirectInput a different way (`CoCreateInstance(CLSID_DirectInput8)`),
        // so its devices never pass through that export and never land in the table. But SDL gets
        // them from the SAME System32 dinput8 module, hence the SAME vtable we patched, so SDL's own
        // joystick polls DO arrive in this hook.
        //
        // Without the gate we would clear consumed buttons out of SDL's read buffer as well as the
        // game's -- the mod suppressing its own input. The router would see the button release
        // itself on the next poll, level-triggered consumption would break, and every claimed button
        // would flicker. `oi >= 0` means "the game created this device", which is exactly the set
        // whose buffer we are entitled to touch.
        if (oi >= 0) {
            // Refresh SDL for this frame, then blank this road so the device cannot also reach the
            // game un-routed -- see BlankDInputPad. A fault here must never reach the input thread.
            PollAndBlankGuarded(self, lpvData, cbData);
        }
    }
    if (cbData >= 256 && IsKeyboardDev(self)) {
        // Diagnostic (rate-limited to transitions): a sustained keyboard GetDeviceState
        // failure (DIERR_INPUTLOST / DIERR_NOTACQUIRED) means the device is UNACQUIRED — the
        // game then sees NO keys at all (Enter included). This is the prime suspect for an
        // intermittent "confirm dead" spell, and it is upstream of us: our hook is read-only
        // and never alters the buffer. Logging the transition proves where Enter was lost.
        static bool s_wasOk = true;
        bool ok = SUCCEEDED(hr);
        if (ok != s_wasOk) {
            s_wasOk = ok;
            char msg[128];
            if (!ok) snprintf(msg, sizeof(msg),
                              "keyboard GetDeviceState FAILING hr=0x%08lX (device unacquired -> game sees no keys)",
                              (unsigned long)hr);
            else     snprintf(msg, sizeof(msg), "keyboard GetDeviceState recovered (hr=DI_OK)");
            Log::Write("INPUT-DIAG", msg);
        }
        if (ok && lpvData) {
            // A fault here must never propagate into the game's input thread.
            __try {
                InputTracker::FeedDInputKeyboard(reinterpret_cast<const unsigned char*>(lpvData));
                // S100, THE ONE SANCTIONED INPUT WRITE (user-authorized; see auto_walk.h and
                // CLAUDE.md). Strictly AFTER the tracker was fed, so every mod-side observation
                // sees the PRE-injection buffer -- and the hook read the real state first by
                // construction, which is what lets a real key suppress injection in the same poll.
                // With the Auto-walk toggle off, this returns on its first line.
                AutoWalk::OnDevicePoll(reinterpret_cast<unsigned char*>(lpvData));
                // S192, THE ONE SANCTIONED KEY SWALLOW (user-authorized; see
                // InputTracker::MaskModMenuKeys and CLAUDE.md). LAST, and after the tracker was fed:
                // the mod must see the real Escape/Backspace press -- that press is what closes the
                // menu -- while the game must not, or closing the mod menu also opens the pause
                // screen. With the mod menu shut this writes nothing.
                InputTracker::MaskModMenuKeys(reinterpret_cast<unsigned char*>(lpvData));
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
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
        } else {
            int idx = g_otherDeviceCount.fetch_add(1);
            if (idx < 8) {
                g_otherDevices[idx] = dev;
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "non-keyboard DirectInput device created: idx=%d guid=%08lX-%04X-%04X",
                         idx, (unsigned long)rguid.Data1, rguid.Data2, rguid.Data3);
                Log::Write("PAD", msg);
            }
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
