#include "navigation/path_danger.h"

#include <windows.h>

#include <cmath>
#include <cstdio>

#include "navigation/entity_list.h"
#include "navigation/map_names.h"
#include "navigation/nav_common.h"
#include "navigation/player_state.h"
#include "core/logger.h"

namespace PathDanger {
namespace {

// ---- The per-map table (USER-AUTHORIZED map-specific data, Session 106) --------------------------
//
// One row = one door on one map whose approach is scripted-dangerous. Identifiers are the game's
// own: `mapId` from planmapname, `doorPos` the door scene object's authored position, `nameIdx` the
// npcdic name index of the danger actors (568's two guards both carry 694 -- the log's dup-label
// "Imperial" pair). Radius/weight are ESTIMATES until the capture diagnostic has measured a real
// catch; both err wide, which for a soft price only makes routes take a broader berth.
//
// Map 569 (Lower Halls -- the bigger sibling minigame, capture rects + eight soldiers) is known and
// deliberately NOT entered yet: not this session's deliverable (user, 2026-07-31).
struct Row {
    uint32_t mapId;
    FVec3    doorPos;    // the door this data arms FOR; any other target gets nothing
    float    doorTolXZ;  // metres, horizontal match tolerance
    float    doorTolY;   // metres, vertical match tolerance
    int16_t  nameIdx;    // npcdic id of the danger actor(s) -- every live match gets a disc
    float    radius;     // disc radius, metres
    float    weight;     // added crossing cost, metres (kBlockedPenalty tier)
};

constexpr Row kRows[] = {
    // Royal Palace: Cellars -- door_gunbit to 569, guarded stair. Guards = npcdic 694.
    { 568u, { 38.60f, 0.00f, 117.85f }, 3.0f, 2.0f, 694, 9.0f, 2000.0f },
};

const Row* RowForMap(uint32_t mapId) {
    for (const Row& r : kRows)
        if (r.mapId == mapId) return &r;
    return nullptr;
}

} // namespace

bool MapHasRow(uint32_t mapId) { return RowForMap(mapId) != nullptr; }

void ActiveZones(uint32_t mapId, const FVec3& target, std::vector<Disc>& out) {
    out.clear();
    const Row* row = RowForMap(mapId);
    if (!row) return;
    // PER-TARGET ARMING. The whole point of the tolerance test: the Palace Servant who starts the
    // event chain stands ~3 m from these guards, and a route TO HIM must never be shaped.
    if (NavCommon::Distance2D(target, row->doorPos) > row->doorTolXZ) return;
    if (std::fabs(target.y - row->doorPos.y) > row->doorTolY) return;

    std::vector<FVec3> actors;
    EntityList::CollectPositionsByNameIdx(row->nameIdx, actors);
    if (actors.empty()) return;      // actors despawned/streamed out: nothing to avoid

    out.reserve(actors.size());
    for (const FVec3& a : actors) out.push_back(Disc{ a, row->radius, row->weight });

    char m[224]; int q = 0;
    q += snprintf(m, sizeof(m), "zones armed: map %u target-door matched; %zu disc(s) r=%.1f",
                  mapId, out.size(), row->radius);
    for (size_t i = 0; i < out.size() && q < static_cast<int>(sizeof(m)) - 32; ++i)
        q += snprintf(m + q, sizeof(m) - static_cast<size_t>(q), " (%.1f,%.1f,%.1f)",
                      out[i].c.x, out[i].c.y, out[i].c.z);
    Log::Write("DANGER", m);
}

float PenaltyAt(const std::vector<Disc>& zones, const FVec3& p) {
    float w = 0.0f;
    for (const Disc& d : zones) {
        const float dx = p.x - d.c.x, dz = p.z - d.c.z;
        if (dx * dx + dz * dz <= d.r * d.r) w += d.w;
    }
    return w;
}

void NoteFieldFrame() {
    // One int compare on every map without a table row -- the early-out that keeps this free.
    const Row* row = RowForMap(MapNames::CurrentMapId());

    static uint64_t s_lastTickMs = 0;
    static FVec3    s_lastPos{};
    static bool     s_havePos = false;

    const uint64_t now = GetTickCount64();
    const uint64_t prev = s_lastTickMs;
    s_lastTickMs = now;

    if (!row) { s_havePos = false; return; }

    // A field-tick gap is a scripted scene (the catch is one; so are dialogues and menus). Log the
    // PRE-GAP player position -- where the player physically was when the scene took control --
    // against the danger actors' current discs. Log-only; one line per gap; only on table maps.
    const bool gapEnded = (prev != 0 && now - prev > 600 && s_havePos);
    FVec3 cur{};
    const bool haveCur = PlayerState::ReadPlayerPos(cur);
    if (gapEnded) {
        std::vector<FVec3> actors;
        EntityList::CollectPositionsByNameIdx(row->nameIdx, actors);
        char m[288]; int q = 0;
        q += snprintf(m, sizeof(m),
                      "scene-gap %llums ended: pre-gap player=(%.1f,%.1f,%.1f) now=(%.1f,%.1f,%.1f)",
                      (unsigned long long)(now - prev),
                      s_lastPos.x, s_lastPos.y, s_lastPos.z,
                      haveCur ? cur.x : 0.0f, haveCur ? cur.y : 0.0f, haveCur ? cur.z : 0.0f);
        for (size_t i = 0; i < actors.size() && q < static_cast<int>(sizeof(m)) - 24; ++i)
            q += snprintf(m + q, sizeof(m) - static_cast<size_t>(q), " | actor %zu at %.1fm",
                          i, NavCommon::Distance2D(s_lastPos, actors[i]));
        Log::Write("DANGER", m);
    }
    if (haveCur) { s_lastPos = cur; s_havePos = true; }
}

} // namespace PathDanger
