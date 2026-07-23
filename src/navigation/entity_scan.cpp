#include "navigation/entity_scan.h"
#include "navigation/entity_labels.h"
#include "navigation/entity_classify.h"
#include "navigation/nav_rva.h"
#include "navigation/map_rva.h"
#include "navigation/nav_common.h"
#include "navigation/player_state.h"
#include "navigation/map_query.h"
#include "navigation/map_names.h"
#include "navigation/map_exits.h"
#include "navigation/map_script.h"
#include "navigation/exit_scan.h"
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
// The per-object judgement layer -- CategoryWord / ResolveObjectName / InGimmickBand /
// ClassifyByNameKey / IsInteractionAvailable -- lives in entity_classify.{h,cpp}. This file finds
// objects; that one decides what each one is and what it is called.

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


// NOTE: the naviicon minimap "markers" were REMOVED (Session 44). Two decompile traces proved they are
// only character/unit dots (party/allies/enemies) that duplicate the combatant scan — no objective/crystal
// source, and a per-frame render buffer. Nothing to surface; deleted.
//
// NOTE: ScanSpawnTriggersLocked / Category::Event are GONE. The mapData+0x54 table it read is the
// map-jump exit table, not spawn/arrival points — it is now merged into ScanExitsLocked above.

// The scene object's own handle (u16 at +0x00 — the value the engine stores as the "nearest
// interactable" id in FUN_0025bad0). Stable for as long as the map is loaded, so it is a sound
// tie-break for numbering. 0xFFFF for a fixed exit, which has no scene object.
uint16_t ObjectHandle(void* sceneObj) {
    uint16_t h = 0xFFFF;
    if (sceneObj) SafeReadU16(sceneObj, 0, &h);
    return h;
}

// One line per handle-table object, once per map (see the detail latch in BuildLocked). This is the dump
// that answers "what IS that thing" for an object the game gives no name to: the container slot says
// whether the engine even considers it interactable, `act`/`talk` are the payloads it would run, and the
// nearest named neighbour usually identifies it outright -- North End's anonymous `Interactables 2` sits
// at the same distance and elevation as The Clan Hall, which is what marked it as that doorway's twin.
void LogObjectDump(const std::vector<Entity>& out) {
    int shown = 0;
    for (const auto& e : out) {
        if (!e.sceneObj) continue;
        // Nearest OTHER object the game did name, so an anonymous entry is placed against a landmark.
        // (Not `near` -- windows.h still defines that as a legacy empty macro.)
        const Entity* landmark = nullptr;
        float nearD = 1e9f;
        for (const auto& o : out) {
            if (&o == &e || !o.gameNamed) continue;
            const float d = NavCommon::Distance2D(o.pos, e.pos);
            if (d < nearD) { nearD = d; landmark = &o; }
        }
        char lbl[48] = {};
        for (size_t k = 0; k < e.label.size() && k < 47; ++k)
            lbl[k] = (e.label[k] < 128) ? static_cast<char>(e.label[k]) : '?';
        char nb[64] = "-";
        if (landmark) {
            char n8[40] = {};
            for (size_t k = 0; k < landmark->label.size() && k < 39; ++k)
                n8[k] = (landmark->label[k] < 128) ? static_cast<char>(landmark->label[k]) : '?';
            snprintf(nb, sizeof(nb), "\"%s\" %.1fm", n8, nearD);
        }
        char m[288];
        snprintf(m, sizeof(m),
                 "obj [%u:%u] kind=%u flags=%08X nameIdx=%d act=%u talk=%u door=%d named=%d avail=%d cat=%d pos=(%.2f,%.2f,%.2f) \"%s\" near=%s",
                 e.container, e.slot, e.kind, e.flags, e.nameIdx, e.actionId, e.talkId,
                 e.doorway ? 1 : 0, e.gameNamed ? 1 : 0, e.available ? 1 : 0,
                 static_cast<int>(e.category), e.pos.x, e.pos.y, e.pos.z, lbl, nb);
        Log::Write("NAV-DIAG", m);
        if (++shown >= 128) { Log::Write("NAV-DIAG", "obj dump: capped at 128"); break; }
    }
}

