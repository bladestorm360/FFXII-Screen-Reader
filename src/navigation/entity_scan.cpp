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
#include "navigation/door_binding.h"
#include "navigation/sigil_colours.h"
#include "navigation/treasure_state.h"
#include "navigation/path_march.h"     // GroundY -- a trap record carries no Y of its own
#include "speech/phrasebook.h"         // CatTrap -- the trap label is a mod word, not a game string
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"
#include "core/game_text.h"
#include "core/logger.h"
// The handle table cannot tell friend from foe; the BtlChr behind an actor can. battle_state's
// reads are pure memory (no game calls), so they are safe on the scan thread -- see the faction
// override in BuildLocked.
#include "battle/battle_state.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace MemRead;

namespace EntityScan {

using EntityList::Category;

// Instrumentation. Log-only tallies, every one of them a FALSIFIER rather than a reassurance:
// Sessions 79, 82 and 83 each shipped a theory about these objects and each was killed by its own
// counter inside one play session. A change that cannot be sized offline ships with the number that
// would disprove it.
namespace {
int s_charByName = 0;   // characters admitted ONLY because they resolve a name (no interaction flags)
int s_poolKind5  = 0;   // actor-pool entries skipped as KIND_DEAD, a constant documented as misnamed
int s_knownName  = 0;   // objects speaking the PERSONAL npcdic name because the player knows it
                        // (odd slot; e.g. "Arjie" rather than "Nomad").
// S148, both dropped in the handle-table walk itself so they never depend on the actor pool.
int s_dropAbsent   = 0; // sceneObj+0x14 reads the measured ABSENT shape -- see LooksAbsent
std::vector<std::string> s_absentLines;   // up to 8, so a WRONG prune names itself in the log
int s_absentSpared = 0; // characters the OLD rule would have deleted and this one keeps (S153)
std::vector<std::string> s_sparedLines;   // up to 4 -- the falsifier for the S153 narrowing
int s_dropOwnParty = 0; // matched a live scene handle in the battle-work party table
int s_dropTaken    = 0; // Category::Treasure whose coordinates the game already awarded (S150)
int s_dropOddKind  = 0; // a named character whose scene KIND the engine's own candidate filter
                        // rejects -- the summoned Esper (kind 8) and whatever it leaves behind
int s_lastOddKind  = -1;// the last such kind seen, so a NEW one is visible instead of silent

// ---- What the name-or-interaction rule REMOVES -------------------------------------------------
// The rule is "the game names it, or the engine is offering an interaction on it". These say what
// that costs, split so the two known populations can be told apart at a glance.
int s_dropKind1  = 0;   // nameless + non-interactive, kind 1. The three stacked bodies every map has.
int s_dropKind5  = 0;   // nameless + non-interactive, kind 5. The shadows standing beside real NPCs.
int s_dropOther  = 0;   // any other kind -- unexpected, and the reason this is its own bucket.
int s_dropPayload = 0;  // OF THOSE, how many carried a real +0xCC/+0xDC payload id. THE SESSION-54
                        // REGRESSION TELL: a story-gated town gate reads zero flags, so if this is
                        // ever non-zero the rule has just deleted something the engine has an action
                        // bound to, and that is the bug that made town gates vanish once already.
// EVERY DROP NAMES ITSELF, capped -- the same falsifier shape the presence pruner already uses.
// A bucket COUNT says how many objects the rule deleted; it cannot say WHAT, and `other=` is by
// construction the bucket holding kinds nobody has characterised. Measured on Draklor 66F: eleven
// objects in `other`, two of them carrying a real payload id, none of which any diagnostic in the
// mod has ever printed. Built only on a rescan (edge-triggered on the container mask), never per
// frame. If the line count ever falls short of the bucket total the shortfall is printed -- a
// truncated dump that does not say it truncated is how a blinded run reads as a clean negative.
constexpr size_t kDroppedLineCap = 16;
std::vector<std::string> s_droppedLines;
int s_namelessAct = 0;  // KEPT, but nameless while offering an interaction. The tester's model says
                        // this cannot happen ("no objects are interactable that don't have an
                        // icon"), so a non-zero here means our NAME source has a gap, not that the
                        // object should go. Each one is dumped in full.
int s_poolOverlap = 0;  // handle-table objects that are ALSO live actor-pool entries...
int s_poolOverlapNamed = 0;  // ...and how many of those the handle table names.
                        // NO LONGER A BARE COUNTER (Session 86). It shipped in Session 84 as
                        // measurement-only, with the note "a non-zero here on a map with enemies is
                        // how 'enemies show up as NPCs' would come back" -- and on Giza Plains it read
                        // 4 (Penelo + a Hyena + two Urstrix) while Enemy read 0. The measurement came
                        // back, so it now DRIVES the faction override below. The counter stays,
                        // because the population is still worth sizing.
int s_poolParty  = 0;   // ...of those, dropped as the player's own party (tester's call).
int s_poolFoe    = 0;   // ...and re-filed from NPC to Enemy by the actor pool's faction.

// Scene objects this scan deliberately erased. Published so the caller's grace window cannot carry
// them straight back in -- see the note on NoteFiltered in entity_scan.h.
std::vector<void*> s_filtered;

// Live actor-pool entries, rebuilt at the top of every scan. The ACTOR is carried alongside its
// scene object because the actor is what reaches the BtlChr, and the BtlChr is the only thing in the
// game that knows faction -- the handle table has no such field.
struct PoolEntry { void* sceneObj; void* actor; };
std::vector<PoolEntry> s_poolObjs;

// One line per combatant the faction override touched, held until the detail latch decides whether
// this scan gets dumped. It cannot be logged inline: `detail` is only known after the object loop
// finishes, and the loop runs several times a second -- writing there would flood the log.
std::vector<std::string> s_facLines;

// DOES `sceneObj+0x14` READ THE SHAPE THE ABSENT STATE WAS ACTUALLY MEASURED IN?
//
// ---- THIRD TIME (Session 153). IT DROPPED A GATE CRYSTAL. --------------------------------------
//
// S148 measured bit 0x40 on combatants and pruned the whole handle table with it. S150 caught it
// deleting a SAVE CRYSTAL and scoped it to `isCharacter`, on the stated reasoning that "a crystal, a
// gate, a door and a treasure are not [characters]". **That reasoning is STRUCK.** The live log has
//     absent: [0:17] +0x14=0x30 kind=4 "Rabanastre Crystal" at (115.0,-10.0,151.0)
// with `Gate=0` on a map that has one, WITH the isCharacter gate compiled in -- so a gate crystal IS
// scene category 5-7 and the gate never excluded it. Losing a gate crystal is losing the map's fast
// travel; it is the same severity as the save crystal, from the same bit, for the third time.
//
// WHAT IS ACTUALLY MEASURED -- four values, and only four:
//     0xF0  live party member / live enemy      present
//     0xB0  defeated enemy, never-spawned slot  ABSENT   <-- the only thing this filter exists for
//     0x70  treasure, field gimmicks            0x40 set unconditionally, means nothing
//     0x30  save crystal, gate crystal          0x40 clear while standing in plain sight
//
// So `0x40` alone separates nothing: it is clear on 0xB0 (drop) and on 0x30 (keep). The bit that
// splits those two is 0x80, which `nav_rva.h` has said in writing since S150 -- *"bit 0x80 is what
// actually separates the two populations"* -- and which nothing ever acted on.
//
// THIS TESTS FOR THE MEASURED SHAPE, NOT FOR A MEANING. No claim is made about what 0x80 denotes.
// The rule is only "drop what looks like the thing we measured as absent", i.e. exactly 0xB0's high
// nibble, and every one of the four observations above falls out right:
//     0xF0 -> 0xC0  keep      0xB0 -> 0x80  DROP      0x70 -> 0x40  keep      0x30 -> 0x00  keep
//
// AND IT WAS NOT ONLY THE CRYSTAL. Every `absent:` line in the reporting session, by byte shape:
//     0xB0  44 drops   all "Hyena" kind=1                       <- correct, the corpses
//     0x30 153 drops   "Rabanastre Crystal" kind=4, and kind=5
//                      "Weather Eye" / "Chocobo Aficionado" /
//                      "Horne" / "Rabanastran" / one nameless   <- FOUR NAMED NPCs, plus the crystal
// So the shipped filter was wrong on three quarters of what it touched, and the report that started
// this ("gate crystals are being dropped") was the visible half of a wider deletion. This rule keeps
// all 44 and stops all 153.
//
// Narrower is the safe direction here and deliberately so. Failing to drop a corpse leaves a stale
// entry in a list; dropping a crystal takes away somewhere the player was trying to walk to.
bool LooksAbsent(uint8_t ready) {
    return (ready & (NavRva::READY_POPULATION_BIT | NavRva::READY_PRESENT_BIT))
           == NavRva::READY_POPULATION_BIT;
}

// Would the pre-S153 rule have deleted this object? Log-only, and the falsifier for the narrowing
// above: every line it produces is something the shipped build was throwing away.
bool OldRuleWouldDrop(uint8_t ready) {
    return (ready & NavRva::READY_PRESENT_BIT) == 0 && !LooksAbsent(ready);
}
}

void NoteFiltered(void* sceneObj) { if (sceneObj) s_filtered.push_back(sceneObj); }

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

// STRUCK (this session) -- `HasModel`, the engine's model-loaded bit at +0x14 & 0x20.
//
// It existed for exactly one caller: Session 79's `present` route, "any object with an interaction
// KIND and a loaded model is a body standing there, so list it". The premise was true and the
// conclusion was wrong -- a loaded model says a body exists, not that the body is anything the
// player can be told about. That one route produced every bare "NPC n", the three stacked bodies on
// every map, AND the enemies-in-the-NPC-category regression, because a field enemy is a character
// with a loaded model and the handle-table walk claimed it before the actor pool could.
//
// IsInteractionAvailable still reads the same bit for the question it actually answers.

// Live entries in the BtlWork actor pool. Cheap (<=32 slots) and read once per scan. Session 84 read
// this only to SIZE the overlap with the handle table; Session 86 also carries the actor, because
// that is what the faction override in BuildLocked resolves through.
void CollectActorPoolObjects(std::vector<PoolEntry>& out) {
    out.clear();
    void* pool = PtrAt(Hooks::ResolveRva(NavRva::ACTOR_POOL_BASE), 0);
    if (!pool) return;
    uint32_t count = 0;
    if (!SafeReadU32(Hooks::ResolveRva(NavRva::ACTOR_POOL_COUNT), 0, &count) || count == 0) return;
    if (count > 64) count = 64;
    for (uint32_t i = 0; i < count; ++i) {
        void* actor = static_cast<char*>(pool) + static_cast<size_t>(i) * NavRva::ACTOR_STRIDE;
        if (!PtrAt(actor, NavRva::ACTOR_DEF_PTR)) continue;
        uint8_t active = 0;
        if (!SafeReadU8(actor, NavRva::ACTOR_ACTIVE_OFF, &active) ||
            (active & NavRva::ACTOR_ACTIVE_BIT) == 0) continue;
        void* so = PtrAt(actor, NavRva::ACTOR_SCENEOBJ);
        if (so) out.push_back(PoolEntry{ so, actor });
    }
}

// Is this combatant one of the player's own party?
//
// ASK THE GAME'S OWN PARTY LIST, NOT A KIND BYTE. Roster list 3 (BtlWork+0x5A7E, nine u16 BtlChr
// indices) IS the party; membership is a lookup, not an inference. The alternative -- the scene-kind
// nibble ScanCombatants uses -- demonstrably cannot do this job on the field: the Giza Plains dump
// reads kind=1 for Penelo AND for all three enemies, so keying on it would file the player's own
// party member as an Enemy. phyre_types.h:113 already flags that constant as unverified; this
// sidesteps it entirely.
bool IsPartyMemberActor(void* actor) {
    void* bc = BattleState::BtlChrForActor(actor);
    if (!bc) return false;
    for (int slot = 0; slot < BattleState::kRosterSlots; ++slot)
        if (BattleState::BtlChrForSlot(slot) == bc) return true;
    return false;
}

// Append FLOOR TRAPS, and only while the game is showing them.
//
// A FOURTH backing store, and the reason this cannot ride on any pass above: a trap is not a scene
// object at all. `FUN_003ec700` sets a MODEL-INSTANCE state, never a scene object, so the handle
// table never lists one and the interaction scanner cannot see one.
//
// Everything here was derived offline from the two functions that walk this data and agree on every
// field of it -- FUN_002f8060 (the per-frame trigger) and FUN_002f82f0 (the visibility toggle).
// Addresses, cap and record layout: nav_rva.h TRAP_*.
//
// THE GATE IS THE GAME'S OWN LATCH. `TRAP_VISIBLE` is set from FUN_0030c300 (the Libra predicate)
// by FUN_002f82f0, which then drives every present trap's model state from it -- so it already IS
// "are traps on screen right now", for one guarded read, and mirroring it is the same principle
// LibraActive() follows for HP digits. Deliberately NOT LibraActive() itself: that reads the
// BATTLE-HUD mirror, and this runs on the field where that context may not be live.
//
// Caller holds g_mutex.
void ScanTraps(std::vector<Entity>& out) {
    uint32_t visible = 0;
    if (!SafeReadU32(Hooks::ResolveRva(NavRva::TRAP_VISIBLE), 0, &visible) || visible == 0)
        return;                       // Libra down -> the game hides them, so the mod lists none

    void* tbl = PtrAt(Hooks::ResolveRva(NavRva::TRAP_TABLE), 0);
    if (!tbl) return;                 // no trap table loaded for this map

    uint32_t rawCount = 0;
    if (!SafeReadU32(tbl, 0, &rawCount)) return;
    const uint32_t count = rawCount > NavRva::TRAP_SLOT_CAP ? NavRva::TRAP_SLOT_CAP : rawCount;
    if (count == 0) return;

    // THE PRESENCE MASK IS WHAT MAKES A STALE TABLE POINTER HARMLESS. The table is loaded with the
    // map, but the mask array is keyed on map id -- so on a map with no entry we get 0 and list
    // nothing, even if `tbl` still points at the previous map's records. That is the same protection
    // the game's own walkers rely on, not an extra safety net invented here.
    const int mapId = MapNames::CurrentMapId();
    uint32_t mask = 0;
    uint32_t nMask = 0;
    if (SafeReadU32(Hooks::ResolveRva(NavRva::TRAP_MASK_COUNT), 0, &nMask) && nMask > 0 &&
        nMask <= 4096) {
        void* arr = Hooks::ResolveRva(NavRva::TRAP_MASK_ARRAY);
        for (uint32_t i = 0; i < nMask; ++i) {
            const uint32_t off = i * NavRva::TRAP_MASK_STRIDE;
            uint32_t entryMap = 0;
            if (!SafeReadU32(arr, off, &entryMap)) break;
            if (static_cast<int>(entryMap) != mapId) continue;
            SafeReadU32(arr, off + 4, &mask);
            break;
        }
    }

    // A trap record carries NO Y -- the game builds its position with a literal 0.0 and zeroes the
    // party's Y too before comparing, so its own test is flat. Seed the walkmap lookup with the
    // player's height: GroundY returns the floor under (x,z) and falls back to the seed off-mesh,
    // so a trap the walkmap does not cover lands at the player's own height instead of at y=0 --
    // which on a map whose floor sits at -32 would put every trap 32 m in the air.
    FVec3 playerPos;
    const bool havePlayer = PlayerState::ReadPlayerPos(playerPos);
    const float seedY = havePlayer ? playerPos.y : 0.0f;

    char diag[512];
    int  diagLen = 0;
    int  listed  = 0;

    for (uint32_t i = 0; i < count; ++i) {
        if (((mask >> i) & 1u) == 0) continue;          // not present on this map (or already sprung)

        uint32_t recOff = 0;
        if (!SafeReadU32(tbl, 4 + i * 4, &recOff)) continue;
        void* rec = static_cast<char*>(tbl) + recOff;

        int16_t x10 = 0, z10 = 0;
        if (!SafeReadS16(rec, NavRva::TRAP_REC_X10, &x10)) continue;
        if (!SafeReadS16(rec, NavRva::TRAP_REC_Z10, &z10)) continue;
        uint8_t radius = 0, flagRaw = 0;
        SafeReadU8(rec, NavRva::TRAP_REC_RADIUS, &radius);
        SafeReadU8(rec, NavRva::TRAP_REC_FLAG,   &flagRaw);

        Entity e;
        e.sceneObj   = nullptr;   // load-bearing: RescanLocked skips the grace window for these, so a
                                  // sprung trap leaves the list on the very next scan
        // IDENTITY, and it is NOT optional. With no scene object, `CursorMatch` falls back to
        // (nameIdx, category) -- so leaving nameIdx at its default 0 would make every trap on the map
        // the SAME entity to the focus clamp, and a cursor parked on trap 5 would re-lock onto
        // whichever trap sorted nearest on the next rescan. The slot index is a real identity: it is
        // the bit position in the presence mask and it is stable for as long as the map is loaded.
        // Negative synthetic band, following the convention already in use -- exits are -(1000+i)
        // (exit_scan.cpp) and ground drops -(2000+i) (item_scan.cpp).
        e.nameIdx    = static_cast<int16_t>(-(3000 + static_cast<int>(i)));
        e.category   = Category::Trap;
        e.label      = Phrase::Get(Phrase::Id::CatTrap);
        e.pos.x      = static_cast<float>(x10) / 10.0f;
        e.pos.z      = static_cast<float>(z10) / 10.0f;
        e.pos.y      = PathMarch::GroundY(e.pos.x, e.pos.z, seedY);
        e.fixed      = true;      // no scene node to refresh a position from
        e.noBearing  = false;     // x/z ARE world space, so a bearing is real
        e.available  = true;      // nothing to interact with -> no filter may hide it
        e.gameNamed  = false;     // the word is ours; NumberDuplicateLabels turns it into "Trap 1"
        out.push_back(e);
        ++listed;

        if (diagLen >= 0 && diagLen < static_cast<int>(sizeof(diag)) - 64) {
            const int n = snprintf(diag + diagLen, sizeof(diag) - diagLen,
                                   " [%u]=(%.1f,%.1f) r=%u flag=%d",
                                   i, e.pos.x, e.pos.z, radius,
                                   static_cast<int>(static_cast<int8_t>(flagRaw)));
            if (n > 0) diagLen += n;
        }
    }

    // THE INSTRUMENT, and it ships with the feature because the feature cannot be play-tested here:
    // there is no save near a trap dungeon, so the first log from any trap map has to be decisive on
    // its own. A plausible count with in-map coordinates confirms the whole chain; an absurd
    // `tableCount` condemns TRAP_TABLE; a latch that never reads 1 under Libra condemns the gate.
    //
    // Keyed on the STATE tuple, so it prints once per distinct state and standing still is silent.
    // Not throttled and not capped -- a rate-limited line is not a measurement (L-04).
    static int      s_lastMap   = -1;
    static uint32_t s_lastMask  = 0xFFFFFFFFu;
    static uint32_t s_lastVis   = 0xFFFFFFFFu;
    static uint32_t s_lastCount = 0xFFFFFFFFu;
    if (mapId != s_lastMap || mask != s_lastMask || visible != s_lastVis || count != s_lastCount) {
        s_lastMap = mapId; s_lastMask = mask; s_lastVis = visible; s_lastCount = count;
        char line[640];
        snprintf(line, sizeof(line), "traps: map=%d latch=%u mask=0x%X tableCount=%u listed=%d%s",
                 mapId, visible, mask, rawCount, listed, diagLen > 0 ? diag : "");
        Log::Write("NAV", line);
    }
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

        // THE SAME PRESENCE TEST THE HANDLE WALK APPLIES, and it has to be here or the drop leaks.
        // This pass only sees objects the handle walk did NOT list -- and as of S148 the handle walk
        // DROPS an absent object rather than listing it, so every corpse it prunes would arrive here
        // looking like a fresh combatant nobody had claimed yet. `AlreadyListed` cannot tell "not
        // listed because it is new" from "not listed because we just refused it". Full derivation of
        // bit 0x40 and of the shape test: nav_rva.h READY_PRESENT_BIT, and `LooksAbsent` above.
        //
        // BOTH WALKS USE THE ONE PREDICATE (S148's rule, and the reason S153's fix is a one-line
        // change here): the two sites drifting apart is how a drop leaks back in.
        {
            uint8_t ready = 0;
            if (SafeReadU8(sceneObj, NavRva::SCENEOBJ_READY_OFF, &ready)) {
                if (LooksAbsent(ready)) {
                    ++s_dropAbsent;
                    NoteFiltered(sceneObj);
                    continue;
                }
                if (OldRuleWouldDrop(ready)) ++s_absentSpared;
            }
        }

        // Enemy vs ally = the scene-kind nibble: kind==3 => ally, kind==5 => dead/removed (drop),
        // else => enemy.
        //
        // STRUCK (Session 86) -- "the game's own faction test". IT IS NOT, at least not on the field.
        // The Giza Plains object dump reads kind=1 for Penelo (a party member) AND for all three
        // enemies present, so this nibble cannot separate them; anything keyed on it alone would file
        // the player's own party as Enemy. phyre_types.h:113 already flagged the constant as
        // unverified. The thing that DOES discriminate is the BtlChr kind byte, which
        // BattleState::FactionOf reads before it ever falls back to this nibble.
        //
        // LEFT AS-IS DELIBERATELY, not endorsed. The field path no longer depends on it -- the
        // faction override in BuildLocked resolves every handle-table combatant through FactionOf --
        // so what reaches here is the battle-only population, which has been correct in play. Fixing
        // it blind would trade a working path for an unmeasured one. If a battle ever mis-files an
        // ally, this is the line, and FactionOf is the replacement.
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
    s_knownName  = 0;
    s_dropKind1  = 0;
    s_dropKind5  = 0;
    s_dropOther  = 0;
    s_dropPayload = 0;
    s_droppedLines.clear();
    s_namelessAct = 0;
    s_poolOverlap = 0;
    s_poolOverlapNamed = 0;
    s_poolParty  = 0;
    s_poolFoe    = 0;
    s_dropOwnParty = 0;
    s_dropOddKind  = 0;
    s_lastOddKind  = -1;
    s_absentSpared = 0;
    s_sparedLines.clear();
    s_dropAbsent   = 0;
    s_dropTaken    = 0;
    s_absentLines.clear();
    s_facLines.clear();
    s_filtered.clear();
    ResetNameStats();
    if (!PlayerState::IsFieldActive()) return 0;

    void* base = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!base) return 0;
    void* leader = PlayerState::ReadLeaderSceneObject();
    CollectActorPoolObjects(s_poolObjs);
    // Read ONCE per scan, not per object: the table is at most four entries and cannot change while
    // one scan runs. The leader is dropped separately (it is `leader` above), so this is the other
    // two plus a guest.
    uint32_t partyHandles[8] = {};
    const int partyHandleCount = BattleState::PartySceneHandles(partyHandles, 8);

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
            // THE RULE: the game names it, or the engine is offering an interaction on it. Anything
            // else is an object the mod has nothing true to say about, and it is not listed.
            //
            // This replaced five routes, four of which collapse into these two: the npcdic gimmick
            // band (433-469) always resolves a name, so it IS `named`; a non-character kind-5 gimmick
            // worth listing is either named or offering a prompt; and `present` is struck below.
            //
            // STRUCK -- `present`, Session 79's include-by-KIND-and-model widening. ONE route, THREE
            // symptoms, all reported from play: the bare "NPC n" SHADOWS beside real NPCs; the three
            // unnamed bodies stacked on one coordinate on EVERY map; and ENEMIES filed as NPCs,
            // because a field enemy is a character with a loaded model, this walk runs before
            // ScanCombatants, and ScanCombatants skips anything AlreadyListed -- so the widening took
            // the enemy classifier's inputs away. Full account, with the four-map evidence and the
            // strike on Session 79's founding premise, in Docs\debug.md.
            //
            // KNOWN COST, accepted deliberately: `+0x1C` is MODE state and the engine clears both
            // prompt bits on a disabled object (FUN_0025ad10), so a story-gated town gate carrying no
            // name is not listed until the script arms it -- the Session 54 case. MEASURED rather than
            // argued: s_dropPayload counts every drop that carried a real +0xCC/+0xDC payload id.
            // Non-zero means the cost was actually paid and the gate needs a route back keyed on THAT
            // field, not on a model.
            const bool interactive = (flags & (NavRva::FLAG_TALK | NavRva::FLAG_ACTION)) != 0;
            // A CHARACTER'S NAME IS AN INCLUSION REASON (Session 77) -- and now the primary one. A
            // named character is a real person standing in the world whether or not you may talk to
            // them yet; `available` carries the "can I act on it right now" half.
            //
            // ONE resolve per object: it costs a codec decode pair.
            std::wstring name = ResolveObjectName(obj);
            const bool named = !name.empty();
            // How many of the names spoken are the PERSONAL one, i.e. a character the player has been
            // introduced to. Session 81 briefly made this decide nothing by preferring the odd slot
            // unconditionally; that was struck the same day (see NpcdicDisplayName), so it is once
            // again both the selector and the measurement. `OddSlotWins()` should now track it.
            if (named && nameIdx > 0 &&
                TalkNameKnown(static_cast<int>(static_cast<uint32_t>(nameIdx) &
                                               NavRva::NPCDIC_NAME_MASK))) ++s_knownName;
            // How many characters we list on the strength of a name alone (no prompt offered right
            // now). That is the population the old widening was aimed at and it is still served.
            if (isCharacter && named && !interactive) ++s_charByName;

