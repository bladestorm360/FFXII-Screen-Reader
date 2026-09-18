#pragma once

// THE SOUNDSCAPE — every interactable near the player sounding from where it actually is, on its own.
//
// Each interactable within ~20 steps gets its own repeating voice: its category's sound, panned
// toward where the thing IS right now, quieter the further away it is. There is no cap and no
// scheduler — **every** entity in range sounds, each on its own clock, for as long as it is in range.
// Nothing is spoken and nothing is a route; it is the picture of the room that a sighted player gets
// for free by looking around.
//
// Off by default, and the whole feature is one mod-menu row (`Soundscape`), with a second row for its
// volume. User's instruction, 2026-09-18: *"the soundscape should, of course, be a toggle ... toggled
// off by default"*.
//
// ---------------------------------------------------------------------------------------------
// HOW TWO OF A KIND ARE TOLD APART — TWO CUES, AND THE SECOND ONE IS THE IMPORTANT ONE
//
// The user's own specification, verbatim:
//
//     NPC1: sound plays 1S apart at normal pitch
//     NPC2: plays 1.1S apart at 5% pitch increase
//
// So each entity takes a SLOT among the others of its category — first tracked takes slot 0 — and the
// slot sets both its pitch and its period. Pitch is what the player hears as "these are two different
// NPCs". **The period is what stops them sounding on top of each other**, and it does it without any
// scheduling at all: two voices at 1.0 s and 1.1 s cannot stay in phase, because every cycle pushes
// them 100 ms further apart and then wraps. That is a property of the numbers, not of a sequencer, so
// there is nothing to keep in step and nothing to go wrong when entities come and go.
//
// A SLOT IS OWNED FOR AS LONG AS THE ENTITY IS TRACKED, and this is not a detail. If slots were
// re-derived from live distance every refresh, two NPCs walking past each other would swap pitch and
// period mid-loop — an audible glitch, and it would re-phase them into the collision the slots exist
// to prevent. Slots are handed out on arrival and released on departure, so a voice keeps its
// identity for as long as you can hear it.
//
// ---------------------------------------------------------------------------------------------
// WHAT ELSE THE PLAYER HEARS
//
//   WHICH sound   the entity's EntityList::Category — door, shop, treasure, npc, enemy, save crystal
//                 and so on. One sound per category, from `FF 12 SFX\`, embedded in the DLL.
//   WHERE         panned by bearing, and attenuated + low-passed + pitched down when behind, all of it
//                 AudioEngine's one shared definition (the beacons get the identical treatment).
//                 Computed from the entity's LIVE position at the instant it sounds, so a voice
//                 tracks a walking NPC rather than where that NPC was when it was last listed.
//   HOW FAR       louder as you close. Distance is the cue a pan cannot carry.
//
// ---------------------------------------------------------------------------------------------
// POLLED MONITOR — DOCUMENTED AND EXPLICITLY APPROVED, like the beacon before it
//
// CLAUDE.md: "NO polling, timers, or per-frame checks -- event-driven hooks only. No exceptions
// outside narrow polled-monitor cases that have been documented and explicitly approved." A
// soundscape is definitionally one: there is no game event for "an NPC is now eleven metres away",
// and the continuous readout of that IS the feature. Approved by the user on 2026-09-18 in the
// request that specified it. This is that documentation.
//
// The cost is bounded, and deliberately so, because the standing rule about the game thread
// (CLAUDE.md, and Lessons `L-88`) is absolute:
//   * switched off — a relaxed atomic load and an immediate return, which is the shipped default;
//   * every frame, switched on — a walk of the tracked list comparing one clock each. No reads of
//     game memory at all on a frame where nothing is due;
//   * per voice that comes due — ONE live position read and one ping, with a hard cap on how many may
//     start in a single frame so a pathological crowd cannot become a frame spike;
//   * every ~200 ms — one locked walk of the entity list refreshing live transforms, the same read the
//     `[` / `]` keys already do on every press, to decide who is in range.
// There is NO search, NO flood, NO rescan and NO engine call anywhere in this module.
//
// NO SPEECH. Nothing here speaks, so the no-dedup rule is not engaged at all; diagnostics go to the
// "SCAPE" log category.
namespace Soundscape {

// GAME THREAD. Once per field frame from the FUN_0022a770 hook, after the beacon -- so that on a
// frame where both want to sound, the beacon (which the player asked for with a keypress) is already
// queued on its own voice and the soundscape is layered under it rather than racing it.
void OnGameFrame();

// Called from the game's DirectInput keyboard poll, which keeps running while the field tick does
// not -- menus, pauses, loads. Silences the voices when the field tick has stopped, so a paused game
// is not left with seconds of soundscape still playing over it. Off the game thread: it reads two
// relaxed atomics and clears the audio streams, and touches nothing else. O(1) and returns on the
// first line whenever nothing is sounding, which is the shipped default.
void OnInputPoll();

// GAME THREAD. Drop every tracked voice and silence anything sounding. Called by OnGameFrame itself
// whenever what it is tracking has gone stale -- the row switched off, the map changed, the player
// stopped driving -- and once at teardown, from the same place the other field-tick modules are torn
// down, after the field hook can no longer fire.
//
// It is NOT declared safe from any thread, deliberately: it owns the same non-atomic track list
// OnGameFrame owns, so calling it from the input thread would be a race by construction. Nothing
// needs to -- the setting is read on the game thread, not written into this module by the menu.
void Stop();

} // namespace Soundscape
