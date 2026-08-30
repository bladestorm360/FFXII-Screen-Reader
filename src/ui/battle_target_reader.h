#pragma once

#include <cstdint>
#include <string>
#include "navigation/nav_types.h"   // FVec3

// Reader for the battle TARGET-SELECTION readout — announces the unit the target cursor is on
// when you aim a battle action (Attack / Magick / Item) at a foe or ally.
//
// Target selection lives on the battle-state object P (DAT_0209be80): the highlighted target
// handle at *(int)(P+0x9FD8), gated by *(P+0x10f78)!=0 while selection is active. The reticle
// path (FUN_005528c0) and the acting-char controller (DAT_0209ac30+0xde0) were both DISPROVEN.
// Two hooks drive it: FUN_002bfd20 (the target nameplate render, which tells us the current unit)
// and the nested FUN_00329220 (per-unit vitals snapshot); we announce the current target on change.
//
// Speech mirrors the top-left TARGET INFO panel (NEVER the lower-right HUD):
//   - enemy = "<name>, HP <pct> percent"  (percentage from real cur/max)
//     ...unless LIBRA is up, when it reads "<name>, HP <cur> of <max>" — the game makes exactly the
//     same swap on exactly the same condition (FUN_002c0400 draws digits or blanks them off the
//     flag FUN_002bfd20 builds from FUN_00290100). See LibraActive() in the .cpp.
//   - ally  = "<name>, HP <cur> of <max>" — "ally" includes the player-controlled leader
//     (self-target, def+5==0), not only party-side scene-kind==3.
// Names carry the instance letter ("Dire Rat B") — every enemy name in this file comes from
// BattleState::DisplayNameForActor, and there is exactly ONE naming path here on purpose.
// Memory-only, SEH-guarded reads; the name is the game's own (decoded via GameText).
namespace BattleTargetReader {

// Installs the vitals-snapshot hook (FUN_00329220). Call once from MenuReader::Init.
bool Init();
void Shutdown();

// On-demand read of the ACTIVE battle target's live world position + name, for the `p`-key route
// (nav_commands) and the pad's R1 in combat. Gates on the LIVE game target-selection state
// (DAT_0209be80 gate + selected handle read at call time) — NOT on cache age — then re-resolves a
// fresh position from the cached target. Returns false when there is no active target (or not in
// battle), INCLUDING when the selected unit has died. Thread-safe (called from the input thread;
// all reads SEH-guarded).
//
// "ACTIVE", NOT "LOCKED" (renamed S174). This was `GetLockedTarget`, and the name cost a session:
// it reads the target the game is acting on, which needs no battle menu open and is NOT the L2
// hold-to-face LOCK-ON. `audio_clips.h` had the right word for it (`ActiveTarget`) all along.
bool GetActiveTarget(FVec3& posOut, std::wstring& labelOut);

// `;` — speak the ACTIVE COMBAT TARGET's status (name + vitals): the target the leader is committed
// to acting on. **SILENT in every other case** — no commitment, a merely browsed cursor, or out of
// battle entirely. It does NOT say "No target"; it says nothing (user instruction, release 0.1).
//
// OUT OF BATTLE THIS KEY DOES NOTHING. That is the point of it — do not "restore" a field-cursor
// readout or a spoken no-target message. Note this is a NARROWER contract than GetActiveTarget
// above, which still accepts a browsed target for routing; the two callers differ deliberately.
// Same liveness rules and formatting as the automatic target-change announcement.
// Thread-safe (input thread).
// TRUE only if it spoke. `;` falls through to InteractTarget::SpeakCurrent() on false, so the same
// key reads the battle target in combat and the field interaction target outside it.
bool SpeakTargetStatus();

// `o` — the LIBRA readout for the enemy the cursor is on: HP as numbers, Level, MP (only when the
// game's own MP-gauge guard says the unit has one), any statuses, and the ELEMENTAL WEAKNESSES —
// all of it in the game's own words.
//
// Three outcomes, and the middle one is the reason this returns bool:
//   - no enemy target        -> SILENT, returns false; `o` keeps its existing meaning (help text).
//   - enemy, but Libra down  -> speaks "Libra not active." and returns true.
//   - enemy, Libra up        -> speaks the readout and returns true.
//
// The weakness clause mirrors the game's own "Weak:" row, which Libra ramps into view on the target
// panel: mask at `BtlChr+0x40`, names via `BattleState::ElementNames`, label from message `0x2331`.
// It is OMITTED for a unit the game marks Libra-proof (marks and bosses, the "????" case — extended
// status bit 41), because the mod must never out-reveal the screen. Only weaknesses are spoken:
// Absorb / Half / Immune belong to the equipment panels and are not part of what Libra shows.
//
// With autodetail ON the same readout is also volunteered on each target change, queued behind the
// short line; the "Libra not active" line is NEVER part of that path. Thread-safe (input thread).
bool SpeakTargetDetail();

} // namespace BattleTargetReader
