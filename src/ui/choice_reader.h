#pragma once

#include <cstdint>
#include <string>

// The FIELD DIALOGUE / CHOICE window: the Hunt notice board, and any mid-dialogue prompt that
// offers a cursored list of options.
//
// WHY THIS EXISTS: the board was the last silent cursored surface. It is NOT painted through the
// universal list painter FUN_002d28e0, so TextCapture has no rows for it and the generic content
// path had nothing to say. The tester's probe run caught it live as window class FUN_002a6190
// emitting an ordinary FUN_00247510 msg 0x8000 with the row index -- the dispatch MenuReader
// already hooks. Only the text lookup was missing.
//
// WHERE THE OPTIONS LIVE -- derived from FUN_002a5590 + FUN_003ffdf0 and then CONFIRMED against a
// raw byte dump of a live notice board. The block is inside the MESSAGE the telop setter delivers
// (NOT on the window: window+0x1A8 reads null here). It follows the question and the three column
// headers, which is why the message decodes to the innocent-looking
// "Which bill would you like to read?\nMarkRankStatus" and stops: GameText::ControlLength(0x0E)
// returns -1, "length unknown -- stop, never guess". FUN_002a5590 locates the same marker with
// `xmlStrchr(text, 0x0E)` and stores its count byte at window+0x3C7 (observed 32, matching the
// dump's `a0 & 0x7f`). The block, with `m` pointing at the 0x0E byte:
//
//     m+0            0x0E
//     m+1            u8 & 0x7F   option count
//     m+2, m+3       u8 & 0x7F   (default / selected indices)
//     m+4            u8 flags    bit0 = a per-option bit table follows
//     m+5 ...        that table, ceil(count/7) bytes, only when bit0 is set
//     then           length-prefixed strings: [len & 0x7F][len codec bytes] ..., 0x00 terminates
//
// A ROW IS MULTI-COLUMN: `<Mark> 0x0A <Rank> 0x05 <0F 2E nn 90>` -- confirmed live as `Thextera|I`,
// `Flowering Cactoid|I`, `White Mousse|V`, `Ring Wyrm|III`. Both 0x0A and 0x05 decode to NOTHING
// (0x05 is deliberately excluded from GameText's space controls), so a straight Decode yields
// "ThexteraI". The columns are split here and rejoined with ", ".
//
// The trailing `0F 2E nn 90` is the STATUS -- a substitution, not an icon. (STRUCK: an earlier note
// here called it a per-row icon "carrying no readable status", reasoning that its parameter merely
// increments with the row. It increments because it IS the row's argument index. The screenshot
// showing "Available" on screen is what refuted that.) `nn & 0x7F` is the index into the inline
// argument table; ResolveArg reads it. The same escape supplies the bill number and status on the
// detail page (`0F 2E 80 80`, `0F 2E 81 90`), where the second byte distinguishes numeric from
// string slots.
//
// The count byte is a CAPACITY (32 on a board with ~10 bills); the real list ends at the 0x00
// length terminator, which is what bounds the walk.
//
// This is ALSO why the mod only ever spoke the question: GameText::ControlLength(0x0E) returns -1
// ("length unknown -- stop, never guess"), so Decode halts at the marker. That is still the right
// behaviour for Decode; the block is parsed here instead, where its layout is known.
//
// A NOTE ON A STRUCK CLAIM: probe_dialogue_choice.js v1 recorded "a choice arrives as ONE codec
// string with an embedded 0x0E option block" as REFUTED. That refutation was about the
// FUN_0057c480 mid-dialogue popup, tested through the telop hook. For THIS window class the 0x0E
// model is what the game's own parser does. Both can be true; do not re-strike this one.
namespace ChoiceReader {

// The message on screen and WHICH BYTE OFFSET into it is the page being shown -- both read straight
// off the widget the game is laying out (`base` = widget+0x28, `byteOffset` = widget+0x8A). Called
// by DialogueReader, which is the only feeder.
//
// A message can carry SEVERAL option blocks -- the notice board's mark list is at the top, and the
// bill detail's "Will you go and speak to the petitioner?" Yes/No is a later page of the very same
// string -- so the block search has to be scoped to the current page. It is scoped by the game's own
// cursor: this replaced a NoteMessageText + NotePage(pageIndex) pair where the index was counted
// from observed keypresses and could drift out of step with the box. The offset cannot drift.
//
// The option block lives in this string; the Status column's values do NOT -- they are substitution
// arguments read from an inline table on the window itself (see choice_reader.cpp ResolveArg).
//
// STRUCK: "the setter's 4th argument is the argument block". It is null on this path, and so is the
// window+0x1A8 copy FUN_002a35b0 makes of it -- both were measured. The real table is param_7 of
// FUN_002b32d0, which Ghidra does not render at the call site.
void NotePage(const uint8_t* base, size_t byteOffset);

// Is this the field dialogue / choice window class (obj[0] == FUN_002a6190)?
bool IsChoiceWindow(void* owner);

// A row was highlighted (FUN_00247510 msg 0x8000) -- the notice board's navigation. Returns true
// when it spoke.
//
// TWO DETECTORS, ONE SPEAKER. This and the per-frame tick both exist because neither covers both
// cases: only this one sees the board's navigation index, and only the tick sees an in-dialogue
// choice (which sends no message). They cannot be told apart by class -- the Yes/No widget IS this
// window's embedded list block -- so whichever one actually fires for the current message/page
// claims it, and the other stands down. Both then emit through the SAME choke point, so the
// queue-vs-interrupt policy and the line format live in one place. An earlier build had them
// speaking independently with different policies: they raced, and the plainer line won.
bool OnFocus(void* window, int visibleIndex);

// The MID-DIALOGUE CHOICE widget -- the "Will you go and speak to the petitioner?" Yes/No and the
// in-conversation option prompts. A DIFFERENT object from the notice board's window, and the reason
// both were silent: it polls the raw input globals itself inside a per-frame state machine
// (FUN_002a9980) and emits NO FUN_00247510 message at all, so neither the dispatch hook nor the
// generic content path ever hears from it.
//
// Everything needed is on the widget (FUN_002a8c50:77 gives the first two):
//     widget+0x28  const uint8_t*  message text base
//     widget+0x8A  u16             CURRENT BYTE OFFSET into it -- the page position, exactly
//     widget+0x58  i16             cursor index
//     widget+0xA2  u8              option count (written by FUN_002a8c50 from the 0x0E header)
//
// Install() hooks FUN_002a9980 and speaks on cursor CHANGE. Returns false if the hook fails.
//
// NOTE that FUN_002a9980 is slot 2 of the text dispatch table and exists ONLY on choice-capable
// widgets (type 0); a plain dialogue box has null there. That is why this tick can drive an option
// cursor but could never page dialogue -- pagination hangs off slot 0, FUN_002a8c50, in
// `ui/dialogue_reader`.
bool Init();
void Shutdown();

} // namespace ChoiceReader
