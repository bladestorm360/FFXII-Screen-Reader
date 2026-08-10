#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "navigation/entity_list.h"      // Category
#include "navigation/entity_classify.h"  // ResolveObjectName / InGimmickBand / CategoryWord / …
#include "navigation/nav_types.h"

// Builds the list of live interactive field objects: the handle-table walk, the combatant pool, the
// map exits, and the npcdic name/category classification that labels them.
//
// Split out of entity_list.cpp (883 lines). Finding things and TRACKING a cursor over them are
// different jobs; they were fused only because the scanner wrote the module's file-scope vector
// directly. Build() takes its destination by reference instead, so this module holds no state and
// the caller keeps ownership of the lock.
//
// Pure, SEH-guarded memory reads. Every LABEL is the game's own text (npcdic, or the map's own
// fieldsignmes string); the npcdic id bands only drive the category FILTER, never the spoken words.
namespace EntityScan {

// A live interactive field object. Identity is the SCENE OBJECT pointer, which is stable
// while the map is loaded (the handle table holds it from load to teardown).
struct Entity {
    void*        sceneObj = nullptr;   // handle-table scene object (identity; nullptr for fixed exits)
    uint32_t     flags    = 0;         // *(sceneObj+0x1C): FLAG_TALK / FLAG_ACTION -- MODE, not identity
    int16_t      nameIdx  = 0;         // *(sceneObj+0x102): npcdic id (>=0) / custom (<0)
    uint8_t      kind     = 0;         // *(sceneObj+0x0E) & 0xF: the engine's object kind (1 person, 5 gimmick)
    EntityList::Category category = EntityList::Category::Object;
    std::wstring label;
    FVec3        pos;
    float        dist2D   = 0.0f;      // to player, refreshed per command
    bool         fixed    = false;     // exit/map-jump: fixed world pos, no scene node (don't refresh via +0xB8)
    bool         noBearing = false;    // pos is NOT world-space (connection-DB exits carry map-atlas offsets)
                                       // -> speak the label only, never a fabricated direction
    bool         available = true;     // the game would let the player interact with it RIGHT NOW
                                       // (memory-only replica of FUN_002675c0). False = story-gated /
                                       // not yet usable. Non-interaction entries (exits, combatants)
                                       // are always true so no filter can hide them.
    // A `+0x70` field-sign record sits on this object, i.e. the map script bound it to a location jump
    // (`setfieldsignlocationjumpinfo`) -- it is a DOORWAY, not a decorative sign. The engine treats both
    // as action targets with identical flags, so this is the only sound way to tell them apart.
    bool         doorway   = false;
    // A text-only twin -- same game-given name, no location jump of its own -- was found repeating
    // this doorway's name, i.e. the doorway has a SHOPFRONT SIGN beside it. That is what separates a
    // shop from an ordinary door or gate, and it is the pairing TagDoorwaysAndDropSignTwins already
    // finds in order to delete the twin; before this flag the fact was computed and thrown away.
    //
    // Evidence caveat worth keeping visible: the pairing is confirmed for East End's shops (every
    // doorway matched a sign within ~2 m, every twin 6-15 m away). It is NOT established that a
    // non-shop door never has a same-named placard. The drop line at the bottom of that function
    // logs every pairing unconditionally, so a session's log is the falsifier -- read it before
    // treating Category::Shop as settled.
    //
    // THE FALSIFIER FIRED (Session 116 play test, fixed 117): map 569 listed a bare unlabelled door
    // as a Shop and listed NO doors, because the pairing had no distance test whatsoever and fused
    // two generic `"Door"` objects **90.06 m apart**. Both of the tests that now gate it --
    // kTwinNearDist and kTwinNameMaxObjects below -- were missing, and the tester read the result
    // before the log did. **A promotion is only as good as the pairing under it**; this flag is set
    // in exactly one place, and that place must stay the only one.
    bool         hasNameSign = false;
    // `label` is the game's own string (npcdic or the map's fieldsignmes text), not the category-word
    // fallback. The sign-twin drop compares labels, and an unnamed object whose label is merely the word
    // "Interactables" must never match another unnamed object.
    bool         gameNamed = false;
    // `label` BEFORE NumberDuplicateLabels appends its " 1" / " 2" suffix, i.e. the words the number was
    // assigned under. EntityLabels keys its store on this, so it must not drift: reading the suffixed
    // `label` back would make "Nomad" and "Nomad 2" different identities and the number could never be
    // found again. Empty until the label settles, at which point it mirrors `label`.
    std::wstring baseLabel;
    // This entry is a map-jump TRANSITION whose position is the trigger surface itself (exit_scan.cpp),
    // so arriving at it IS crossing it. The planner uses this to say "At the exit" instead of grinding
    // out two-metre legs when the player is already standing on the seam.
    //
    // (It replaces a `crossRad` heading that was spoken as "walk east" in S59 and refuted in play. There
    // is no crossing direction to derive any more: the route ends ON the trigger.)
    bool         isTransition = false;
    // THE SEAM'S MAP-JUMP GROUP, or 0 when this is not a walk-onto surface (Session 98).
    //
    // `pos` above is ONE POINT on a surface that can be 27 m across, chosen as the nearest tagged
    // VERTEX to the player. That is the right answer for "how far away is this exit" and the wrong
    // one for "where should the route end": straight-line-nearest is not walking-nearest, and on map
    // 315 the nearest vertex was the corner of the strip that the walkable approach reaches last.
    // The route validated 21 of 22 legs, drove 20 m ALONG the exit surface to get to that corner,
    // and was reported as "No path" 16.4 m short.
    //
    // The group is the handle back to the whole surface: `MapQuery::CachedMapJumpSurfaces` turns it
    // into the poly set that IS the destination. Carried here rather than re-derived from the goal
    // poly's flags, because bits 3-6 are both the map-jump group and the index into `FUN_00232020`'s
    // group override bank -- an override can rewrite the very bits the group would be read from, so
    // a group taken from EFFECTIVE flags is not the group the seam sweep bucketed by.
    //
    // 0 for everything that is not a transition, which keeps every other route on its old path.
    int          seamGroup = 0;
    // The actor pool ANSWERED for this object this scan, i.e. BuildLocked found its scene object in
    // s_poolObjs and called FactionOf on the matching actor. Distinguishes "the pool said not a foe"
    // from "the pool said nothing", which the category alone cannot express.
    //
    // Why it has to exist: on the field, `Enemy` is reachable by exactly ONE route. The classifier
    // returns NPC for anything `isCharacter` (entity_classify.cpp:213 -- enemies are characters too),
    // and ScanCombatants, the only other pass that assigns Enemy, runs AFTER the handle-table loop and
    // skips anything AlreadyListed. So the faction override is the whole mechanism, and a single scan
    // where the pool read comes back empty silently re-files every enemy on the map as an NPC. Since
    // every cycle keypress rebuilds the list from scratch, that verdict is not sticky -- the grace
    // window carries entities the scan MISSED, not categories the scan got wrong.
    bool         factionVerdict = false;
    // When this object was last actually reported by a scan (GetTickCount64). The handle table streams
    // objects in and out, so entity_list keeps a recently-missing entity listed for a grace window
    // rather than deleting somebody the player is walking toward. 0 = never confirmed.
    uint64_t     lastSeenMs = 0;
    // Where the object sits in the scene-object handle table, and the interaction payload ids the engine
    // would run. Diagnostics only -- these are what identify an object the game gives no name to.
    uint8_t      container = 0xFF;
    uint16_t     slot      = 0xFFFF;
    uint16_t     actionId  = 0xFFFF;   // sceneObj+0xCC (0xFFFF = inherit from the map's object record)
    uint16_t     talkId    = 0xFFFF;   // sceneObj+0xDC
};
// Sanity bound on an exit's distance from the player -- rejects garbage positions. Shared with the
// per-area diagnostic dump so both use one number.
constexpr float kExitMaxDist = 2000.0f;

// How near a `+0x70` field sign must be to a `+0x54` jump slot to be describing the same doorway. A
// sign marks the "-> area" arrow and the slot is the volume you step into, so they are never coincident:
// measured 3.6-6.4 m apart on every East End district door, against ~25 m to the next-nearest door.
constexpr float kSignMatchDist = 8.0f;

// ---- Which scene object a `+0x70` field-sign record is describing --------------------------------
//
// STRUCK (Session 92): `kSignObjectDist = 2.5f`, "how near a field-sign record must be to a scene
// object for that object to BE the doorway", on the evidence "every East End shop doorway matched
// inside ~2 m". That radius is REFUTED, and by the constant ten lines above it: `kSignMatchDist`
// already records these records sitting **3.6-6.4 m** from the doorway they describe, because a
// record marks the "-> area" ARROW and not the thing you press. Two constants for one geometric
// fact, and the tighter, wrong one was the load-bearing one. Measured on the tester's Rabanastre
// map (map 306), both gates fell in the gap between them and stayed Interactables:
//
//   g0[0] (119.95,-10,127.00) destIdx=20 -> "South Gate" (124.00,-10,127.00)  = 4.05 m   MISSED
//   g0[1] (138.24,-10,140.53) destIdx=21 -> "Lowtown"    (137.82,-10,144.00)  = 3.50 m   MISSED
//   g2[0] (115.00,-10,151.00)            -> gate crystal (115.00,-10,151.00)  = 0.00 m   tagged!
//
// Lowtown's own map passed only because its records happen to land inside 2.5 m -- the threshold was
// never right, it was lucky.
//
// So doorway matching is NOT a radius test. Two rules replace it, both from the data above:
//
//   1. ONLY GROUP 0. `EnumerateFieldSignRaw` walks every `+0x70` group, and only group 0 holds
//      press-Enter doorways: group 1 is the walk-onto map-jump surface (`g1[0]` above is the one
//      record whose `areaId` resolves, 14, matching the exit surface at z 198-225 -- already the
//      Exit category), group 2 is a GATE CRYSTAL's own teleport record sitting exactly on the
//      crystal, group 3 is arrival markers. Consulting all of them is what tagged the crystal.
//   2. NEAREST WINS, not everything-in-radius. A doorway record describes exactly ONE door, so each
//      record claims its closest eligible object. `kSignMatchDist` is then a sanity BOUND rather
//      than a discriminator -- and the separation is wide enough for that to be unambiguous: 4.05 m
//      to the right object against 24 m to the next candidate here, 3.6-6.4 m against ~25 m on East
//      End. `g1[0]` was 47 m from anything, which is what a record with no object looks like.
constexpr int   kSignDoorwayGroup = 0;
// Two PEOPLE this close together are one actor registered twice, not two NPCs. Deliberately tiny:
// the rule is "literally the same coordinates", because at any real separation they are two people
// the game happened to give one name, and deleting one of those cost a tester a story NPC.
constexpr float kStackedDist    = 0.05f;

// ---- How far apart a shopfront's SIGN and its DOORWAY may stand (Session 117) --------------------
//
// THE INTERACTABLE HALF OF THE TWIN FILTER HAD NO DISTANCE TEST AT ALL. It matched on label plus
// `doorway` and nothing else, and on map 569 it paired two generic `"Door"` objects **90.06 m apart**,
// deleted one, and promoted the survivor to Category::Shop -- so the Royal Palace's Lower Halls
// listed a bare unlabelled door as a shop and listed no doors. A shop SIGN stands BESIDE its doorway;
// that proximity is the filter's entire premise and it was the one thing unchecked.
//
// 20 m, from the measurement the filter was built on: on East End the two objects of each shop pair
// sit **6-15 m apart** (see the note above TagDoorwaysAndDropSignTwins), so the bound has to admit 15
// and reject 90. It is a sanity BOUND, not a discriminator -- the distinctiveness rule below is what
// separates a shopfront from two ordinary doors that happen to share a word.
constexpr float kTwinNearDist   = 20.0f;
// ...AND THE SHARED NAME MUST BE DISTINCTIVE, which is measured, not listed. A shopfront's name is
// carried by exactly the two objects that make it up; `"Door"` is carried by every door on the map
// (568 has three). So the pair is only fused when the map holds NO THIRD object of that name --
// nothing is compared against a word list, and no label is invented or assumed generic.
//
// BOTH halves gate the DROP, not just the Shop promotion: deleting a real door because it stands near
// a bound one and shares the game's generic word for "door" costs a blind player a way out of the
// room, which is the same failure this filter has had twice before with NPCs.
constexpr size_t kTwinNameMaxObjects = 2;
// STRUCK (this session) -- `kFloatingDrop = 2.0f` and `kNpcReachTol = 1.5f`, the thresholds
// DropUnplacedCharacters judged unnamed NPCs by. Both were read off ONE map's object dump, and the
// pass only ever fired on that map: Nomad Village's three stacked bodies float at Y=6.06, while the
// identical three on Lowtown, Eastgate and Garamsythe stand on the floor and inside the reachable
// set, so the same code kept them. Deleted with the pass. Nothing about an object's POSITION decides
// whether it is listed any more -- whether the game names it does.

// ---- POST-SCAN PASSES (entity_postscan.cpp) -----------------------------------------------------
// Everything that runs over the FINISHED object list rather than finding objects. Declared here so
// the two translation units cannot drift apart on a signature.
//
// Note the deliberate asymmetry between the two dedupe passes: FFXII reuses display names constantly
// (109 npcdic ids all read "Rabanastran"), so NumberDuplicateLabels NUMBERS same-named objects while
// TagDoorwaysAndDropSignTwins DELETES one -- and the latter is scoped hard, because getting that
// scope wrong deleted four NPCs on Nomad Village, one of them story-critical (Session 77).
uint16_t ObjectHandle(void* sceneObj);
void LogObjectDump(const std::vector<Entity>& out);
void DropShadowRegistrations(std::vector<Entity>& out);
void TagDoorwaysAndDropSignTwins(std::vector<Entity>& out, bool logDetail);

// The category-word fallback for anything the game gave no name. Runs LAST, after doorway tagging,
// because "Sign" depends on `Entity::doorway` — which only exists once TagDoorways has run. It used
// to sit inline in BuildLocked, where `doorway` is always false, so the word was unreachable.
void ApplyFallbackLabels(std::vector<Entity>& out);

// ---- What this scan DELIBERATELY removed --------------------------------------------------------
// Every drop pass records the scene object it erased, and `Build` publishes the list.
//
// THE CALLER'S GRACE WINDOW MUST CONSULT IT. RescanLocked carries an entity over when its scene object
// is missing from the fresh list, to survive the handle table streaming an object out for a frame --
// and it cannot otherwise tell "the engine stopped reporting it" from "we just filtered it out". A
// filtered object is a LIVE engine object that Build will keep finding and keep dropping, so carrying
// it re-admits it on every rescan while Build logs the drop again: a filter that logs success and
// changes nothing. A timeout is the right instrument for an ABSENCE; a deliberate removal needs an
// explicit signal, which is what this pair is. (Before S148 this note also said such an entry could
// never age out at all. That was true of the time, but the cause was RefreshPositionsLocked re-stamping
// `lastSeenMs` off a still-readable transform -- a bug, now fixed there, not a property of filtering.)
void NoteFiltered(void* sceneObj);
bool WasFilteredThisScan(void* sceneObj);

// These two run in the CALLER (EntityList::Internal::RescanLocked), after it has merged its
// grace-window survivors -- not inside Build. Numbering is assigned within one scan now, so it has to
// see the same list the player hears or a carried entity keeps a suffix the survivors have re-used.
// Order matters: player labels first, so a named entity leaves its duplicate group entirely.
void ApplyPlayerLabels(std::vector<Entity>& out);
void NumberDuplicateLabels(std::vector<Entity>& out, bool logDetail);

// Slack allowed when testing an exit against the reachable set. Door triggers sit ON the map seam and are
// routinely a cell or two past the last cell with a floor sample, so a zero-tolerance test would report
// perfectly ordinary district doors as unreachable and hide them.
constexpr float kExitReachTol = 4.5f;

// Rebuild `out` from scratch: handle-table objects, then combatants, then map exits. Returns the
// count; empty and 0 when the field isn't active. The caller holds its own list lock.
//
// `outDetail` receives the once-per-map diagnostic latch. The caller needs it because the two LABEL
// passes below no longer run in here -- they run after the caller has merged its grace-window
// survivors, so that the list which gets numbered is the list the player actually hears.
int Build(std::vector<Entity>& out, bool* outDetail = nullptr);

// Bitmask of currently-active handle-table containers (bit c set iff container c is active).
// Memory-only; touches only the handle table, so the per-frame tick can detect a container-set
// change without paying for a full rescan.
uint32_t ActiveContainerMask();

// (ResolveObjectName / InGimmickBand / CategoryWord / ClassifyByNameKey / IsInteractionAvailable
//  moved to navigation/entity_classify.h, included above so existing callers are unaffected.)

} // namespace EntityScan