            // ---- THE RULE ---------------------------------------------------------------------
            // Nameless AND offering nothing -> the mod cannot describe it, so it is not listed. This
            // is the single test that replaced five routes; everything above is why.
            if (!named && !interactive) {
                if      (kind == NavRva::KIND_TALK_TARGET)    ++s_dropKind1;
                else if (kind == NavRva::KIND_ACTION_GIMMICK) ++s_dropKind5;
                else                                          ++s_dropOther;
                // THE SESSION-54 REGRESSION TELL, and the whole reason this drop is safe to ship.
                // A story-gated town gate reads ZERO flags, so it looks exactly like a shadow from
                // here -- but it still carries the event index the script bound to it. If this ever
                // reads non-zero, the rule has deleted something the engine has a real action on and
                // the gate case needs a route back, keyed on THIS field rather than on a model.
                uint16_t aid = 0xFFFF, tid = 0xFFFF;
                SafeReadU16(obj, NavRva::SCENEOBJ_ACTION_ID, &aid);
                SafeReadU16(obj, NavRva::SCENEOBJ_TALK_ID,   &tid);
                if (aid != 0xFFFF || tid != 0xFFFF) ++s_dropPayload;
                if (s_droppedLines.size() < kDroppedLineCap) {
                    uint8_t dReady = 0;
                    SafeReadU8(obj, NavRva::SCENEOBJ_READY_OFF, &dReady);
                    FVec3 dPos{};
                    const bool dHavePos = PlayerState::ReadSceneObjectPos(obj, dPos);
                    // The event-table signature is the field that says WHAT this is. Kind and
                    // category are shared by every prop on the map; the handler names are the
                    // authoring template, and a doorway template does not look like a rect.
                    char dSig[224];
                    const int dNames = MapScript::ObjectEventSignature(obj, dSig, sizeof(dSig));
                    char dl[512];
                    snprintf(dl, sizeof(dl),
                             "dropped (nameless, no prompt): [%u:%u] cat=%02X kind=%u en=%u "
                             "r14=%02X mask=%08X act=%u talk=%u at (%.2f,%.2f,%.2f)%s events(%d): %s",
                             c, i, catByte, kind,
                             (kindByte & NavRva::INTERACT_ENABLE_BIT) ? 1u : 0u,
                             dReady, flags,
                             static_cast<unsigned>(aid), static_cast<unsigned>(tid),
                             dPos.x, dPos.y, dPos.z, dHavePos ? "" : " <NO POS>",
                             dNames, dSig);
                    s_droppedLines.emplace_back(dl);
                }
                continue;
            }

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

