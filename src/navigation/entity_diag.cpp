#include "navigation/entity_list.h"
#include "navigation/entity_scan.h"
#include "navigation/nav_rva.h"
#include "navigation/map_rva.h"
#include "navigation/nav_common.h"
#include "navigation/player_state.h"
#include "navigation/map_query.h"
#include "navigation/map_names.h"
#include "navigation/map_exits.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/game_text.h"
#include "core/logger.h"

#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace MemRead;

// The backtick object-dump diagnostic. File-only -- it never speaks, so it can afford to be
// verbose, and it is the tool that pins down where a mis-scanned object lives and how it should
// classify. Split out of entity_list.cpp, where 130 lines of pure logging were a seventh of the file.
namespace EntityDiag {



// Dump the raw handle table: every live scene object in every container, with its
// interaction flags, npcdic name key, resolved name, and world position. This is the
// data that proves where a given interactive object (e.g. the tutorial gate) actually
// lives and how it should classify. File-only (never spoken); bounded by the per-
// container count clamp. Interactive (talk/action) objects get a full line; the rest
// are only counted.
// Caller (EntityList::LogDiagnostic) already holds the entity-list mutex -- this walks the handle
// table live, so it must not race a rescan.
void DumpLocked() {
    FVec3 pp; bool haveP = PlayerState::ReadPlayerPos(pp);
    void* leader = PlayerState::ReadLeaderSceneObject();
    void* base = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);

    char hdr[160];
    snprintf(hdr, sizeof(hdr),
             "==== handle-table diag: player=(%.2f,%.2f,%.2f) haveP=%d leader=%p ====",
             pp.x, pp.y, pp.z, haveP ? 1 : 0, leader);
    Log::Write("NAV-DIAG", hdr);

    for (uint32_t c = 0; c < NavRva::HANDLE_TABLE_CONTAINERS && base; ++c) {
        void* table = static_cast<char*>(base) + static_cast<size_t>(c) * NavRva::HANDLE_TABLE_STRIDE;
        uint8_t active = 0;
        SafeReadU8(table, NavRva::TBL_ACTIVE_OFF, &active);
        void* entries = PtrAt(table, NavRva::TBL_ENTRIES_OFF);
        uint32_t count = 0;
        if (entries) SafeReadU32(entries, NavRva::ENTRIES_COUNT_OFF, &count);

        uint32_t shown = 0, plain = 0;
        if (entries && (active & 1) && count <= 4096) {
            for (uint32_t i = 0; i < count; ++i) {
                void* obj = PtrAt(entries, NavRva::ENTRIES_SLOT0_OFF + i * 8);
                if (!obj) { ++plain; continue; }
                uint32_t flags = 0;
                SafeReadU32(obj, NavRva::SCENEOBJ_FLAGS_OFF, &flags);
                int16_t nameIdx = 0;
                SafeReadS16(obj, NavRva::SCENEOBJ_NAME_IDX, &nameIdx);
                std::wstring lbl = EntityScan::ResolveObjectName(obj);
                // Skip the mass of anonymous, flagless props/triggers; dump anything
                // interactive, named, or in the gimmick band (where the gate must fall).
                if (flags == 0 && lbl.empty() && !EntityScan::InGimmickBand(nameIdx)) { ++plain; continue; }
                ++shown;

                uint8_t catByte = 0, readyByte = 0;
                SafeReadU8(obj, NavRva::SCENEOBJ_TYPE_BYTE, &catByte);   // low5=category, high3=class
                SafeReadU8(obj, 0x14, &readyByte);                      // & 0x20 model, & 0x40 ready
                FVec3 pos; bool havePos = PlayerState::ReadSceneObjectPos(obj, pos);
                char nlabel[48] = {};
                for (size_t k = 0; k < lbl.size() && k < 47; ++k)
                    nlabel[k] = (lbl[k] < 128) ? static_cast<char>(lbl[k]) : '?';
                char line[256];
                snprintf(line, sizeof(line),
                         "    [%u:%u] obj=%p cat=%02X r14=%02X flags=%08X%s%s nameIdx=%d \"%s\" pos=(%.2f,%.2f,%.2f) hp=%d",
                         c, i, obj, catByte, readyByte, flags,
                         (flags & NavRva::FLAG_TALK) ? " TALK" : "",
                         (flags & NavRva::FLAG_ACTION) ? " ACT" : "",
                         nameIdx, nlabel, pos.x, pos.y, pos.z, havePos ? 1 : 0);
                Log::Write("NAV-DIAG", line);
            }
        }
        char ch[128];
        snprintf(ch, sizeof(ch),
                 "  container %u: active=%u count=%u shown=%u plain=%u",
                 c, active & 1, count, shown, plain);
        Log::Write("NAV-DIAG", ch);
    }
    // Combatant pool dump (party + enemies) — CONFIRMS the enemy discriminator. def+5 is
    // player-vs-AI (0 = the leader, skipped), NOT faction. The faction test is the scene-kind
    // nibble kind = *(u8)(sceneObj+0x0e)&0xf: kind==3 ally, {1,2,7} enemy, 5 dead. Expect the
    // enemies (Imperial Swordsman, Air Cutter Remora) kind!=3 and the party (Reks/Basch/soldiers)
    // kind==3. charid = def+4 (roster index) is a secondary cross-check.
    void* pool = PtrAt(Hooks::ResolveRva(NavRva::ACTOR_POOL_BASE), 0);
    uint32_t pcount = 0;
    SafeReadU32(Hooks::ResolveRva(NavRva::ACTOR_POOL_COUNT), 0, &pcount);
    if (pcount > 64) pcount = 64;
    char ph[96];
    snprintf(ph, sizeof(ph), "==== combatant pool: base=%p count=%u ====", pool, pcount);
    Log::Write("NAV-DIAG", ph);
    for (uint32_t i = 0; pool && i < pcount; ++i) {
        void* actor = static_cast<char*>(pool) + static_cast<size_t>(i) * NavRva::ACTOR_STRIDE;
        void* def = PtrAt(actor, NavRva::ACTOR_DEF_PTR);
        if (!def) continue;                                  // empty slot
        uint8_t active = 0, def5 = 0xff, charid = 0, kindByte = 0;
        SafeReadU8(actor, NavRva::ACTOR_ACTIVE_OFF, &active);
        SafeReadU8(def, NavRva::DEF_KIND_BYTE, &def5);
        SafeReadU8(def, NavRva::DEF_CHARID, &charid);
        void* sceneObj = PtrAt(actor, NavRva::ACTOR_SCENEOBJ);
        if (sceneObj) SafeReadU8(sceneObj, NavRva::SCENEOBJ_KIND_OFF, &kindByte);
        int kind = kindByte & NavRva::KIND_MASK;
        const char* faction = (kind == NavRva::KIND_DEAD) ? "dead"
                            : (def5 == NavRva::PLAYER_DEF_KIND || kind == NavRva::KIND_ALLY) ? "ally"
                            : "enemy";
        FVec3 pos{}; bool havePos = sceneObj && PlayerState::ReadSceneObjectPos(sceneObj, pos);
        std::wstring lbl;
        const uint8_t* codec = reinterpret_cast<const uint8_t*>(PtrAt(actor, NavRva::ACTOR_NAME_STR));
        if (codec) { std::wstring s = GameText::Decode(codec, 256); if (GameText::IsMostlyPrintable(s)) lbl = s; }
        char nlabel[48] = {};
        for (size_t k = 0; k < lbl.size() && k < 47; ++k) nlabel[k] = (lbl[k] < 128) ? static_cast<char>(lbl[k]) : '?';
        char line[256];
        snprintf(line, sizeof(line),
                 "    pool[%u] actor=%p active=%02X def+5=%d kind=%d charid=%d -> %s \"%s\" pos=(%.2f,%.2f,%.2f) posOk=%d",
                 i, actor, active, static_cast<int>(static_cast<int8_t>(def5)),
                 kind, charid, faction,
                 nlabel, pos.x, pos.y, pos.z, havePos ? 1 : 0);
        Log::Write("NAV-DIAG", line);
    }

