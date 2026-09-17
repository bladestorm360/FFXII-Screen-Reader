#include "navigation/sochen_doors.h"

#include <atomic>
#include <cstdio>

#include "core/hooks.h"
#include "core/logger.h"
#include "navigation/map_names.h"
#include "navigation/shout_script.h"
#include "ui/mod_menu.h"

namespace SochenDoors {
namespace {

// Every shipped Sochen Cave Palace map script shares this authoring prefix (rui_a01..a05, rui_b01..b03,
// rui_c01, rui_d01, rui_e01), and every one of them jumps only to Sochen maps 184-202 or to the save
// crystal teleport list. Gating on the script rather than a map id is the Stilshrine arrangement.
constexpr char kPalacePrefix[] = "rui_";
constexpr int  kMaxModules     = 5;   // the controller array is five slots

// The storage-class-0 save block: FUN_002ef2b0 returns &DAT_02164280 + 0x200.
constexpr uint32_t RVA_SAVE_BLOCK = 0x2044480;

constexpr uint32_t kFlagOff       = 0x918;
constexpr uint32_t kFlagDesc      = 0x00000918;   // u8, storage class 0, offset 0x918
constexpr uint8_t  kClockSolved   = 0x01;         // Destiny's March -> the Ascetic's Door
constexpr uint8_t  kWaterSolved   = 0x02;         // Falls of Time   -> both Pilgrim's Doors
constexpr uint8_t  kBothSolved    = kClockSolved | kWaterSolved;

// The descriptor API takes a uint8_t index, so the format caps a table at 256.
constexpr uint32_t kMaxVars = 256;

std::atomic<bool> s_inPalace{false};
bool s_attempted = false;   // game thread only; one attempt per visit while the row is On

enum class Decl { Absent, Matches, Conflicts };

// Does the live module declare class0+0x918, and if so, as the u8 every Sochen script declares?
Decl CheckDeclaration(const ShoutScript::RawModule& mod, uint32_t* outDesc) {
    *outDesc = 0;
    uint32_t n = ShoutScript::VarCount(mod.record);
    if (n > kMaxVars) n = kMaxVars;
    for (uint32_t i = 0; i < n; ++i) {
        void* addr = nullptr;
        uint8_t type = 0xFF;
        uint32_t raw = 0;
        // The return value is ignored on purpose: a class-2 descriptor has no address, but its raw
        // word is still filled in and is all this needs.
        ShoutScript::VarAddressRaw(mod.record, mod.ebpBase, static_cast<uint8_t>(i), &addr, &type, &raw);
        if (((raw >> 24) & 7) != 0 || (raw & 0xFFFFFF) != kFlagOff) continue;
        *outDesc = raw;
        return raw == kFlagDesc ? Decl::Matches : Decl::Conflicts;
    }
    return Decl::Absent;
}

void Apply(const ShoutScript::RawModule& mod) {
    char m[256];
    const int mapId = MapNames::CurrentMapId();

    void* block = Hooks::ResolveRva(RVA_SAVE_BLOCK);
    void* base  = ShoutScript::ClassBaseRaw(mod.record, mod.ebpBase, 0);
    if (!block || base != block) {
        snprintf(m, sizeof(m),
                 "declining: %s (slot %d, map %d) class-0 base %p != save block %p (RVA 0x%X)",
                 mod.srcName, mod.slot, mapId, base, block, RVA_SAVE_BLOCK);
        Log::Write("SOCHEN", m);
        return;
    }

    uint32_t desc = 0;
    const Decl decl = CheckDeclaration(mod, &desc);
    if (decl == Decl::Conflicts) {
        snprintf(m, sizeof(m),
                 "declining: %s declares class0+0x%X as 0x%08X, expected 0x%08X -- the script changed",
                 mod.srcName, kFlagOff, desc, kFlagDesc);
        Log::Write("SOCHEN", m);
        return;
    }

    void* addr = static_cast<char*>(block) + kFlagOff;
    int32_t before = 0;
    if (!ShoutScript::ReadVar(addr, 0, &before)) {
        snprintf(m, sizeof(m), "declining: flag byte @%p unreadable", addr);
        Log::Write("SOCHEN", m);
        return;
    }

    const char* declWord = decl == Decl::Matches ? "declared u8 here" : "not declared by this room";
    if ((before & kBothSolved) == kBothSolved) {
        snprintf(m, sizeof(m), "both puzzles already solved: class0+0x%X @%p = 0x%02X (%s, map %d, %s)",
                 kFlagOff, addr, before & 0xFF, mod.srcName, mapId, declWord);
        Log::Write("SOCHEN", m);
        return;
    }

    const int32_t want = (before | kBothSolved) & 0xFF;
    const bool wrote = ShoutScript::WriteVar(addr, 0, want);
    int32_t after = -1;
    ShoutScript::ReadVar(addr, 0, &after);
    snprintf(m, sizeof(m),
             "%s class0+0x%X @%p: 0x%02X -> 0x%02X, read back 0x%02X (waterfall %s, clock %s; %s, map %d, %s)",
             wrote ? "WROTE" : "WRITE FAULTED at", kFlagOff, addr, before & 0xFF, want, after & 0xFF,
             (before & kWaterSolved) ? "was solved" : "now solved",
             (before & kClockSolved) ? "was solved" : "now solved",
             mod.srcName, mapId, declWord);
    Log::Write("SOCHEN", m);
}

} // namespace

void OnFieldFrame() {
    ShoutScript::RawModule mods[kMaxModules];
    const int n = ShoutScript::FindModulesBySrcPrefix(kPalacePrefix, mods, kMaxModules);
    s_inPalace.store(n > 0, std::memory_order_relaxed);
    if (n <= 0) return;

    // Off re-arms, so switching the row On later in the same visit still gets its attempt.
    if (!ModMenu::SochenPuzzlesOn()) { s_attempted = false; return; }
    if (s_attempted) return;
    s_attempted = true;
    Apply(mods[0]);
}

void OnMapTeardown() {
    s_attempted = false;
    s_inPalace.store(false, std::memory_order_relaxed);
}

bool InPalace() { return s_inPalace.load(std::memory_order_relaxed); }

} // namespace SochenDoors
