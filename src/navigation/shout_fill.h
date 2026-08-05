#pragma once

#include <cstdint>

#include "navigation/shout_script.h"

// INSTANT FILL — complete Bhujerba's shout minigame on the first clean shout.
//
// ⚠ THIS IS THE MOD'S THIRD WRITE-CATEGORY EXCEPTION, after auto-walk (S100) and sneak assist
// (S107). CLAUDE.md's standing rule is that the mod is strictly read-only on input and game
// memory. USER-AUTHORIZED in the conversation of 2026-08-05, explicitly, as an accessibility skip:
// the minigame asks a blind player to walk a crowd shouting at people they cannot see while
// avoiding Imperials they cannot see, and the gauge is drawn as art with no text behind it.
//
// THE BOUNDARIES, all non-negotiable:
//   1. THE SCRIPT TABLE IS THE ONLY GATE. `ShoutTable` names fourteen map scripts by their own
//      authoring name (`byu_a01.src` ...). There is no toggle, no mod-menu row and no hotkey --
//      the same shape S115 settled for sneak assist, and for the same reason the tester gave then:
//      arming a fix for a puzzle they cannot see, on every entry to the map that needs it, is
//      work the feature exists to remove.
//   2. OFF-TABLE THE WRITE IS UNREACHABLE, NOT SKIPPED. The gauge hook's first branch returns
//      before this file is ever called when no shout module is live, so on every map in the game
//      but these fourteen the build behaves exactly as the read-only mod does.
//   3. ONE WRITE, ONE TARGET, ONE VALUE. The single store is `row->fillValue` -- the script's OWN
//      success threshold, recovered from its OWN `v >= N` comparison, never a mod constant -- into
//      the script's OWN meter variable, and only inside a rising `setgaugecounter` burst that has
//      already proved the variable live. Nothing else is written, ever. The mod authors no story
//      flag, no save state and no outcome text: after the write the GAME's own loop clamps the
//      value, prints its own messages, and runs its own success branch.
//   4. IT FAILS OPEN, AND THE FALSIFIER IS THE LOG LINE. Every attempt logs the three numbers it
//      compared. On any mismatch -- unreadable descriptor, unsupported storage class, a value that
//      is not what the script must be holding -- nothing is written and the minigame plays vanilla.
//
// WHY THE FALSIFIER IS A FACT AND NOT A GUESS. The increment loop, decoded from byu_a01 at
// 0x34EF8, is:
//         PUSHV meter / PUSHII 1 / OPADD / CALLPOPA setgaugecounter    <- gauge = meter + 1
//         ... ; PUSHV meter / PUSHII 1 / OPADD / POPV meter            <- meter += 1, AFTERWARDS
// so at the instant the gauge native runs, the script variable still holds the PRE-increment
// value. The relation `storage == newValue - 1` is therefore what the bytecode guarantees, and a
// storage word that does not satisfy it is not the meter -- which is exactly the condition under
// which this file declines to write.
//
// ACCEPTED LIMIT, stated rather than buried: the write lands mid-loop, so the script's next
// iteration calls `setgaugecounter(101)` before its own `if (v >= 100)` clamp pulls it back to
// 100. The gauge may therefore tick one point past full for a single frame. That is the game's own
// clamp doing its job, not a defect, and it is why the mod does not clamp anything itself.
namespace ShoutFill {

// GAME THREAD, from inside the gauge-writer detour, on a RISING set only.
//
// `preSet` is the gauge's own value read before the original ran; `newValue` is the value the
// script just asked for. Both are logged. At most one write per map visit.
void TryInstantFill(const ShoutScript::Module& mod, int preSet, int newValue);

// GAME THREAD, from the field-teardown hook. Clears the once-per-visit latch and the log latches:
// a resolved variable address belongs to the map it was read on and must never outlive it.
void OnMapTeardown();

} // namespace ShoutFill
