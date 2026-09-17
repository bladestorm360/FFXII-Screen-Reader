#include "navigation/reach_gate.h"
#include "navigation/map_script_routines.h"
#include "navigation/path_planner.h"
#include "navigation/nav_reach.h"
#include "navigation/nav_rva.h"
#include "navigation/nav_common.h"
#include "navigation/player_state.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/logger.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace ReachGate {

namespace {

using EntityList::Category;
using EntityScan::Entity;

// A recorded answer matches a listed entity with the same label at (effectively) the same point: the route
// request carries the entity's own position, so a static object or an exit matches at 0 m. Kept tight
// because Destiny's March lists each Door of Hours twice, one object per side (spacing not measured), and
// the two sides open onto different rooms. A walking NPC simply stops matching, and is listed.
constexpr float  kSameTargetM = 0.25f;
constexpr size_t kMaxRecords  = 256;

// The party class -> the effective-flags bit that refuses it (GameArchitecture.md: bit 23 class 0,
// 24 class 5, 25 class 1, 26 class 2, 27 class 3).
uint32_t RefuseBit(uint16_t cls) {
    switch (cls) {
    case 0: return 1u << 23;
    case 1: return 1u << 25;
    case 2: return 1u << 26;
    case 3: return 1u << 27;
    case 5: return 1u << 24;
    default: return 0;
    }
}

// What a recorded answer was given IN. Any field differing means the world may have changed under it.
struct World {
    uint32_t epoch    = 0;   // PathPlanner's map generation
    uint64_t tableFp  = 0;   // the floor override banks' bytes: a door or waterfall moving changes them
    uint32_t reachGen = 0;   // NavReach restarts: a new map, or the player in another mesh component
    bool operator==(const World& o) const {
        return epoch == o.epoch && tableFp == o.tableFp && reachGen == o.reachGen;
    }
};

// FNV-1a over the two override banks FUN_00232020 applies (material 0x00-0x1F, group 0x40-0x4F). One
// guarded 640-byte read; a torn read only yields a different print, which lists the entity again.
uint64_t TableFingerprint() {
    void* tbl = Hooks::ResolveRva(NavRva::WALK_FLAG_TABLE);
    if (!tbl) return 0;
    uint8_t buf[NavRva::WALK_FLAG_ENTRIES * 8] = {};
    if (!MemRead::SafeReadBytes(tbl, buf, sizeof(buf))) return 0;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(buf); ++i) {
        const size_t entry = i / 8;
        if (entry >= 0x20 && entry < NavRva::WALK_FLAG_GROUP_BASE) continue;   // wall bank: not floor
        h ^= buf[i];
        h *= 1099511628211ull;
    }
    return h;
}

World CurrentWorld() {
    return World{ PathPlanner::CurrentEpoch(), TableFingerprint(), NavReach::Generation() };
}

struct Record {
    std::wstring label;
    FVec3        pos;
    bool         reachable;
    World        world;
};

std::mutex          g_mutex;
std::vector<Record> g_records;

Record* FindLocked(const std::wstring& label, const FVec3& pos) {
    for (Record& r : g_records)
        if (r.label == label && NavCommon::Distance2D(r.pos, pos) <= kSameTargetM) return &r;
    return nullptr;
}

std::string Ascii(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back((c < 128) ? static_cast<char>(c) : '?');
    return s;
}

} // namespace

uint32_t PartyRefuseBit() { return RefuseBit(PlayerState::PartyMovementClass()); }

uint32_t OpenableFloorMask() {
    // Keyed on the SCRIPT, not the map id: the id flips at the leading edge of a transition while the
    // old blob is still resident (S93), so a map-id cache can be filled from the previous map.
    static uint64_t s_fingerprint = 0;
    static uint32_t s_mask        = 0xFFFFFFFFu;
    static bool     s_have        = false;

    const uint64_t fp = MapScript::ScriptFingerprint();
    if (s_have && fp == s_fingerprint) return s_mask;

    std::vector<MapScript::RoutineFacts> facts;
    const bool ok = MapScript::ReadRoutineFacts(facts);
    uint32_t mask = 0;
    if (ok) for (const MapScript::RoutineFacts& f : facts) mask |= f.opensFloorMask;

    // A FAILED READ KEEPS S182's BEHAVIOUR, it does not invent a new one. All-bits-set means every
    // closed-flag floor is still cut, exactly as before this change -- so the only thing a successful
    // read can ever do is NARROW the cut, and an unreadable script can never widen it.
    s_mask        = ok ? mask : 0xFFFFFFFFu;
    s_fingerprint = fp;
    s_have        = true;

    char m[192];
    snprintf(m, sizeof(m),
             "closed-floor: script %s -- material ids a door script can OPEN on this map: 0x%08X "
             "(%zu routine(s)); anything closed outside this mask is PRICED, not cut",
             ok ? "read" : "UNREADABLE (falling back to cut-everything, S182 behaviour)",
             s_mask, facts.size());
    Log::Write("NAV-ROUTE", m);
    return s_mask;
}

bool ScriptClosed(NavMesh::PolyId p) {
    const uint32_t bit = PartyRefuseBit();
    uint32_t raw = 0, eff = 0;
    return bit != 0 && NavMesh::PolyFlags(p, raw, eff) && ScriptClosedFlags(raw, eff, bit);
}

void NoteRouteResult(const std::wstring& label, const FVec3& target, bool reachable) {
    const World world = CurrentWorld();   // outside the lock: the read is the only non-trivial step
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        Record* r = FindLocked(label, target);
        if (!r) {
            if (g_records.size() >= kMaxRecords) g_records.erase(g_records.begin());
            g_records.push_back(Record{ label, target, reachable, world });
        } else {
            r->reachable = reachable;
            r->world     = world;
        }
    }
    if (!reachable) {
        char m[320];
        snprintf(m, sizeof(m),
                 "reach-gate: \"%s\" at (%.1f,%.1f,%.1f) answered No path -- hidden from the list while the "
                 "Unreachable filter is on, until the area, a door or waterfall, or the player's mesh "
                 "component changes (epoch %u)",
                 Ascii(label).c_str(), target.x, target.y, target.z, world.epoch);
        Log::Write("NAV", m);
    }
}

void Annotate(std::vector<Entity>& list) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_records.empty()) {
        for (Entity& e : list) e.reach = static_cast<uint8_t>(Verdict::Unknown);
        return;
    }
    const World now = CurrentWorld();
    for (Entity& e : list) {
        e.reach = static_cast<uint8_t>(Verdict::Unknown);
        if (e.category == Category::Enemy || e.noBearing) continue;
        const Record* r = FindLocked(e.label, e.pos);
        if (!r || !(r->world == now)) continue;
        e.reach = static_cast<uint8_t>(r->reachable ? Verdict::Reachable : Verdict::NoPath);
    }
}

} // namespace ReachGate