            // ---- STALE ENTITY PRUNER: the engine has stopped presenting this object --------------
            //
            // NOT a dead-enemy rule. `sceneObj+0x14` bit 0x40 is a PRESENCE flag and the categories
            // fall out of it cleanly, which is what makes it general:
            //
            //   SET    live party, live enemies, AND treasure / field gimmicks (0x70, 0xF0)
            //   CLEAR  a defeated enemy, and reserve slots that were never spawned (0xB0)
            //
            // Bit 0x20 beside it is already known to the mod as "model loaded"
            // (NavRva::READY_MODEL_BIT, from the engine's own interaction predicate FUN_002675c0);
            // 0x40 is its neighbour. It prunes an NPC that walked off, a trigger that was cleared
            // and a corpse by the same test, which is the pruner that was actually wanted rather
            // than a kill detector.
            //
            // STRUCK (Session 150): this block used to claim the same test also prunes "a chest that
            // was consumed", and that the categories "fall out of it cleanly". They do not.
            // TREASURE OBJECTS SET 0x40 UNCONDITIONALLY -- including slots the game never placed at
            // all (world origin, no layer), measured in
            // `x64\logs\FFXII-Screen-Reader-2026-08-10_12-08-35.log:1133-1145`. So this pruner runs
            // on every treasure and can never fire on one, which is exactly the tester report of
            // 2026-08-11 ("once you pick one up they are still there"). The treasure drop is a
            // SEPARATE test below; do not try to fix it by widening this bit.
            //
            // MEASURED BEFORE IT WAS APPLIED, over two play sessions: a counter on exactly this
            // condition read 1 across 35 rescans while exactly one defeated Hyena stayed listed and
            // `Enemy=1` refused to fall, and 0 before the kill. The object dump behind it is in
            // debug.md.
            //
            // NoteFiltered is MANDATORY (Session 83): the object stays in the handle table, so the
            // scan re-finds and re-drops it every pass -- without the explicit signal the caller's
            // grace window re-admits it every time and the drop reports success while changing
            // nothing.
            //
            // ---- SCOPED TO CHARACTERS (Session 150). IT DROPPED A SAVE CRYSTAL. -------------------
            //
            // The one and only time this filter has ever been observed to fire in the whole log
            // archive, it deleted a SAVE CRYSTAL:
            //     absent: [0:14] +0x14=0x30 kind=4 "Save Crystal" at (227.6,13.0,26.4)
            // That is a landmark a blind player navigates to in order to SAVE THE GAME, and losing it
            // is far worse than the corpse this filter exists to remove. Meanwhile the four archived
            // logs that actually contain enemies all predate this code, so the drop has never once
            // been seen to hit its target.
            //
            // WHY IT WAS ALWAYS WRONG TO ASK EVERY OBJECT. The bit was measured on COMBATANTS -- 0xF0
            // live party and enemies, 0xB0 a defeated enemy and never-spawned reserve slots -- and
            // then applied to the whole handle table on the assumption that it generalised. It does
            // not: this session proved treasure sets it UNCONDITIONALLY (0x70 even on slots the game
            // never placed), and the Save Crystal above reads 0x30 with it CLEAR while sitting on the
            // map in plain sight. So on gimmicks the bit takes both values and means neither.
            //
            // `isCharacter` (scene category 5-7, computed above) is the population it was measured on
            // and the only one it may speak for. A corpse is a character; a crystal, a gate, a door
            // and a treasure are not, so none of them can ever reach this drop again.
            //
            // This does not weaken the corpse case at all -- a defeated enemy still reads 0xB0 and is
            // still a character. It only stops the filter answering a question it was never asked.
            //
            // ---- AND SCOPED AGAIN TO THE MEASURED BYTE SHAPE (Session 153) ----------------------
            //
            // `isCharacter` was not enough, and the sentence above explaining why it would be is
            // STRUCK: a GATE CRYSTAL is scene category 5-7, so it passed that gate and was deleted
            // anyway (`+0x14=0x30`, `Gate=0` on a map with one). The test is now `LooksAbsent`,
            // which requires the byte to match the shape ABSENT was measured in instead of reading
            // bit 0x40 on its own -- see its derivation at the top of this file.
            if (isCharacter) {
                uint8_t ready = 0;
                if (SafeReadU8(obj, NavRva::SCENEOBJ_READY_OFF, &ready)) {
                    if (LooksAbsent(ready)) {
                        ++s_dropAbsent;
                        if (s_absentLines.size() < 8) {
                            char al[192];
                            char nm[96]; Log::ToUtf8(name, nm, sizeof(nm));
                            snprintf(al, sizeof(al),
                                     "absent: [%u:%u] +0x14=0x%02X kind=%u \"%s\" at (%.1f,%.1f,%.1f)",
                                     c, i, ready, kind, nm, pos.x, pos.y, pos.z);
                            s_absentLines.emplace_back(al);
                        }
                        NoteFiltered(obj);
                        continue;
                    }
                    // KEPT, and the old rule would have deleted it. This is the line that says what
                    // the fix bought -- if it never appears, the defect was not what we think.
                    if (OldRuleWouldDrop(ready)) {
                        ++s_absentSpared;
                        if (s_sparedLines.size() < 4) {
                            char sl[192];
                            char nm[96]; Log::ToUtf8(name, nm, sizeof(nm));
                            snprintf(sl, sizeof(sl),
                                     "spared: [%u:%u] +0x14=0x%02X kind=%u \"%s\" at (%.1f,%.1f,%.1f)"
                                     " -- pre-S153 rule would have dropped this",
                                     c, i, ready, kind, nm, pos.x, pos.y, pos.z);
                            s_sparedLines.emplace_back(sl);
                        }
                    }
                }
            }

