#pragma once

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
