#include "input/pad_hook.h"
#include "input/gamepad_sdl.h"
#include "core/logger.h"
#include "core/hooks.h"
#include "core/mem_read.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

typedef uint32_t (WINAPI* Pfn_XInputGetState)(uint32_t, PadHook::State*);

Pfn_XInputGetState  g_orig      = nullptr;
void**              g_iatSlot   = nullptr;   // the patched IAT cell, for Shutdown
std::atomic<bool>   g_active{false};
std::atomic<bool>   g_faulted{false};        // a fault latches the hook off for the session
// Logged once, on the first poll that actually returns a pad. Without it an empty survey cannot be
// told apart from a pad that was never plugged in -- and that ambiguity is unreadable in someone
// else's log, where we cannot ask what was connected. Presence of this line is what makes the
// survey's silence mean something.
std::atomic<bool>   g_sawPad{false};

// ERROR_SUCCESS / ERROR_DEVICE_NOT_CONNECTED from XInputGetState. Named rather than inlined so the
// guards below read as intent.
constexpr uint32_t kXiOk           = 0;
constexpr uint32_t kXiNotConnected = 0x48F;

// Swap one pointer-sized cell (aligned pointer write = atomic on x64). Same helper shape as
// dinput8_proxy.cpp's PatchVtableEntry -- kept separate because that one indexes a vtable and this
// one takes an already-resolved cell.
bool PatchPointer(void** cell, void* newValue, void** origOut) {
    DWORD oldProt = 0;
    if (!VirtualProtect(cell, sizeof(void*), PAGE_READWRITE, &oldProt)) return false;
    if (origOut) *origOut = *cell;
    *cell = newValue;
    VirtualProtect(cell, sizeof(void*), oldProt, &oldProt);
    return true;
}

// Find the exe's IAT cell for `wantFn` in the import descriptor whose DLL name starts with `dllPrefix`
// (case-insensitive). Returns null when the import is absent -- which is a legitimate outcome to log,
// not an error to assert on: a build that reaches the pad another way would look exactly like this.
void** FindImportSlot(const char* dllPrefix, const char* wantFn) {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) return nullptr;
    auto base = reinterpret_cast<uint8_t*>(exe);
    auto dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return nullptr;

    const size_t prefixLen = strlen(dllPrefix);
    auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        const char* dllName = reinterpret_cast<const char*>(base + desc->Name);
        if (_strnicmp(dllName, dllPrefix, prefixLen) != 0) continue;

        // OriginalFirstThunk carries the NAMES; FirstThunk is the live IAT we patch. They are
        // parallel. A descriptor with no OriginalFirstThunk is bound-by-ordinal only -- skip it
        // rather than guess, and let the log say the import was not found by name.
        if (desc->OriginalFirstThunk == 0) continue;
        auto nameThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto iatThunk  = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; nameThunk->u1.AddressOfData != 0; ++nameThunk, ++iatThunk) {
            if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) continue;
            auto imp = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + nameThunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(imp->Name), wantFn) == 0)
                return reinterpret_cast<void**>(&iatThunk->u1.Function);
        }
    }
    return nullptr;
}

