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
> **UPDATE (Session 92) — the audio beacon.** `\` now also starts a repeating sound that leads you
> along the route it just spoke. Each corner where a spoken leg runs out is a beacon point; the ping
> is panned toward it and speeds up from about one per second to five per second as you close on it.
> Reaching a corner advances to the next one **silently**; reaching the destination plays the sound
> pitched up once and stops. Wander well off the route and it silently re-plans. In combat it
> switches to tracking whatever your party is committed to attacking, using a different sound, and
> goes quiet if there is no such target; when the fight is over it picks the route back up on the leg
> it was holding. `F9` or the `F8` menu turns the whole thing off. **This needs `SDL3.dll` in the
> game's `x64\` folder — see README.md; without it the mod does not load at all.**
>
> **UPDATE (Session 95) — the two beacons are separate now.** The in-combat target sound used to be
> part of the route beacon and only played if a route beacon happened to be running. It is its own
> setting in the `F8` menu, plays in battle whether or not you have a route, and has its own volume;
> the route beacon has a volume too. Everything else is unchanged — the route beacon still stands
> down for the length of a fight and resumes on the leg it was holding.
>
> **The pan is a TRAVEL direction, not a turn instruction.** It is the same angle as the spoken leg,
> from the same number: if the voice says "Northeast", the beacon sits about 45 degrees right.
>
> **"Behind" means the whole rear half, not just directly astern.** Anything past your shoulders is
> behind: quieter, duller, and — since Session 95 — about 20% lower in pitch. All three cues arrive
> together and at full strength a few degrees past the abeam line, so front and back are never a
> matter of degree; a plain left/right pan renders them identically otherwise.
>
> **The pan keeps working all the way round.** Something behind and to your left is panned left AND
> carries the behind cues, so you get bearing and hemisphere at once. This is a requirement, not a
> side effect: there is no spatial audio here, so left/right is the only bearing information there is
> and nothing is allowed to flatten it.
>
> **All of this applies to both beacons** — route and target — because it lives in one place that
> every ping goes through.
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

> **VOCABULARY (Session 93, reversed Session 158).** The row above is the GAME's own label, captured
> verbatim from its Controls screen, and it stays that way — the mod reads that screen back to you, so
> rewriting it here would put a word on your screen that the game never says. **The project uses the
> game's word too: the `R` menu is the party menu.** Its first command, the screen for managing who is
> in the active party, is the **Party screen** — never "party menu", which is the whole reason that
> name is free to mean what the game means by it.
>
> Session 93 had called the `R` menu the *field menu* to keep the two apart. In practice the two words
> never got confused, and one of them was not the game's.

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

> ## ⚠ STRUCK (Session 112): "free — game binds F1/F2/F3 only"
>
> That claim appears in the Conflict column of nearly every row below and it is **FALSE**. It was
> derived from the game's own **Controls configuration screen**, which lists only rebindable actions
> — and the game has bindings that screen never shows.
>
> **`F9` is bound by the game to "Hide On-Screen Keyboard".** Measured from the game's own
> on-screen-keyboard overlay, photographed by the tester on map 568: the panel's footer reads
> `F9  Hide On-Screen Keyboard`, and `Space  Close`. The mod also binds `F9` (audio beacon), and the
> mod is strictly read-only on input — **it cannot swallow a key** — so every beacon toggle ALSO
> toggles that full-screen overlay, and every dismissal of the overlay toggles the beacon.
>
> **THE LESSON: "the config screen does not list it" is not evidence that a key is free.** Absence
> from a rebinding UI proves only that the binding is not rebindable. Before claiming any key,
> check the on-screen-keyboard overlay, which is the game showing its own layout.
>
> Also visible in that overlay and **NOT** reconciled with this file's Session 44 correction below:
> `1 Game Speed/Change Group`, `2 Lock On`, `3 Change Group`. Session 44 recorded runtime evidence
> that 1/2/3 are all Game Speed; the overlay disagrees. **Neither is struck here** — one of them is
> reading a different profile and it has not been measured which. Do not act on either until it is.

## Mod-reserved keys (all STANDALONE — no Shift)

> **EVERY F-KEY IS BARE-PRESS ONLY (Session 155).** `F4`, `F5`, `F6`, `F7`, `F8` and `F11` do
> nothing at all while Shift, Ctrl, Alt or a Windows key is held. A chord belongs to whatever owns
> the chord — **Alt+F4 is the OS closing the game**, and until this session the mod read it as a bare
> `F4` and flipped Combat verbosity on the way out. The mod cannot swallow a key, so an unguarded
> F-key fires *in addition* to whatever the chord already does.
| Key | Mod function | Conflict status |
|---|---|---|
| `o` | Describe / read focused tooltip — **and, with an enemy under the battle target cursor, the LIBRA readout** (HP as numbers, Level, MP where the unit has a gauge, statuses, and elemental weaknesses). Says **"Libra not active."** when an enemy is targeted and Libra is down; falls through to the tooltip everywhere else, so no existing meaning was taken away. The weakness clause is omitted for the marks and bosses the game itself refuses Libra on — the mod does not out-reveal the screen (S147) | free |
| `t` | **Re-read the last line of DIALOGUE** — a conversation page, a prompt, or an obtained-item toast. **Silent unless one of those is actually on screen** (Session 130). It was never "repeat whatever the reader last said" — only those three surfaces ever fed it — but nothing cleared the line when the box closed, so it went on repeating a finished conversation in the field, in menus and mid-battle | free |
| `\` | Nav: turn-by-turn route to current selection — **and starts the audio beacon** (Session 92). Press it again at any time to re-aim. **With Auto-walk On in the `F8` menu (Session 100) it also WALKS you there** — see the Auto-walk section | free |
| `F4` | **Combat verbosity — Normal ⇄ Verbose.** Speaks the new setting. Same setting the mod menu holds; this is the shortcut for mid-fight | free — game binds F1/F2/F3 only |
| `F5` | Nav: availability filter — **All ⇄ Story-gated**. Orthogonal to the `-`/`=` category cycle; speaks the mode and the resulting count. Default All, so nothing is ever hidden unless you ask | free — game binds F1/F2/F3 only |
| `F6` | label the focused entity with the clipboard text (persists; clears if the clipboard is empty) | mod-only |
| `F7` | **Autodetail — Off ⇄ On (Session 147).** Speaks the new setting. Same value the `F8` menu holds. Changes only what is VOLUNTEERED: the shop equipment comparison on each highlight, and the Libra readout on each target change. `4`-`9` and `o` answer identically in both modes | free — reserved from S90, spent in S147 |
| `F8` | **Mod menu** — open/close the mod's own settings. Up/Down pick a setting, Left/Right change it, `o` reads its description, `F8` closes | free — game binds F1/F2/F3 only |
| `F11` | **Audio beacon — On ⇄ Off.** Speaks the new setting. Same setting the mod menu holds; this is the shortcut. Turning it **off** silences a running beacon immediately; turning it **on** only re-arms the feature — press `\` to start one, since an On press has no destination to aim at. **BARE PRESS ONLY** — see the rule below, which now covers every F-key. **Shift+F11 is an NVDA command the tester uses while playing** and the mod cannot swallow keys, which is where that rule started (S112). **Moved off `F9`, which belongs to the game** | free |
| `F9` | **NOT A MOD KEY — the GAME uses it** for *Hide On-Screen Keyboard* (S112). Left alone deliberately | game-owned |
| `F10` | ~~**Sneak assist — On ⇄ Off (Sessions 107/109).**~~ **STRUCK, Session 115: the mod no longer binds `F10`.** Sneak assist is now **automatic** on the maps `path_danger.cpp` lists (the Royal Palace Cellars and Lower Halls) and has no setting, no menu row and no key — see the Sneak assist note below. Before re-binding this key for anything, check the game's **on-screen-keyboard overlay**, not its Controls screen (S112) | unbound by the mod |
| `p` | Nav: turn-by-turn route to the current battle target (see note) | free |
| `[` | Nav: previous object | free |
| `]` | Nav: next object | free |
| `-` | Nav: previous category | free |
| `=` | Nav: next category — All, Exit, **Door**, **Shop**, Save Crystal, Gate Crystal, Treasure, NPC, Interactables, Enemy, **Items**. **Items sits next to Enemy on purpose** (Session 72): one press flips between the enemies you are fighting and the loot they dropped. **Door and Shop sit next to Exit on purpose** (Session 92): all three are ways off this map | free |
| `` ` `` | Nav: rescan + area name | free |
| `;` | **Context-gated target readout.** In battle: **committed** target status (name + instance letter + HP), silent on a merely browsed cursor — see below. In the field: **who Confirm will address**, e.g. "Talk: Montblanc" / "Action: Save Crystal", silent when nothing is in reach | free |
| `/` | Nav: describe current (name + bearing + distance + obstacle) | free |
| `'` | Nav: diagnostic probe (speaks "Diagnostic logged") | free |
| `4` | Party: slot 1 status (name, HP / MP with maximums, statuses) — **and, in a shop or on an equip screen, the equipment comparison for character 1** (see below) | free |
| `5` | Party: slot 2 status — or comparison character 2 | free |
| `6` | Party: slot 3 status — or comparison character 3 | free |
| `7` | Party: **guest** slot status (silent when there is no guest) — or comparison character 4 | free |
| `8` | The **summoned Esper**: name, statuses, HP and its summon gauge. Silent when no Esper is out — or, in a shop / on an equip screen, equipment comparison for character 5 | free — measured in play (S125) |
| `9` | Equipment comparison, character 6. Does nothing outside a shop / equip screen | free — measured in play (S125) |
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
> **`8` is NOT roster slot 4 (Session 148).** A summoned Esper is absent from roster list 3
> altogether, which is why no party key could ever reach it — it gets its own HUD row and its own
> field on BtlWork (`+0x5AD4`, gated on the summon bit at `+0x5B04`). `8` reads that directly, and
> adds the summon gauge the Esper alone has. Same silence rule as `7`.
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
| `B` | Bhujerba shout minigame: the infamy meter as a percentage — **and**, in the Stilshrine of Miriam, all three statue guardians' state | unproven — collision watch |
| `N` | Bhujerba shout minigame: the crowd around you, and the nearest guard's bearing and distance | unproven — collision watch |