            // ---- YOUR OWN PARTY IS NOT A DESTINATION, and the pool cannot be asked ----------------
            //
            // S148, reported in play: *"party is indeed not being ignored by the NPC tracker… there is
            // no reason the player needs to track party members on the map."* The drop existed, but it
            // hung off FactionOf(poolActor) two hundred lines below, which needs the object to be in
            // the ACTOR POOL. Measured in one field session on the S148 build: the own-party drop fired
            // in **4 rescans out of 26**, with `actorPool=0 poolAnswered=0` in six of them -- so `]`
            // walked the player through "Vaan. Northeast, 2 steps" and "Penelo. East, 2 steps" most of
            // the time. A drop that only works when an unrelated subsystem happens to be populated is
            // not a drop.
            //
            // BattleState::PartySceneHandles reads the battle-work party table instead (the one
            // GambitsEnabled already walks), which is populated regardless. The handle decomposes the
            // same way the mod's own handle-table resolver does -- `selector = (h>>16) & 0xF`,
            // `slot = h & 0xFFFF` -- so it is compared against the (container, slot) this walk is
            // standing on, with the generation nibble deliberately ignored: a stale generation on a
            // live party member is still that party member.
            //
            // NoteFiltered is MANDATORY (Session 83): these are live objects the scan re-finds every
            // pass, so without the explicit signal the grace window re-admits every one of them.
            {
                bool isOwnParty = false;
                for (int p = 0; p < partyHandleCount; ++p) {
                    if (((partyHandles[p] >> 16) & 0xF) == c &&
                        (partyHandles[p] & 0xFFFF) == i) { isOwnParty = true; break; }
                }
                if (isOwnParty) { ++s_dropOwnParty; NoteFiltered(obj); continue; }
            }

