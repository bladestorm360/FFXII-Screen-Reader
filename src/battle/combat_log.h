#pragma once

#include <cstdint>
#include <string>

// The combat log: a continuous, non-modal history of battle events.
//
// DESIGN (combat_system.md §9, superseding the older stored design):
//   * NOT modal. There is no overlay to open, no WH_KEYBOARD_LL intercept, and the game is never
//     paused -- every value the log wants is an on-demand read.
//   * 100 entries, continuous FIFO ACROSS battles. Never cleared on battle entry/exit, area change
//     or save-load; only on Shutdown().
//   * `,` back (older) · `.` forward (newer) · Home oldest · End newest. Deliberately a TIMELINE,
//     not a chat scrollback, so `,` goes back in time.
//   * The game keeps NO scrollback of its own (the ticker's +0x4098 list is a forward queue whose
//     nodes are recycled the moment they leave the screen), so this is genuinely new capability
//     rather than a mirror of something the game already has.
//
// THREADING: producers are game-thread hooks, the consumer is the input thread. Text is rendered
// AT APPEND TIME on the game thread, because game pointers (BtlChr, actor pool, name codecs, and
// the message buffer, which dies when its frame returns) are only reliably readable there. The
// input thread only ever touches the finished wstring.
namespace CombatLog {

enum class Kind : uint8_t {
    GameMessage,   // Tier 1 -- the game's own composed sentence, read verbatim
    Damage,        // Tier 2 -- synthesized "X attacks Y. N"
    System         // mod-only concepts (battle start/end, target confirmed, ...)
};

void Init();
void Shutdown();

// Append a fully-rendered line. Game thread only. `speakNow` requests an immediate interrupting
// announcement in addition to being logged.
void Append(Kind kind, const std::wstring& text, bool speakNow);

// Cursor navigation -- input thread. Each speaks the entry it lands on.
void StepBack();      // `,`  older
void StepForward();   // `.`  newer
void JumpOldest();    // Home
void JumpNewest();    // End

} // namespace CombatLog