> **Both keys answer only while the shouting sequence is actually RUNNING**, not merely while the
> player is on a Bhujerba street. The gate is the game's own gauge-shown bit (`gauge+0xD8` bit 2,
> written by the script's `setgaugeshowstatus`), so it opens and closes with the sequence itself.
> Off it, and with the **Puzzle guide** setting off, both keys are silent no-ops with one log line.

> **`B` is CONTEXT-GATED, not double-bound.** Its second meaning is the Stilshrine of Miriam statue
> readout, gated on a live `mrm_` map script — and the two contexts can never both be live, because
> one is the Bhujerba streets and the other is a dungeon. The dispatcher raises BOTH requests and
> each drains on the next field frame against its own gate, so the decision stays on the game thread
> where reading script modules is safe. Neither half has a **Puzzle guide** row of its own in the
> Stilshrine: that setting is context-gated to the shout sequence, so the statue readout is not
> subject to it.
>
> Two mod-menu rows are **context-gated to that same predicate** and appear only during the
> sequence: **Puzzle guide** (default ON — the spoken meter and these two keys) and **Instant
> success** (default OFF — one shout completes the minigame; it writes game state, so it follows
> auto-walk's rule that the player must switch it on deliberately).
>
> **A HIDDEN ROW READS AS OFF** (S133, standing rule for every context-gated setting, not just
> these two). `ModMenu::EffectiveValue` forces a gated setting's read to 0 whenever its visibility
> predicate is false, so the feature cannot act outside its context whatever the stored value says —
> and a consumer does not have to remember to re-check the context, because asking the setting is
> already asking it. The stored value is untouched and returns with the context.
| `Up` / `Down` | **Status Attributes page** and **an open Clan Primer entry:** previous / next entry in the virtual buffer | free on those pages |
| `Left` / `Right` | **Status Attributes page only:** previous / next group (Character / Attributes / Status effects) | free on that page |

> **Party screen — who is in the party (Session 93).** The party menu's first command, **Party**, is a
> membership toggle rather than a stat screen. Moving between the portraits speaks the character and
> their membership — "Vaan: In party", "Balthier: Not in party" — and pressing Confirm (or Left / Right)
> speaks just the new state, because the toggle does not move the cursor. The game refuses some presses
> on its own: a guest cannot be removed, and it will not let you drop below one or go above three
> members. A refused press plays the game's own error sound and the mod re-speaks the unchanged state,
> so a press always tells you where you ended up.
>
> Status and Equipment share this same portrait grid, and there they still speak name, Level, HP and MP
> as before — the readout follows the command you came in on.

> **Save / load slot list (Session 127).** Each row speaks its number, location, playtime, party
> leader and level, gil, and clan points — the row plus the detail panel underneath it, which is what
> a sighted player sees at a glance. The row the game marks with an icon instead of a number is
> spoken without one.
>
> `4` to `9` read **party members 1 to 6 of the highlighted save** — the same six portraits the panel
> shows, each as name and level. Away from this screen those keys keep their existing meanings
> (equipment columns in a shop, live party status elsewhere); the switch is automatic and there is no
> mode to get stuck in. An empty party slot says nothing.
>
> Not spoken: the small HP/MP bars beside each portrait — they carry no number to read, so naming
> them would mean inventing a scale the screen never states.
>
> **Clan Primer entry body (Session 127).** Opening a Bestiary / Hunts / Traveller's Tips entry
> speaks its page, and **turning the page speaks the new one** — the mod hooks the game's own set-page
> call, so whichever key or pad button the screen binds, the announcement follows. `Left` / `Right`
> stay entirely the game's: the mod deliberately does not claim them, because the page announcement
> already covers what they do and two speakers on one keypress race each other.
>
> `Up` / `Down` walk the open page a sentence at a time, `Home` / `End` jump to its start or end, and
> `o` re-reads the whole page. The page's own heading ("Observations", "The Adventurer's Handbook")
> is the first entry, so you always know which of an entry's pages you are on. Nothing is claimed
> once you leave the entry.
>
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

### Mod menu (`F8`) — the mod's own settings

> `F8` opens and closes it. `Up`/`Down` move between settings, `Left`/`Right` change the focused
> setting, and `o` reads its description — the description changes with the value, so it always
> describes what the setting is doing right now. Closing speaks "Mod menu closed".
>
> **`Left` and `Right` are directional as of Session 95.** They used to both advance, which was the
> same thing while every setting had two values. The volume settings have five, so `Left` goes down
> and `Right` goes up. A two-valued setting still flips on either arrow. The volumes **stop** at their
> ends rather than wrapping round — you hear the same number again, which is how you know you are at
> the limit.
>
> **Settings it holds:**
>
> | Setting | Values | What it does |
> |---|---|---|
> | Combat verbosity | **Normal** (default) / Verbose | What the combat log speaks aloud on top of what it always logs. Normal speaks enemy defeat and EXP, party member low HP and KO, and loot drops. Verbose adds enemies readying abilities and beginning to cast. **Damage lines are log-only in both modes** — they have always been read back with `,` / `.` rather than spoken as they happen. |
> | Audio beacon | Off / **On** (default) | The repeating sound that leads you along the route `\` just spoke, panned toward where you need to walk. See below. |
> | Audio beacon volume | 20% / 40% / 60% / 80% / **100%** (default) | How loud that sound plays. |
> | Target beacon | Off / **On** (default) | A separate repeating sound that tracks the enemy your party is fighting, panned toward it. **Session 95 split this off from the route beacon**, which it used to be part of — it now plays in battle whether or not you had a route running, and switching the route beacon off no longer takes it with it. Still battle-only: it sounds when your party has committed to a target and stops when the fight does. |
> | Target beacon volume | 20% / 40% / 60% / 80% / **100%** (default) | How loud that sound plays. |
> | ~~Text glyphs~~ | — | ⚠ **REMOVED, Session 147.** The row existed because *"the mod cannot detect the patch — it repacks the game archive in place and leaves nothing behind to test"*. **That claim is STRUCK.** It is true of the DISK and beside the point: what the setting described was which atlas the GAME LOADED, and the patch's own marker was written down in S130 without being recognised — it *"adjusts ten advance widths"*. The two `font00.dat` files differ in exactly 20 bytes, all of them those ten advances, so `GameText::DetectVariantOnce` reads them back through the game's own font manager and picks the table itself. Nothing to set; a stale `text_glyphs=1` in an old settings file is ignored. |
> | Auto detail | **Off** (default) / On | **Session 147.** Whether the extra detail about the highlighted thing is VOLUNTEERED, or only answered when you ask. Off is exactly today's behaviour: the shop's per-character equipment comparison on `4`-`9`, and the Libra readout on `o`. On adds both to the highlight itself — queued behind the short line, never interrupting it — and takes nothing away, because both keys keep answering in both modes. `F7` is the shortcut. |
> | Auto-walk | **Off** (default) / On | **Session 100.** With it On, `\` does not just speak the route and start the beacon — the mod walks your character along it, steering with the same directions the voice speaks. It stops the instant you touch a movement key (W/A/S/D or the arrows), the instant combat starts, when you arrive, when a menu opens, and after 15 seconds of no progress ("Auto-walk stopped"). It never re-starts on its own — press `\` again. **Gamepad players:** the mod cannot see the stick, so the stick does NOT cancel it — tap any movement key or use this toggle. |
>
> Neither volume goes to zero on purpose — each beacon has its own Off, so a switched-on beacon is
> never silent for a reason you cannot hear.
>
> `F4` toggles Combat verbosity, `F7` toggles Auto detail and `F11` toggles the Audio beacon, all from anywhere without
> opening the menu. Each route changes the same stored value and speaks the same confirmation.
> **`F11` is the route beacon only** — the target beacon has no shortcut key and is changed from the
> menu. (~~`F9`~~ is **STRUCK**: the game owns it — see the `F9` row above, S112. This paragraph said
> `F9` in two places until Session 115.)
>
> **Sneak assist is NOT in this menu, and has no key (Session 115).** A few points in the story make
> you sneak past guards who put you back to the start of the sequence if they notice you. The mod
> handles those **automatically**, on the two maps that have one — the Royal Palace Cellars and Lower
> Halls — and does nothing anywhere else. There is nothing to switch on and nothing to remember. It
> writes no game state; every other trigger on those maps (doors, conversations, the story rects)
> keeps working exactly as the game wrote it. It had a toggle on `F10` from Session 107 to Session
> 114; play confirmed it silences only the guards' catch, so the toggle went and `F10` went back to
> the game. See `src\navigation\sneak_assist.h`.
>
> Settings persist to `%LOCALAPPDATA%\FFXII-Screen-Reader\mod_settings.txt`. The game folder is never
> written to. If `%LOCALAPPDATA%` is unavailable the menu still works; the choice just resets on
> restart.
>
> ⚠ **The menu does not swallow keys.** The mod is read-only on input, so while the menu is open the
> arrow keys still reach the game and will move your character. Same constraint the status buffer has
> always had. Open it while standing still, or use `F4`, which needs no arrow keys at all.
>
> Verbose means "announce when the game announces". The game itself stays quiet when an enemy repeats
> the same ability on the same target, and when the caster is more than roughly 24 units away — that
> pacing is the game's, not the mod's.

> **The combat log is NOT modal (decided Session 48).** ~~`F4` to open / `Esc` to close~~ is **STRUCK**
> (and `F4` now means combat verbosity — see the mod menu above; that struck design was never built)
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

**Update, Session 74 (2026-07-27) — `'` is now a navigation PROBE, not a dump.**

> The key kept its slot but not its contents. It used to emit the move-frame snapshot, a wall
> self-test, the walkmap grid cross-check, the `+0x70` / `+0x54` legacy exit tables,
> `ExitDiag::DumpCoverage` and `MapExits::DiagScanScriptMapjumps` — the last of which alone hex-dumped
> 0x9000 bytes as roughly 1,152 log lines. All of it answered questions that were already settled.
>
> It now reports three things, under the new `NAV-PROBE` log category: every walkable floor layer in a
> block of columns around the player and around each map transition (next to the engine's own ground
> answer for the same point); the engine's interaction reach beside the mod's replica of it; and each
> transition seam's middle beside its near edge, in metres and steps.
>
> Two behavioural notes. It **runs on the game thread** now — the key only raises a flag, and the probe
> drains on the next field frame — because one of its reads is a game call that was never safe from the
> input thread. And it is **field-only**, which the old dump was not: press it somewhere the field is
> not live and nothing is logged at that moment. If the field is running but not yet settled (a map
> still fading in) it retries for about a second and a half and then says "Diagnostic unavailable"; if
> the field tick is not running at all, the request simply waits and fires when you are next on the
> field. **Press it while standing in the area you want measured.**

## Equipment comparison — and the AUTODETAIL mode it is waiting for

Keys `4`-`9` read the game's per-character equipment comparison while a shop item or an equip
screen is highlighted, and fall back to their party-status meaning everywhere else. The gate is
structural (a live, class-validated comparison panel **plus** the surface that drives it), so
leaving a shop restores party status with nothing to unstick.

### The two surfaces do NOT behave the same, and that is deliberate for now

| Surface | Mechanism | How it reads |
|---|---|---|
| Shop list / equip-to-whom | up to **6 characters**, signed delta + arrow | **On keypress** (`4`-`9`) |
| Pause menu -> Equipment | **1 character**, `current > preview` absolutes | **Automatically on highlight**, plus `4` to re-read |

The split is not an oversight. Six characters x two stats is far too much to hear on every cursor
move, so the shop stays on demand; the Equipment screen concerns one character and usually one or
two changed stats, which is short enough to volunteer. Tester-confirmed 2026-08-03: *"you put the
delta reader on keypress which is perfect."*

### AUTODETAIL — BUILT, Session 147 (`F7` + the `F8` row)

Autodetail is the **toggle** that reads the relevant detail on highlight instead of on a keypress.
The shop comparison is its first consumer; the Libra readout is its second.

- **OFF (default, today's behaviour):** shop comparison on `4`-`9` only, Libra on `o` only; the
  Equipment screen keeps its automatic single-character line, which is already short enough to be
  welcome.
- **ON:** each volunteers itself on its own highlight too — **queued, never interrupting**, so the
  item name and price (or the enemy's name and HP) are heard first.
- The keys keep working in BOTH modes. Autodetail changes what is *volunteered*, never what is
  *reachable* — a toggle that removed a way to ask would be a regression, not a setting.
- Each surface emits through its own existing choke point (`ShopReader` for the list,
  `BattleTargetReader::AnnounceTargetBc` for the target line) and **appends to that line rather than
  speaking a second time**. Two speakers on one surface race, and the plainer line wins — that is
  exactly how the notice board lost its Status column.
- One thing autodetail must NOT carry: the `o` key's **"Libra not active."** answer. That is a reply
  to a question the player asked; volunteered on every cursor move it would be pure nag, and the
  never-speak-filler rule stands everywhere the player did not ask.
- It follows the existing pattern for a toggle of this kind — a `ModMenu` row plus a bare-key
  shortcut, the way Combat verbosity pairs the `F8` menu with `F4`.

Worth stating plainly because it will be tempting: the Equipment screen's automatic line is **not**
autodetail already existing. It is one line about one character on a screen whose whole purpose is
that comparison. Autodetail is the general "volunteer the detail everywhere" switch, and the shop
is what it is for.

## Controller — the pad scheme (Session 173, revised 174)

> **STATUS: the right stick and the D-pad are PLAY-CONFIRMED; the rest is built and untried.** The
> first play pass (2026-08-29) confirmed the pathfinder stick, the swallowed field camera and the
> party slots. Everything below them changed after that pass and has not been played yet.

**What FFXII itself uses a pad for**, which is what the scheme had to be built around:

| Button | Field | Battle |
|---|---|---|
| L1 | Speed mode (x2 / x4) | Speed mode |
| L2 | Toggle zoom | **Lock on to target** |
| L3 | Show area map | Show area map |
| **R1** | *nothing* | Selects **Reserve** in the target list |
| R2 | Zoom the map and the license board | Hold to run from enemies |
| R3 | Recentre the camera | Recentre the camera |
| Select | Display map | Display map |
| Start | Pause | Pause |
| Triangle | Party menu | Party menu |

**R1 is the only control the game leaves free on the field**, which is why the route lives there.
The mod claims three things the game does use — Select's map, L3's area map, and the D-pad — and
each was a deliberate trade. The `Controller` row in the `F8` menu, or **L3**, turns the whole thing
off and hands the pad back untouched.

The pad reaches the mod through an `XInputGetState` intercept, and **it can only take an input away
from the game, never press one** — see the second input-write exception in `CLAUDE.md`. Anything not
listed here arrives at the engine exactly as it would with no mod installed.

**Nothing is bound to A, B, X or Y in normal play.** Those are the game's own verbs, and a mod that
eats one is a mod you cannot play through. Everything that would have wanted a face button lives
behind the modifier instead.

### Normal — no modifier

| Control | Does | Same as |
|---|---|---|
| Right stick Up | **In a menu, a message box or a battle: reads the description** — and Libra on a targeted enemy. **On the open field: previous category** | `o` / `-` |
| Right stick Down | Next category | `=` |
| Right stick Left | Previous object | `[` |
| Right stick Right | Next object | `]` |
| D-pad (field only) | Party status — **clockwise from Up: member 1, 2, 3, then the guest** | `4` `5` `6` `7` |
| D-pad (anywhere else, combat included) | Walks the Status Attributes page and an open Clan Primer entry, exactly as the arrow keys do. The game still gets the press | Arrow keys |
| R1 (field) | Route to the current selection, and start the audio beacon | `\` |
| R1 (battle) | Route to the **active target** | `p` |
| Back / Select | Mod mode — says **"Mod"** | — |
| L3 (left stick click) | Switch the pad intercept off or on — says **"Controller, Off"** | `F8` → Controller |

> **Right stick Up is the one control that changes meaning, and it is the only one.** On a plain
> idle field the description key has nothing to answer and the pathfinder has everything, so Up
> cycles the category there. Anywhere a description could be read, it reads it.

> **R1 is the other control that changes meaning.** Out of combat it routes to whatever the stick
> has selected; in a fight it routes to the target you are already acting on. Both are one press,
> in the context where each is wanted. With a targeting cursor up the mod does not take R1 at all,
> so the game keeps its Reserve switch.

> **The D-pad is only taken on the open field — never in combat.** Everywhere else it is dispatched
> to the mod *and* passed straight through, so the game's own cursor still moves. That is what the
> arrow keys already do, since the mod cannot swallow a key. A fight is always one command menu
> away, and party slots are not worth costing you that cursor.

> **L3 hands the whole pad back, and hands it back again.** It works whether the intercept is on or
> off — a switch you could only throw once would leave you at the keyboard to undo it. While the
> intercept is off the mod reads that one button and nothing else, and changes nothing the game sees.

### Mod mode — press Back, then one button

Back says **"Mod"**. The next button is a mod command and the mode ends. Anything unmapped — Back
again included — says **"Cancelled"**, and so does five seconds of silence, so there is no mode to
get stuck in.

> It moved off L3 in Session 174. A stick click cannot be reached without taking your thumb off the
> stick you are steering with, which is why the mode went a whole session without being tried once.

| Button | Does | Same as |
|---|---|---|
| Start | Open the mod's settings menu | `F8` |
| A | Describe / Libra | `o` |
| B | Re-read the last line of dialogue | `t` |
| X | Rescan, and say the area name | `` ` `` |
| Y | Describe the current selection | `/` |
| D-pad Up | License Points | `U` |
| D-pad Down | Party gil | `g` |
| D-pad Left / Right | Combat log: older / newer | `,` `.` |
| L1 | Target readout | `;` |

> **No setting has a pad button of its own, with one exception.** Combat verbosity, the audio
> beacon, the availability filter, autodetail and the volumes are all changed in the settings menu,
> which is Back then Start —
> and the menu says what it changed and what the new value does. The keyboard shortcuts for them are
> unchanged. A pad has few buttons and none of them is worth spending on a second route to a switch.
>
> The exception is the `Controller` switch on L3, and only because it is the way out: reaching the
> pad's own off switch through a menu you drive with the pad is circular.

### The mod menu, from the pad

While the mod's settings menu is open it owns the pad: D-pad or right stick moves between settings
and changes the focused one, **A** reads its description, **B**, **Start** or **Back** closes it. Everything
the pad has no direct binding for is reachable this way — the availability filter, autodetail, the
volumes, and the `Controller` switch itself.

### What has no pad binding

`F6` (label from the clipboard), `'` (the diagnostic probe), `8` and `9` (equipment columns 5 and 6),
and `B` / `N` (the Bhujerba shout minigame). The first two are development keys; the rest are
keyboard-only for now rather than deliberately excluded.