            // ---- A KIND THE ENGINE ITSELF REFUSES TO OFFER ---------------------------------------
            //
            // THE SUMMONED ESPER, and what is left of it after a dismissal. Belias reads scene KIND 8,
            // and `FUN_0025bad0` -- the game's own near-object candidate filter, recorded at 0.99 in
            // nav_rva.h -- accepts kind 1 (talk), 4 (both), 5 (action) and 7 (talk) and **rejects
            // everything else**. So the engine never offers a kind-8 object to the player at all; the
            // mod listed it only because it has a NAME.
            //
            // This is why the Esper survived a dismissal in the tracker. Two `'` dumps, one taken
            // while it was summoned and one 13 s after "Dismiss Belias? Yes", are IDENTICAL in every
            // field the mod reads -- `kind=8 en=1 flags=00030800 r14=F0`, only the position differs.
            // Nothing about that object changes when the Esper leaves, so no liveness test could ever
            // have caught it and the candidate bit 0x40 does not fire on it either. What is wrong is
            // admitting it in the first place: your own summon is not a place to walk to, before or
            // after it goes away.
            //
            // Scoped to CHARACTERS ADMITTED BY NAME. An object the engine is actively offering an
            // interaction on keeps its entry whatever its kind says -- `interactive` is a live
            // engine offer and outranks this.
            if (isCharacter && !interactive &&
                kind != NavRva::KIND_TALK_TARGET && kind != 4 &&
                kind != NavRva::KIND_ACTION_GIMMICK && kind != 7) {
                ++s_dropOddKind;
                s_lastOddKind = kind;
                NoteFiltered(obj);
                continue;
            }

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

