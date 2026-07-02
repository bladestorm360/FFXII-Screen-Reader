#pragma once

#include <cstdint>
#include <string>

// Observes FFXII's menu architecture: the registry at RVA 0x216EA60 holds
// pointers to currently-open menus (by type), and FUN_00241d40 (RVA 0x121D40)
// is the per-frame state machine for in-game menus. We hook the controller
// to snapshot per-fire state and edge-trigger on focus-position change.
//
// Designed for additional controllers to be registered later (Stream A G-A2
// findings) — the title menu may use a peer controller; if so, register it
// here and the rest of the pipeline works unchanged.
namespace MenuObserver {

struct MenuSnapshot {
    void*    menuObj = nullptr;       // the controller's param_1
    uint32_t controllerRva = 0;       // which controller fired
    uint8_t  typeByte = 0;            // +0x3c8 (1/2/4 or other)
    uint16_t cursorX = 0;             // +0x3b8 + +0x3ba composite already applied
    int16_t  cursorY = 0;             // +0x9e
    bool     cursorVisible = false;   // +0x3bc
    void*    subWidget = nullptr;     // +0xc8
    uint64_t timestampMs = 0;         // when captured
    uint32_t frameId = 0;             // counter ++ per fire across all controllers
};

bool Init();
void Shutdown();

// Register an additional controller RVA. The default Init() registers
// FUN_00241d40 (RVA 0x121D40). After Stream A identifies the title controller,
// call this at startup to add it.
bool RegisterController(uint32_t rva, const char* label);

// Returns the latest snapshot for any open menu, or {.menuObj == nullptr} if
// no menu is currently open.
MenuSnapshot LatestSnapshot();

// Subscribe to focus-change events. The callback fires on the game thread,
// each time the (X, Y) cursor position changes for the same menuObj.
// IMPORTANT: callbacks must be fast and not lock anything held during
// snapshot writing — keep them to a few atomics + an enqueue.
typedef void (*FocusChangeCallback)(const MenuSnapshot& snap);
void SetFocusChangeCallback(FocusChangeCallback cb);

// Diagnostic: which registry slots are non-null right now (one-shot read).
// Returns three pointers for type 1 (slot+8), type 2 (slot+16), type 4 (slot+32).
void ReadRegistry(void** outType1, void** outType2, void** outType4);

} // namespace MenuObserver
