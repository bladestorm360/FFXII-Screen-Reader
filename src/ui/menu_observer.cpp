#include "ui/menu_observer.h"
#include "core/hooks.h"
#include "core/logger.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

// Architecture constants from Docs/MenuArchitecture.md (2026-05-12 update).
constexpr uint32_t RVA_REGISTRY        = 0x1F6EA60;  // corrected 2026-07-01 (was 0x216EA60; abs 0x208EA60 = FUN_00241d40's DAT_0208ea60)
constexpr uint32_t RVA_CONTROLLER_INGAME = 0x121D40;
constexpr uint32_t OFF_TYPE            = 0x3c8;
constexpr uint32_t OFF_X_POS           = 0x3b8;
constexpr uint32_t OFF_X_ADJ           = 0x3ba;
constexpr uint32_t OFF_Y_POS           = 0x9e;
constexpr uint32_t OFF_VIS             = 0x3bc;
constexpr uint32_t OFF_SUB             = 0xc8;

// Registry slot byte offsets (8-byte slots, type-byte index).
constexpr uint32_t SLOT_TYPE1 = 8;
constexpr uint32_t SLOT_TYPE2 = 16;
constexpr uint32_t SLOT_TYPE4 = 32;

// One detour per controller; MinHook lets us install several. Each controller
// has its own original trampoline. We use a fixed-size array because dynamic
// detour generation (closure-based) is non-trivial and we expect at most a
// handful of controllers.
constexpr size_t MAX_CONTROLLERS = 4;

struct ControllerSlot {
    uint32_t rva = 0;
    const char* label = nullptr;
    void (*originalTramp)(void*, void*) = nullptr;
};

ControllerSlot g_controllers[MAX_CONTROLLERS];
std::atomic<size_t> g_controllerCount{0};

// Per-menuObj prior (X,Y) for edge-trigger.
std::mutex g_mutex;
std::unordered_map<void*, std::pair<uint16_t, int16_t>> g_priorXY;
MenuObserver::MenuSnapshot g_latest;
std::atomic<uint32_t> g_frameCounter{0};

std::atomic<MenuObserver::FocusChangeCallback> g_focusCb{nullptr};

bool g_initialized = false;

// SEH-safe field reads (the game can — and does — destruct menu objects
// asynchronously; reading their fields from our detour after lifecycle
// changes could fault otherwise).
uint8_t  SafeReadU8 (void* p) { __try { return *reinterpret_cast<uint8_t*>(p);  } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; } }
uint16_t SafeReadU16(void* p) { __try { return *reinterpret_cast<uint16_t*>(p); } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; } }
int16_t  SafeReadS16(void* p) { __try { return *reinterpret_cast<int16_t*>(p);  } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; } }
void*    SafeReadPtr(void* p) { __try { return *reinterpret_cast<void**>(p);    } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; } }

void* BytePtr(void* base, uint32_t off) {
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(base) + off);
}

// Snapshot a menu_obj's state and update the latest cache. Fires the focus
// callback when (X, Y) changes for the same menuObj.
void SnapshotAndDispatch(uint32_t controllerRva, void* menuObj) {
    if (!menuObj) return;

    MenuObserver::MenuSnapshot snap;
    snap.menuObj       = menuObj;
    snap.controllerRva = controllerRva;
    snap.typeByte      = SafeReadU8 (BytePtr(menuObj, OFF_TYPE));
    uint16_t xPos      = SafeReadU16(BytePtr(menuObj, OFF_X_POS));
    uint16_t xAdj      = SafeReadU16(BytePtr(menuObj, OFF_X_ADJ));
    snap.cursorX       = static_cast<uint16_t>(xPos + xAdj);
    snap.cursorY       = SafeReadS16(BytePtr(menuObj, OFF_Y_POS));
    snap.cursorVisible = SafeReadU8 (BytePtr(menuObj, OFF_VIS)) != 0;
    snap.subWidget     = SafeReadPtr(BytePtr(menuObj, OFF_SUB));
    snap.timestampMs   = GetTickCount64();
    snap.frameId       = ++g_frameCounter;

    bool focusChanged = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_priorXY.find(menuObj);
        if (it == g_priorXY.end()) {
            // First time we see this menuObj — establish baseline; do NOT
            // fire focus change on first sight (it would speak the default
            // option on every menu re-open, but only after we know the
            // baseline is meaningful — actually, speaking the initial focus
            // IS desired behavior for accessibility; flip this later via
            // MenuReader logic, not here).
            g_priorXY[menuObj] = { snap.cursorX, snap.cursorY };
            focusChanged = true;  // first observation = "speak the default"
        } else if (it->second.first != snap.cursorX ||
                   it->second.second != snap.cursorY) {
            it->second = { snap.cursorX, snap.cursorY };
            focusChanged = true;
        }
        g_latest = snap;
    }

    if (focusChanged) {
        MenuObserver::FocusChangeCallback cb = g_focusCb.load();
        if (cb) cb(snap);
    }
}

