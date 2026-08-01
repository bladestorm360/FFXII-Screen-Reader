#include "navigation/path_danger.h"

#include <windows.h>

#include <cstdio>
#include <vector>

#include "navigation/entity_list.h"
#include "navigation/map_names.h"
#include "navigation/nav_common.h"
#include "navigation/nav_types.h"
#include "navigation/player_state.h"
#include "core/logger.h"

namespace PathDanger {
namespace {

// ---- The per-map table --------------------------------------------------------------------------
//
// One row = one map that runs a proximity-punishing stealth sequence. Identifiers are the game's
// own: `mapId` from planmapname, `nameIdx` the npcdic name index of the watching actors (568's two
// guards both carry 694 -- the log's dup-label "Imperial" pair).
//
// THE TABLE IS COMPLETE FOR THE ROYAL PALACE, and that is a MEASUREMENT (Session 115). The palace is
// maps 567-572 (`rrp_a01`..`rrp_a06`); each map's own `.ebp` was scanned for capture-routine names
// and for the detection natives' CALLACT sites, and the two signals agree:
//
//     567 rrp_a01   0 capture routines                                   -- nothing to do
//     568 rrp_a02   1 (`ヴァン捕獲`)   distance x8, touch x4, wait x21
//     569 rrp_a03  12 (`捕獲レクトＡ/Ｂ/Ｃ`, `捕獲レクト兵士０１..０７`,
//                      `捕獲監視監督`)  distance x4
//     570 rrp_a04   0 |  571 rrp_a05   0 |  572 rrp_a06   0             -- nothing to do
//
// So ONLY 568 and 569 run a capture sequence at all, and the palace needs no further rows. This
// replaces the older note that 569 was "deliberately absent because it uses capture RECTS": the
// distance native fires there too (the log's `native FIRED on map 569`), and the touch suppression
// is keyed on the guards' npcdic identity, which 569's fourteen "Imperial" actors share with 568's
// two. If a capture DOES still happen on 569 it is the rect actors, and the falsifier in
// sneak_assist.cpp names the object rather than leaving it to another guessing round.
struct Row {
    uint32_t mapId;
    int16_t  nameIdx;    // npcdic id of the watching actor(s)
};

constexpr Row kRows[] = {
    { 568u, 694 },   // Royal Palace: Cellars     -- the guarded stair to 569 (2 "Imperial" actors)
    { 569u, 694 },   // Royal Palace: Lower Halls -- 14 "Imperial" actors, the SAME npcdic id
};

const Row* RowForMap(uint32_t mapId) {
    for (const Row& r : kRows)
        if (r.mapId == mapId) return &r;
    return nullptr;
}

} // namespace

bool MapHasRow(uint32_t mapId) { return RowForMap(mapId) != nullptr; }

int16_t DangerNameIdx(uint32_t mapId) {
    const Row* r = RowForMap(mapId);
    return r ? r->nameIdx : static_cast<int16_t>(-1);
}

void NoteFieldFrame() {
    // One int compare on every map without a row -- the early-out that keeps this free.
    const Row* row = RowForMap(static_cast<uint32_t>(MapNames::CurrentMapId()));

    static uint64_t s_lastTickMs = 0;
    static FVec3    s_lastPos{};
    static bool     s_havePos = false;

    const uint64_t now = GetTickCount64();
    const uint64_t prev = s_lastTickMs;
    s_lastTickMs = now;

    if (!row) { s_havePos = false; return; }

    // A field-tick gap is a scripted scene (a capture is one; so are dialogues and menus). Log the
    // PRE-GAP player position -- where the player physically was when the scene took control --
    // against the watching actors. Log-only; one line per gap; only on table maps.
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