    // EXIT dump: the field-sign array (+0x70) is the exit source — each record's world pos + the game's own
    // resolved destination. +0x54 (below) logs the raw map-jump points for cross-reference, and the region's
    // floor list (map-connection DB) is logged last since it is NOT an exit list. A single ' press near an
    // exit pins the source and confirms it matches the "Entering X" heard on arrival.
    Log::Write("NAV-DIAG", "==== map exits (+0x70 field signs — world pos + destination) ====");
    {
        std::vector<MapExits::ExitRec> fs;
        MapExits::EnumerateFieldSignExits(fs, /*logRaw=*/true);
        char es[72];
        snprintf(es, sizeof(es), "  named exits: %zu", fs.size());
        Log::Write("NAV-DIAG", es);
    }
    Log::Write("NAV-DIAG", "==== map-jump points (+0x54) ====");
    {
        std::vector<MapExits::ExitRec> jumps;
        const FVec3* pep = haveP ? &pp : nullptr;
        MapExits::EnumerateMapJumps(pep, EntityScan::kExitMaxDist, jumps, /*logRaw=*/true);
        char es[72];
        snprintf(es, sizeof(es), "  map-jumps accepted (sanity-gated): %zu", jumps.size());
        Log::Write("NAV-DIAG", es);
    }
    // The region's floor list — logged for reference only. NOT exits: these are the sub-areas of the
    // current REGION as the map screen stacks them (atlas coords, includes the area you're standing in).
    Log::Write("NAV-DIAG", "==== region sub-area list (map-screen DB — NOT exits) ====");
    {
        std::vector<MapExits::ExitRec> conns;
        MapExits::EnumerateMapConnections(conns, /*logRaw=*/true);
        char es[72];
        snprintf(es, sizeof(es), "  region sub-areas: %zu", conns.size());
        Log::Write("NAV-DIAG", es);
    }
    Log::Write("NAV-DIAG", "==== end handle-table diag ====");
}

} // namespace EntityDiag
