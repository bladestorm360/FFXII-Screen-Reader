# FFXII:TZA Keyboard Controls & Mod-Reserved Keys

The game's default keyboard bindings (from the in-game Controls menu — this user's
config) cross-referenced against the screen-reader mod's reserved hotkeys, so future
features pick conflict-free keys and we swallow/rebind any collisions.

> **STATUS: COMPLETE** (2026-07-07). Full Controls list captured (top + middle + bottom).
> **None of the mod's nav keys (`\ [ ] ``) or `o`/`t` appear anywhere in the game's
> bindings** — they are all free. Remaining conflicts are only the Shift modifier and Esc
> (see Conflicts).
>
> **UPDATE (Session 45):** added the party-status keys `4`/`5`/`6` — the game binds `1`/`2`/`3`
> (Game Speed) but **nothing to `4`/`5`/`6`**, so they are free. `;` changed from the facing
> readout to **active target status**; the facing readout was dropped.
>
> **UPDATE (Session 54):** `F5` added (nav availability filter).
>
> **UPDATE (Session 69):** `g` added — speaks the party **gil** total (works on the field, in shops, and in
> menus; silent on the title screen). The game binds nothing to `G` (letters it uses: W/S/A/D, I/K/J/L, Q/E,
> C, F, R, H, X, M, Z) and the mod reserved nothing to it — free on both sides. Shop Buy/Sell/Bazaar item
> lists and the buy/sell quantity selector also now vocalize (no new key — automatic on cursor moves).
>
> **UPDATE (Session 56) — direction model, corrected.** Spoken directions use **COMPASS words on the
> camera-relative frame**: "North" is the way an UP push currently sends you, "East" is right, and so
> on. Session 54's note that the shipped words are EGOCENTRIC (`ahead`/`left`/…) is wrong — the
> egocentric vocabulary exists in `nav_common.cpp` (`kEgocentric`) but is NOT shipped; `RelativeWord`
> returns `kCardinal`. Compass-on-relative is the tester's stated preference.
>
> **The camera-relative frame is a hard limitation, not a bug.** The game moves the camera on its own
> (ledges, walls, and continuously in battle as it tracks the target), and when it does, the same route
> is described from the new angle — a leg can flip 180°. This is accepted and documented in `README.md`:
> movement is camera-relative, so the mod cannot pick a frame the stick does not act in, and locking the
> camera would break battle lock-on (it uses the same camera). Session 56 rejected a camera lock, a
> travel-anchored frame, and a spoken "camera changed" notice (the battle camera would trigger it
> constantly); it added only a `ref=`/`src=` log line so a genuine camera move is distinguishable from a
> mod bug. No facing readout is needed — the direction words already describe where things are.
>
> **UPDATE (release 0.1):** `7` added for the guest slot. **`;` is now battle-only and silent
> otherwise** (user instruction): it reports ONLY the committed combat target, never a browsed
> cursor, and out of battle it does **nothing at all** — not even "No target". Applied at
> `SpeakTargetStatus`, not in `ResolveTarget`, so `p`-key routing to a browsed target is unchanged.
> Empty/absent slots (notably `7`, silent for most of the game) speak nothing; diagnostics go to the
> log via `BattleState::DiagnoseSlot`, never to speech.

## Game bindings (captured)

### Character Movement
| Action | Main | Alt 1 | Alt 2 |
|---|---|---|---|
| Up / Down / Left / Right | W / S / A / D | — | — |

### Camera Movement
| Up / Down / Left / Right | ↑ / ↓ / ← / → | — | — |

### Cursor Movement (menu navigation)
| Action | Main | Alt 1 | Alt 2 |
|---|---|---|---|
| Up | I | Numpad 8 | — |
| Down | K | Numpad 2 | — |
| Left | J | Numpad 4 | Q |
| Right | L | Numpad 6 | E |

### Others (action buttons)
| Action | Main | Alt 1 | Alt 2 |
|---|---|---|---|
| Confirm | Space | Enter | Left Mouse |
| Cancel | C | Backspace | Right Mouse |
| Battle Menu | F | — | — |
| Party Menu | R | — | — |
| Game Speed (1×/2×/4×) | 1 | 2 | 3 |
| Escape | Left Ctrl | — | — |

> **CORRECTION (Session 44):** an earlier capture of this menu row mislabeled `1`/`2`/`3`
> as "Game Speed / Target Group / Lock On". **Runtime disproved that — `1`, `2`, and `3`
> all change GAME SPEED** (they mirror the `F1`/`F2`/`F3` "Regular/Double/Quadruple Game
> Speed" bindings below; the tester's sudden speed jump was their own `1`/`2`/`3` keypress,
> not the mod). There is **no keyboard "Lock On" or "Target Group" binding** in this config.
> **The mod reserves NONE of `1`/`2`/`3`** (its hotkeys are all letters/punctuation, listed
> below) and never injects them — it is strictly read-only on input (see debug.md).

