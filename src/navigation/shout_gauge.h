#pragma once

#include <cstdint>

// THE HUD GAUGE the script natives drive — the one surface the Bhujerba shout minigame's meter is
// visible on, and the only place its live value can be read.
//
// Split out of shout_meter.cpp (S132) so the meter, the diagnostics and the mod menu's visibility
// predicate all read the gauge through ONE set of offsets rather than three copies of them.
//
// ALL OFFSETS ARE CONFIRMED from `FUN_004085B0` (the writer this mod hooks) and `FUN_00408360` /
// `FUN_00408190` (setgaugeshowstatus on and off). See Docs/GameArchitecture.md.
namespace ShoutGauge {

// RVA of the writer the mod hooks: `void FUN_004085B0(int newValue)` — ONE int, and it IS the new
// counter value. Its arity is counted from the CALLEE, per the S129 rule.
constexpr uint32_t RVA_WRITER = 0x2E85B0;

struct State {
    bool     ok        = false;   // the manager and the gauge object were both readable
    int      value     = 0;       // gauge + 0xE4
    int      max       = 0;       // gauge + 0xE0
    uint32_t flags     = 0;       // gauge + 0xD8 — bit 2 set = on screen, bit 3 set = hidden
    bool     shown     = false;   // flags & 4
    uint8_t  styleC0   = 0;       // gauge + 0xC0 \  logged, never branched on: the decompile folds
    uint8_t  styleC1   = 0;       // gauge + 0xC1 /  these two and cannot be trusted to mean "style"
    uint8_t  mgrFlags  = 0;       // manager + 0xC8 — bit 1 suppresses the engine's own write
};

// Re-resolves the manager and the gauge object from scratch every call — they are owned by the HUD
// and must never be cached across frames. Every read is SEH-guarded; a torn pointer yields
// `ok == false` rather than a fault. Safe from any thread.
State Read();

// Just the sequence-live bit, for the hot paths that need nothing else.
//
// `setgaugeshowstatus(1)` sets bit 2 and clears bit 3; `setgaugeshowstatus(0)` does the reverse. So
// this is the SCRIPT's own statement that its gauge is on screen, which is what makes it a usable
// "the puzzle is running now" predicate rather than the far coarser "the player is in Bhujerba".
bool IsShown();

} // namespace ShoutGauge