            // ---- COLLECTED TREASURE ------------------------------------------------------------
            //
            // Has to be HERE and not in the stale-entity pruner above: the pruner runs before the
            // npcdic classification, and this test is only meaningful once we know the entry is a
            // treasure. It is deliberately scoped to that ONE category -- the same test widened to
            // named gimmicks would reach gates and crystals, which are not consumable.
            //
            // The presence bit 0x40 cannot answer this (it is set unconditionally on treasure -- see
            // NavRva::READY_PRESENT_BIT, struck this session), and neither can anything else ON the
            // object: the engine never writes a treasure's identity there. TreasureState instead
            // records the coordinates the game's own award function was handed, which are the exact
            // floats the object was placed at. See navigation/treasure_state.h.
            //
            // NoteFiltered is MANDATORY here for the same reason it is above: the object stays in
            // the handle table and the scan re-finds it every pass, so without the explicit signal
            // the caller's 2 s grace window re-admits it and the drop reports success while changing
            // nothing (entity_list.cpp:122).
            if (e.category == Category::Treasure &&
                TreasureState::WasCollectedAt(MapNames::CurrentMapId(), pos.x, pos.z)) {
                ++s_dropTaken;
                NoteFiltered(obj);
                continue;
            }
            // `gameNamed` records whether the words came from the GAME or from our category fallback.
            // The sign-twin drop keys on it, so two anonymous objects that both fell back to the word
            // "Interactables" can never be mistaken for a duplicate pair.
            e.gameNamed = !e.label.empty();
            // NO FALLBACK LABEL HERE -- see ApplyFallbackLabels, called after the doorway tagging.
            //
            // This used to read `if (e.label.empty() && e.doorway) e.label = L"Sign";` and it was DEAD
            // CODE from the day it was written: `e.doorway` is set by TagDoorwaysAndDropSignTwins,
            // which runs over the FINISHED list, so it is always false on a freshly built entity. The
            // word "Sign" -- which the tester authorised specifically -- could never once be spoken,
            // and every unnamed doorway said "Interactables" instead.
            // ---- FACTION OVERRIDE: the ACTOR POOL owns combatants ------------------------------
            //
            // THE HANDLE-TABLE WALK WINS EVERY TIE, and that is the whole bug. A field enemy is a
            // named character with a loaded model, so `named` admits it here; ClassifyByNameKey then
            // returns NPC for anything `isCharacter`; and ScanCombatants -- the only pass that can
            // tell friend from foe -- skips it as AlreadyListed. Enemy=0 on a map full of enemies.
            //
            // Session 84 struck the `present` (KIND+model) route for exactly this reason but left the
            // older `named` route open, which is why the fix held on four enemy-free maps and failed
            // on Giza Plains. Rather than reverse the ordering (which would re-break the things the
            // handle table is genuinely better at -- the npcdic name, the interaction flags, the
            // payload ids), keep the entry and let the pool correct the one field it owns.
            void* poolActor = nullptr;
            for (const PoolEntry& p : s_poolObjs)
                if (p.sceneObj == obj) { poolActor = p.actor; break; }
            if (poolActor) {
                ++s_poolOverlap;
                if (e.gameNamed) ++s_poolOverlapNamed;
                e.factionVerdict = true;   // the pool ANSWERED -- see Entity::factionVerdict

                const bool party = IsPartyMemberActor(poolActor);
                const BattleState::Faction fac = BattleState::FactionOf(poolActor);

                if (s_facLines.size() < 16) {
                    char fl[224];
                    snprintf(fl, sizeof(fl),
                             "faction: [%u:%u] \"%s\" party=%d faction=%d -> %s",
                             e.container, e.slot,
                             e.gameNamed ? "named" : "(unnamed)", party ? 1 : 0,
                             static_cast<int>(fac),
                             party ? "DROPPED (own party)"
                                   : (fac == BattleState::Faction::Foe ? "Enemy" : "kept as-is"));
                    s_facLines.emplace_back(fl);
                }

                // The player's own party is not something to navigate to (tester's call). NoteFiltered
                // is MANDATORY here: a party member is a live engine object this pass finds and drops
                // on EVERY scan, so without the explicit signal the caller's grace window re-admits it
                // every time while this pass logs the drop -- the exact failure Session 83 hit.
                //
                // SECOND NET ONLY, since S148: this route needs the object to be in the ACTOR POOL and
                // it usually is not, so the pool-independent handle test above is what actually keeps
                // the party out of the list. Kept because when the pool DOES answer it also answers
                // faction, and the two verdicts should not be able to disagree.
                if (party) { ++s_poolParty; NoteFiltered(obj); continue; }

                // Only a POSITIVE foe verdict re-files anything. Guest/Ally/Neutral/Unknown keep the
                // category they already have: this pass exists to stop enemies being called NPCs, not
                // to re-adjudicate every character on the map.
                if (fac == BattleState::Faction::Foe) { e.category = Category::Enemy; ++s_poolFoe; }
            }
            // KEPT, but the game gives us no words for it, so it will fall through to the category
            // word and speak as a bare "NPC" / "Interactables". The tester's model says this cannot
            // exist -- *"no objects are interactable that don't have one of these [icons]"* -- so a
            // non-zero count is evidence that our NAME source has a gap, not that the object should
            // have been dropped. Every one is logged in full so the next session can chase it: walk
            // to the position and press `;`. interact_target.cpp resolves the label through the same
            // function and stays silent when it is empty, so speech means a gap in our resolver and
            // silence means the object genuinely has no icon and the rule should drop it too.
            if (!e.gameNamed) {
                ++s_namelessAct;
                char nm[224];
                snprintf(nm, sizeof(nm),
                         "nameless BUT interactive: [%u:%u] kind=%u flags=%08X nameIdx=%d act=%u "
                         "talk=%u avail=%d pos=(%.2f,%.2f,%.2f)",
                         e.container, e.slot, e.kind, e.flags, e.nameIdx, e.actionId, e.talkId,
                         e.available ? 1 : 0, e.pos.x, e.pos.y, e.pos.z);
                Log::Write("NAV-DIAG", nm);
            }
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
    // field-sign table.
    //
    // STRUCK (this session) -- `DropUnplacedCharacters`, which used to run between these two.
    //
    // It dropped an unnamed NPC floating >2 m above its floor or outside the reachable set. Its
    // candidate set was "Category::NPC and not gameNamed", which the rule above has now emptied, so
    // it had nothing left to judge -- but the reason it is deleted rather than left harmless is that
    // its thresholds came from ONE map's dump and it only ever fired on that map. Nomad Village's
    // three stacked bodies sit at Y=6.06; the identical three on Lowtown, Eastgate and Garamsythe sit
    // ON the floor and reachable, so it saw them and kept them. Classification answers this globally;
    // geometry answered it for one map and looked like a fix.
    DropShadowRegistrations(out);
    // Doorway tagging + sign-twin removal, while the list is still just handle-table objects.
    TagDoorwaysAndDropSignTwins(out, detail);
    // S179: an interactable whose own routine jumps to a map, or opens the closed floor it stands in,
    // is a Door -- on every map, including the ones TagDoorways returns early on (no +0x70 table).
    DoorBinding::PromoteDoors(out);
    // S183: the Pharos Sigils of Sacrifice share one game name; their colour comes from each one's own glow
    // effect in the map script. Before the fallback labels, after the routine cache is warm.
    SigilColours::Apply(out);
    // Category words LAST, so `doorway` is set by the time an unnamed sign is named. Anything still
    // unlabelled here is an object the game refused to name that is nonetheless offering a prompt.
    ApplyFallbackLabels(out);
    if (detail) LogObjectDump(out);
    // Held back from the object loop above -- see s_facLines. If a party member ever lands anywhere
    // other than DROPPED, or a townsperson reads faction=Foe, the answer is on these lines.
    if (detail) for (const std::string& fl : s_facLines) Log::Write("NAV-DIAG", fl.c_str());

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