// One detour per controller slot. Because MinHook needs distinct C function
// addresses (cannot use closures), each slot has its own thin trampoline.
// All thunks share the same logic, differing only in which g_controllers[N]
// they reference.
template <size_t Idx>
void DetourThunk(void* menuObj, void* opcodePtr) {
    auto& slot = g_controllers[Idx];
    if (slot.originalTramp) {
        slot.originalTramp(menuObj, opcodePtr);
    }
    SnapshotAndDispatch(slot.rva, menuObj);
}

// Detour-thunk address table. Compile-time enumeration to satisfy MinHook's
// "function address, not lambda" requirement.
void (*const kDetours[MAX_CONTROLLERS])(void*, void*) = {
    &DetourThunk<0>,
    &DetourThunk<1>,
    &DetourThunk<2>,
    &DetourThunk<3>,
};

} // namespace

namespace MenuObserver {

bool Init() {
    if (g_initialized) {
        Log::Write("MENU", "MenuObserver::Init called twice — ignoring");
        return true;
    }

    bool ok = RegisterController(RVA_CONTROLLER_INGAME, "FUN_00241d40 (in-game)");
    if (!ok) {
        Log::Write("MENU", "Failed to register in-game controller — MenuObserver dormant");
        return false;
    }
    g_initialized = true;
    Log::Write("MENU", "MenuObserver initialized (in-game controller registered). "
                       "Title controller registers later, after Frida G-A1/G-A2.");
    return true;
}

void Shutdown() {
    if (!g_initialized) return;
    size_t n = g_controllerCount.load();
    for (size_t i = 0; i < n; i++) {
        if (g_controllers[i].rva) Hooks::Uninstall(g_controllers[i].rva);
        g_controllers[i].rva = 0;
        g_controllers[i].label = nullptr;
        g_controllers[i].originalTramp = nullptr;
    }
    g_controllerCount.store(0);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_priorXY.clear();
        g_latest = {};
    }
    g_initialized = false;
    Log::Write("MENU", "MenuObserver shut down");
}

bool RegisterController(uint32_t rva, const char* label) {
    size_t idx = g_controllerCount.load();
    if (idx >= MAX_CONTROLLERS) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "RegisterController: out of slots (max %zu); refusing 0x%X",
                 MAX_CONTROLLERS, rva);
        Log::Write("MENU", msg);
        return false;
    }

    g_controllers[idx].rva = rva;
    g_controllers[idx].label = label ? label : "(unnamed)";
    g_controllers[idx].originalTramp = nullptr;

    void (*detour)(void*, void*) = kDetours[idx];
    bool ok = Hooks::InstallTyped(rva, detour, &g_controllers[idx].originalTramp);
    if (!ok) {
        g_controllers[idx].rva = 0;
        g_controllers[idx].label = nullptr;
        return false;
    }

    g_controllerCount.store(idx + 1);
    char msg[160];
    snprintf(msg, sizeof(msg),
             "Registered menu controller [%zu]: %s @ RVA 0x%X",
             idx, g_controllers[idx].label, rva);
    Log::Write("MENU", msg);
    return true;
}

MenuSnapshot LatestSnapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_latest;
}

void SetFocusChangeCallback(FocusChangeCallback cb) {
    g_focusCb.store(cb);
}

void ReadRegistry(void** outType1, void** outType2, void** outType4) {
    void* reg = Hooks::ResolveRva(RVA_REGISTRY);
    if (!reg) {
        if (outType1) *outType1 = nullptr;
        if (outType2) *outType2 = nullptr;
        if (outType4) *outType4 = nullptr;
        return;
    }
    if (outType1) *outType1 = SafeReadPtr(BytePtr(reg, SLOT_TYPE1));
    if (outType2) *outType2 = SafeReadPtr(BytePtr(reg, SLOT_TYPE2));
    if (outType4) *outType4 = SafeReadPtr(BytePtr(reg, SLOT_TYPE4));
}

} // namespace MenuObserver
