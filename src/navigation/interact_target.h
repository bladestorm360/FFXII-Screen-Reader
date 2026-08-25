#pragma once

#include <cstdint>
#include <string>

// The game's OWN chosen interaction target -- who Confirm will actually address.
//
// WHY THIS EXISTS (Session 73). The tester stood next to Montblanc, the mod said "right next to
// you", and Confirm kept talking to a Clan Member 1.86 units away instead. Nothing the mod showed
// could explain it, because the mod was reporting PROXIMITY while the game was choosing by its own
// three-gate score. This reads the game's answer directly, so "who am I about to talk to" stops
// being a guess.
//
// THE ENGINE KEEPS EXACTLY ONE TARGET. `FUN_0025b820` rescans every field frame, `FUN_0025bad0` /
// `FUN_0025be50` score the survivors and keep only the minimum, and `FUN_0025d650` resets the
// globals each frame. There is no candidate list and NO CYCLING -- do not add a "next interaction
// target" key, the engine has no such concept (0.98, GameArchitecture.md).
//
// Read-only: every value here is a plain memory read of a global or a scene object. No game call,
// so it is safe from the input thread.
namespace InteractTarget {

// What the game will act on if Confirm is pressed right now.
struct Chosen {
    bool         valid     = false;   // DAT_0209a2aa set AND container/slot both >= 0
    int32_t      container = -1;      // DAT_0209a2b4
    int32_t      slot      = -1;      // DAT_0209a2b8 (index into that container's entries)
    int32_t      mode      = 0;       // DAT_0209a2bc: 10 = talk, 2 = action
    float        score     = 0.0f;    // DAT_0209a2b0 (minimised; ~1e10 when nothing was chosen)
    void*        sceneObj  = nullptr; // resolved from (container, slot)
    std::wstring label;               // resolved display name, may be empty
};

// Read the five globals and resolve the object. Memory-only.
Chosen Read();

// The engine's own VERTICAL BAND for interacting with an object: the player's Y must satisfy
// `lo <= playerY <= hi` or `FUN_0025bad0` rejects the candidate before it is ever scored.
//
// Validated in play (Session 73): Montblanc reported `band [4.63,8.02] centre=6.92 sc=1.00 up=1.10
// dn=0.50 pad=1.79`, and this arithmetic reproduces both bounds exactly. Distance is a CYLINDER --
// Y is excluded from it entirely -- so for a target on a dais or behind a counter, this band is the
// ONLY thing that decides where you have to be standing.
//
// That makes it the right goal test for routing: any walkable cell whose floor lies inside the band
// is somewhere you could stand and interact from. `unconstrained` is set when the target carries
// the skip-band byte (xform+0xDD), in which case lo/hi are widened and impose nothing.
struct Band {
    bool  valid         = false;
    bool  unconstrained = false;   // xform+0xDD set -> the engine skips the band test entirely
    float lo            = 0.0f;
    float hi            = 0.0f;
};
Band ReadBandFor(void* sceneObj);

// `sceneObj+0x03 >> 5`. THERE ARE TWO INTERACTABLE CLASSES WITH DIFFERENT FIELD LAYOUTS, scored by
// two different functions, and reading one class's offsets on the other returns plausible-looking
// floats that are simply wrong. Every interaction read here branches on it.
//   3 = character / NPC   (FUN_0025bad0)
//   1 = gimmick / volume  (FUN_0025be50)
uint8_t ObjectClass(void* sceneObj);

// The engine's own HORIZONTAL INTERACTION REACH for a target -- the quantity `kApproachRadius = 4.0f`
// was invented to stand in for. `FUN_0025bad0` rejects a candidate unless `FUN_003da5a0` returns < 0,
// and that return is `dist2D - (ellipsePlayer + extraPlayer + ellipseTarget + extraTarget)`. Every one
// of those four terms is a plain memory read (see the NavRva XFORM_*_SHAPE block), so the reach is
// exactly replicable -- the "direction-dependent shape queries we do not replicate" note on
// LogGatesFor is out of date.
//
// SELF-CHECKING. `FUN_0025bad0` stores `DAT_0209a2b0 = (dist2D - reach) + dist2D` for whichever
// candidate won, and the mod already reads that as `Chosen::score`. So whenever this target IS the
// chosen one, `2*dist2D - score` is the engine's own answer for the same quantity and `measured`
// carries it. The two agreeing is what lifts the replica to the project's >=0.98 bar; until they do,
// treat `radius` as unconfirmed and do NOT narrow the routing goal set with it.
//
// `radius` is direction-DEPENDENT (both shapes are ellipses evaluated along the line between them), so
// it is only exact for the current relative position. `radiusMin` is the direction-independent lower
// bound -- min semi-axis of each shape plus both extras -- which is what routing wants, because a cell
// inside it is interactable from ANY approach angle.
// THE WHOLE MODEL ABOVE IS CLASS 3, AND `FUN_0025be50` -- the CLASS-1 scorer -- HAS NO RADIUS IN
// IT AT ALL. Its candidate test is, in order: the vertical band (skipped when `node+0x5C` is set),
// the mode bit `node+0x60 >> mode`, the facing cone `FUN_003a1bb0`, and then `FUN_003a1960` --
// which is a bare squared 2D distance kept only to MINIMISE against `DAT_0209a2b0`. Nearest wins;
// nothing is rejected for being far away. So a class-1 target has no engine reach to replicate,
// and the four ellipse terms above are read from the class-3 shape layout on a node that does not
// use it -- the S76 failure, one function away from where S76 already fixed it in ReadBandFor.
// `engineRadius` says which of the two is in front of you; `radius`, `radiusMin` and `passes` are
// meaningless when it is false, and are left at zero rather than filled with the wrong layout.
struct Reach {
    bool  valid     = false;
    bool  engineRadius = true;  // false = class 1: the engine gates on band+cone, never a radius
    float radius    = 0.0f;   // replica of the engine's reach for the CURRENT relative position
    float radiusMin = 0.0f;   // direction-independent lower bound (safe for goal-cell admission)
    float dist2D    = 0.0f;   // horizontal player->target distance, target position offset applied
    bool  passes    = false;  // dist2D < radius, i.e. the distance gate accepts right now
    // Components, so a wrong offset shows up as an absurd term rather than a plausible total.
    float ellipsePlayer = 0.0f, ellipseTarget = 0.0f;
    float extraPlayer   = 0.0f, extraTarget   = 0.0f;
    // Engine cross-check; only meaningful when this target is the chosen one.
    bool  haveMeasured = false;
    float measured     = 0.0f;   // 2*dist2D - score
};
Reach ReadReachFor(void* sceneObj);

// The FIELD half of the `;` key. No new binding: `;` is the target-status key, and it is
// STRUCTURALLY silent outside battle (battle_target_reader.cpp -- ResolveTarget needs a commitment
// or an open select UI, neither of which exists in the field). So `;` already carries the context
// gate this needs, and its field-side silence was a dead slot. `OnNavKey` tries the battle reader
// first and calls this only when that one had nothing to say.
//
// Speaks who Confirm will address, e.g. L"Talk: Montblanc" / L"Action: Save Crystal".
// SILENT when nothing is in reach or the object has no readable name -- an empty reach is the
// normal state while walking, and filler speech is a standing violation.
void SpeakCurrent();

// Diagnostic, file-only: the chosen target plus, for one candidate object, a replica of the three
// geometric gates the engine applies (horizontal distance, vertical band, facing cone). Called from
// the `'` dump for every listed object, so the log shows WHICH gate rejects a given NPC.
void LogChosen();
void LogGatesFor(void* sceneObj, const char* label);

} // namespace InteractTarget
