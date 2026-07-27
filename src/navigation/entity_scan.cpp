#include "navigation/entity_scan.h"
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
#include "navigation/item_scan.h"
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

// Instrumentation for two changes shipped on documented INFERENCE rather than observed evidence.
// Log-only tallies; they exist so the next play log says whether each change does anything, instead
// of leaving a no-op looking like a fix.
namespace {
int s_charByName = 0;   // characters admitted ONLY because they resolve a name (no interaction flags)
int s_poolKind5  = 0;   // actor-pool entries skipped as KIND_DEAD, a constant documented as misnamed
int s_newKind1   = 0;   // admitted ONLY by the KIND+model route, kind 1 (person)
int s_newKind5   = 0;   // admitted ONLY by the KIND+model route, kind 5 (gimmick/story NPC)
int s_newChar    = 0;   // of those, how many are CHARACTERS -- the class that used to be rejected
int s_knownName  = 0;   // objects speaking the PERSONAL npcdic name because the player knows it
                        // (odd slot; e.g. "Arjie" rather than "Nomad"). Shipped WITH the change that
                        // introduced it -- Session 79's lesson was that a change which cannot be sized
                        // offline must carry its own counter, not an assurance.
int s_unplaced   = 0;   // unnamed characters dropped for floating above the floor
int s_unreach    = 0;   // unnamed characters dropped for being outside the reachable component

// Scene objects this scan deliberately erased. Published so the caller's grace window cannot carry
// them straight back in -- see the note on NoteFiltered in entity_scan.h.
std::vector<void*> s_filtered;
}

void NoteFiltered(void* sceneObj) { if (sceneObj) s_filtered.push_back(sceneObj); }

void NoteUnplacedDrop(bool floating) { if (floating) ++s_unplaced; else ++s_unreach; }

bool WasFilteredThisScan(void* sceneObj) {
    if (!sceneObj) return false;
    for (void* p : s_filtered) if (p == sceneObj) return true;
    return false;
}
// --- helpers ---------------------------------------------------------------
// The per-object judgement layer -- CategoryWord / ResolveObjectName / InGimmickBand /
// ClassifyByNameKey / IsInteractionAvailable -- lives in entity_classify.{h,cpp}. This file finds
// objects; that one decides what each one is and what it is called.

bool AlreadyListed(const std::vector<Entity>& out, void* sceneObj) {
    for (const auto& e : out) if (e.sceneObj == sceneObj) return true;
    return false;
}

