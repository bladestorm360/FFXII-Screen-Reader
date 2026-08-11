# FFXII-Screen-Reader — Session Log (Sessions 151–current)

Continues `sessions_101_150.md`, which is closed at **Session 150** (the tester-report session:
the dialogue-path audit, collected treasure, and the equipment-category instrument).

Entry format is mandatory: `## Session N — YYYY-MM-DD — [track] <title>`, where `N` is a single,
global, monotonically increasing integer shared by all parallel tracks. Never a date-only header,
never a letter sub-session. Before appending, grep this file for the highest `## Session N` AND
check `git log` for an unlogged session after it; the next session takes `N+1`. Split again after
Session 200 (`sessions_151_200.md` + `sessions_201_current.md`). Every entry carries a KEYWORDS
line for grep.

**Play-confirmed at the close of Session 150:** the enemy pruner (98 drops, all `+0x14=0xB0 kind=1`
on named enemies, nothing collateral) and the collected-treasure drop.

**Open at the close of Session 150** — carried forward so it is not lost with the file split:

- ~~**Mariam's Tomb pathfinding** (tester, 2026-08-11)~~ — **NOT A DEFECT. Report withdrawn
  2026-08-11**, verified in play by the user AND the tester. The statue "would not route" because
  **the doors leading to it had not been opened yet** — there was no route to plan, and the planner
  said so correctly. No session was spent on it and none is needed.
  **The lesson is about triage, not pathing:** a routing refusal on a map with a closed door is the
  planner working. Before opening an investigation into "X will not route", establish that a route
  EXISTS — the `oracle:` line already answers this
  (see the S115 note: *read the `oracle:` line before proposing a cause for any "No path"*).
- **Offhand shields — root-caused and FIXED in Session 150 §5b, but the fix is UNDEPLOYED.** The
  build carrying it (and the four `NoteRowGate` exits) could not be copied because the game held
  `dinput8.dll`. Deploy it and confirm: Confirm into a slot with **nothing equipped** must announce
  the first candidate. It is empty-vs-equipped, not shields — any bare slot is affected.
- **The gamepad hotkey gap.** `t` re-read, `o` describe, and the Status / Clan Primer line-by-line
  walks are keyboard-only, so a pad player hears each surface's entry line and cannot step through
  the rest (`input_tracker.h:9`).
