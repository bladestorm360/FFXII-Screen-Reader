#include "ui/gil_reader.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "speech/speech.h"
#include "core/logger.h"
#include "input/input_tracker.h"

#include <cstdint>
#include <string>

namespace {

// Party gil. The game's getter FUN_00253690 returns *(u32*)(DAT_02092758 + 8); we mirror it as a pure
// two-step memory read so the key is safe off the game thread (no game call). CONFIRMED live
// (Session 68 probe_shop): this candidate matched the getter's value (99999999).
constexpr uint32_t RVA_GIL_BASE = 0x1F72758;   // &DAT_02092758 : global slot holding the gil-context ptr
constexpr uint32_t OFF_GIL      = 0x08;        // context + 8 = party gil (u32)

void OnGilKey() {
    void* slot = Hooks::ResolveRva(RVA_GIL_BASE);   // &DAT_02092758
    void* ctx  = MemRead::PtrAt(slot, 0);           // *DAT_02092758 (null before a save is loaded)
    if (!ctx) return;                               // no party context -> say nothing (never filler)
    uint32_t gil = 0;
    if (!MemRead::SafeReadU32(ctx, OFF_GIL, &gil)) return;
    std::wstring line = std::to_wstring(gil) + L" gil";   // game draws the unit as "GIL"
    Log::WriteW("SHOP", "gil:", line);
    Speech::Output(line, /*interrupt=*/true);
}

} // namespace

namespace GilReader {

bool Init() {
    InputTracker::SetGilCallback(&OnGilKey);
    Log::Write("SHOP", "GilReader initialized ('g' -> party gil total)");
    return true;
}

void Shutdown() { InputTracker::SetGilCallback(nullptr); }

void Announce() { OnGilKey(); }

} // namespace GilReader
