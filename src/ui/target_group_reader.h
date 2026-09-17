#pragma once

// The battle TARGET LIST's four groups -- Foes, Party, Reserve, Allies -- and the one of them no other
// reader can speak: RESERVE, the party members who are not in the active party (roster slots 4-8).
//
// Aiming an action (an item most of all) puts up a target list, and L1 / R1 step it through whichever
// groups that action may target. Two things were silent (reported at the close of S183):
//   * the GROUP itself -- nothing said which list the press had landed on;
//   * every RESERVE row. Foes / Party / Allies hold field units, so the nameplate path in
//     battle_target_reader.cpp already reads their highlight. Reserve members are not on the field,
//     have no nameplate, and the list's own row was never resolved.
//
// What is said, and when:
//   * on a group SWITCH only (never on entering targeting): the game's own group title, e.g.
//     "Reserve", interrupting. Whatever row the new list lands on then QUEUES behind it.
//   * on each Reserve row: "<name>, HP <cur>/<max>" -- word for word the ally target line.
//
// All four hook points live in the battle command controller family; the model and every offset is in
// the .cpp header and GameArchitecture.md "Battle target groups (S184)".
namespace TargetGroupReader {

// Installs the group-switch hook (FUN_0027b430). Called from MenuReader::Init.
bool Init();
void Shutdown();

// IngameMenuReader::HookedBcmdCtrl (FUN_002778c0) calls these around the original, for EVERY message.
// BEFORE is where the title speaks: the original sends the new list's first-row focus synchronously,
// so a title spoken after it would cut that row off. AFTER only logs the game's own title write
// against what was spoken. Game thread.
void BeforeCtrlMessage(void* ctrl, int msgId, void* msg);
void AfterCtrlMessage(void* ctrl, int msgId);

// IngameMenuReader::OnBattleCommandFocus calls this first. TRUE = the panel is the Reserve target list
// and this reader owns the row (spoken, or silent with the reason logged); FALSE = not ours, carry on.
bool OnPanelFocus(void* panel, int index);

} // namespace TargetGroupReader
