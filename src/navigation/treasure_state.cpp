#include "navigation/treasure_state.h"

#include "navigation/map_names.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"

#include <Windows.h>
#include <cstdio>
#include <mutex>
#include <vector>

namespace {

using MemRead::SafeReadS16;
using MemRead::SafeReadU8;

// ---- offline-derived RVAs / offsets (abs = RVA + 0x120000) --------------------------------------
constexpr uint32_t RVA_AWARD = 0x1DAFD0;   // FUN_002fafd0(def, _, out) -- the treasure award

// The treasure DEFINITION record, from FUN_002faf60 (position), FUN_002faed0 (spawn roll) and
// FUN_002fafd0 (award). Only the three fields this module needs are named.
constexpr uint32_t DEF_INDEX = 0x00;   // u8   bit position in the map's treasure presence mask
constexpr uint32_t DEF_X10   = 0x04;   // s16  world X * 10
constexpr uint32_t DEF_Z10   = 0x06;   // s16  world Z * 10

// How near a listed treasure's position must be to the award record's to be the same treasure.
//
// The object was PLACED at exactly `(s16)/10.0f` by FUN_00355210 via FUN_0026af80, and the scan
// reads that same transform back, so the honest expectation is equality. This is slack for float
// round-tripping through the transform node, NOT a search radius: treasures on a map stand metres
// apart, so 0.25 m cannot reach a neighbour. If a log ever shows a miss just outside it, the
// assumption "nothing moves a placed treasure" is what has failed -- widen nothing until that is
// read, because a loose radius would drop the treasure BESIDE the one collected.
constexpr float kMatchTol = 0.25f;

// A map cannot hold more treasure than its presence mask has bits (FUN_002faed0 indexes a u32 by
// `def+0x00`), so 32 is the engine's own ceiling and this can never grow unbounded.
constexpr size_t kMaxRecords = 32;

typedef uint64_t (*Pfn_Award)(void* def, void* p2, void* out);
Pfn_Award s_origAward = nullptr;

bool g_initialized = false;

struct Collected {
    float x = 0.0f;
    float z = 0.0f;
};

std::mutex             g_mutex;
int                    g_mapId = -1;      // which map g_collected belongs to
std::vector<Collected> g_collected;

// "Collected" is a fact about THIS visit to THIS map and must not outlive it.
//
// The spawn roll FUN_002faed0 shows two populations: a def with a real one-time flag id is skipped
// for good once `istreasureflag(def+0x09)` is set, but a def with `+0x09 == 0xFF` records nothing at
// award time and re-rolls its spawn PERCENTAGE on every map load. How often the second kind is
// actually seen to come back is not something this module needs to settle, because dropping the set
// on a map change is the safe direction in BOTH cases: if the treasure is gone for good the engine
// simply never places it again, so there is nothing for a retained record to match and clearing
// costs nothing; if it can come back, a retained record would hide it. Holding it is the only
// choice that can be wrong.
void EnsureMapLocked(int mapId) {
    if (mapId == g_mapId) return;
    if (!g_collected.empty()) {
        char m[128];
        snprintf(m, sizeof(m), "treasure: map %d -> %d, dropping %zu collected record(s)",
                 g_mapId, mapId, g_collected.size());
        Log::Write("NAV", m);
    }
    g_mapId = mapId;
    g_collected.clear();
}

// FUN_002fafd0(def, _, out): the treasure award. Runs the original FIRST -- it is what actually
// gives the item and clears the game's own presence bit -- then reads the record it was handed.
//
// The record is script-segment data and immutable, so reading it after the call is safe and keeps
// this detour off the critical path. IT SPEAKS NOTHING: the item the player received is already
// announced by message_reader's own "obtained <item>" toast (FUN_0035e070), and a second voice here
// would race it.
uint64_t HookedAward(void* def, void* p2, void* out) {
    const uint64_t ret = s_origAward ? s_origAward(def, p2, out) : 0;
    STALL_SCOPE("TreasureState::Award");
    if (!def) return ret;

    int16_t x10 = 0, z10 = 0;
    uint8_t idx = 0xFF;
    if (!SafeReadS16(def, DEF_X10, &x10) || !SafeReadS16(def, DEF_Z10, &z10)) {
        Log::Write("NAV", "treasure: award record unreadable -- entry stays listed");
        return ret;
    }
    SafeReadU8(def, DEF_INDEX, &idx);

    const float x = static_cast<float>(x10) / 10.0f;
    const float z = static_cast<float>(z10) / 10.0f;

    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        EnsureMapLocked(MapNames::CurrentMapId());
        if (g_collected.size() < kMaxRecords) g_collected.push_back(Collected{x, z});
        n = g_collected.size();
    }

    // The falsifier. If the nav list never drops a treasure, compare this position against the
    // `Treasure` lines in the same log's object dump: equal means the scan-side test is wrong,
    // different means the placement assumption above is wrong. One line per collection.
    char m[176];
    snprintf(m, sizeof(m), "treasure: collected idx=%u at (%.2f, %.2f) on map %d -- %zu recorded",
             static_cast<unsigned>(idx), x, z, MapNames::CurrentMapId(), n);
    Log::Write("NAV", m);
    return ret;
}

} // namespace

namespace TreasureState {

bool Init() {
    if (g_initialized) {
        Log::Write("NAV", "TreasureState::Init called twice - ignoring");
        return true;
    }
    const bool ok = Hooks::InstallTyped(RVA_AWARD, &HookedAward, &s_origAward);
    g_initialized = true;
    Log::Write("NAV", ok
        ? "TreasureState initialized (award FUN_002fafd0; collected treasure leaves the nav list)"
        : "TreasureState: FUN_002fafd0 hook FAILED - collected treasure will stay listed");
    return ok;
}

void Shutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_collected.clear();
    g_mapId = -1;
    g_initialized = false;
}

bool WasCollectedAt(int mapId, float x, float z) {
    std::lock_guard<std::mutex> lk(g_mutex);
    // The map check has to RETIRE the set, not merely decline to use it. Treasure respawns: a def
    // with `+0x09 == 0xFF` re-rolls on every map load, so records kept across a map change would
    // drop a treasure that has legitimately come back when the player returns. The award hook alone
    // cannot do this -- it only runs when something is collected, so a player who leaves a map and
    // comes back without collecting anything would still be carrying the old set.
    EnsureMapLocked(mapId);
    if (g_collected.empty()) return false;
    for (const Collected& c : g_collected) {
        const float dx = c.x - x;
        const float dz = c.z - z;
        if (dx > -kMatchTol && dx < kMatchTol && dz > -kMatchTol && dz < kMatchTol) return true;
    }
    return false;
}

int RecordedCount() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return static_cast<int>(g_collected.size());
}

} // namespace TreasureState