// THE GAME'S PAD, BUILT FROM SDL3. Runs on whichever thread the game polls XInput from.
//
// IT NO LONGER READS THE HARDWARE AND IT NO LONGER MASKS BITS. `GamepadSDL` is the mod's one and
// only pad reader, for every controller type, and this function hands the game the state that reader
// produced minus whatever `PadRouter` claimed. The real `XInputGetState` result is discarded when the
// mod is driving -- which is the point: an Xbox pad, a DualSense, a Switch Pro pad and a handheld's
// built-in sticks all reach the game through these same lines, so there is no per-device path left to
// test separately. See gamepad_sdl.h.
//
// WHY THIS ALSO MAKES NON-XBOX PADS WORK AS GAME CONTROLLERS. A DualSense does not speak XInput, so
// the real call here returns ERROR_DEVICE_NOT_CONNECTED for it. Building the state ourselves means
// the game gets a working XInput pad regardless of what the player actually holds -- the same service
// Steam Input performs, done by the mod, for free.
//
// Contract, in order:
//   1. mod not driving (no pad open) -> return the original call VERBATIM. No pad means
//      byte-identical, so nothing is written over the game's own input path.
//   2. only index 0 carries the pad; 1-3 report not connected, because the mod opens one controller.
//   3. refresh SDL for this frame, then fill `state` from it.
// A fault anywhere latches the whole thing off for the session and the pad reverts to the game's.
uint32_t WINAPI HookedXInputGetState(uint32_t userIndex, PadHook::State* state) {
    if (g_faulted.load(std::memory_order_relaxed) || !GamepadSDL::DriveGame())
        return g_orig ? g_orig(userIndex, state) : kXiNotConnected;

    if (!state) return kXiNotConnected;
    if (userIndex != 0) return kXiNotConnected;

    uint32_t hr = kXiNotConnected;
    __try {
        GamepadSDL::PollIfStale();
        if (GamepadSDL::BuildGameState(state)) {
            if (!g_sawPad.exchange(true))
                Log::Write("PAD", "the game's pad is now being driven from SDL3 (XInput index 0)");
            hr = kXiOk;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!g_faulted.exchange(true))
            Log::Write("PAD", "EXCEPTION building the game's pad state -- latched OFF for this "
                              "session; the controller reverts to the game's own reading");
        return g_orig ? g_orig(userIndex, state) : kXiNotConnected;
    }
    return hr;
}

} // namespace

namespace PadHook {

bool Init() {
    if (g_active.load(std::memory_order_relaxed)) return true;

    void** slot = FindImportSlot("xinput", "XInputGetState");
    if (!slot) {
        Log::Write("PAD", "XInputGetState import not found -- gamepad support inactive this session "
                          "(the mod behaves exactly as it did before)");
        return false;
    }

    void* orig = nullptr;
    if (!PatchPointer(slot, reinterpret_cast<void*>(&HookedXInputGetState), &orig) || !orig) {
        Log::Write("PAD", "ERROR: could not patch the XInputGetState IAT entry -- gamepad inactive");
        return false;
    }
    g_orig    = reinterpret_cast<Pfn_XInputGetState>(orig);
    g_iatSlot = slot;
    g_active.store(true, std::memory_order_release);
    Log::Write("PAD", "XInput IAT patched -- the game's pad will be built from SDL3");
    return true;
}

void Shutdown() {
    if (!g_active.exchange(false)) return;
    if (g_iatSlot && g_orig) PatchPointer(g_iatSlot, reinterpret_cast<void*>(g_orig), nullptr);
    g_iatSlot = nullptr;
    g_orig    = nullptr;
}

bool Active() {
    return g_active.load(std::memory_order_acquire) && !g_faulted.load(std::memory_order_relaxed);
}

bool DrivingXInput() {
    return g_sawPad.load(std::memory_order_relaxed) && !g_faulted.load(std::memory_order_relaxed);
}

bool ReadGamePadWords(uint16_t out[3]) {
    // Pad 0's +0x08/+0x0A/+0x0C. See the header for why this is a WITNESS and never a driver.
    return out &&
           MemRead::SafeReadU16(Hooks::ResolveRva(0x2E77368), 0, &out[0]) &&
           MemRead::SafeReadU16(Hooks::ResolveRva(0x2E7736A), 0, &out[1]) &&
           MemRead::SafeReadU16(Hooks::ResolveRva(0x2E7736C), 0, &out[2]);
}

const char* ButtonName(uint16_t bit) {
    switch (bit) {
        case kDpadUp:        return "D-pad Up";
        case kDpadDown:      return "D-pad Down";
        case kDpadLeft:      return "D-pad Left";
        case kDpadRight:     return "D-pad Right";
        case kStart:         return "Start";
        case kBack:          return "Back";
        case kLeftThumb:     return "L3";
        case kRightThumb:    return "R3";
        case kLeftShoulder:  return "L1";
        case kRightShoulder: return "R1";
        case kA:             return "A";
        case kB:             return "B";
        case kX:             return "X";
        case kY:             return "Y";
        default:             return "?";
    }
}

} // namespace PadHook
