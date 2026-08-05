#include "navigation/shout_diag.h"

#include <cstdio>
#include <vector>

#include "navigation/entity_list.h"
#include "navigation/map_names.h"
#include "navigation/nav_types.h"
#include "navigation/player_state.h"
#include "navigation/shout_gauge.h"
#include "navigation/shout_meter.h"
#include "navigation/shout_table.h"
#include "core/logger.h"
#include "ui/mod_menu.h"

namespace ShoutDiag {
namespace {

// The whole measurement fits in one map visit's worth of shouts; past that the log has said
// everything it can and further captures are repetition.
constexpr int kMaxCaptures = 24;
int s_captures = 0;

// Write the npcdic census, nearest first. `cap` bounds both the read and the log volume.
void LogNpcs(const char* tag, const char* indent, int cap) {
    FVec3 me{};
    if (!PlayerState::ReadPlayerPos(me)) return;
    std::vector<EntityList::NearbyNPC> npcs;
    EntityList::CollectNearestNPCs(me, cap, npcs);
    for (const EntityList::NearbyNPC& e : npcs) {
        char row[112];
        snprintf(row, sizeof(row), "%snameIdx=%-5d %6.2fm  ", indent, e.nameIdx, e.dist2D);
        Log::WriteW(tag, row, e.label);
    }
}

} // namespace

void CaptureBurst(const ShoutScript::Module& mod, bool clean, int start, int end) {
    if (s_captures >= kMaxCaptures || !mod.valid) return;
    ++s_captures;
    char head[224];
    snprintf(head, sizeof(head), "%s on %s: meter %d -> %d (%+d). Nearest NPCs at that instant:",
             clean ? "CLEAN" : "PENALTY", mod.srcName, start, end, end - start);
    Log::Write("SHOUT-MEASURE", head);
    LogNpcs("SHOUT-MEASURE", "  ", 8);
}

void Dump() {
    char names[192] = {};
    const ShoutScript::Module m = ShoutScript::FindShoutModule(names, sizeof(names));
    char head[288];
    snprintf(head, sizeof(head), "-- shout minigame -- map %d, script modules: %s",
             MapNames::CurrentMapId(), names);
    Log::Write("NAV-PROBE", head);

    // THE GAUGE IS DUMPED WHETHER OR NOT A MODULE MATCHED. It used to return here, which is exactly
    // why S133's play log said nothing at all about a sequence that was demonstrably running: the
    // one line that would have shown the gauge was behind the check that failed.
    const ShoutGauge::State g = ShoutGauge::Read();
    char line[384];
    snprintf(line, sizeof(line),
             "   gauge read=%d value=%d max=%d flags=0x%08X shown=%d (+0xC0=%u +0xC1=%u "
             "mgr+0xC8=%u) | mapIsShoutStreet=%d puzzleActive=%d guide=%d skip=%d",
             g.ok ? 1 : 0, g.value, g.max, g.flags, g.shown ? 1 : 0, g.styleC0, g.styleC1,
             g.mgrFlags, ShoutTable::MapIsShoutStreet(MapNames::CurrentMapId()) ? 1 : 0,
             ShoutMeter::PuzzleActive() ? 1 : 0, ModMenu::PuzzleGuideOn() ? 1 : 0,
             ModMenu::PuzzleSkipOn() ? 1 : 0);
    Log::Write("NAV-PROBE", line);

    // The raw record dump: the question "what is actually in these five records" answered by bytes.
    ShoutScript::DumpRecords();

    if (m.valid) {
        snprintf(line, sizeof(line),
                 "   module=%s slot=%d meterVar=0x%02X goal=%d guardNameIdx=%d earshot=%.2f",
                 m.srcName, m.slot, m.row->meterVarIdx, m.row->fillValue, m.row->guardNameIdx,
                 m.row->earshotRadius);
        Log::Write("NAV-PROBE", line);
    } else {
        Log::Write("NAV-PROBE", "   NO shout script module resolved -- the instant fill needs one; "
                                "the meter and the keys fall back to the map-id gate");
    }

    // Run this standing beside whoever punished the last shout and the guard's id is in this list.
    Log::Write("NAV-PROBE", "   NPCs here, nearest first:");
    LogNpcs("NAV-PROBE", "     ", 40);
}

void OnMapTeardown() { s_captures = 0; }

} // namespace ShoutDiag
