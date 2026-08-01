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
// Map 569 (Lower Halls -- capture RECTS and eight soldiers, a different mechanism) is known and
// deliberately absent: sneak assist clamps a distance native, which is not what 569 checks, so
// listing it would promise a player something F10 cannot deliver there.
struct Row {
    uint32_t mapId;
    int16_t  nameIdx;    // npcdic id of the watching actor(s)
};

constexpr Row kRows[] = {
    { 568u, 694 },   // Royal Palace: Cellars -- the guarded stair to 569
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