    // Floor traps — a FOURTH store again, and the only one the mod lists conditionally: a trap is a
    // model instance with no scene object, and the game hides them until a party member has Libra up.
    ScanTraps(out);

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
    // EVERY category is counted, so the tally always sums to `out.size()`. Door and Shop were missing
    // here for the whole of the session that introduced them, which is how a gate crystal sitting in
    // the wrong bucket stayed invisible: the line read "Save=1 Gate=0" and the two new categories it
    // had been promoted into were simply not shown. A breakdown that does not add up hides the bug it
    // exists to expose -- if a category is added to the enum, add it to this line.
    char msg[288];
    // `actorPool` is on THIS line and not the conditional inclusion line below, because the case it
    // exists to expose -- the pool coming back empty, which re-files every enemy as an NPC -- can
    // easily leave every counter that line is gated on at zero, so it would go unprinted exactly when
    // it mattered. `poolAnswered=0` beside a non-zero `Enemy=` count is the signature to look for:
    // it means the categories on screen are carried verdicts, not fresh ones.
    snprintf(msg, sizeof(msg),
             "rescan: %zu field objects (NPC=%d Enemy=%d Object=%d Exit=%d Door=%d Shop=%d Save=%d "
             "Gate=%d Treasure=%d Items=%d) story-gated=%d actorPool=%zu poolAnswered=%d",
             out.size(), cc[(int)Category::NPC], cc[(int)Category::Enemy], cc[(int)Category::Object],
             cc[(int)Category::Exit], cc[(int)Category::Door], cc[(int)Category::Shop],
             cc[(int)Category::SaveCrystal],
             cc[(int)Category::GateCrystal], cc[(int)Category::Treasure],
             cc[(int)Category::Items], gated, s_poolObjs.size(), s_poolOverlap);
    Log::Write("NAV", msg);

