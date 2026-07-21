#include "navigation/entity_scan.h"
#include "navigation/nav_rva.h"
#include "navigation/map_rva.h"
#include "navigation/nav_common.h"
#include "navigation/player_state.h"
#include "navigation/map_query.h"
#include "navigation/map_names.h"
#include "navigation/map_exits.h"
#include "navigation/map_script.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"
#include "core/game_text.h"
#include "core/logger.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace MemRead;

namespace EntityScan {

using EntityList::Category;
// --- helpers ---------------------------------------------------------------

const wchar_t* CategoryWord(Category c) {
    switch (c) {
        case Category::All:         return L"All";
        case Category::Exit:        return L"Exit";
        case Category::SaveCrystal:  return L"Save Crystal";
        case Category::GateCrystal:  return L"Gate Crystal";
        case Category::Treasure:    return L"Treasure";
        case Category::NPC:         return L"NPC";
        case Category::Object:      return L"Interactables";
        case Category::Enemy:       return L"Enemy";
        default:                    return L"Interactables";
    }
}

// npcdic codec-string pointer for a name index — replicates FUN_003eac10 (the game's
// own npcdic lookup) memory-only. The blob is loaded once at boot; DAT_02b5e0d8 holds
// its base. Even slot = display name (odd = yomi/reading). The offset table stores
// relocated absolute pointers as s32, read here exactly as the game does; 0 /
// out-of-range -> null.
const uint8_t* NpcdicName(int id) {
    void* blob = PtrAt(Hooks::ResolveRva(NavRva::NPCDIC_BASE), 0);
    if (!blob) return nullptr;
    uint32_t count = 0;
    if (!SafeReadU32(blob, NavRva::NPCDIC_COUNT_OFF, &count)) return nullptr;
    uint32_t slot = static_cast<uint32_t>(id) * 2;
    if (slot >= count) return nullptr;
    uint32_t entry = 0;
    if (!SafeReadU32(blob, NavRva::NPCDIC_TABLE_OFF + slot * 4, &entry) || entry == 0)
        return nullptr;
    // Sign-extend the s32 to a full pointer, as the game does (blob mapped low).
    return reinterpret_cast<const uint8_t*>(
        static_cast<intptr_t>(static_cast<int32_t>(entry)));
}

// The game's own (current-locale) display name for a field object, read memory-only
// from its SCENE OBJECT exactly as FUN_00263990 does: a name index at +0x102 selects
// the global npcdic dictionary; a negative index means a per-map custom string at
// +0xf8 (set by the map's fieldsignmes script). Empty on failure -> caller falls back
// to a category word. No game-function call — pure reads, SEH-guarded via MemRead.
std::wstring ResolveObjectName(void* sceneObj) {
    if (!sceneObj) return L"";
    int16_t idx = 0;
    if (!SafeReadS16(sceneObj, NavRva::SCENEOBJ_NAME_IDX, &idx)) return L"";
    const uint8_t* codec =
        (idx < 0) ? reinterpret_cast<const uint8_t*>(PtrAt(sceneObj, NavRva::SCENEOBJ_NAME_STR))
                  : NpcdicName(static_cast<int>(static_cast<uint32_t>(idx) & NavRva::NPCDIC_NAME_MASK));
    void* empty = Hooks::ResolveRva(NavRva::EMPTY_STRING);
    if (!codec || reinterpret_cast<void*>(const_cast<uint8_t*>(codec)) == empty) return L"";
    std::wstring s = GameText::Decode(codec, 256);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// (The current-area name is resolved via MapNames::CurrentMapId + ResolveMapName/ResolveRegionName —
//  FUN_003778b0 takes a MAP ID it was being called without, which returned the empty sentinel. See
//  CurrentAreaName below.)

// True when an npcdic name key falls in the field gimmick-object band (433-469:
// treasure, urn, crystals, anchor). Used so a named gimmick is always listed even if
// its interaction flag is momentarily clear.
bool InGimmickBand(int16_t nameIdx) {
    if (nameIdx < 0) return false;
    int id = static_cast<int>(static_cast<uint32_t>(nameIdx) & NavRva::NPCDIC_NAME_MASK);
    return id >= 433 && id <= 469;
}

// Category from the npcdic id band + the scene-object CHARACTER category (both locale-independent).
// The named gimmick sub-types come from the npcdic id (sceneObj+0x102 when >= 0): ids 433-469 are the
// field gimmick-object band (434 Treasure, 468 Urn, 466 Gate Crystal, 469 Save Crystal, 435-459/467
// area/life crystals). NPC-vs-object then rides: FLAG_TALK => a talk target (person); else the scene
// object's CHARACTER type — `isCharacter` = scene category (sceneObj+0x03 & 0x1f) in 5-7, the classes
// that carry a char component (people/actors) — vs a non-character gate/door/sign/switch. (The old
// "named person outside the gimmick band => NPC" heuristic mislabeled named GATES as NPCs; replaced.)
// The spoken LABEL is always the game's own text; this only drives the category FILTER.
Category ClassifyByNameKey(uint32_t flags, int16_t nameIdx, bool isCharacter) {
    if (nameIdx >= 0) {
        int id = static_cast<int>(static_cast<uint32_t>(nameIdx) & NavRva::NPCDIC_NAME_MASK);
        if (id == 434 || id == 468)                    return Category::Treasure;
        if (id == 466)                                 return Category::GateCrystal;
        if (id == 469 || id == 467 || (id >= 435 && id <= 459))
                                                       return Category::SaveCrystal;
        if (id >= 433 && id <= 469)                    return Category::Object;   // misc gimmick
    }
    if (flags & NavRva::FLAG_TALK) return Category::NPC;   // talk target => person
    if (isCharacter)               return Category::NPC;   // char-component scene object => person/actor
    return Category::Object;                               // non-character = gate/door/sign/switch
}

bool AlreadyListed(const std::vector<Entity>& out, void* sceneObj) {
    for (const auto& e : out) if (e.sceneObj == sceneObj) return true;
    return false;
}

// Append live COMBATANTS (allies + enemies) from the BtlWork pool. Field NPCs/gimmicks come
// from the handle table above; battle combatants (party, guests, enemies) live in this pool
// with their `def` record — they carry NO talk/action flag and NO npcdic key, so the handle-
// table filter drops them, and they must be read here (this is what makes them navigable in a
// battle). Per slot: def = actor+0x698 (null = empty); active = actor+0 & 0x10; SKIP the player-
// controlled unit (def+5 == 0 = the leader). Enemy vs ally = the def-attribute bit 24 the game's
// own target classifiers read (def+5 is player-vs-AI, NOT faction — runtime-disproven). Name =
// codec* at actor+0x18; position via the actor's scene node. Identity = the scene object, so it
// dedupes against the handle-table pass. Caller holds g_mutex.
void ScanCombatants(std::vector<Entity>& out) {
    void* pool = PtrAt(Hooks::ResolveRva(NavRva::ACTOR_POOL_BASE), 0);
    if (!pool) return;
    uint32_t count = 0;
    if (!SafeReadU32(Hooks::ResolveRva(NavRva::ACTOR_POOL_COUNT), 0, &count) || count == 0)
        return;
    if (count > 64) count = 64;   // sanity clamp (pool is 32 slots)

    for (uint32_t i = 0; i < count; ++i) {
        void* actor = static_cast<char*>(pool) + static_cast<size_t>(i) * NavRva::ACTOR_STRIDE;
        void* def = PtrAt(actor, NavRva::ACTOR_DEF_PTR);
        if (!def) continue;                                              // empty slot
        uint8_t active = 0;
        if (!SafeReadU8(actor, NavRva::ACTOR_ACTIVE_OFF, &active) ||
            (active & NavRva::ACTOR_ACTIVE_BIT) == 0) continue;          // not active / no model
        uint8_t def5 = 0xff;
        if (!SafeReadU8(def, NavRva::DEF_KIND_BYTE, &def5) ||
            def5 == NavRva::PLAYER_DEF_KIND) continue;                   // player-controlled leader -> skip
        void* sceneObj = PtrAt(actor, NavRva::ACTOR_SCENEOBJ);
        if (!sceneObj || AlreadyListed(out, sceneObj)) continue;

        // Enemy vs ally = the scene-kind nibble (the game's own faction test): kind==3 => ally,
        // kind==5 => dead/removed (drop), else => enemy.
        uint8_t kindByte = 0;
        if (!SafeReadU8(sceneObj, NavRva::SCENEOBJ_KIND_OFF, &kindByte)) continue;
        const uint8_t kind = kindByte & NavRva::KIND_MASK;
        if (kind == NavRva::KIND_DEAD) continue;                         // dead/removed

        FVec3 pos;
        if (!PlayerState::ReadSceneObjectPos(sceneObj, pos)) continue;
        if (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f) continue;   // unplaced reserve unit

        Entity e;
        e.sceneObj = sceneObj;
        e.flags    = 0;
        e.nameIdx  = -1;
        e.pos      = pos;
        e.category = (kind == NavRva::KIND_ALLY) ? Category::NPC : Category::Enemy;
        const uint8_t* codec = reinterpret_cast<const uint8_t*>(PtrAt(actor, NavRva::ACTOR_NAME_STR));
        if (codec) {
            std::wstring s = GameText::Decode(codec, 256);
            if (GameText::IsMostlyPrintable(s)) e.label = s;
        }
        if (e.label.empty()) e.label = CategoryWord(e.category);
        out.push_back(e);
    }
}

// Append the current map's EXITS — the transitions that move the party between areas (Inner Ward ->
// Upper Apartments). These are NOT scene objects in the handle table, so the interaction scanner is blind
// to them; they come from the per-map FIELD-SIGN array (mapData+0x70) via the game's own getters. Each is
// a FIXED-position Category::Exit entity (no scene node) with a synthetic stable identity so the cursor
// can lock to it.
//
// Destination names come from the game itself: FUN_002648f0 walks the record's +0x1d -> the +0x8c dest
// table -> a destination MAP ID, which ResolveFullAreaName renders as "<region>: <sub-area>". Because the
// same record also holds the world X/Y/Z, one record yields the whole format — name AND bearing — with no
// join between unrelated tables. An exit whose destination doesn't resolve is dropped, not spoken as a
// bare "Exit". Strict gates live in the enumerator, so a wrong offset yields no exits, never garbage.
// Caller holds g_mutex.

void ScanExits(std::vector<Entity>& out) {
    // Exits come from the +0x54 map-jump table — the REAL, WALKABLE world positions, so the player can
    // route to one. The destination NAME now comes from the map's own field script (see below), giving
    // "Exit, <region>: <sub-area>" + live bearing/steps: enough to choose an exit by where it goes and
    // walk to it. Only slots owned by a `__MJ_CTRL` controller are listed; the rest are arrival points.
    //
    // (The connection DB was dropped as the exit source: it is the region's FLOOR LIST — all sub-areas of the
    //  region, most not reachable from this room — which is why it reported 4 where there are 2. It has names
    //  but only map-atlas coords, so it can never be walked to. See MapExits::EnumerateMapConnections.)
    FVec3 p{}; const FVec3* pp = PlayerState::ReadPlayerPos(p) ? &p : nullptr;
    std::vector<MapExits::ExitRec> jumps;
    MapExits::EnumerateMapJumps(pp, kExitMaxDist, jumps, /*logRaw=*/false);

    // Destinations come from the map's OWN script. The toolchain emits one routine per map-jump door
    // named `__MJ_CTRL<N>`, owning `+0x54` slot N+1, with the destination as a `mapjump` literal — so a
    // door's destination is readable on the first frame of any map, with no cross-map data, no cache and
    // no learning. (Walk-tested across five Nalbina maps, and cross-checked against the arrival relation.)
    //
    // Match by POSITION, never by slot number: the `+0x54` table repeats records (one map's slot 1 is
    // byte-identical to slot 0) and EnumerateMapJumps de-duplicates them, so a surviving entry's index can
    // differ from the owning slot while naming the same physical doorway. Position matching also fails
    // safe — a mismatch drops the exit rather than mislabelling it.
    static int s_loggedMap = -1;
    const int  mapId     = MapNames::CurrentMapId();
    const bool logDetail = (mapId != s_loggedMap);   // once per map, not once per rescan
    if (logDetail) s_loggedMap = mapId;

    std::vector<MapScript::ExitDest> dests;
    MapScript::ReadExitDests(dests, logDetail);

    for (const auto& j : jumps) {
        const MapScript::ExitDest* d = nullptr;
        for (const auto& c : dests) {
            if (!c.posOk || c.destName.empty()) continue;
            if (std::fabs(c.pos.x - j.pos.x) < 0.05f &&
                std::fabs(c.pos.y - j.pos.y) < 0.05f &&
                std::fabs(c.pos.z - j.pos.z) < 0.05f) { d = &c; break; }
        }
        // No controller owns this position: it is an arrival/spawn point the party is placed on, not a
        // door the player can leave through. Listing it sends the player walking to a dead end, so skip it.
        if (!d) {
            if (logDetail) {
                char m[160];
                snprintf(m, sizeof(m), "  door +0x54[%d] (%.1f,%.1f,%.1f) -> no controller (arrival point)",
                         j.index, j.pos.x, j.pos.y, j.pos.z);
                Log::Write("NAV-DIAG", m);
            }
            continue;
        }

        Entity e;
        e.sceneObj  = nullptr;
        e.fixed     = true;                                      // fixed world pos, no scene node
        e.flags     = 0;
        e.nameIdx   = static_cast<int16_t>(-(1000 + j.index));   // distinct stable id for the cursor
        e.pos       = j.pos;                                     // WORLD -> bearing/steps are honest
        e.category  = Category::Exit;
        // "Exit, <region>: <sub-area>" — SpeakEntityLocked appends the live steps/bearing.
        e.label     = std::wstring(CategoryWord(Category::Exit)) + L", " + d->destName;
        out.push_back(e);

        if (logDetail) {
            char n8[96] = {};
            for (size_t k = 0; k < d->destName.size() && k < 95; ++k)
                n8[k] = (d->destName[k] < 128) ? static_cast<char>(d->destName[k]) : '?';
            char m[224];
            snprintf(m, sizeof(m), "  door +0x54[%d] (%.1f,%.1f,%.1f) -> __MJ_CTRL%03d \"%s\"",
                     j.index, j.pos.x, j.pos.y, j.pos.z, d->ctrlIndex, n8);
            Log::Write("NAV-DIAG", m);
        }
    }
}

// NOTE: the naviicon minimap "markers" were REMOVED (Session 44). Two decompile traces proved they are
// only character/unit dots (party/allies/enemies) that duplicate the combatant scan — no objective/crystal
// source, and a per-frame render buffer. Nothing to surface; deleted.
//
// NOTE: ScanSpawnTriggersLocked / Category::Event are GONE. The mapData+0x54 table it read is the
// map-jump exit table, not spawn/arrival points — it is now merged into ScanExitsLocked above.

// Rebuild the set from the scene-object HANDLE TABLE — the game's own registry of live
// interactive field objects (DAT_02098e10, 5 containers), populated at map load and
// walked every frame by FUN_0025b820 to decide what the player is near. We list every
// live talk/action object with a readable position: NPCs (FLAG_TALK) and action gimmicks
// like gates/doors/switches/treasure/crystals (FLAG_ACTION) — the latter are present from
// load, not spawned on approach. Positions come from the scene object's transform
// (sceneObj+0xB8), the same chain the leader uses. Caller holds g_mutex.
int BuildLocked(std::vector<Entity>& out) {
    out.clear();
    if (!PlayerState::IsFieldActive()) return 0;

    void* base = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!base) return 0;
    void* leader = PlayerState::ReadLeaderSceneObject();

    for (uint32_t c = 0; c < NavRva::HANDLE_TABLE_CONTAINERS; ++c) {
        void* table = static_cast<char*>(base) + static_cast<size_t>(c) * NavRva::HANDLE_TABLE_STRIDE;
        uint8_t active = 0;
        if (!SafeReadU8(table, NavRva::TBL_ACTIVE_OFF, &active) || (active & 1) == 0) continue;
        void* entries = PtrAt(table, NavRva::TBL_ENTRIES_OFF);
        if (!entries) continue;
        uint32_t count = 0;
        if (!SafeReadU32(entries, NavRva::ENTRIES_COUNT_OFF, &count) || count == 0 || count > 4096)
            continue;

        for (uint32_t i = 0; i < count; ++i) {
            void* obj = PtrAt(entries, NavRva::ENTRIES_SLOT0_OFF + i * 8);
            if (!obj || obj == leader) continue;

            uint32_t flags = 0;
            SafeReadU32(obj, NavRva::SCENEOBJ_FLAGS_OFF, &flags);
            int16_t nameIdx = 0;
            SafeReadS16(obj, NavRva::SCENEOBJ_NAME_IDX, &nameIdx);
            // Scene CATEGORY (sceneObj+0x03 & 0x1f): classes 5-7 carry a char component (people/actors),
            // classes 1-4 are position-only gates/doors/signs/props, class 0 is a null-node trigger.
            uint8_t catByte = 0;
            SafeReadU8(obj, NavRva::SCENEOBJ_TYPE_BYTE, &catByte);
            const int  sceneCat    = catByte & 0x1f;
            const bool isCharacter = (sceneCat >= 5 && sceneCat <= 7);
            // Include: interactive objects (talk/action), any named gimmick, AND — only for NON-character
            // objects — anything with a resolvable name (gates/doors/field-sign path-markers, cat 1-4).
            // Character objects (NPCs/party/enemies, cat 5-7) are deliberately NOT surfaced by the name
            // widening: unflagged ones are left to the combatant scan (ally/Enemy/dead) or the talk-flag
            // path, so defeated enemies and non-talk NPCs don't fall into the "Interactables" bucket.
            const bool interactive = (flags & (NavRva::FLAG_TALK | NavRva::FLAG_ACTION)) != 0;
            std::wstring name;
            if (nameIdx != 0 && !isCharacter) name = ResolveObjectName(obj);
            const bool named = !name.empty();
            if (!interactive && !InGimmickBand(nameIdx) && !named) continue;

            FVec3 pos;
            if (!PlayerState::ReadSceneObjectPos(obj, pos)) continue;
            if (AlreadyListed(out, obj)) continue;

            Entity e;
            e.sceneObj = obj;
            e.flags = flags;
            e.nameIdx = nameIdx;
            e.pos = pos;
            e.label = name;   // resolved above (empty for a flagged/character object)
            e.category = ClassifyByNameKey(flags, e.nameIdx, isCharacter);
            if (e.label.empty()) e.label = ResolveObjectName(obj);         // flagged char/gimmick name
            if (e.label.empty()) e.label = CategoryWord(e.category);
            out.push_back(e);
        }
    }

    // Combatants (allies + enemies) come from the BtlWork pool, not the handle table — the
    // handle-table filter drops them, so in a battle this is what makes them navigable.
    ScanCombatants(out);

    // Map exits — the map-jump points (+0x54), named from the field-sign array (+0x70) where possible.
    // Fixed-position, invisible to the interaction scanner. (Naviicon "markers" removed — they only
    // duplicated the combatant scan.)
    ScanExits(out);

    // Per-category breakdown (confirms the categorization: NPCs/Enemies stay out of Interactables).
    int cc[static_cast<int>(Category::Count)] = {};
    for (const auto& e : out) { int ci = static_cast<int>(e.category); if (ci >= 0 && ci < static_cast<int>(Category::Count)) ++cc[ci]; }
    char msg[176];
    snprintf(msg, sizeof(msg),
             "rescan: %zu field objects (NPC=%d Enemy=%d Object=%d Exit=%d Save=%d Gate=%d Treasure=%d)",
             out.size(), cc[(int)Category::NPC], cc[(int)Category::Enemy], cc[(int)Category::Object],
             cc[(int)Category::Exit], cc[(int)Category::SaveCrystal],
             cc[(int)Category::GateCrystal], cc[(int)Category::Treasure]);
    Log::Write("NAV", msg);
    return static_cast<int>(out.size());
}

// Bitmask of currently-active handle-table containers (bit c set iff container c's active
// flag is set). Memory-only + SEH-guarded; touches only the handle table, not out.
uint32_t ActiveContainerMask() {
    void* base = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!base) return 0;
    uint32_t mask = 0;
    for (uint32_t c = 0; c < NavRva::HANDLE_TABLE_CONTAINERS; ++c) {
        void* table = static_cast<char*>(base) + static_cast<size_t>(c) * NavRva::HANDLE_TABLE_STRIDE;
        uint8_t active = 0;
        if (SafeReadU8(table, NavRva::TBL_ACTIVE_OFF, &active) && (active & 1))
            mask |= (1u << c);
    }
    return mask;
}

int Build(std::vector<Entity>& out) { return BuildLocked(out); }

} // namespace EntityScan
