#pragma once

#include <cstdint>

// WHICH FIELD TREASURE HAS ALREADY BEEN COLLECTED.
//
// THE PROBLEM THIS EXISTS FOR. A collected treasure never left the nav list, so the player kept
// walking to spots they had already emptied (tester, 2026-08-11). The Session 148 stale-entity
// pruner was believed to cover it -- `sceneObj+0x14` bit 0x40, "present in the world" -- and the
// docs said so in three places. That claim was STRUCK in Session 150: treasure objects set that bit
// UNCONDITIONALLY, including slots the game never placed at all (world origin, no walkmap layer,
// `r14=0x70`), so the pruner runs on every treasure and can never fire on one. See
// `nav_rva.h`'s READY_PRESENT_BIT note for the log lines that refuted it.
//
// WHY THE ANSWER IS NOT ON THE OBJECT. The engine never writes a treasure's identity onto its scene
// object. `setuptreasure`'s handler reads the definition record out of the map's SCRIPT BYTECODE
// (a literal decoded by FUN_002650b0), places the object from it, and keeps the id only in the VM
// state block, which it then discards. So there is no field to read back, and no per-object test
// reaches this project's 0.98 bar: the ACTION bit `+0x1C & 0x004` is ~0.55 and would blink LIVE
// treasure out of the list, because `talktreasure` disarms it for the whole message window.
//
// WHAT IS READ INSTEAD -- the award itself, which is an EVENT and carries the record.
// `FUN_002fafd0(def, _, out)` (RVA 0x1DAFD0) is the treasure award, reached from the script native
// at FUN_0050faf0:21 and from nowhere else. It sets the one-time save flag (FUN_0032ad70), clears
// the map's treasure presence bit (FUN_002fb430 on ring DAT_02ec3e60), and rolls the item. It is
// identified beyond doubt by the Diamond Armlet branch -- the only place in the binary that swaps
// the whole common/rare item pair on `BtlChr+0x6B & 2 || +0x7B & 2`.
//
// Its `def` is the same record `setuptreasure` placed the object from, and position comes out of it
// by pure arithmetic (FUN_002faf60): `x = (s16)(def+0x04)/10`, `z = (s16)(def+0x06)/10`, y = 0. The
// object was placed at exactly those floats, so the collected treasure is identified by the
// coordinates the GAME used to put it there -- not by a mod-side guess about which one the player
// was standing near.
//
// DO NOT CONFUSE THIS WITH THE TRAP SYSTEM. `FUN_002fa740` / `FUN_002f8060` / the ring
// `DAT_02ec3ea0` look like treasure and are TRAPS: that path fires per-frame from the actor tick at
// distance <= 1.3 with no button press, applies its effect to every party member in radius, and
// shares ONE model across all 32 slots. `GameArchitecture.md` already recorded its sibling
// `FUN_002f82f0` as the Libra trap-visibility toggle. The treasure ring is `DAT_02ec3e60`.
//
// FAIL-SAFE BY CONSTRUCTION: this only ever removes an entry in response to a real award event. If
// the hook fails to install, or a record's coordinates match nothing, the list behaves exactly as it
// did before -- the failure mode is the old bug, never a live treasure going missing.
//
// PLAY-CONFIRMED 2026-08-11: collected treasure leaves the nav list. That also settles the one link
// the decompile could not -- the scene transform DOES read back the floats the placement wrote, so
// the award record's coordinates identify the object exactly.
namespace TreasureState {

// Installs the award hook. Speaks NOTHING and must never be made to -- it is a state recorder.
bool Init();
void Shutdown();

// Has a treasure been collected at these coordinates on this map? Asked by the entity scan for
// `Category::Treasure` entries only. `mapId` is `MapNames::CurrentMapId()`.
//
// Matching is on X/Z with a tolerance, never Y: the record carries no height (FUN_002faf60 writes
// y = 0) while the listed entity's Y comes from its transform.
bool WasCollectedAt(int mapId, float x, float z);

// How many awards have been recorded for the CURRENT map. Diagnostics only.
int RecordedCount();

} // namespace TreasureState