// "IS THERE A BODY STANDING THERE?" -- the engine's model-loaded bit, and the only honest test for
// existence we can read. It is deliberately NOT the +0x1C prompt flags: those are MODE state that
// FUN_0025ad10 clears on a disabled object, so they answer "may I act on this right now" (which
// `available` carries) and say nothing about whether the thing exists. Same bit
// IsInteractionAvailable already reads, used here for the separate question.
bool HasModel(void* sceneObj) {
    uint8_t ready = 0;
    return SafeReadU8(sceneObj, NavRva::SCENEOBJ_READY_OFF, &ready) &&
           (ready & NavRva::READY_MODEL_BIT) != 0;
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
        // COUNTED, NOT FLIPPED. `KIND_DEAD = 5` is a constant phyre_types.h itself labels "NAME IS
        // WRONG", and debug.md records that on the field kind 5 = NPC -- so this line may be deleting
        // NPCs out of the actor pool. Left in place because the combat track owns the constant and the
        // pool is documented to hold no gimmicks (making it a no-op today). If this tally is ever
        // non-zero on a field map, that premise is false and the skip has to go.
        if (kind == NavRva::KIND_DEAD) { ++s_poolKind5; continue; }

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
int BuildLocked(std::vector<Entity>& out, bool* outDetail) {
    out.clear();
    s_charByName = 0;
    s_poolKind5  = 0;
    s_newKind1   = 0;
    s_newKind5   = 0;
    s_newChar    = 0;
    s_knownName  = 0;
    s_unplaced   = 0;
    s_unreach    = 0;
    s_filtered.clear();
    ResetNameStats();
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
            // Include an object if ANY of these holds: it is offering a prompt (`interactive`); it is
            // a non-character action gimmick (`gimmick`); it has an interaction KIND and a loaded
            // model (`present`); it sits in the treasure/crystal npcdic band; or it resolves a name.
            //
            // STRUCK (Session 79) — "Character objects (cat 5-7) are deliberately NOT surfaced by the
            // widening ... unflagged ones are left to the combatant scan or the talk-flag path". That
            // was this comment's own claim, and it was wrong: the combatant scan reads the BtlWork
            // pool, which holds no field NPCs, and the talk-flag path needs a flag the engine clears
            // on any object the script has not armed. A character with neither was reachable by NO
            // path at all and vanished on every map. See `present` below for the object that proved
            // it. The fear behind the old rule -- defeated enemies falling into "Interactables" -- is
            // handled where it belongs, in ClassifyByNameKey, whose `isCharacter` test dominates and
            // sends every character to the NPC bucket.
            //
            // `kind == 5` is an inclusion reason in its OWN right because +0x1C is mode state: the
            // engine clears both prompt bits on a disabled object (FUN_0025ad10), so a story-gated
            // town gate had no flags, and unless it happened to carry a name it vanished from the
            // list entirely — exactly when the player most needs to find it (to find whoever gates it).
            const bool interactive = (flags & (NavRva::FLAG_TALK | NavRva::FLAG_ACTION)) != 0;
            // NON-CHARACTER kind-5, with no model requirement. Kept EXACTLY as it was so this pass
            // stays purely additive: a trigger-volume gimmick that carries no model must not start
            // falling out of the list as a side effect of the `present` route below.
            const bool gimmick     = (kind == NavRva::KIND_ACTION_GIMMICK) && !isCharacter;
            // INCLUDE BY KIND — THE GLOBAL FIX (Session 79).
            //
            // The clause that used to decide this was `gimmick`'s `&& !isCharacter`, and it is not map
            // data: it rejected an entire class of NPC on EVERY map in the game. Nomad Village is only
            // where a tester needed one of them. The object that exposed it:
            //
            //   [0:55] cat=66 kind=5 en=1 r14=70 flags=00030000 nameIdx=-1 "" pos=(46.00,0.00,57.70)
            //
            // — a woman standing 1.28 m behind the Nomad Elder's tent, exactly where the elder's own
            // dialogue sends the player, confirmed present by sighted assistance. cat 0x66 masks to 6,
            // so she is class 3 (a CHARACTER); en=1 and r14=0x70 mean enabled with a model loaded. She
            // failed all four inclusion routes at once, and the decisive one was being a character.
            //
            // debug.md already stated the rule -- "a disabled object has zero flags ... Include by
            // KIND; use the flags only for 'what can I do with it right now'". That lesson was applied
            // to gimmicks in Session 54 and to NAMED characters in Session 77; it was never applied to
            // people without names, which is her and everyone like her.
            //
            // BOTH interaction kinds, deliberately. She is kind 5, but kind 1 is the same defect
            // wearing the other kind -- a story NPC whose talk hook the script has not yet armed is
            // exactly as invisible. Fixing only kind 5 would repair this map and leave the kind-1 case
            // to be rediscovered on some later one.
            //
            // STRUCK (Session 83) -- the `&& storyOn` story gate this route carried for one session.
            //
            // It required `+0x0E & 0x10`, the engine's own interactivity switch, on the theory that the
            // bare "NPC n" entries were bodies the script had not switched on. It shipped with the
            // counter that would falsify it, and the counter came back **zero**: every one of those
            // objects reads `en=1`. The theory was wrong and the clause is gone.
            //
            // What replaced it is not another guess about what these objects ARE. It is
            // DropUnplacedCharacters, which asks where they are -- on the floor, and reachable -- and
            // that is a question the walkmap can answer. See entity_postscan.cpp.
            const bool present     = (kind == NavRva::KIND_TALK_TARGET ||
                                      kind == NavRva::KIND_ACTION_GIMMICK) && HasModel(obj);
            // A CHARACTER'S NAME IS NOW AN INCLUSION REASON TOO (Session 77).
            //
            // Until now `name` was resolved only for non-characters, so the ONLY way a person could
            // enter the list was non-zero `+0x1C` flags -- and this project's own record says that
            // word is MODE STATE that reads zero on a disabled object (debug.md: "a disabled object
            // has zero flags ... therefore dropped story-gated gates entirely ... Include by KIND;
            // use the flags only for 'what can I do with it right now'"). That lesson was applied to
            // gimmicks in Session 54 and never to people, so a story NPC whose talk hook the script
            // has not yet armed was invisible by construction.
            //
            // A named character is a real person standing in the world whether or not you may talk to
            // them yet; `available` already carries the "can I act on it" half.
            // ONE resolve per object. This used to run under `if (nameIdx != 0)` and then again
            // unconditionally further down, which was pure repetition for anything that resolved
            // empty -- and it now costs a codec decode pair, so the second call is gone.
            std::wstring name = ResolveObjectName(obj);
            const bool named = !name.empty();
            // How many of the names spoken are the PERSONAL one, i.e. a character the player has been
            // introduced to. Session 81 briefly made this decide nothing by preferring the odd slot
            // unconditionally; that was struck the same day (see NpcdicDisplayName), so it is once
            // again both the selector and the measurement. `OddSlotWins()` should now track it.
            if (named && nameIdx > 0 &&
                TalkNameKnown(static_cast<int>(static_cast<uint32_t>(nameIdx) &
                                               NavRva::NPCDIC_NAME_MASK))) ++s_knownName;
            // STRUCK (Session 82) -- the "party / roster body" exclusion that used to sit here.
            //
            // It dropped unnamed scene-category-5 objects on the theory that they were the player's
            // party. The tester refuted it in one line: *"these are not party members, I have no other
            // party members currently."* The falsification dump it shipped with says the same thing
            // from the other side -- every object it removed read `pos=(0.00,0.00,0.00)`, i.e. the
            // WORLD ORIGIN, which the unplaced-reserve guard further down already drops. The
            // exclusion was simultaneously built on a wrong premise and doing nothing.
            //
            // LESSON: the three bodies that inspired it sat at ONE shared position 6 m above the
            // floor. That is "unplaced", which is a fact about their position, and I read it as
            // "party", which is a claim about their role. The dump was added to catch exactly this
            // and it did -- on the first map that was not the one the theory came from.
            // Counted, not assumed: this widening was shipped on a documented inference, not on
            // evidence that it fires. The tally says whether it actually admits anything, and how
            // much, before anyone treats it as the fix.
            if (isCharacter && named && !interactive) ++s_charByName;
            // BLAST RADIUS IS MEASURED, NOT ASSUMED. This widening could not be sized offline: the
            // object dump is Session 77 code and every archived log predates it, so Nomad Village's is
            // the only dump that exists. Rather than guess what a crowded city map costs, count what
            // the KIND route admits that nothing else would have -- split by kind, because kind 1 is
            // the half that could sweep in townspeople and it is the half that comes out if it does.
            const bool droppedBefore =
                !interactive && !gimmick && !InGimmickBand(nameIdx) && !named;
            if (droppedBefore && present) {
                if (kind == NavRva::KIND_TALK_TARGET) ++s_newKind1; else ++s_newKind5;
                if (isCharacter) ++s_newChar;
            }
            if (droppedBefore && !present) continue;

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
            e.label = name;   // resolved ONCE above, for every object
            e.category = ClassifyByNameKey(flags, e.nameIdx, isCharacter, kind);
            e.available = IsInteractionAvailable(obj, kind, flags);
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

    // Second registrations of somebody already listed, removed BEFORE doorway tagging so a shadow can
    // never become a tagging anchor. Runs unconditionally: unlike the sign-twin filter it needs no
    // field-sign table, and the include-by-KIND widening above is what makes shadows reachable at all.
    DropShadowRegistrations(out);
    // Unnamed characters that are not standing on the map, or that the party cannot walk to. After the
    // shadow drop so a phantom cannot have been someone else's stacking anchor first.
    DropUnplacedCharacters(out, detail);
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

    // Ground loot — what an enemy dropped when it died. A third source with a third backing store:
    // not the handle table, not the actor pool, but the engine's own 10-slot drop pool.
    ItemScan::ScanDrops(out);

    // ApplyPlayerLabels + NumberDuplicateLabels USED TO RUN HERE, and that was a latent bug the
    // persistent store happened to hide. RescanLocked merges the grace-window survivors into this
    // list AFTER Build returns, so the list the player hears was never the list that got numbered.
    // While numbers were persistent that was harmless -- a carried entity's number was permanent and
    // could not collide. Once numbers are assigned within a scan it is not: a group member that
    // streams out for a frame leaves the survivors to compact 1..N-1 while the carried entity still
    // holds its old suffix, so two entries can answer to the same number for up to the 2 s grace
    // window. Both passes now run in RescanLocked, after the merge, over the list that is actually
    // spoken. `detail` rides out through the out-param so the dup-group dump still fires once per map.
    if (outDetail) *outDetail = detail;

    // Per-category breakdown (confirms the categorization: NPCs/Enemies stay out of Interactables).
    int cc[static_cast<int>(Category::Count)] = {};
    int gated = 0;
    for (const auto& e : out) {
        int ci = static_cast<int>(e.category);
        if (ci >= 0 && ci < static_cast<int>(Category::Count)) ++cc[ci];
        if (!e.available) ++gated;
    }
    char msg[224];
    snprintf(msg, sizeof(msg),
             "rescan: %zu field objects (NPC=%d Enemy=%d Object=%d Exit=%d Save=%d Gate=%d Treasure=%d Items=%d) story-gated=%d",
             out.size(), cc[(int)Category::NPC], cc[(int)Category::Enemy], cc[(int)Category::Object],
             cc[(int)Category::Exit], cc[(int)Category::SaveCrystal],
             cc[(int)Category::GateCrystal], cc[(int)Category::Treasure],
             cc[(int)Category::Items], gated);
    Log::Write("NAV", msg);
    if (s_charByName > 0 || s_poolKind5 > 0 || s_newKind1 > 0 || s_newKind5 > 0 ||
        s_unplaced > 0 || s_unreach > 0 || OddSlotWins() > 0) {
        char im[416];
        snprintf(im, sizeof(im),
                 "inclusion: by-KIND+model kind1=%d kind5=%d (of which %d are CHARACTERS) | "
                 "%d character(s) admitted by NAME ONLY (no interaction flags) | "
                 "%d actor-pool entr(ies) skipped as KIND_DEAD(5), which debug.md records as NPC | "
                 "%d unnamed dropped as NOT ON THE FLOOR, %d as UNREACHABLE | "
                 "%d spoke the PERSONAL npcdic name, %d of them already introduced (%d newly revealed)",
                 s_newKind1, s_newKind5, s_newChar, s_charByName, s_poolKind5,
                 s_unplaced, s_unreach,
                 OddSlotWins(), s_knownName, OddSlotWins() - s_knownName);
        Log::Write("NAV", im);
    }
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

int Build(std::vector<Entity>& out, bool* outDetail) { return BuildLocked(out, outDetail); }

} // namespace EntityScan
