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
// this is the SCRIPT's own statement that its gauge is on screen.
bool IsShown();

// ---- IS THE GAUGE ON SCREEN THE SHOUT MINIGAME'S? -----------------------------------------------
//
// "A gauge is showing" is NOT the shout minigame. A sweep of all 1115 EBP2 scripts found **126
// modules using gauge natives and 46 driving a counter** -- `mic_*`, `rsn_*`, `sav_*`, `gil_*`,
// `frs_*`, `srb_*` and an event script besides Bhujerba's fourteen -- and every one of them uses
// `max = 100`, so the maximum separates nothing either. A gauge-alone gate would have had the mod
// announce "Infamy" at several unrelated points in the game.
//
// **THE CONDITION TRIPLE IS THE EXACT FLAG.** Every gauge in the game is configured with
// `setgaugecountercondition(a, b, c)`, and across the entire corpus the triples are:
//
//     (200, 200, 100)  byu   <- the shout minigame, and NOTHING else in the game
//     ( 90,  60,  30)  byu   (a second, unrelated configuration on byu_a04)
//     ( 60,  49,  28)  mic
//     ( 10,  50, 100)  rsn
//     (no condition call)    sav, gil, frs, srb
//
// So the script tells us outright which gauge it is building, in its own numbers. This arms on the
// RAW SCRIPT ARGUMENTS rather than on the stored fields, because the engine transforms them on the
// way in (`FUN_00407300` multiplies by 60 when the counter type is 1, and swaps the order depending
// on `gauge+0xDC`) and a fingerprint read back through two transformations would be inference where
// this is a measurement.
//
// It is CROSS-MAP by construction: all fourteen Bhujerba scripts configure the same triple, so
// nothing here needs a map id, and none is used.
bool IsShoutGauge();

// Installs the condition hook that arms the flag above. Non-fatal: without it `IsShoutGauge` stays
// false and every shout feature stays silent, which is the correct failure.
bool InitHooks();

// Clears the arming. A gauge identity belongs to the map it was configured on.
void OnMapTeardown();

} // namespace ShoutGauge
