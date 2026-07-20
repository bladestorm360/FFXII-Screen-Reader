#include "navigation/entity_list.h"
#include "navigation/nav_rva.h"
#include "navigation/nav_common.h"
#include "navigation/player_state.h"
#include "navigation/map_query.h"
#include "navigation/map_script.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/game_text.h"
#include "core/logger.h"
#include "speech/speech.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>
#include <cstdio>

using namespace MemRead;

namespace EntityList {

namespace {

// A live interactive field object. Identity is the SCENE OBJECT pointer, which is stable
// while the map is loaded (the handle table holds it from load to teardown).
struct Entity {
    void*        sceneObj = nullptr;   // handle-table scene object (identity; nullptr for fixed exits)
    uint32_t     flags    = 0;         // *(sceneObj+0x1C): FLAG_TALK (NPC) / FLAG_ACTION
    int16_t      nameIdx  = 0;         // *(sceneObj+0x102): npcdic id (>=0) / custom (<0)
    Category     category = Category::Object;
    std::wstring label;
    FVec3        pos;
    float        dist2D   = 0.0f;      // to player, refreshed per command
    bool         fixed    = false;     // exit/map-jump: fixed world pos, no scene node (don't refresh via +0xB8)
    bool         noBearing = false;    // pos is NOT world-space (connection-DB exits carry map-atlas offsets)
                                       // -> speak the label only, never a fabricated direction
};

std::mutex             g_mutex;
std::vector<Entity>    g_entities;
Category               g_currentCategory = Category::All;

// The [ / ] focus, tracked across rescans by STABLE IDENTITY (not a bare scene-object pointer,
// which can be reused/aliased by a pooled combatant slot or momentarily drop from a rescan and
// silently re-anchor the cursor to the nearest object). Match tiers: exact (pointer + name-key +
// label) beats an identity re-lock (name-key + label + category, adopting the object's new pointer).
struct CursorId {
    void*        obj     = nullptr;
    int16_t      nameIdx = 0;
    std::wstring label;
    Category     cat     = Category::All;
    bool         valid   = false;
};
CursorId               g_cursor;

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

// (The current-area name is resolved via MapQuery::CurrentMapId + ResolveMapName/ResolveRegionName —
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

// Re-read live positions for the current set; drop objects whose transform no longer
// reads (despawned / mid-teardown). Keeps identity (scene-object ptr) stable so focus
// survives. Caller holds g_mutex.
void RefreshPositionsLocked(const FVec3& playerPos) {
    for (auto it = g_entities.begin(); it != g_entities.end();) {
        if (it->fixed) {   // exit/map-jump: fixed world pos, no scene node — keep pos, just re-range
            it->dist2D = NavCommon::Distance2D(playerPos, it->pos);
            ++it;
            continue;
        }
        FVec3 p;
        if (!PlayerState::ReadSceneObjectPos(it->sceneObj, p)) { it = g_entities.erase(it); continue; }
        it->pos = p;
        it->dist2D = NavCommon::Distance2D(playerPos, p);
        ++it;
    }
}

// Indices into g_entities matching the active category filter, nearest-first.
std::vector<size_t> FilteredSortedLocked() {
    std::vector<size_t> v;
    for (size_t i = 0; i < g_entities.size(); ++i)
        if (g_currentCategory == Category::All || g_entities[i].category == g_currentCategory)
            v.push_back(i);
    std::sort(v.begin(), v.end(), [](size_t a, size_t b) {
        return g_entities[a].dist2D < g_entities[b].dist2D;
    });
    return v;
}

bool ReadPlayer(FVec3& pos) {
    return PlayerState::ReadPlayerPos(pos);
}

// --- cursor identity (lock-by-identity across rescans) ---------------------

// 2 = exact (same object): pointer + name-key + label all agree — rejects a pointer that a pooled
// slot reused for a DIFFERENT unit. 1 = identity re-lock: the same logical object under a new
// pointer (name-key + label + category agree). 0 = no match.
int CursorMatch(const Entity& e) {
    if (!g_cursor.valid) return 0;
    if (e.sceneObj == g_cursor.obj && e.nameIdx == g_cursor.nameIdx && e.label == g_cursor.label)
        return 2;
    if (e.nameIdx == g_cursor.nameIdx && e.label == g_cursor.label && e.category == g_cursor.cat)
        return 1;
    return 0;
}

// Index WITHIN `view` of the focused object, preferring an exact match over an identity re-lock;
// -1 if the focus is genuinely gone. So the "fall back to nearest" path is taken only when the
// object truly departed, never on a transient rescan wobble or a pointer alias.
int FindFocusInViewLocked(const std::vector<size_t>& view) {
    int relock = -1;
    for (int i = 0; i < static_cast<int>(view.size()); ++i) {
        int m = CursorMatch(g_entities[view[i]]);
        if (m == 2) return i;
        if (m == 1 && relock < 0) relock = i;
    }
    return relock;
}

void SetFocusLocked(const Entity& e) {
    g_cursor.obj = e.sceneObj;
    g_cursor.nameIdx = e.nameIdx;
    g_cursor.label = e.label;
    g_cursor.cat = e.category;
    g_cursor.valid = true;
}

void ClearFocusLocked() { g_cursor = CursorId{}; }

void SpeakEntityLocked(const Entity& e, const FVec3& playerPos) {
    if (e.noBearing) { Speech::Output(e.label); return; }       // map-atlas pos: name only, no direction
    float facingRad = 0.0f;
    PlayerState::ReadCameraForward(facingRad);                  // "North" = forward = where UP takes you
    std::wstring phrase = e.label;
    phrase += L". ";
    phrase += NavCommon::DescribeDirectionRelative(playerPos, e.pos, facingRad);   // egocentric
    Speech::Output(phrase);
}

void SpeakNoTargets() { Speech::Output(L"No targets"); }

bool AlreadyListed(void* sceneObj) {
    for (const auto& e : g_entities) if (e.sceneObj == sceneObj) return true;
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
void ScanCombatantsLocked() {
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
        if (!sceneObj || AlreadyListed(sceneObj)) continue;

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
        g_entities.push_back(e);
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
constexpr float kExitMaxDist = 2000.0f;   // generous sanity bound (reject garbage positions)

void ScanExitsLocked() {
    // Exits come from the +0x54 map-jump table — the REAL, WALKABLE world positions, so the player can
    // route to one. The destination NAME now comes from the map's own field script (see below), giving
    // "Exit, <region>: <sub-area>" + live bearing/steps: enough to choose an exit by where it goes and
    // walk to it. Only slots owned by a `__MJ_CTRL` controller are listed; the rest are arrival points.
    //
    // (The connection DB was dropped as the exit source: it is the region's FLOOR LIST — all sub-areas of the
    //  region, most not reachable from this room — which is why it reported 4 where there are 2. It has names
    //  but only map-atlas coords, so it can never be walked to. See MapQuery::EnumerateMapConnections.)
    FVec3 p{}; const FVec3* pp = PlayerState::ReadPlayerPos(p) ? &p : nullptr;
    std::vector<MapQuery::ExitRec> jumps;
    MapQuery::EnumerateMapJumps(pp, kExitMaxDist, jumps, /*logRaw=*/false);

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
    const int  mapId     = MapQuery::CurrentMapId();
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
        g_entities.push_back(e);

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
int RescanLocked() {
    g_entities.clear();
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
            if (AlreadyListed(obj)) continue;

            Entity e;
            e.sceneObj = obj;
            e.flags = flags;
            e.nameIdx = nameIdx;
            e.pos = pos;
            e.label = name;   // resolved above (empty for a flagged/character object)
            e.category = ClassifyByNameKey(flags, e.nameIdx, isCharacter);
            if (e.label.empty()) e.label = ResolveObjectName(obj);         // flagged char/gimmick name
            if (e.label.empty()) e.label = CategoryWord(e.category);
            g_entities.push_back(e);
        }
    }

    // Combatants (allies + enemies) come from the BtlWork pool, not the handle table — the
    // handle-table filter drops them, so in a battle this is what makes them navigable.
    ScanCombatantsLocked();

    // Map exits — the map-jump points (+0x54), named from the field-sign array (+0x70) where possible.
    // Fixed-position, invisible to the interaction scanner. (Naviicon "markers" removed — they only
    // duplicated the combatant scan.)
    ScanExitsLocked();

    // Per-category breakdown (confirms the categorization: NPCs/Enemies stay out of Interactables).
    int cc[static_cast<int>(Category::Count)] = {};
    for (const auto& e : g_entities) { int ci = static_cast<int>(e.category); if (ci >= 0 && ci < static_cast<int>(Category::Count)) ++cc[ci]; }
    char msg[176];
    snprintf(msg, sizeof(msg),
             "rescan: %zu field objects (NPC=%d Enemy=%d Object=%d Exit=%d Save=%d Gate=%d Treasure=%d)",
             g_entities.size(), cc[(int)Category::NPC], cc[(int)Category::Enemy], cc[(int)Category::Object],
             cc[(int)Category::Exit], cc[(int)Category::SaveCrystal],
             cc[(int)Category::GateCrystal], cc[(int)Category::Treasure]);
    Log::Write("NAV", msg);
    return static_cast<int>(g_entities.size());
}

// Bitmask of currently-active handle-table containers (bit c set iff container c's active
// flag is set). Memory-only + SEH-guarded; touches only the handle table, not g_entities.
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

} // namespace

// --- public API ------------------------------------------------------------

bool Init() {
    Log::Write("NAV", "entity list ready");
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_entities.clear();
    ClearFocusLocked();
}

int Rescan() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return RescanLocked();
}

void OnFieldFrame() {
    // GAME THREAD, once per field frame. The handle-table containers stream in lazily —
    // the object containers register AFTER the field-active bit is set (notably on a
    // save-load, where the player is dropped past the map seam), and the scanner otherwise
    // only rebuilds on the `` ` `` key, so a loaded map's objects never get picked up. Here
    // we auto-rebuild the moment the active-container SET changes. The builder writes each
    // container's entries+count BEFORE flipping its active bit last, so one rescan on the
    // change captures the just-streamed objects. O(1) (a 5-byte mask read) while stable.
    static uint32_t s_lastMask = 0;
    uint32_t mask = ActiveContainerMask();
    if (mask == s_lastMask) return;          // container set unchanged — nothing to do
    s_lastMask = mask;

    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<Entity> prev;
    const bool hadObjects = !g_entities.empty();
    if (hadObjects) prev = g_entities;                 // snapshot a good set
    int n = RescanLocked();
    // Never replace a populated list with a transient empty while a container is still live
    // (honors the "don't let the edge check wipe the scan" caution); a real teardown (mask==0)
    // is allowed to clear it, and the next cycle command prunes any departed objects.
    if (n == 0 && hadObjects && mask != 0)
        g_entities = std::move(prev);

    // Announce the area just entered ("Entering <name>") — ONLY on a real field area, and only when the
    // area actually changed (edge-triggered on the name; re-entering the same-named area after a menu
    // won't re-announce).
    //
    // CONTEXT GATE: IsFieldActive() alone is NOT enough — its 0x10 bit is already set at boot (the log
    // caught this block running pre-title with "no sub-map index"). NavSafeFailMask bits 0/1/2 = field sim
    // live + field module STARTED (only 1 after the first field entry) + valid area id (not mid-transition);
    // all three clear == we are genuinely on a field area. We deliberately do NOT use the full
    // IsFieldNavSafe(), which also demands the Bullet world — the prologue never builds one, so that would
    // suppress the announcement entirely.
    constexpr uint8_t kFieldContextBits = 0x07;   // 0 field, 1 field-started, 2 areaId
    if (mask != 0 && (PlayerState::NavSafeFailMask() & kFieldContextBits) == 0) {
        static std::wstring s_lastArea;
        std::wstring area = CurrentAreaName();
        if (!area.empty() && area != s_lastArea) {
            s_lastArea = area;
            std::wstring phrase = L"Entering ";
            phrase += area;
            // The spoken text itself is logged by Speech (SPEAK-OUT). Log the map id + each half here,
            // since those are what the speech log can't show — and they are exactly what distinguishes a
            // region-only announcement from a full one when a name looks wrong.
            const int aid = MapQuery::CurrentMapId();
            const std::wstring sub = MapQuery::ResolveAreaName(aid);
            const std::wstring reg = MapQuery::ResolveRegionName(aid);
            char s8[64] = {}, r8[64] = {};
            for (size_t k = 0; k < sub.size() && k < 63; ++k) s8[k] = (sub[k] < 128) ? static_cast<char>(sub[k]) : '?';
            for (size_t k = 0; k < reg.size() && k < 63; ++k) r8[k] = (reg[k] < 128) ? static_cast<char>(reg[k]) : '?';
            char m[192];
            snprintf(m, sizeof(m), "announce: mapId=%d sub=\"%s\" region=\"%s\"", aid, s8, r8);
            Log::Write("NAV", m);
            Speech::Output(phrase);
        }

        // Per-area dump: the field-sign exits we now list (world pos + resolved destination), plus the
        // +0x54 map-jump world points for cross-reference. The +0x70 count is the number this session's
        // ABI fix unblocked — it read 0 on every map while the group index was being passed in junk.
        std::vector<MapQuery::ExitRec> fs;
        MapQuery::EnumerateFieldSignExits(fs, /*logRaw=*/true);                 // named exits (WORLD pos)
        FVec3 pep; const FVec3* ppp = PlayerState::ReadPlayerPos(pep) ? &pep : nullptr;
        if (ppp) {
            char m[96];
            snprintf(m, sizeof(m), "map-exits: player world=(%.1f,%.1f,%.1f)", pep.x, pep.y, pep.z);
            Log::Write("NAV-DIAG", m);
        }
        char fsm[64];
        snprintf(fsm, sizeof(fsm), "map-exits(+0x70) usable+named: %zu", fs.size());
        Log::Write("NAV-DIAG", fsm);
        std::vector<MapQuery::ExitRec> jp;
        MapQuery::EnumerateMapJumps(ppp, kExitMaxDist, jp, /*logRaw=*/true);    // +0x54 world points (diag)

        // The destination is a LITERAL in the loaded field script: scan for mapjump(dest,entrance,flags)
        // calls and log each with its resolved name + dispatch context. This is the real exit-dest source;
        // the log pins the source-door<->dest pairing for wiring.
        MapQuery::DiagScanScriptMapjumps();
    }
}

std::wstring CurrentAreaName() {
    // "<region>: <sub-area>" for the current map id, e.g. "Nalbina Fortress: Lower Apartments".
    const int id = MapQuery::CurrentMapId();
    if (id <= 0) return L"";
    return MapQuery::ResolveFullAreaName(id);
}

void CmdRescan() {
    int n = Rescan();
    std::wstring area = CurrentAreaName();   // lock-free; does not touch g_entities
    wchar_t buf[160];
    if (!area.empty())
        _snwprintf_s(buf, _TRUNCATE, L"%s. %d objects", area.c_str(), n);
    else
        _snwprintf_s(buf, _TRUNCATE, L"%d objects", n);
    Speech::Output(buf);
}

// Shared body for Next/Prev: refresh, build the nearest-first view, move focus.
static void CycleLocked(int dir, const FVec3& playerPos) {
    RefreshPositionsLocked(playerPos);
    std::vector<size_t> view = FilteredSortedLocked();
    if (view.empty()) { ClearFocusLocked(); SpeakNoTargets(); return; }

    // Find current focus within the view by stable identity (exact, else identity re-lock).
    int cur = FindFocusInViewLocked(view);
    const int nv = static_cast<int>(view.size());
    int next = (cur < 0) ? 0 : ((cur + dir) % nv + nv) % nv;
    const Entity& e = g_entities[view[next]];
    SetFocusLocked(e);
    SpeakEntityLocked(e, playerPos);
}

void CmdNext() {
    std::lock_guard<std::mutex> lk(g_mutex);
    RescanLocked();   // fresh — pick up objects that appeared since the last command
    FVec3 p;
    if (!ReadPlayer(p)) { Speech::Output(L"Position unavailable"); return; }
    CycleLocked(+1, p);
}

void CmdPrev() {
    std::lock_guard<std::mutex> lk(g_mutex);
    RescanLocked();
    FVec3 p;
    if (!ReadPlayer(p)) { Speech::Output(L"Position unavailable"); return; }
    CycleLocked(-1, p);
}

void CmdDescribeCurrent() {
    std::lock_guard<std::mutex> lk(g_mutex);
    RescanLocked();
    FVec3 p;
    if (!ReadPlayer(p)) { Speech::Output(L"Position unavailable"); return; }
    RefreshPositionsLocked(p);
    std::vector<size_t> view = FilteredSortedLocked();
    if (view.empty()) { SpeakNoTargets(); return; }
    // Speak the current focus (by stable identity), or the nearest if the focus is gone / unset.
    int fi = FindFocusInViewLocked(view);
    size_t sel = (fi >= 0) ? view[fi] : view[0];
    SetFocusLocked(g_entities[sel]);
    SpeakEntityLocked(g_entities[sel], p);

    // Obstacle-aware hint toward the selection (<=5 rays; safe on the input thread).
    // A full A* grid is a later enhancement (thousands of rays => needs game-thread
    // execution to avoid racing the physics step).
    if (MapQuery::HasWorld()) {
        float facingRad = 0.0f;
        PlayerState::ReadCameraForward(facingRad);   // egocentric "bear <cardinal>" hint (forward = UP)
        const FVec3 tgt = g_entities[sel].pos;
        const float bodyPad = 0.9f;                  // test at body height, not at the feet
        const FVec3 from{ p.x, p.y + bodyPad, p.z };
        if (MapQuery::SegmentClear(from, FVec3{ tgt.x, p.y + bodyPad, tgt.z })) {
            Speech::SpeakQueued(L"Path clear");
        } else {
            // Heading convention matches nav_common::BearingDeg: north = -Z, so a
            // heading `a` maps to world offset (sin a, -cos a) in (x, z).
            const float base = std::atan2(tgt.x - p.x, -(tgt.z - p.z));
            float dist = NavCommon::Distance2D(p, tgt);
            const float probe = dist < 5.0f ? dist : 5.0f;   // look ~5 m per heading
            const float offs[4] = { 0.785398f, -0.785398f, 1.570796f, -1.570796f };  // +/-45, +/-90
            bool found = false;
            for (float o : offs) {
                const float a = base + o;
                const FVec3 pt{ p.x + std::sin(a) * probe, p.y + bodyPad, p.z - std::cos(a) * probe };
                if (MapQuery::SegmentClear(from, pt)) {
                    std::wstring s = L"Blocked, bear ";
                    s += NavCommon::CardinalOfHeadingRelative(a, facingRad);
                    Speech::SpeakQueued(s);
                    found = true;
                    break;
                }
            }
            if (!found) Speech::SpeakQueued(L"Blocked");
        }
    }
}

static void ChangeCategoryLocked(int dir) {
    int c = static_cast<int>(g_currentCategory);
    int n = static_cast<int>(Category::Count);
    c = ((c + dir) % n + n) % n;
    g_currentCategory = static_cast<Category>(c);
    // Rescan live actors BEFORE counting — the other commands (Next/Prev/Describe)
    // rescan, but this one used to count over the previous scan's stale set, so a
    // category whose actors weren't in that scan spoke a stale "0" even though the
    // object exists and appears once the user cycles. Rescanning makes the count live.
    RescanLocked();
    // Count matches + speak category name.
    size_t matches = 0;
    for (auto& e : g_entities)
        if (g_currentCategory == Category::All || e.category == g_currentCategory) ++matches;
    wchar_t buf[96];
    _snwprintf_s(buf, _TRUNCATE, L"%s, %zu", CategoryWord(g_currentCategory), matches);
    Speech::Output(buf);
    ClearFocusLocked();   // re-anchor to nearest on next cycle
}

void CmdNextCategory() { std::lock_guard<std::mutex> lk(g_mutex); ChangeCategoryLocked(+1); }
void CmdPrevCategory() { std::lock_guard<std::mutex> lk(g_mutex); ChangeCategoryLocked(-1); }

bool GetCurrentTarget(FVec3& outPos, std::wstring& outLabel) {
    if (!PlayerState::IsFieldActive()) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    RescanLocked();
    if (g_entities.empty()) return false;
    FVec3 p;
    if (PlayerState::ReadPlayerPos(p)) RefreshPositionsLocked(p);
    // Prefer the focused object (by stable identity: exact, else re-lock); else the nearest in
    // the active filter. Read-only query (drives `\`) — does not mutate the cursor.
    int relock = -1;
    for (size_t i = 0; i < g_entities.size(); ++i) {
        int m = CursorMatch(g_entities[i]);
        if (m == 2) { outPos = g_entities[i].pos; outLabel = g_entities[i].label; return true; }
        if (m == 1 && relock < 0) relock = static_cast<int>(i);
    }
    if (relock >= 0) { outPos = g_entities[relock].pos; outLabel = g_entities[relock].label; return true; }
    std::vector<size_t> view = FilteredSortedLocked();
    if (view.empty()) return false;
    outPos = g_entities[view[0]].pos;
    outLabel = g_entities[view[0]].label;
    return true;
}


// Dump the raw handle table: every live scene object in every container, with its
// interaction flags, npcdic name key, resolved name, and world position. This is the
// data that proves where a given interactive object (e.g. the tutorial gate) actually
// lives and how it should classify. File-only (never spoken); bounded by the per-
// container count clamp. Interactive (talk/action) objects get a full line; the rest
// are only counted.
void LogDiagnostic() {
    std::lock_guard<std::mutex> lk(g_mutex);
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
                std::wstring lbl = ResolveObjectName(obj);
                // Skip the mass of anonymous, flagless props/triggers; dump anything
                // interactive, named, or in the gimmick band (where the gate must fall).
                if (flags == 0 && lbl.empty() && !InGimmickBand(nameIdx)) { ++plain; continue; }
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
        std::vector<MapQuery::ExitRec> fs;
        MapQuery::EnumerateFieldSignExits(fs, /*logRaw=*/true);
        char es[72];
        snprintf(es, sizeof(es), "  named exits: %zu", fs.size());
        Log::Write("NAV-DIAG", es);
    }
    Log::Write("NAV-DIAG", "==== map-jump points (+0x54) ====");
    {
        std::vector<MapQuery::ExitRec> jumps;
        const FVec3* pep = haveP ? &pp : nullptr;
        MapQuery::EnumerateMapJumps(pep, kExitMaxDist, jumps, /*logRaw=*/true);
        char es[72];
        snprintf(es, sizeof(es), "  map-jumps accepted (sanity-gated): %zu", jumps.size());
        Log::Write("NAV-DIAG", es);
    }
    // The region's floor list — logged for reference only. NOT exits: these are the sub-areas of the
    // current REGION as the map screen stacks them (atlas coords, includes the area you're standing in).
    Log::Write("NAV-DIAG", "==== region sub-area list (map-screen DB — NOT exits) ====");
    {
        std::vector<MapQuery::ExitRec> conns;
        MapQuery::EnumerateMapConnections(conns, /*logRaw=*/true);
        char es[72];
        snprintf(es, sizeof(es), "  region sub-areas: %zu", conns.size());
        Log::Write("NAV-DIAG", es);
    }
    Log::Write("NAV-DIAG", "==== end handle-table diag ====");
}

} // namespace EntityList