// Mark every object the map script bound to a location jump, then drop the same-named twins that were
// NOT bound to one.
//
// WHAT THE TWINS ACTUALLY ARE (East End, `'` dump 2026-07-23): each shop appears twice, and the two
// objects are indistinguishable by every field the engine exposes -- same scene category (0x21), same
// kind (4), same enable bit, same flags (0x2134, ACTION + the on-screen name-draw bit), and BOTH sit
// inside the container's action span [19,35), the exact range FUN_0025b820 walks to decide what the
// player is near. They are 6-15 m apart, so no proximity radius separates them either. Both are genuine
// field signs: `nameIdx = -1` means the name comes from `sceneObj+0xf8`, the string the map script writes
// with `fieldsignmes` / `fieldsignmesbyid`.
//
// The one thing that DOES separate them is `setfieldsignlocationjumpinfo` -- the script native that binds
// a sign to a map transition. A bound sign has a `+0x70` record; an unbound one does not. Every doorway
// matched a record inside ~2 m; not one of its twins matched at all.
//
// So the twin is a sign that only repeats the doorway's name, and it earns no place in the list. A sign
// carrying ANY other text (a slogan, a notice) has a different label and survives untouched -- the test
// is exact string equality on the game's own words, so nothing is invented and nothing is paraphrased.
// This is collection dedup, the same category as AlreadyListed; it never suppresses a repeat announcement.
void TagDoorwaysAndDropSignTwins(std::vector<Entity>& out, bool logDetail) {
    const std::vector<MapExits::SignRec>& signs = CachedSigns();
    if (signs.empty()) return;                 // no field-sign table on this map -> nothing to judge with

    for (auto& e : out) {
        if (!e.sceneObj) continue;             // fixed exits have no scene node
        for (const auto& s : signs) {
            if (NavCommon::Distance2D(s.pos, e.pos) <= kSignObjectDist) { e.doorway = true; break; }
        }
    }

    for (size_t i = 0; i < out.size();) {
        const Entity& cur = out[i];
        if (cur.doorway || !cur.gameNamed || !cur.sceneObj) { ++i; continue; }
        int twin = -1;
        for (size_t j = 0; j < out.size(); ++j) {
            if (j == i) continue;
            if (out[j].doorway && out[j].gameNamed && out[j].label == cur.label) {
                twin = static_cast<int>(j);
                break;
            }
        }
        if (twin < 0) { ++i; continue; }       // no doorway shares this name -> a sign in its own right
        if (logDetail) {
            char n8[64] = {};
            for (size_t k = 0; k < cur.label.size() && k < 63; ++k)
                n8[k] = (cur.label[k] < 128) ? static_cast<char>(cur.label[k]) : '?';
            char m[256];
            snprintf(m, sizeof(m),
                     "sign-twin dropped \"%s\": [%u:%u] (%.2f,%.2f,%.2f) repeats doorway [%u:%u] (%.2f,%.2f,%.2f) %.1fm away",
                     n8, cur.container, cur.slot, cur.pos.x, cur.pos.y, cur.pos.z,
                     out[twin].container, out[twin].slot,
                     out[twin].pos.x, out[twin].pos.y, out[twin].pos.z,
                     NavCommon::Distance2D(cur.pos, out[twin].pos));
            Log::Write("NAV-DIAG", m);
        }
        out.erase(out.begin() + static_cast<long long>(i));
    }
}

// Append " 1", " 2", ... to labels that occur more than once, so fifteen identically-named townsfolk
// become addressable. Labels that occur once are untouched: nothing gains a number it does not need.
//
// THE NUMBER IS ASSIGNED ONCE AND KEPT (Session 65). It used to be an ordinal within whatever group the
// current scan happened to see, which meant it moved constantly: the handle table streams objects in
// and out (measured NPC=14 <-> 15 across 118 rescans on one map) and every cycle keypress rebuilds the
// list, so one flickering NPC renumbered everyone after it. The tester's "Rabanastran 7" kept becoming
// "Rabanastran 5". EntityLabels now hands out the lowest number not yet used under that name on that
// map and remembers it -- across rescans, streaming, reloads and sessions.
//
// This is NOT a fabricated label (see the no-invented-UI-text rule): the words are still the game's
// own string; only a counting suffix is added, in the same spirit as the step counts already spoken.
// The player's own name for an entity outranks everything the mod would otherwise say -- the game's
// string, the duplicate number, the category word. Applied after all of those are settled and BEFORE
// duplicate numbering, so a labelled entity leaves its old counting group entirely: name the gate guard
// and the remaining Rabanastrans keep the numbers they already had.
void ApplyPlayerLabels(std::vector<Entity>& out) {
    const int mapId = MapNames::CurrentMapId();
    for (auto& e : out) {
        if (!e.sceneObj) continue;                      // fixed exits are named from the map script
        std::wstring custom = EntityLabels::LabelFor(mapId, e.container, e.slot, e.nameIdx);
        if (custom.empty()) continue;
        e.label     = custom;
        e.gameNamed = true;   // it is a real name for list purposes: never a category-word fallback
    }
}

