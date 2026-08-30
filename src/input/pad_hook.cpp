#include "input/pad_hook.h"
#include "input/pad_router.h"
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

// ERROR_SUCCESS from XInputGetState. Named rather than inlined so the guard below reads as intent.
constexpr uint32_t kXiOk = 0;

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

// THE HOOK. Runs on whichever thread the game polls the pad from, once per poll.
//
// Contract, in order, and the order is the contract:
//   1. call the original and keep its result verbatim -- we never invent a pad state;
//   2. on anything other than a connected pad, return untouched;
//   3. hand the PRE-consumption state to the router (every mod-side observation sees the player's
//      real input, exactly as FeedDInputKeyboard is fed the pre-injection keyboard buffer);
//   4. apply the router's consumption mask to what the GAME will see.
// A fault anywhere in 3-4 latches the hook off for the session rather than propagating into the
// game's input thread.
uint32_t WINAPI HookedXInputGetState(uint32_t userIndex, PadHook::State* state) {
    const uint32_t hr = g_orig ? g_orig(userIndex, state) : 0x48F /*ERROR_DEVICE_NOT_CONNECTED*/;
    if (hr != kXiOk || !state) return hr;
    if (!g_sawPad.exchange(true)) {
        char m[112];
        snprintf(m, sizeof(m), "controller CONNECTED on index %u -- pad lines below are real data",
                 userIndex);
        Log::Write("PAD", m);
    }
    if (g_faulted.load(std::memory_order_relaxed)) return hr;

    __try {
        PadRouter::OnPoll(userIndex, state);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Latch off and say so once. A pad that keeps working beats a pad that is right.
        if (!g_faulted.exchange(true))
            Log::Write("PAD", "EXCEPTION in the pad router -- intercept latched OFF for this session; "
                              "the pad now passes through untouched");
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
    Log::Write("PAD", "gamepad intercept installed (XInputGetState, IAT)");
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
