#pragma once

// The FIELD DIALOGUE reader: NPC conversations, in-engine cutscene captions and the on-screen
// tutorial banners -- every message the game paginates.
//
// WHY THIS IS ITS OWN MODULE. Pagination used to be driven by watching for Space/Enter on the
// keyboard (`InputTracker::SetConfirmCallback`), so a player on a controller heard page 1 and then
// silence: the mod was observing "a key that usually advances the box", not "the box advanced".
// That entire path is deleted -- there is no keyboard route and no controller route, just this one.
// The page comes from the game's own page cursor, so whatever moved it moves the reader with it.
//
// THE HOOK is `FUN_002a8c50` (RVA 0x188C50), the message widget's text walk. It is slot 0 of BOTH
// live entries in the text-draw dispatch table `PTR_FUN_009164c8` (RVA 0x7F64C8, stride 3 pointers,
// keyed on `widget+0xA3`):
//     type 0 (choice-capable):  slot 0 = FUN_002a8c50, slot 1 = FUN_002a9f00, slot 2 = FUN_002a9980
//     type 1 (plain)         :  slot 0 = FUN_002a8c50, slot 1 = FUN_002aa800, slot 2 = NULL
// That table is also the shape of the old bug: ChoiceReader's tick hooks FUN_002a9980, which is
// slot 2 and exists only on choice widgets, so it could never page a plain dialogue box. Slot 0
// serves both.
//
// WHAT IT READS, all on the widget FUN_002a8c50 receives:
//     widget+0x28  const uint8_t*  message text base
//     widget+0x8A  u16             BYTE OFFSET of the page on screen  <-- the page identity
//     widget+0xA3  u8              dispatch type (0 = choice-capable, 1 = plain)
//     widget+0xB0  u32             state word; low byte = mode (5 = parked at a page break)
//     widget+0x54  i32             why it is parked: 3 = page break, 0x23 = the `0F 23` wait escape
//     widget+0xC0  u32             1 = the message ended (codec 0x00) -- CONSUMED by the call
// `FUN_002a8c50:199-200` is the instruction that advances `+0x8A` past a `0x03` page break, so this
// function is the cursor's WRITER. Hooking it is the event, not a poll of one.
//
// THERE IS NO PAGE INDEX ANYWHERE IN HERE, deliberately. Counting pages assumes one Confirm turns
// one page, and that is false: the first press on a page skips the typewriter reveal without
// turning it. An offset that did not move means no page turned.
namespace DialogueReader {

bool Init();
void Shutdown();

} // namespace DialogueReader