### System / Other (category header off-screen)
| Action | Main | Alt 1 | Alt 2 |
|---|---|---|---|
| Reset Camera | H | Numpad 0 | X |
| Pause Game | Esc | — | — |
| Display Map | M | Numpad + | Z |
| Regular Game Speed | F1 | — | — |
| Double Game Speed | F2 | — | — |
| Quadruple Game Speed | F3 | — | — |
| Toggle Walk/Run | **Left Shift** | — | — |

### Mouse / Controller / Misc
- Mouse Sensitivity L/R and U/D (sliders); Camera Auto-Rotation On/Off
- Confirm Button: Type A (A) / Type B (B) — user has Type B
- Miscellaneous → Default (restore-defaults button)

## Mod-reserved keys (all STANDALONE — no Shift)
| Key | Mod function | Conflict status |
|---|---|---|
| `o` | Describe / read focused tooltip | free |
| `t` | Re-read last spoken line | free |
| `\` | Nav: turn-by-turn route to current selection | free |
| `F4` | **Diagnostic:** toggle menu-text capture (painter interception) on/off | free — game binds F1/F2/F3 only |
| `F5` | Nav: availability filter — **All ⇄ Story-gated**. Orthogonal to the `-`/`=` category cycle; speaks the mode and the resulting count. Default All, so nothing is ever hidden unless you ask | free — game binds F1/F2/F3 only |
| `F6` | label the focused entity with the clipboard text (persists; clears if the clipboard is empty) | mod-only |
| `Space` / `Enter` | *(observed only)* advances the spoken dialogue page with the game's own Confirm | the game's Confirm — never swallowed or injected |
| `p` | Nav: turn-by-turn route to the current battle target (see note) | free |
| `[` | Nav: previous object | free |
| `]` | Nav: next object | free |
| `-` | Nav: previous category | free |
| `=` | Nav: next category — All, Exit, Save Crystal, Gate Crystal, Treasure, NPC, Interactables, Enemy, **Items**. **Items sits next to Enemy on purpose** (Session 72): one press flips between the enemies you are fighting and the loot they dropped | free |
| `` ` `` | Nav: rescan + area name | free |
| `;` | **Context-gated target readout.** In battle: **committed** target status (name + instance letter + HP), silent on a merely browsed cursor — see below. In the field: **who Confirm will address**, e.g. "Talk: Montblanc" / "Action: Save Crystal", silent when nothing is in reach | free |
| `/` | Nav: describe current (name + bearing + distance + obstacle) | free |
| `'` | Nav: diagnostic dump | free |
| `4` | Party: slot 1 status (name, HP / MP with maximums, statuses) | free |
| `5` | Party: slot 2 status | free |
| `6` | Party: slot 3 status | free |
| `7` | Party: **guest** slot status (silent when there is no guest) | free |
| `U` | License board: current License Points (also announced on board entry) | free |
| `g` | Party **gil** total (field / shop / menus; silent on the title screen) | free — no game/mod binding uses G |

> **`4`/`5`/`6` FIXED (Session 49, implemented).** `party_status.cpp` treated `DAT_02ebf190`
> (RVA `0x2D9F190`) as the BtlWork struct; **it is a POINTER**. Every roster read landed in
> unrelated memory, so `bcIdx >= 0x28`, the lookup returned null, and `SpeakSlot` returned
> silently. The deref + the engine's own magic check (`0x5071901`) now live in
> `battle/battle_state.cpp`. Also fixed in the same pass: `PARTY` was added to the logger's flush list so its diagnostics
> survive a hard exit, and the readout now speaks **status names** read from the game's own table.
> Roster list 3 has **nine** slots (0-2 active, 3 guest, 4-8 reserve) and the game's own bound
> check is literally `slot < 9`; keys `4`/`5`/`6` cover 0-2 and `7` covers the guest.
>
> **An empty or unreadable slot is SILENT — standing user instruction.** It must never announce
> "Empty slot" or any other filler; it behaves like every other mod key with nothing to report. The
> `7` key is silent whenever there is no guest, which is most of the game, and that is correct. The
> distinguishing diagnostic goes to the **log** (`BattleState::DiagnoseSlot`), never to speech.
> ~~"an empty slot now says **Empty slot**"~~ is **STRUCK** — it was proposed in
> `combat_system.md` §8.2, reversed by the user before it shipped, and never existed in built code.
>
> ~~Remaining suspects: the `g_extraDown[6]` -> `[9]` array growth~~ — **STRUCK.** The input path
> was correct end to end all along.

| `,` | Combat log: back one entry, older | free |
| `.` | Combat log: forward one entry, newer | free |
| `Home` | Combat log: jump to oldest entry — **except on the Status screen**, see below | free |
| `End` | Combat log: jump to newest entry — **except on the Status screen**, see below | free |
| `Up` / `Down` | **Status Attributes page only:** previous / next entry in the virtual buffer | free on that page |
| `Left` / `Right` | **Status Attributes page only:** previous / next group (Character / Attributes / Status effects) | free on that page |

