#pragma once

#include <cstdint>
#include <string>

// The FIELD DIALOGUE / CHOICE window: the Hunt notice board, the gate crystal's destination list,
// and any mid-dialogue prompt that offers a cursored list of options.
//
// WHY THIS EXISTS: these surfaces are NOT painted through the universal list painter FUN_002d28e0,
// so TextCapture has no rows for them and the generic content path had nothing to say. The options
// live in a 0x0E block inside the message itself; ui/choice_block.h walks it.
//
// THE GAME LAYS THAT BLOCK OUT IN TWO FLAVOURS, and knowing this is the whole design:
//
//   INLINE      FUN_002a9980 -- the per-frame selection tick -- runs the cursor itself in
//               widget+0x58 and the window sends NO focus message at all. Nilbasse's "Take me to
//               Nilbasse.", the gate crystal's Save / Teleport.
//   CHILD LIST  FUN_002a5590 builds a separate list window at window+0xC0 and then sets bit 22
//               (0x400000) of the state word. FUN_002a9980's mode-2 path tests that bit FIRST and
//               returns immediately, so widget+0x58 never moves; the child owns the highlight and
//               announces it as a FUN_00247510 msg 0x8000. The notice board, the 26 teleport
//               destinations, and the Archades "Commit this tale to memory." prompt.
//
// The bit sits at window+0x180, which is widget+0xB0 (0xD0 + 0xB0).
//
// THIS USED TO NEED TWO DETECTORS, and they covered half a surface each: one keyed on widget+0x58
// (blind to every child-list prompt, because the game itself declines to move that field) and one on
// the 0x8000 dispatch (which never fires for an inline one), with a stand-down flag arbitrating
// them. The dispatch detector also read the page from a snapshot DialogueReader pushed in, and a
// page consisting of NOTHING BUT an option block decodes to no text -- so DialogueReader never
// pushed it, the snapshot stayed on the previous page, and the reader searched the wrong bytes for
// as long as the prompt was up. That was the Archades defect: the opening option spoke, no
// highlight ever did.
//
// ONE FIELD COVERS BOTH. Each flavour resolves the highlight through the SAME helper FUN_002b2ce0
// (the hidden-slot walk) and stores the result in the SAME PLACE:
//
//     inline       FUN_002a9980 writes widget+0x54 at the end of every mode-2 tick
//     child list   FUN_002a6190 (msg 0x8000 / 0x8001) writes window+0x124
//     and window+0x124 IS widget+0x54    (0xD0 + 0x54 = 0x124)
//
// So the reader keys on widget+0x54 -- the absolute option slot, already resolved by the game -- and
// there is exactly one detector, the per-frame tick, which fires in both flavours. Gone with the
// second one: the 0x8000 path, its cached message, the arbitration flag, and our re-implementation
// of FUN_002b2ce0.
//
// A ROW IS MULTI-COLUMN: `<Mark> 0x0A <Rank> 0x05 <0F 2E nn 90>` -- confirmed live as `Thextera|I`,
// `Flowering Cactoid|I`, `White Mousse|V`, `Ring Wyrm|III`. Both 0x0A and 0x05 decode to NOTHING
// (0x05 is deliberately excluded from GameText's space controls), so a straight Decode yields
// "ThexteraI". The columns are split here and rejoined with ", ".
//
// The trailing `0F 2E nn 90` is the STATUS -- a substitution, not an icon. (STRUCK: an earlier note
// called it a per-row icon "carrying no readable status", reasoning that its parameter merely
// increments with the row. It increments because it IS the row's argument index. The screenshot
// showing "Available" on screen is what refuted that.) `nn & 0x7F` is the index into the inline
// argument table; ResolveArg reads it.
//
// A NOTE ON A STRUCK CLAIM: probe_dialogue_choice.js v1 recorded "a choice arrives as ONE codec
// string with an embedded 0x0E option block" as REFUTED. That refutation was about the
// FUN_0057c480 mid-dialogue popup, tested through the telop hook. For THIS window class the 0x0E
// model is what the game's own parser does. Both can be true; do not re-strike this one.
namespace ChoiceReader {

// Called by DialogueReader when the message box shows a page -- including a page it cannot speak,
// which is exactly the page an options-only prompt sits on.
//
// It does TWO things and no longer caches anything:
//   * arms the QUEUE for the next option, so the first one falls in behind the page text the box
//     just spoke rather than cutting it off;
//   * seeds the numeric field's baseline with the value that page line already said, so opening the
//     Draklor lift prompt speaks the whole sentence once and each MOVE speaks the new number.
//
// `base` / `byteOffset` are accepted for the caller's convenience and deliberately ignored: the
// parse reads widget+0x28 and widget+0x8A live, on the frame it speaks. A cached page cannot be
// current, and a page the feeder declined to speak is one it never refreshed -- both were the same
// bug.
void NotePage(void* widget, const uint8_t* base, size_t byteOffset, bool numericField, int32_t value);

// Is this the field dialogue / choice window class (obj[0] == FUN_002a6190)? MenuReader uses it to
// keep the generic painted-row path off this window, which the paint cache has no rows for.
bool IsChoiceWindow(void* owner);

// Install the selection tick, FUN_002a9980. Returns false if the hook fails.
//
// NOTE that FUN_002a9980 is slot 2 of the text dispatch table and exists ONLY on choice-capable
// widgets (type 0); a plain dialogue box has null there. That is why this tick can drive an option
// cursor but could never page dialogue -- pagination hangs off slot 0, FUN_002a8c50, in
// `ui/dialogue_reader`.
bool Init();
void Shutdown();

// Drop the remembered selection, so the next prompt speaks whatever it opens on.
//
// The guard is scoped to the prompt on screen, but it needs a RE-ARM: its comment once claimed a
// rebuilt widget would not match, which assumes the engine hands back a fresh address, and
// menu_reader.cpp:102 records that it does not. A re-opened prompt at a recycled address, selection
// back at its starting slot, matched the stale key and stayed silent.
//
// Called from DialogueReader on the game's own end-of-message latch (the box that owned the prompt
// is finished) and from ForgetLivePages when a list screen opens over it -- events, not polls. This
// only ever ADDS speech.
void ForgetLastCursor();

} // namespace ChoiceReader
