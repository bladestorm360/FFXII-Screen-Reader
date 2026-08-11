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
- ~~**Offhand shields**~~ — **root-caused and CLOSED in Session 151 below.** The Session 150 note
  here claimed it was "root-caused and FIXED in §5b… empty-vs-equipped, not shields"; **that claim
  was wrong and had already been refuted in play** when this file was written. The real cause is the
  off-hand's cursor HOST object; see Session 151.
- **The gamepad hotkey gap.** `t` re-read, `o` describe, and the Status / Clan Primer line-by-line
  walks are keyboard-only, so a pad player hears each surface's entry line and cannot step through
  the rest (`input_tracker.h:9`).

## Session 151 — 2026-08-11 — [menus] The off-hand's cursor lives on a different object

KEYWORDS: offhand, off-hand, shield, shields, ammunition, equipment candidate list, FUN_003fdfe0,
FUN_003fd860, FUN_003fd6b0, FUN_003fd1d0, FUN_002d47c0, cursor host, container+0xC0, widget+0xC8,
0x8000, IsFocusedPane, DAT_0208ebc0, IsCursorHost, IsCandidateList, navigation silent, S150 followup

**The last open defect before release, and it closed on a measurement S150 had already shipped.**
**PLAY-CONFIRMED 2026-08-11 by the user — the off-hand list now speaks every row as the cursor
moves.** Navigating the off-hand (shield) candidate list was silent: the pane announced its category and its
first row on entry, then said nothing for any cursor move. Every other equipment slot — including an
unequipped helm — read correctly, on the SAME window instance.

### The answer was the S150 diagnostic's own output

`FFXII-Screen-Reader-2026-08-11_15-41-40.log`, cursor sitting on the shield list:

```
[READER] focus msg on a NON-cursor pane: owner=…CB5BBA0 val=1 -> cursor pane=…BE9CDC0 class RVA=0x2DDFE0
[READER] focus msg on a NON-cursor pane: owner=…CB5BBA0 val=0 -> cursor pane=…BE9CDC0 class RVA=0x2DDFE0
```

`val` tracks the player up and down the list. The same instance `…BE9CDC0` addresses its own 0x8000
directly for WEAPONS and HELMS, minutes apart in the same log:

```
[READER] pane owner=…BE9CDC0 focus=…BE9CDC0 focused=1 rowOff=0x0
[INV] item: owner=…BE9CDC0 "Magoroku"
```

So the message was never missing and the reader was never wrong about the rows. Only the ADDRESSEE
differed. **The instrument that answered this was shipped in the session that could not answer it —
because it was built to record which branch declined, not what was seen.**

### Root cause — slot 1 is the only slot whose cursor widget is not its own

`FUN_003fdfe0`'s init (`:31-36`) splits on the slot: `== 1` → `FUN_003fd860`, everything else →
`FUN_003fd6b0`. Slot 1 is the OFF-HAND (`FUN_003fd360`: `category = slot + 0x40`, so `0x41`).

- `FUN_003fd6b0:38-40` — every other slot takes its cursor widget straight from the container's own
  scene subtree. The widget's notify target is the container, so the pane that holds the cursor is
  the pane that receives the focus message.
- `FUN_003fd860:33-38` — the off-hand first creates an intermediate object,
  `FUN_00244f50(200, FUN_003fd1d0, 0)`, parks it at **container+0xC0**, attaches it as a child, and
  takes the cursor widget from THAT object's subtree. `FUN_002d47c0:15-16` sends 0x8000 to
  `widget+0xC8` — the widget's host — so the message arrives on the host, not the list.

`FUN_003fd1d0:41-47` then forwards every category-0xC message to its parent's handler **as a direct
call, not another `FUN_00247510`** — which is why exactly one focus message is ever observed, why it
carries the host, and why `val` is unchanged when `FUN_003fdfe0` indexes `val * 0x20 +
container[+0xE0]` (the same array, stride and `+0x08` id field this reader already walks).

**One mechanism explains BOTH halves of the defect.** The entry silence S150 patched had the same
cause: the stash arms on the message's owner (the host) while `HookedFocusSet` replays on `newWin`
(the list), so the two could never match and the replay could not fire.

### The fix

`InventoryReader::IsCursorHost(cursorPane, host)` — two gates, both required: obj[0] class
`0x2DDFE0`, **and** `cursorPane[+0xC0] == host`, an identity no shape test could establish.
`+0xC0` is written only by the off-hand's build path, so nothing else in the family can match.
`HookedDispatch` then speaks `TryFocus(focusWin, index)` and claims the row either way — an empty
category must stay deliberately silent rather than fall to the generic painted-cell path (S89).

The `IsFocusedPane` gate is untouched; it is still what stops the inventory reading several panes at
once. The S150 entry announce is kept exactly as it shipped (play-confirmed) and now shares the
class predicate instead of repeating the base arithmetic.

### Struck

- **STRUCK: "it is empty-vs-equipped, not shields — any bare slot is affected"** (S150's own carry
  forward, above). It was refuted in play before it was written down: an unequipped HELM reads fine,
  and the off-hand fails WITH a shield equipped.
- **STRUCK as the cause: `FUN_0057cf20` case `0x41`'s two-pool shields+ammunition merge.** Real, but
  innocent — it decides which ROWS the list holds, never who is told about the cursor. It was the
  live hypothesis for two sessions on the strength of being the only bespoke branch anyone had found.

### Committed in passing — an S150 change that was never committed or logged

`5a388c8` staged `equip_compare.h` but **not `equip_compare.cpp`**, so the AutoDetail gate on the
per-highlight stat preview — the tester's *"the delta comparison is vocalizing automatically in the
unequip menu with autodetail off"* — had been sitting in the working tree unversioned ever since,
and the Session 150 entry never mentioned it. Found by `git status` while staging this session.
Committed on its own so the history stays one-commit-per-session; the code is unchanged from how
S150 wrote it. **`git status` before staging is what catches this** — an uncommitted file is
invisible to a grep of the session log, exactly like the unlogged session `6f619e3`.

### Lessons

- **A surface that will not speak has two candidate faults, and they are not the same question:
  "is the message wrong?" and "is the ADDRESSEE wrong?"** Three sessions searched the row build —
  what the list CONTAINS — because that is where a list's differences are expected to live. The
  difference was in who owns its cursor, which is settled at construction and never appears in the
  data the list holds.
- **When one member of a family misbehaves, diff its CONSTRUCTOR, not its contents.** The split was
  one branch in the init handler, on the slot index, in plain sight.
- **The line that answers a defect is the one naming which branch declined.** S150 shipped that
  instrument, and it was enough on the first pass through the surface. Contrast the three throttled
  diagnostics that same session mistook for measurements.
