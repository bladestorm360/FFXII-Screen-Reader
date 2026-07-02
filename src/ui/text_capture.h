#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Captures source-text strings the game submits to the text-wrapper cluster
// (RVA 0x5F650-0x5FD30). The whole point of this layer is to NEVER name an
// option in code — MenuReader queries this for "what did the game draw" and
// speaks the bytes verbatim. No option-name table lives here or anywhere.
namespace TextCapture {

struct TextEvent {
    uint64_t timestampMs;       // wall-clock ms (GetTickCount64)
    uint32_t frameId;           // game frame id (best-effort: ++counter per hook fire)
    uint32_t callerRva;         // wrapper-caller's RVA; useful for diagnostics
    std::wstring text;          // decoded text (UTF-16 LE / ASCII auto-detected)
};

// Install hooks on known/candidate text-wrapper RVAs. Currently hooks
// FUN_0017fa10 (RVA 0x5FA10) — the top candidate. Additional wrappers can
// be added once Stream A G-A3 identifies them.
bool Init();
void Shutdown();

// Most-recent N text events captured for the given menu-object pointer,
// optionally constrained to events at or after `sinceTimestampMs`. Returns
// events in chronological order (oldest first).
//
// `menuObj == nullptr` returns all menu-agnostic events (useful while the
// menu↔text association is not yet established).
std::vector<TextEvent> RecentEvents(void* menuObj,
                                    uint64_t sinceTimestampMs = 0,
                                    size_t maxEvents = 32);

// Diagnostic: dump the current ring contents to the log (used when
// MenuReader fails to resolve focus text).
void DumpRingToLog(const char* reason);

} // namespace TextCapture