    // The three S148 drops, on their own line so they are greppable and cannot be truncated off the
    // end of the inclusion line.
    //   absent   -- the stale pruner. Should rise when something dies, despawns or is consumed and
    //               fall back as the engine recycles the slot. If it is 0 while the player can still
    //               hear a corpse, bit 0x40 is the wrong flag and it goes.
    //   ownParty -- should equal the non-leader party members on screen.
    //   oddKind  -- 1 exactly while an Esper exists, 0 otherwise. Any kind other than 8 here is a NEW
    //               population and wants looking at before it is trusted.
    //   taken    -- collected treasure (S150). Should rise the moment an award fires and stay up for
    //               the rest of the visit; `TreasureState` drops it on a map change. If a treasure is
    //               still spoken while the `treasure: collected` line is in the log, this counter is
    //               the one that says whether the scan side or the match tolerance is at fault.
    if (s_dropAbsent > 0 || s_dropOwnParty > 0 || s_dropOddKind > 0 || s_dropTaken > 0 ||
        s_absentSpared > 0) {
        char dm[352];
        snprintf(dm, sizeof(dm),
                 "handle-walk drops: %d ABSENT (sceneObj+0x14 matches the measured 0xB0 shape) | "
                 "%d spared (0x40 clear but NOT that shape -- the pre-S153 rule deleted these) | "
                 "%d own-party (by scene handle, pool-independent) | "
                 "%d named character(s) on a kind the engine's own filter rejects (last kind=%d) | "
                 "%d collected treasure (%d award record(s) held)",
                 s_dropAbsent, s_absentSpared, s_dropOwnParty, s_dropOddKind, s_lastOddKind,
                 s_dropTaken, TreasureState::RecordedCount());
        Log::Write("NAV", dm);
    }
    // EVERY PRUNE NAMES ITSELF, capped at 8 per scan. This is the falsifier that matters: the risk of
    // a presence test is that it removes something the player still needs, and a bare count could not
    // tell "one corpse" from "one NPC we just deleted by mistake".
    for (const std::string& al : s_absentLines) Log::Write("NAV-DIAG", al.c_str());
    // And every SPARE names itself too, capped at 4 -- the other half of the same falsifier. A
    // `spared:` line is an object the shipped build was deleting; no `spared:` line anywhere in a
    // session that visits a gate crystal means S153 fixed something else.
    for (const std::string& sl : s_sparedLines) Log::Write("NAV-DIAG", sl.c_str());
    // ...and so does every object the name-or-interaction rule deleted. `s_dropPayload` is the
    // alarm; these lines are what it is pointing at.
    for (const std::string& dl : s_droppedLines) Log::Write("NAV-DIAG", dl.c_str());
    {
        const int dropped = s_dropKind1 + s_dropKind5 + s_dropOther;
        if (dropped > static_cast<int>(s_droppedLines.size())) {
            char dm[160];
            snprintf(dm, sizeof(dm),
                     "dropped: %d more not printed (cap %zu) -- raise kDroppedLineCap to see them",
                     dropped - static_cast<int>(s_droppedLines.size()), kDroppedLineCap);
            Log::Write("NAV-DIAG", dm);
        }
    }

    if (s_charByName > 0 || s_poolKind5 > 0 || s_dropKind1 > 0 || s_dropKind5 > 0 ||
        s_dropOther > 0 || s_namelessAct > 0 || s_poolOverlap > 0 || OddSlotWins() > 0) {
        // 768, not 512: the line already measured 507 characters in the field before this session
        // added two more counters to it. snprintf truncates silently, and the counters at the END are
        // the falsifiers -- losing them is how a diagnostic starts lying by omission.
        char im[768];
        snprintf(im, sizeof(im),
                 "inclusion: nameless dropped: kind1=%d kind5=%d other=%d, of which %d carried a "
                 "PAYLOAD ID (must be 0 -- non-zero means a real gimmick was deleted) | "
                 "%d kept while nameless BUT INTERACTIVE (should be 0; each is dumped) | "
                 "%d character(s) admitted by NAME ONLY (no interaction flags) | "
                 "%d handle-table object(s) also in the ACTOR POOL (%d of them named), of which "
                 "%d DROPPED as own party and %d re-filed NPC->Enemy by faction | "
                 "%d actor-pool entr(ies) skipped as KIND_DEAD(5), which debug.md records as NPC | "
                 "%d spoke the PERSONAL npcdic name, %d of them already introduced (%d newly revealed)",
                 s_dropKind1, s_dropKind5, s_dropOther, s_dropPayload,
                 s_namelessAct, s_charByName,
                 s_poolOverlap, s_poolOverlapNamed, s_poolParty, s_poolFoe, s_poolKind5,
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