> **Status screen virtual buffer (Session 71).** The Status screen's **Attributes page** is a static
> display with no in-game cursor, so the mod exposes it as an FF1-style navigable buffer on the arrow
> keys — Character (name, Level, HP, MP, LP, EXP, Next), the nine Attributes, and Status effects.
> Entering the screen announces the first entry.
>
> **Its other two pages — Magicks and Technicks/Quickenings/Remedy Lore/Espers — DO have a real
> in-game cursor** and are the same pages the license board's `F` overlay shows, so you browse them
> with the game's own controls and `ability_summary_reader` speaks each row, including the section
> heading when you cross into Technicks / Quickenings / Remedy Lore / Espers. The mod's arrow keys
> deliberately do nothing there (the buffer declines every key while `menuCtx+0xDE7 != 0`), so the
> game's cursor is never fought. Backing out to the Attributes page re-announces it.
>
> **`Home`/`End` are claimed by the status buffer while that screen is open** (top/bottom of the
> page) and do **not** reach the combat log there. This is a **deliberate, user-instructed exception**
> to the "combat log usable everywhere" requirement in `CLAUDE.md` — decided Session 71. Everywhere
> else, including every other menu and while paused, `Home`/`End` still reach the log. The buffer
> re-validates that its container is still parked at `menuCtx+0x140` on every keypress, so a missed
> teardown can never leave it holding those keys.
>
> **The arrow keys are safe to claim here:** the game binds no arrow-key function on this screen
> (user-confirmed), and the mod cannot swallow keys anyway. Character switching is L1/R1, untouched.

> **The combat log is NOT modal (decided Session 48).** ~~`F4` to open / `Esc` to close~~ is **STRUCK**
> — there is no overlay to open, no `WH_KEYBOARD_LL` modal intercept, and the game is never paused.
> The four keys above read a 100-entry continuous FIFO on demand. See `Docs/combat_system.md` §9.
>
> **`Shift` is deliberately NOT used.** The game binds **Left Shift → Toggle Walk/Run**, and the mod
> cannot swallow keys (it passes the DirectInput buffer as `const`, per the read-only rule), so
> `Shift+,`/`Shift+.` would silently flip walk/run on every press — an invisible state change. `Home`
> and `End` are unbound and have no side effects.

> **`p` target source (Session 44):** `p` routes to the battle **target the game is currently
> selecting**, read from the target-selection object at `DAT_0209be80 + 0x9FD8`
> (`battle_target_reader`). It does **NOT** press or depend on any keyboard "Lock On" key —
> there is no such binding in this config (see the 1/2/3 correction above). `p` is read-only:
> it reads the selected target's position and computes a route; it presses nothing.

## Conflicts & resolutions
1. **Left Shift = Toggle Walk/Run.** RESOLVED — the mod uses **no Shift modifier** at all;
   category switching moved to standalone `-` / `=`, facing to `;`, diagnostic to `'`.
2. **Esc = Pause Game** (not cancel/back, as an older note assumed). Combat-log close on
   Esc is still fine because the log's modal input intercept swallows Esc while open.
3. `[ ] \ `` (non-shift): **no conflict — confirmed against the full list.** (`[` failing
   in-game is a scan-code/read bug, not a binding clash — under diagnosis.)

## Note on input capture (important)
FFXII acquires the keyboard via **DirectInput (exclusive)**, which starves OS-level
keyboard hooks (WH_KEYBOARD_LL) and NVDA's own commands. The mod therefore reads its
hotkeys by hooking the game's **`IDirectInputDevice8::GetDeviceState`** (via the dinput8
proxy) and inspecting the same 256-byte DIK buffer the game polls each frame — so mod
keys work regardless of exclusivity, and the game's behaviour is unchanged.

**UPDATED 2026-07-23 (Session 65): NVDA's own key commands DO work while the game runs.** The tester
confirmed this in play. The long-standing note here said they "remain blocked under the game's exclusive
grab" — that is no longer true in practice, so do not design around it. **The cause is not established:**
the tester's read is that it was a mod-side problem since fixed. That is recorded as an observation, not
a mechanism — nobody has traced why it changed, and it should not be quoted as one until somebody does.

Unaffected either way: the mod still **never swallows or injects** a key (it passes the DirectInput
buffer as `const`, per the read-only rule), so there is still no way to run a text field in-game. That is
why labelling entities (**F6**) reads the CLIPBOARD instead of capturing typing.

> **`;` is context-gated, not double-bound (Session 73).** The battle reader is *structurally*
> silent in the field — `ResolveTarget` needs either a committed action or an open select UI, and
> neither exists outside combat — so its field-side silence was a dead slot, not a behaviour worth
> preserving. `SpeakTargetStatus()` now returns whether it spoke, and `OnNavKey` calls
> `InteractTarget::SpeakCurrent()` only on false. Battle behaviour is unchanged; a battle with no
> target stays silent in both halves, exactly as before.
>
> The field half reads the engine's OWN chosen target (`DAT_0209a2b8` / `DAT_0209a2bc`), not the
> mod's nearest-object list. Those disagree — that disagreement is the bug it was built to expose:
> the mod said "Montblanc, right next to you" while the game had selected a Clan Member 1.86 units
> away. **There is exactly one engine target and no cycling** (`FUN_0025b820` keeps the minimum
> score, `FUN_0025d650` resets per frame) — do not add a "next interaction target" key.