void NumberDuplicateLabels(std::vector<Entity>& out, bool logDetail) {
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i].label.empty()) continue;
        std::vector<size_t> same;
        for (size_t j = i; j < out.size(); ++j)
            if (out[j].label == out[i].label) same.push_back(j);
        if (same.size() < 2) continue;   // unique label -> speak it as the game wrote it
        std::sort(same.begin(), same.end(), [&out](size_t a, size_t b) {
            if (out[a].nameIdx != out[b].nameIdx) return out[a].nameIdx < out[b].nameIdx;
            return ObjectHandle(out[a].sceneObj) < ObjectHandle(out[b].sceneObj);
        });
        // OPEN BUG (reported in play): every shop appears twice. Numbering makes the twins
        // addressable but does NOT explain them, so dump each duplicate group ONCE PER MAP with the
        // fields that tell twins apart from genuinely distinct objects: pointer, slot, kind, scene
        // category, flags, position. Two entries at the SAME position with different pointers are a
        // shadow/duplicate registration; two at different positions are two real objects that share
        // the game's own name (which is normal -- 109 npcdic ids all read "Rabanastran").
        // Cross-reference the slot against the container's grp0/grp1 spans logged by the ` dump.
        if (logDetail) {
            char m[128];
            char n8[64] = {};
            for (size_t k = 0; k < out[i].label.size() && k < 63; ++k)
                n8[k] = (out[i].label[k] < 128) ? static_cast<char>(out[i].label[k]) : '?';
            snprintf(m, sizeof(m), "dup-label \"%s\" x%zu:", n8, same.size());
            Log::Write("NAV-DIAG", m);
            for (size_t idx : same) {
                const Entity& e = out[idx];
                char l[208];
                snprintf(l, sizeof(l),
                         "    obj=%p handle=%u kind=%u nameIdx=%d flags=%08X avail=%d pos=(%.2f,%.2f,%.2f)",
                         e.sceneObj, ObjectHandle(e.sceneObj), e.kind, e.nameIdx, e.flags,
                         e.available ? 1 : 0, e.pos.x, e.pos.y, e.pos.z);
                Log::Write("NAV-DIAG", l);
            }
        }
        const int mapId = MapNames::CurrentMapId();
        for (size_t idx : same) {
            Entity& e = out[idx];
            const int n = EntityLabels::NumberFor(mapId, e.container, e.slot, e.nameIdx, e.label);
            e.label += L" " + std::to_wstring(n);
        }
    }
}

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
            // The engine's object KIND (sceneObj+0x0E & 0xF): 5 = ACTION gimmick, 1 = TALK person.
            uint8_t kindByte = 0;
            SafeReadU8(obj, NavRva::SCENEOBJ_KIND_OFF, &kindByte);
            const uint8_t kind = kindByte & NavRva::KIND_MASK;
            // Include: interactive objects (talk/action), EVERY action gimmick whether or not it is
            // currently offering a prompt, any named gimmick, AND — only for NON-character objects —
            // anything with a resolvable name (gates/doors/field-sign path-markers, cat 1-4).
            // Character objects (NPCs/party/enemies, cat 5-7) are deliberately NOT surfaced by the name
            // widening: unflagged ones are left to the combatant scan (ally/Enemy/dead) or the talk-flag
            // path, so defeated enemies and non-talk NPCs don't fall into the "Interactables" bucket.
            //
            // `kind == 5` is an inclusion reason in its OWN right because +0x1C is mode state: the
            // engine clears both prompt bits on a disabled object (FUN_0025ad10), so a story-gated
            // town gate had no flags, and unless it happened to carry a name it vanished from the
            // list entirely — exactly when the player most needs to find it (to find whoever gates it).
            const bool interactive = (flags & (NavRva::FLAG_TALK | NavRva::FLAG_ACTION)) != 0;
            // NON-CHARACTER kind-5 only. Characters are excluded for the same reason they dominate in
            // ClassifyByNameKey: an earlier version widened on kind alone and swept NPCs into
            // Interactables. Characters already reach the list via the flags or the combatant scan.
            const bool gimmick     = (kind == NavRva::KIND_ACTION_GIMMICK) && !isCharacter;
            std::wstring name;
            if (nameIdx != 0 && !isCharacter) name = ResolveObjectName(obj);
            const bool named = !name.empty();
            if (!interactive && !gimmick && !InGimmickBand(nameIdx) && !named) continue;

            FVec3 pos;
            if (!PlayerState::ReadSceneObjectPos(obj, pos)) continue;
            // UNPLACED RESERVE SLOT -- drop it. The map allocates object slots at load and positions
            // them later; one that is still at the world ORIGIN was never placed. They are unnamed,
            // so they fell through to the category word and were announced as "NPC" (then "NPC 1",
            // "NPC 2" … once duplicate labels got numbered), and being off the walkable map there is
            // no route to any of them -- exactly as reported in play. East End alone listed 37, which
            // is where that map's whole "NPC=37, Object=0" count came from: not one was a real NPC.
            // ScanCombatants has had this same guard from the start ("unplaced reserve unit"); the
            // handle-table walk simply never got one.
            if (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f) continue;
            if (AlreadyListed(out, obj)) continue;

            Entity e;
            e.sceneObj = obj;
            e.flags = flags;
            e.nameIdx = nameIdx;
            e.kind = kind;
            e.pos = pos;
            e.container = static_cast<uint8_t>(c);
            e.slot = static_cast<uint16_t>(i);
            SafeReadU16(obj, NavRva::SCENEOBJ_ACTION_ID, &e.actionId);
            SafeReadU16(obj, NavRva::SCENEOBJ_TALK_ID, &e.talkId);
            e.label = name;   // resolved above (empty for a flagged/character object)
            e.category = ClassifyByNameKey(flags, e.nameIdx, isCharacter, kind);
            e.available = IsInteractionAvailable(obj, kind, flags);
            if (e.label.empty()) e.label = ResolveObjectName(obj);         // flagged char/gimmick name
            // `gameNamed` records whether the words came from the GAME or from our category fallback.
            // The sign-twin drop keys on it, so two anonymous objects that both fell back to the word
            // "Interactables" can never be mistaken for a duplicate pair.
            e.gameNamed = !e.label.empty();
            // An unnamed object that carries a `+0x70` field-sign record IS a sign: the map script bound
            // it with `setfieldsignlocationjumpinfo`, which is what `doorway` records. Shop doorways
            // carry the same record but resolve a real name, so they are untouched. The tester
            // authorised this word explicitly after finding one on North End that the game itself shows
            // as "???" / "(You're not sure what this sign is for.)".
            if (e.label.empty() && e.doorway) e.label = L"Sign";
            if (e.label.empty()) e.label = CategoryWord(e.category);
            out.push_back(e);
        }
    }

    // DETAIL LATCH. This used to hang off the map id alone, which meant it fired at the moment the map
    // CHANGED -- before the object containers had streamed in. The log therefore recorded East End's six
    // exits and never its 34 objects, and the duplicate-shop dump that was supposed to explain the twins
    // ran on a list that did not contain them. Re-arming whenever the population reaches a new high for
    // this map fires it once the containers are actually populated, and at most a handful of times.
    static int    s_detailMap   = -1;
    static size_t s_detailPeak  = 0;
    const int     mapNow        = MapNames::CurrentMapId();
    if (mapNow != s_detailMap) { s_detailMap = mapNow; s_detailPeak = 0; }
    const bool detail = out.size() > s_detailPeak;
    if (detail) s_detailPeak = out.size();

    // Doorway tagging + sign-twin removal, while the list is still just handle-table objects.
    TagDoorwaysAndDropSignTwins(out, detail);
    if (detail) LogObjectDump(out);

    // Combatants (allies + enemies) come from the BtlWork pool, not the handle table — the
    // handle-table filter drops them, so in a battle this is what makes them navigable.
    ScanCombatants(out);

    // Map exits — the map-jump points (+0x54), named from the field-sign array (+0x70) where possible.
    // Fixed-position, invisible to the interaction scanner. (Naviicon "markers" removed — they only
    // duplicated the combatant scan.)
    ScanExits(out);

    // Same-label disambiguation. The game itself gives 109 different npcdic ids the display name
    // "Rabanastran" (1141 ids, 554 distinct names), and there is no second name to fall back on --
    // FUN_00263990's odd npcdic slot is byte-identical to the even one in the US build. So a list of
    // twelve "Rabanastran"s is the game's own text, and the only honest fix is to NUMBER them rather
    // than invent descriptions. Labels that occur once are left alone.
    //
    // This also numbers the exits whose destination did not resolve: several bare "Exit" entries become
    // "Exit 1", "Exit 2", which keeps them separable without inventing a destination for any of them.
    //
    // The player's own labels are applied FIRST, so a named entity leaves its counting group entirely
    // and the rest keep the numbers they already had.
    ApplyPlayerLabels(out);
    NumberDuplicateLabels(out, detail);

    // Per-category breakdown (confirms the categorization: NPCs/Enemies stay out of Interactables).
    int cc[static_cast<int>(Category::Count)] = {};
    int gated = 0;
    for (const auto& e : out) {
        int ci = static_cast<int>(e.category);
        if (ci >= 0 && ci < static_cast<int>(Category::Count)) ++cc[ci];
        if (!e.available) ++gated;
    }
    char msg[208];
    snprintf(msg, sizeof(msg),
             "rescan: %zu field objects (NPC=%d Enemy=%d Object=%d Exit=%d Save=%d Gate=%d Treasure=%d) story-gated=%d",
             out.size(), cc[(int)Category::NPC], cc[(int)Category::Enemy], cc[(int)Category::Object],
             cc[(int)Category::Exit], cc[(int)Category::SaveCrystal],
             cc[(int)Category::GateCrystal], cc[(int)Category::Treasure], gated);
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
