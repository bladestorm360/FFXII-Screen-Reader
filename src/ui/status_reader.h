#pragma once

#include <cstdint>

// Status-screen virtual buffer (FF1-style). The FFXII Status screen is a STATIC display -- there is
// no in-game cursor to move through it -- so the mod exposes its data as a navigable virtual buffer
// read with the arrow keys (Up/Down = entry, Left/Right = group, Home/End = ends).
//
// Controller: FUN_002c2320 (RVA 0x1A2320), parked at menuCtx+0x140. This is ONE container shared
// with the Equipment screen; the two are told apart by the pause command id cached at ctrl+0x160 --
// 0x4b4 = Status, 0x4b6 = Equipment -- so this reader gates on 0x4b4 and ignores Equipment entirely.
//
// The buffer activates on category 1 and is torn down on category 0x12. Measured (Session 71): a
// Status visit delivers 0x6, 0xe, 0x8, 0x1, 0xb, 0x9, 0x13 exactly ONCE each, then repeats
// 0x2/0x19/0x3/0x4 per frame forever. Category 1 is the only one-shot at which the attribute panel
// (menuCtx+0x138) is already built, which is why it -- not 0xe -- is the activation edge.
//
// Data is snapshotted on the GAME thread (message-book lookups are unsafe off it) and the input
// thread only ever touches the finished strings -- the same threading split the combat log uses.
// Because category 0x12 was never observed live, ShouldConsume() ALSO re-validates
// *(menuCtx+0x140) == the container we activated on, so a missed teardown can never leave the
// buffer stuck owning Home/End.
//
// SCOPE: the Attributes page ONLY. The Status screen's other two pages -- Magicks, and Technicks /
// Quickenings / Remedy Lore / Espers -- are the SAME page objects the license board's `F` overlay
// uses, and they DO have a browsable in-game cursor here too, so ability_summary_reader speaks them
// exactly as it does from the board. The mode byte menuCtx+0xDE7 says which page is up (0 = ours,
// 1 or 3 = Magicks, 2 = Technicks etc.), and OnMenuNavKey declines every key while it is non-zero so
// the game's own cursor is never fought.
//
// ~~a virtual buffer that enumerated those two pages~~ was built and REMOVED (Session 71): they are
// not static displays, they are cursor lists, and the buffer duplicated what the summary reader
// already does well.
//
// Returning from a summary page is an EVENT, not something to wait on: FUN_002c1a80 (0x1A1A80) is
// the only writer of menuCtx+0xDE7, and it returns 2 exactly when it closes back to the Attributes
// page. The reader rebuilds and re-announces there, so a character switched while a summary page was
// up is picked up too.
//
// CONTRACT: read-only, SEH-guarded memory reads. Game calls (the FUN_002f9860 label lookups and the
// FUN_0035d330 name lookups) happen ONLY on the game thread at snapshot time; the input thread
// touches nothing but finished strings and two guarded memory reads.
namespace StatusReader {

bool Init();
void Shutdown();

// Fired on the INPUT thread when the user presses a buffer-navigation key while the game is
// foregrounded. `vk` is one of VK_UP / VK_DOWN / VK_LEFT / VK_RIGHT / VK_HOME / VK_END. Returns TRUE
// iff the status buffer consumed the key (i.e. the Status page is active) -- the input tracker uses
// the return to decide whether Home/End should fall through to the combat log.
bool OnMenuNavKey(int vk);

// A CLAIMING READER, called from MenuReader::OnFocus's one claim block. Returns TRUE when this
// reader has taken responsibility for the focus event and the generic path must stay silent.
//
// It claims exactly one thing: the focus the L1/R1 character switch CAUSES. FUN_002c2c50 refills the
// ailment grid (menuCtx+0x110) as part of the screen's refresh, the game re-fires focus index 0 on
// that pane, and the generic reader spoke it with interrupt=true in the same millisecond this reader
// spoke the new character's name -- so the name was cut off by "Regen" on every switch. Two speakers
// with two policies on one surface; the notice-board failure again, and the same fix: arbitrate.
//
// NOT a dedup filter and NOT a mute. The claim is a ONE-SHOT armed by the refresh and consumed by the
// first focus event that follows, so it detects a specific transition rather than suppressing a
// repeat -- the ailment pane is player-navigable (the archived logs reach index 1) and must keep
// speaking under the player's own cursor. Those statuses are also already the third group of this
// reader's own buffer, so nothing is lost.
bool TryFocus(void* owner, int index);

} // namespace StatusReader
