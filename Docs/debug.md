# FFXII-Screen-Reader — Debugging Log

This file is structured for keyword searching. **Always grep before proposing solutions.**

## Tried & Failed

Approaches that were attempted and did NOT work. Each entry tagged with `KEYWORDS:` for
grep. Check this FIRST to avoid repeating failed approaches.

**KEYWORDS: system notification banner added to the Party Menu Clan Primer full-screen overlay
desaturated blur icon escape not spoken st2e FUN_002f9860 OPEN ISSUE**

**OPEN — full-screen SYSTEM NOTIFICATION banners are not vocalized.** Observed 2026-07-21:

    <wing icon> Clan Primer has been added to the Party Menu. <wing icon>

drawn centred over the field with the whole scene desaturated and blurred behind it. The mod says
nothing. Other notifications of this shape ("X has been added to...", feature/menu unlocks) are the
same surface and equally silent.

What is already known, so the next session does not start cold:

- **It is NOT event dialogue.** Searched all 17,268 messages across the 617 extracted US `.ebp`
  scripts (`FFXII-Decompile/extracted/`, see `tools/ebp_find_pagebreak.py` for the walk) for both
  "added to the Party Menu" and "Clan Primer": **zero hits**. So the telop reader
  (`FUN_002e16b0`) and the `.ebp` message table can never see it -- do not go looking there.
- **So it is a system/UI string**, which points at the st2e tables the menu text uses --
  `FUN_002f9860(id) -> codec byte*`, already hooked by `TextCapture::HookResolve` (that hook
  currently only caches ids 1000/1001 and the key-binding block, so the id would be visible there
  with the filter widened). `tools/st2e_decode.py` is the offline counterpart.
- **The line is composed, not literal.** "Clan Primer" is coloured differently from the rest, so the
  banner is a template with a substitution slot -- almost certainly the codec-sprintf STRING slot
  `0x0f 31` (3 params) already in our table -- filled with the item/feature name.
- **The flanking wings are icon escapes**, the `0x0f 0x40-0x6B` glyph family (1 parameter) in
  `game_text.cpp`'s `EscapeParamCount`. They decode to nothing today, which is correct for speech.
- The display surface is NOT identified. It is not the item toast (`FUN_0035e070`, which is the
  "Obtained <item>" path and works), and not the menu system-message surface (`FUN_0057c480`, which
  is menu-only). The screen-wide desaturation suggests its own overlay/effect object -- that effect
  may be the easier thing to find first and work back from.

DEFERRED by user instruction 2026-07-21: document now, fix in a later session.

**KEYWORDS: telop burst ambient NPC lines area load 18 messages 140ms FUN_002e16b0 slot 0
ambient chatter spoken on entry OPEN ISSUE**

**OPEN — the telop setter fires for EVERY ambient NPC line at area load, and we speak them all.**
Found 2026-07-21 while fixing multi-page dialogue. At Rabanastre entry the log shows **18 telop
messages in 140 ms** (+28641 .. +28781), all `slot=0`, all different NPCs: *"There you are,
Kytes."*, *"Creature spotted in the Estersand."*, *"Ah, Vaan. Migelo send you, too, did he?"*,
*"Quite the affair, throwing a banquet..."* — the area's whole ambient chatter table.

Each is spoken with `interrupt=true`, so they cancel one another and the player hears fragments of
the last one. This is SEPARATE from the multi-page fix (that one was a single message containing
several pages, now split on codec 0x03).

Not yet known: whether these calls are the game registering the table or genuinely displaying each
line for a frame. `FUN_002e16b0` param_2 is the slot (0-7, clamped) and all 18 use slot 0, which
argues for sequential replacement rather than 18 simultaneous displays. Needs the display-vs-set
distinction settled before any gating — do NOT simply rate-limit it, that would be a dedup in
disguise (see the no-dedup rule).

**KEYWORDS: dialogue choice pop-up options not spoken Tomaj hunt bill yes no selectable
conversation branch cursor pointing hand R Log Space Confirm OPEN ISSUE**

**OPEN — dialogue CHOICE options are not vocalized.** Observed 2026-07-21 in the Rabanastre tavern
(Tomaj, hunt-bill conversation). On screen:

- Speaker nameplate **Tomaj** with a portrait icon.
- Body: *"Do you want to hear all the details?"* — believed already spoken by the existing reader.
- **Two selectable choices below it**, a pointing-hand cursor on the first:
  *"Yeah, that would help."* and *"No, I think I got it."*
  **Neither is announced when navigated.** This is the defect.
- Footer hints: `R` = Log, `Space` = Confirm.

What is known: this is a *choice* dialogue and is NOT any of the surfaces already handled.
It is not the `FUN_0057c480` menu-message surface (that one is classified + logged but deliberately
muted, and is menu-only — it never fires for field text), and it is not the title/new-game confirm
(`FUN_00241d40`, handled via the 0x8000 focus path). The read-point for the choice LIST and its
cursor index is **not yet identified** — that is the work. Start from the message-surface section of
`GameArchitecture.md` and from whatever draws the pointing-hand cursor; the body text arriving
correctly suggests the body and the choices come from different objects.

DEFERRED by user instruction 2026-07-21: document now, fix in a later session.

**KEYWORDS: pathfinding accuracy near target reroute sand sea legs increase expands zero
recalculation oscillation fineCell 1.5m string-pull OPEN ISSUE**

**OPEN — routing gets LESS accurate the closer you are to the target.** Reported 2026-07-21: while
approaching a target the player was rerouted around the Sand Sea several times as the route
recalculated. Log evidence, same target cell throughout (`tgtCell=(99,21)`):

| seq | from (x,z)    | legs | expands | rays |
|-----|---------------|------|---------|------|
| 80  | 138.9, 35.4   | 3    | 16      | 454  |
| 82  | 143.2, 38.3   | **4**| 7       | 182  |
| 85  | 142.6, 39.8   | **4**| 8       | 210  |
| 86  | 143.8, 32.9   | 2    | 4       | 140  |
| 87  | 148.0, 30.8   | 3    | 1       | 28   |
| 88  | 149.4, 32.2   | 2    | **0**   | 0    |

The leg count RISES (3 -> 4) while search effort collapses (`expands` 16 -> 0), and the player's own
position oscillates rather than converging. `fineCell=1.5m`: at close range the target is only a
couple of cells away, so the coarse grid plus the string-pull can flip the chosen cell between
recalcs — a plausible starting hypothesis, NOT a diagnosis.

DEFERRED by user instruction 2026-07-21: recorded with the numbers; the planner was deliberately
NOT changed this session.

**KEYWORDS: dedup deduplication debounce speech suppressed silent menu re-entry focus cache
stale owner index text battle command Attack list status chooser MaybeAnnounce S51**

**Dedup as the fix for repeated speech — WRONG, and it caused silence.** Whenever an announcement
repeated, past sessions added a "same as last time, stay quiet" check. Every one of those was on an
EVENT-DRIVEN hook, where each fire is a real event the player needs. The damage:

- `menu_reader.cpp` `OnFocus` compared `(owner, index, text)`. The active-pane gate above it returns
  early WITHOUT refreshing that cache, so the stale entry survived an excursion to another pane and
  swallowed the return — leaving a pane and coming back said **nothing**.
- `ingame_menu_reader.cpp` `OnBattleCommandFocus` compared spoken TEXT alone, with no owner: backing
  out of the Attack list and reopening it was silent.
- `title_reader.cpp` put its per-draw guard on the shared path, so re-focusing the same title row
  was silent.

A repeat is an annoyance; silence is a lost position. The rule is now: **no speech dedup**, except
(1) the user asks in the current conversation, or (2) it guards a per-frame/per-draw function AND
the comment names that function. A repeat from an event hook means a second call path — find it.
Removed in Session 51. **Do not re-add a filter to quieten a repeat.**

**KEYWORDS: combat_system.md struck claims S49 FUN_0028e110 no message id FUN_00536410 outcome 9
not preview result+0x00 0x20 DAT_0209a1f0 not leader P-E cancelled row+0x00 not action name
neutral foes group 0 only aggression actor+0x6A0 pointer deref actor+0xEA4 not hostility
FUN_00313b30 per action FUN_0035c7b0 0x17 dead FUN_00312280 no reward args** (Session 49) —
**eleven claims in `Docs/combat_system.md` were disproved offline and are STRUCK there.** Do not
rebuild on them. The eight that would have shipped a bug:

1. ~~Hook `FUN_0028e110` for the game's combat sentences~~ — **it never receives the message id**,
   only a dwell class, so the realtime-vs-log-only policy has nothing to key on. Use
   **`FUN_00536410`** (RVA `0x416410`): id at `RCX+4 & 0x7FFF`, finished string into `RDX`, one
   frame, and upstream of the toast fork where the ticker is never called.
2. ~~`result+0x04 == 9` is the AI preview, filter it~~ — 9 is the **default seed**. Preview is
   `result+0x00 & 0x20` (`FUN_00308b90`), which **never calls the applier**. No filter needed.
3. ~~Filter status ticks on `attacker == 0`~~ — **filter `actionId == 0xFFFF`**; `FUN_00310db0`
   makes two *real* calls with a null attacker.
4. ~~`DAT_0209a1f0[3]` is the party leader~~ — it is 4 × `sceneObj*` in roster order. Leader is
   `*(u8*)(W + 0x5AA4)` → BtlChr → pool scan (`FUN_00327150`). **Probe P-E cancelled.** Also note
   the shipped `*(u8*)(bc+5) == 0` test matches *every* roster member, not the leader.
5. ~~Ability name is the id at `row+0x00`~~ — that is a **description** id; `Attack` and every
   `Reserve` row share `4000`. The name is **`row+0x34`** → the `word.bin` pool.
6. ~~The engine files Neutrals under Foes, so a Neutral is a legal attack target~~ — **backwards.**
   The emit gate is `(g & ~2) == 0`: foes = group **0 only**, and Neutrals reach neither list.
7. ~~Aggression: `f = *(u32*)(actor+0x6A0)`, radius `+0x6A4`~~ — **`+0x6A0` is a POINTER.** Same
   missing-deref class as the `4`/`5`/`6` bug. Use `*(u32*)(*(u64*)(actor+0x6A0))`, radius at
   `aiData+0x04`. Still only 0.97 structurally, so it does **not** ship.
8. ~~"In battle" = any party actor with `+0xEA4 != 0`~~ — that mask is "who has an action aimed at
   me", **not gated on hostility**, so an out-of-combat Cure trips it. Use
   `*(u32*)(actor+4) & 0x100000` (`FUN_002fead0`).

Plus three lifecycle mislabels: `FUN_00313b30` fires ~2× **per action**, not per battle;
`FUN_0035c7b0` never receives `0x17` from any caller (use **`FUN_0035c8a0`**); and `FUN_00312280`
passes **none** of EXP/LP/gil/loot as arguments (gil → `FUN_00469e80`, loot → `FUN_003180f0`).

**KEYWORDS: game_text.cpp high-bit fallback wrong 75 of 102 codec escape 0x29 eats punctuation
0x2d numeric slot missing level number 0x10-0x1f two-byte glyph stray letter icon family 0x40
ffxii_codec.py** (Session 49) — the decoder's *"after `0x0f <sel>`, consume every following byte
`>= 0x80`"* fallback is **wrong on 75 of the 102 battle messages**, not just the one apostrophe
previously documented. Digits (`0x85`–`0x8e`) and punctuation (`0x99`/`0x9a`/`0xa0`–`0xaf`) are all
`>= 0x80`, so sentence-final `.` and `!` are eaten as parameters. Additionally: `0x2d` is the
**numeric substitution slot** (why the level-up line lost its number), `0x10`–`0x1f` are **2-byte
extended glyphs** whose trailing byte the mod renders as a stray letter, and the `0x40`–`0x6b` icon
family takes exactly **1** parameter byte. The complete table is
`..\FFXII-Decompile\tools\ffxii_codec.py`, regression-tested to **zero structurally-unexplained
bytes**. Port `game_text.cpp` from it; do not re-derive.

**KEYWORDS: exit destination door pairing getmapdestposbyindex nowjumpindex lastjumpindex jump-index
dispatch gateway record arrival symmetry routine table 2 entries 0x5C routine index __MJ_CTRL
mapData+0x54 +0x84 +0x70 +0x8c** — the long hunt for "which door goes where" (Session 46). Everything
below was tried and is **DEAD**; the answer was the map's own field script (`__MJ_CTRL<N>` owns `+0x54`
slot `N+1` — see `GameArchitecture.md`, *Exit destinations = the map's own FIELD SCRIPT*).

- **Looking for a `destination-by-door-index` getter — there is none.** Every `mapData` accessor was
  traced across all 33,105 functions. `getmapdestposbyindex` is `FUN_00264b90` selector 1 and returns an
  arrival **position** from `mapData+0x84`, not a map id. The doc table said otherwise and **sent this
  session chasing a getter that does not exist** — now struck.
- **Expecting the destination to be a FIELD on the exit record.** It is not. `+0x54` records are
  x/y/z/angle then zero bytes (raw dump confirms); `+0x70` field-sign records hold positions with an
  empty destination slot on interior maps (`destIdx=0`, `areaId=0xffff`); `+0x8c` is keyed by a
  field-sign `+0x1d` byte, never a jump index.
- **"The routine table has 2 entries."** A parse bug, not data: **word 0 is an entry COUNT**. Reading it
  as a record, plus a "stop on zero offset" guard, truncated a 24-routine table to 2.
- **A jump-index dispatch in the script.** `nowjumpindex` (native `0x8f`) has **ZERO** call sites in the
  whole script; `lastjumpindex` (`0x90`) returns a **map id** (compared against the teleport-menu ids
  306/630/66…), not a door index. There is no `switch(jumpIndex)`.
- **Reading `0x5C`'s operand as a routine index.** It is a label/target — observed values (221) exceed
  the routine count (24). Any "routine X calls routine Y" conclusion drawn from `5c <n>` is invalid.
- **The "gateway record"** (`{dest, entrance, ptr}` around `+0x5E00`) is a denormalized mirror of the
  script literal; **no engine code reads its fields**, and it carries no source-door position.
- **Cross-map arrival-symmetry as a *blocker*.** The relation is real and was used to cross-check the
  door rule, but it needs every neighbour's data, so it is not a way to label the map you are standing
  in. The in-map `__MJ_CTRL` naming makes it unnecessary.
- **Offline `.mpk` extraction of mapjump literals** finds zero hits — on disk the bytecode is
  Phyre-encoded/relocated; plain bytecode exists only in the **loaded** blob.

**KEYWORDS: message read-point spec refuted FUN_0057c480 menu system message DAT_0209ac30 0x328
obtained item field chest e5f0 FUN_003c02b0 FUN_002baf80 world map screen page+0x138 map id
mini_face_c texture bundle 0.90 0.98 below bar** — `notes/message_text_readpoints_spec.md` §A and
§B(field) are **REFUTED** (Session 45). §B claimed field-chest "obtained X" reuses the `FUN_0057c480`
surface at **0.90 — below the project's 0.98 bar, and it was wrong**: that proc's case 1 does
`*(longlong*)(DAT_0209ac30 + 0x328) = surface`, i.e. it registers into the MENU manager; it is the
menu system-message window ("cannot equip", "sold") and never fired once in a 17-minute play log.
§A put dialogue on `FUN_002baf80` + `FUN_003c02b0`; those are the **world MAP screen** (`page+0x138`
= map id, `out+8` = map name, `DAT_02b457e0+0xe8` = map DB, assets `ArtData/menu/localmap/`). §A's
"decisive" evidence — that the widget owns the `mini_face_c` speaker portrait — was a misread: it is
a **texture-bundle name** passed to `FUN_0024a5a0` alongside `battle_4_p`/`s_font_c`, and the
`002baf80` family never references it. LESSON: the spec self-tagged these 0.98/0.90; a confident tag
is not evidence. The REAL obtained-item path is `FUN_0035e070` + `widget+0xC8`.

**KEYWORDS: Ghidra dropped argument register passthrough FUN_00264ac0 FUN_00264ae0 group index
Pfn_ExitCount no parameters count=0 every map +0x70 field sign dead never measured** — "the
`mapData+0x70` exit array is empty/dead" (Session 44) was **measuring the mod's own bug**, not the
data. `Pfn_ExitCount` was typed `int(__fastcall*)()` and called as `fn()`, but `FUN_00264ac0` takes a
GROUP index: Ghidra decompiles it as `(void)` only because it never *writes* `ecx` — it passes its
own incoming `ecx` straight through to `FUN_00264ae0(group)`, which uses it as a real array index
(`if (param_1 < *(int*)groupTable) return groupTable[param_1+1] + blob;`). Calling with no argument
left register junk in `rcx`, the bounds check failed, and `count` was 0 on **every** map regardless
of the data. LESSON: when Ghidra shows a call-through with no args and the callee indexes `param_1`,
that is a **passthrough signature**, not a no-arg function — check the game's own caller (here
`FUN_003f9720` passes the same group to both getters).

**KEYWORDS: parse_mapdata.py wrong blob base mpk+0x10 cluster child +0x8c zero all 550 files
contradicted by live log destCount=2 offline extraction premise failed** — `parse_mapdata.py`'s
"`+0x70` and `+0x8c` are zero in all 550 `.mpk`" is **not credible**. The live mod log shows
`destBase` non-null with `destCount=2`, i.e. `mapData+0x8c` IS populated at runtime — so the parser
is reading the wrong blob. Session 44's own note concedes the map-control blob "is a cluster child,
not `mpk[+0x10]`", which is exactly what the parser uses (`mld = mpk[u32(mpk,0x10):]`). LESSON: an
offline parse that disagrees with a runtime read is wrong until proven otherwise — the runtime is
ground truth.

**KEYWORDS: FUN_00353490 getmapjumpanglebyindex not party placement mapData+0x54 arrival spawn
demotion Category::Event Session 43 reverted** — Session 43's "`mapData+0x54` is the party
ARRIVAL/SPAWN table, not exits" rested entirely on *"proven: FUN_00353490 places the party
post-jump"*. `FUN_00353490` is abs `0x353490` = RVA `0x233490` = the mod's OWN
`GETMAPJUMPANGLEBYINDEX` constant (`nav_rva.h`); its body returns a jump's **angle**. It places
nothing. `+0x54` is the map-jump point table = the exits, tester-confirmed by walking into them.
LESSON: check a claimed function identity against the RVAs the mod already has before building on it.

**KEYWORDS: dbg_idx 5140 formula broken native slot action_binding_tables selector 0 index
arithmetic btlAtel resolve by behaviour** — `nav_rva.h:247`'s *"sel-0 slot = mapctrl.dbg_idx - 5140"*
does **not** reproduce: `getmapjumpposbyindex` (dbg 5671 -> slot 529) implies 5142 while
`getmapjumpanglebyindex` (dbg 5943 -> slot 803) implies 5140. The `.dbg` symbol list interleaves
variables and source markers (e.g. `mapctrl.src`) with actions, and duplicate symbol names exist, so
index arithmetic is contaminated. Resolve natives **by behaviour** instead (a run of consecutive
functions funnelling through one resolver, address order matching .dbg order, each body confirmed
against its name) — that is how the seven `btlAtel*FromPartySlot` natives landed at 0.99.

**KEYWORDS: GameText Decode 0x0f escape high bit run eats digits punctuation 0x85 0x8e obtained 3
Potions The command can selector param count FUN_002ac5f0** — skipping a `0x0f` escape by consuming
every following byte `>= 0x80` is **wrong** and corrupts live text. Digits are `0x85`-`0x8e` and
punctuation `0x99/0x9a/0xa0`-`0xaf`, all `>= 0x80`, so a zero-parameter escape (`0x21`) followed by a
number ate it. Seen live as `"Sometimes it's necessary to escape.The  command can"`. Param counts are
**selector-dependent**, from the game's own interpreter `FUN_002ac5f0` (RVA `0x18C5F0`): `0x21`=0;
`0x20/0x27/0x2f/0x32/0x34/0x35/0x37/0x3a/0x3c/0x3d/0x3e/0x56`=2 (`FUN_003ffab0`); `0x31`=3
(`FUN_003fff10`). `tools/ebp_msg_decode.py` has the same latent bug — its rule only held on the
offline corpus.

**KEYWORDS: pathfinder scanner actor pool BtlWork gimmick gate missing enumeration** — Enumerating
field objects by walking the 32-slot BtlWork actor pool `DAT_0208e688` (RVA 0x1F6E688) finds ONLY
characters/combatants (NPCs, party). Static field gimmicks — gates, doors, switches, treasure,
crystals — are NOT in that pool, so a gate the tutorial needs is invisible to the scan (found "2
objects" = the 2 NPCs only). FIX under Solved: enumerate the scene-object HANDLE TABLE instead.

**KEYWORDS: pathfinder classify def+5 kind NPC object inverted** — Classifying NPC vs object by the
actor def kind byte `def+0x05` (`(kind==0)?NPC:Object`) mislabels every NPC as "Object". `def+0x05`
is 0=static-prop / 1=animated-character, and field NPCs are kind **1** — inverted and too weak.
FIX: classify by npcdic id band + the scene-object interaction flags.

**KEYWORDS: pathfinder route turn-by-turn silent drain FUN_00314020 hook game thread** — Draining
the game-thread route planner from `FUN_00314020` (0x1F4020, the render step, `mode==0`) leaves `/`
SILENT: that path sits behind a fixed-timestep accumulator AND an else-branch bypass
(`FUN_001800e0()!=0` → `FUN_002f1770` directly) that a scripted tutorial holds open, so the drain
never runs. FIX: drain from `FUN_0022a770` (0x10A770), the no-arg per-field-frame tick.

**KEYWORDS: scene object position 0xB8 FUN_00265020 class nibble guard gate zero** — Reading a scene
object's world position by replicating engine getter `FUN_00265020` INCLUDING its class-nibble guard
`(*(u8)(sceneObj+3)>>5) ∈ {1,3}` returns (0,0,0) for a gate whose class byte isn't 1/3 → the object
is dropped for "no position". FIX: read the raw chain `*(sceneObj+0xB8)+0/4/8`, guard only on the
transform node != 0 (valid for all world-present categories 1–7).

**KEYWORDS: name resolver FUN_0035d380 party roster model index object** (Session 24) — Resolving a
field object's name with `FUN_0035d380(1, def+4)` always returns empty ("Object"): it is the
party/roster resolver fed a model index. FIX: read the name key at `sceneObj+0x102` → npcdic.

**KEYWORDS: pathfinder walkability Bullet world FUN_006a1a70 FUN_006a0310 ctx capture prologue never
fires no region** (Session 33) — Building route walkability on the **Bullet** raycast world
(`FUN_006a1a70`, world at `*(ctx+0x60)`) fails in the **Nalbina prologue**: it builds NO Bullet
region-world. `FUN_006a0310` is the sole world constructor and runs only inside the master physics
tick `FUN_00698c80`'s per-region loop, SKIPPED when region count==0. FOUR ctx-capture hooks (builder
`FUN_006a0310`, step `FUN_0069f070`, raycast `FUN_006a1a70`, char-ground `FUN_006a5c00`) all install
but NEVER capture; `failMask=0x40[world]`. DO NOT retry Bullet for field walkability. FIX under
Solved: the SQEX floor/wall walkmap (`FUN_003208c0` / `FUN_00230b60`), which is what the AI NPCs use.

**KEYWORDS: egocentric directions faceNode reference wrong node+0xA4 stale idle combat target-facing
DAT_02aedf94 view matrix** (Session 37) — Anchoring egocentric directions ("North"=forward) to the
character facing `faceNode` (node+0xA4) is WRONG: `FUN_00358cb0` writes it ONLY while the leader is
moving, so when stationary (exactly when you query directions) it's stale, and in combat the battle
action/steering subsystems (`FUN_00307300` 0x1c7 / `FUN_0037b4d0` / `FUN_0031adb0`) turn it to face the
TARGET → boss called "NE" while audibly to the left, and ~90°-off field directions. ALSO wrong: the
scalar camera-yaw `DAT_02aedf94` (RVA 0x29CDF94) — `FUN_003820c0` builds it from the view/sibling matrix
`DAT_02aede70` with sign-flipped rows, so its delta to the move heading wanders (the diagnostic's
`camLookDeg` never held a constant offset). FIX under Solved: read camera-forward from the MOVEMENT
matrix `DAT_02aedf30` row 2. (`bVar5 & 4`, the skip-move-facing flag, is script-only — NOT a combat lock,
so don't look for a lock flag.)

## Solved Problems

Problems that were resolved. Each entry has `KEYWORDS:` + `SOLUTION:`. Check this to
reuse known-good solutions.

**KEYWORDS: menu re-entry silent pane focus not spoken returning to pane dedup removed S51**

**Re-entering a menu pane was silent.** SOLUTION: the `(owner, index, text)` dedup in
`menu_reader.cpp`'s `OnFocus`. Because the pane-isolation gate returns before the cache is written,
the cache still held the row you left, so the identical focus on return was dropped as a duplicate.
Removed the gate; `g_lastOwner`/`g_lastIndex` survive (renamed `g_focusOwner`/`g_focusIndex`) purely
as the "current row" the config value-change hooks read. See the Tried & Failed entry above for the
general rule.

**KEYWORDS: duplicate RVA different names aliased global BTLWORK PARTY_MGR_PTR FIELD_STATE_BLOCK
value grep centralization S51**

**Duplicate offsets hiding under different names.** SOLUTION: grep constant VALUES, not names — a
name-based check misses `0x2D9F190` appearing as `RVA_BTLWORK`, `PARTY_MGR_PTR` and
`FIELD_STATE_BLOCK` in three files. The exact command is in `Docs/PerformanceIssues.md`
("How the duplicates were found"). Shared offsets now live in `src/core/phyre_types.h`.

**KEYWORDS: read-only input controls game speed 1 2 3 mod not modifying no injection no
SendInput no WriteProcessMemory DirectInput const buffer** (Session 44) SOLUTION: The tester's
player speed jumped mid-session and asked whether the mod alters game controls. **It does not — the
mod is strictly READ-ONLY on input and game memory, verified by audit:** (1) the DirectInput hook
`HookedGetDeviceState` (`src/proxy/dinput8_proxy.cpp`) calls the real `GetDeviceState` and passes the
buffer to the tracker as a `const unsigned char*` — it never writes the buffer; `InputTracker::
FeedDInputKeyboard` only edge-detects/reads. (2) Repo-wide there is **no** `SendInput` / `keybd_event`
/ `mouse_event` / `PostMessage(WM_KEY…)`, **no** `WriteProcessMemory`, and **no** memory-write helper —
the mod has no path to write game memory. The only `VirtualProtect` is the one-time vtable patch that
installs the read-only hook. (3) Every game function the mod calls is a pure getter (area name, ground/
segment/exit queries). **The speed change was the tester's own `1`/`2`/`3` keypress** (those are Game
Speed 1×/2×/4× — see Controls.md; the old "Lock On / Target Group" labels were wrong). The mod reserves
none of `1`/`2`/`3`.

**KEYWORDS: left ctrl escape toggle battle menu won't open menus locked stuck ctrl sound cue ping
whoosh Controls.md binding INPUT-DIAG modifiers not the mod** (Session 47) SOLUTION: The tester could
not open the battle menu and the game acted as though **Ctrl were held** on every keypress. **It is the
game's own binding: Left Ctrl = "Escape"** (`Controls.md:43`, captured from the in-game Controls menu
2026-07-07), and it **toggles** — one press emits a sound cue and locks the menus, a second press emits
another cue and unlocks them. **STRIKE the "stuck Ctrl / the game sees `Ctrl+<key>`" framing** — there
is no stuck modifier and no OS/engine fault to split. A stuck-Ctrl diagnostic was written, committed
(`7d51917`) and reverted the same session; do not re-add it. It would also have spammed the log, since
its change-detector includes Shift and **Left Shift is bound to Walk/Run**. **Lesson: grep
`Controls.md` before instrumenting any suspected input bug** — the game's bindings are captured there
so this is a lookup, not an investigation. Same family as the Session 44 entry above: the "bug" was a
game key doing its job.

**KEYWORDS: egocentric directions camera-relative camera-forward DAT_02aedf30 row2 DAT_02aedf50
DAT_02aedf58 atan2(-fwd.x,-fwd.z) ReadCameraForward North=forward calibration** (Session 37) SOLUTION:
FFXII movement is camera-relative (`FUN_004742a0` RVA 0x3542A0 rotates the stick by camera matrix
`DAT_02aedf30`: `worldMove = stickX·row0 − stickY·row2`; NO top-down branch). Anchor egocentric
directions ("North"=forward=where UP takes you) to the LIVE camera-forward from the MOVEMENT matrix
`DAT_02aedf30` row 2 (+0x20): `fwd.x=DAT_02aedf50` (RVA 0x29CDF50), `fwd.z=DAT_02aedf58` (RVA 0x29CDF58);
**up-direction yaw = `atan2(−fwd.x,−fwd.z)`** (UP ⇒ stickY>0 ⇒ worldMove = −row2). Same `atan2(x,z)`
convention as faceNode → drop-in for the existing egocentric transform (`ego = BearingDeg(target) − (180 −
facingDeg)`). Writer `FUN_003820c0` (RVA 0x2620C0), live in field gameplay. Valid idle, after a camera
rotate, AND in combat (unlike faceNode). Shipped as `PlayerState::ReadCameraForward`; all `facingRad`
sources repointed to it; `;`="Forward points <cardinal>". **Sign + handedness CONFIRMED** by the baked `'`
position-delta calibration: W leg `camFwdNeg==moveVec` (168.9°), D leg right=forward−90 ⇒ ego +90=East.

**KEYWORDS: pathfinder field object enumeration handle table DAT_02098e10 gate NPC gimmick**
SOLUTION: Enumerate the scene-object HANDLE TABLE `DAT_02098e10` (RVA 0x1F78E10) — the master
registry the game's own interaction scanner `FUN_0025b820` (0x13B820) walks, holding EVERY live
field object (NPCs + static gimmicks) from map load. 5 containers × 0x288: active `+0x10&1`; entry
array `*(base+c*0x288+0x08)`; count `*(int)entries`; object i `*(entries+0x08+i*8)`. Filter
interactive by `sceneObj+0x1C` flags (`0x400`=talk/NPC, `0x4`=action/gate-door-switch) + npcdic
band. Position `*(sceneObj+0xB8)+0/4/8`. This is the correct source for a proactive object list.

**KEYWORDS: pathfinder SQEX field walkmap ground wall FUN_003208c0 FUN_00230b60 mask 4 walk class
getgroundy prologue** (Session 33) SOLUTION: Field walkability = the game's own **SQEX floor/wall
mesh** (the AI NPCs walk it; loaded per-map, independent of Bullet, so it's live in the prologue).
Ground-at-XZ: **`FUN_003208c0`** (RVA 0x2008C0) `bool(float x, float z, float* outY)` → walkable-floor
exists + height (uses cached ctx `DAT_02ec1370`). Wall/segment: **`FUN_00230b60`** (RVA 0x110B60)
`int(ctx0, out16, from[4], to[4], u16 mask, u32 flags)` → hit index >=0 BLOCKED / <0 CLEAR. ctx0 =
`*DAT_0209a678` (RVA 0x1F7A678), gate `DAT_0209a670` (RVA 0x1F7A670). Both reentrant/read-only, safe
for thousands of calls/route. **mask is a query-CLASS enum (compared `==4` in `FUN_0022cc50`), NOT a
bitmask: pass mask=4, flags=0 (WALK class)** — exactly what the player leader's own wall feelers use
(`FUN_0032cf50`→`FUN_003d97e0(…,4)`→`FUN_00230b60(…,4,0)`). Class 4 blocks real + character-only
walls, skips camera-only planes/floors/ceilings; **0xffff/1 is the CAMERA class and is doubly wrong**
(falsely blocks camera planes → phantom-wall detours, falsely passes character-only walls). Shipped as
`src/navigation/map_query.{h,cpp}` (`MapQuery::GroundAt` / `SegmentClear` / `SegmentHit`).

**KEYWORDS: pathfinder route drain FUN_0022a770 game-thread per-frame tick turn-by-turn** SOLUTION:
Drain the game-thread route planner from `FUN_0022a770` (RVA 0x10A770), the no-arg per-field-frame
tick (walking state `DAT_02064ad3==2`, returns u64). Drain unconditionally at entry — no accumulator
or bypass can hold it silent (unlike FUN_00314020). Match the u64 return.

**KEYWORDS: scene object world position sceneObj 0xB8 transform node universal** SOLUTION: Any scene
object's world XYZ = `node = *(sceneObj+0xB8); x=node+0, y=node+4, z=node+8` (floats). Works for
NPCs and static gimmicks alike (categories 1–7). Guard only on `node != 0`; do NOT apply
FUN_00265020's class-nibble gate.

**KEYWORDS: pathfinder classify npcdic id band interaction flags NPC person object** SOLUTION:
Classify by npcdic id (`sceneObj+0x102 & 0xbfff`): 433–469 = gimmick band (434 Treasure, 466 Gate
Crystal, 469 Save Crystal, 468 Urn, 435–459/467 crystals) → sub-type; else scene-object talk flag
(`+0x1C & 0x400`) → Person; else Object. Locale-independent; the spoken label is always the game's
own text.

**KEYWORDS: non-live scanner category switch stale count objects-0 ChangeCategoryLocked rescan** SOLUTION:
Switching nav categories (`-`/`=`) spoke a STALE count (e.g. "Interactables, 0") when the target
category's actors weren't in the last scan — `ChangeCategoryLocked` (`entity_list.cpp`) counted
`g_entities` WITHOUT a fresh scan, while every other command (`[`/`]`/`\`/`` ` ``) calls
`RescanLocked()`. FIX: call `RescanLocked()` at the top of `ChangeCategoryLocked` before counting, so
the count reflects live actors. This was the last non-live path in the scanner (Session 26).

**KEYWORDS: enemies scanner BtlWork pool DAT_0208e688 def+5 foe polarity Category::Enemy** SOLUTION:
Enemies are NOT in the scene-object handle table's interactive set (no talk/action flag) — list them
from the BtlWork combatant pool `DAT_0208e688` (32×0xf50, count `DAT_0208e6a0`): per slot def=`+0x698`
(null=empty), active `+0x00 & 0x10`, ENEMY = `*(u8)(def+5)==0` (0.85 — verify via the `'` pool dump;
flip `NavRva::ENEMY_DEF_KIND` if party shows as enemy), name codec* = `+0x18`, position via
`sceneObj=+0x10`→node+0xB8. `ScanEnemiesLocked` appends them as `Category::Enemy` (Session 26, pending
in-game polarity confirmation).

**KEYWORDS: in-game menu reading field FUN_002a6190 battle FUN_0055cd40 0x8000 not-index cell-pointer** SOLUTION:
The "universal" 0x8000 focus reader was silent in-game because `FUN_00247510`/0x8000 is a shared
transport, not a shared contract. FIELD menu = window class `FUN_002a6190` (0x186190; ALL submenus);
row text = `0x03`-separated codec buffer at `window+0xF8`, focused row DATA index at `window+0x124`
(set by the game handler → read AFTER `s_origDispatch`). BATTLE command menu = window class
`FUN_0055cd40` (0x43CD40; ALL submenus); 0x8000 `val` is a POINTER to the focused 0x38-byte cell, name
codec* at `cell+0x00`, validity s16 at `cell+0x08` (-1=empty). Handled in `src/ui/ingame_menu_reader.cpp`,
delegated from `menu_reader`'s single hook (Session 26, pending in-game validation).

## Mod Architecture

Current module interaction diagram + logging format. Keep up to date as modules land.

```
                  ┌────────────────────────┐
                  │ FF12 Module Loader     │
                  │ (dinput8.dll proxy,    │
                  │  ffgriever, BSD-2)     │
                  └─────────┬──────────────┘
                            │ loads our DLL from modules/
                            ▼
┌──────────────────────────────────────────────────────────┐
│ FFXII-Screen-Reader.dll                                  │
│                                                          │
│  ┌─────────────┐                                         │
│  │ proxy/      │ → DllMain → deferred init thread        │
│  │ module_entry│                                         │
│  └─────┬───────┘                                         │
│        │                                                 │
│        ▼                                                 │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────┐     │
│  │ core/       │  │ speech/     │  │ input/       │     │
│  │ logger      │  │ speech      │  │ keyboard_    │     │
│  │ memory      │  │ locale      │  │ hook         │     │
│  │ config      │  │ phrasebook  │  │ hotkeys      │     │
│  │ hooks       │  └──────┬──────┘  └──────┬───────┘     │
│  │ events      │         │                │              │
│  │ phyre_types │         │ LoadLibrary    │              │
│  └─────────────┘         ▼                │              │
│                     Tolk.dll              │              │
│                  (user-supplied)          │              │
│                                                          │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────┐     │
│  │ ui/         │  │ navigation/ │  │ battle/      │     │
│  │ game_handler│  │ entity_list │  │ combat_log   │     │
│  │ menu_state  │  │ pathfind    │  │ event_capture│     │
│  │ dialogue    │  │ compass     │  │ battle_state │     │
│  │ menus/*     │  └─────────────┘  └──────────────┘     │
│  └─────────────┘                                         │
└──────────────────────────────────────────────────────────┘
```

## Logging Format

`FFXII-Screen-Reader-Latest.log` (next to `FFXII_TZA.exe` in `x64\`):

```
[+0000ms] [INIT     ] Mod entry; deferred init started
[+0123ms] [SPEECH   ] Tolk.dll loaded
[+0124ms] [SPEECH   ] Screen reader detected: NVDA
[+0125ms] [HOOKS    ] MinHook initialized
[+0140ms] [CONFIG   ] mod_config.ini loaded; 12 RVAs validated, 0 self-healed
[+0145ms] [SPEAK    ] FFXII screen reader loaded
```

## Known Issues

### Session 40 fixes (SHIPPED, pending runtime confirmation) — supersede the S39 pending items
Three tester regressions, root-caused (Bug 3 from the live log; Bugs 1-2 from two agreeing decompile traces):
- **`p` routed once then "No target" (FIXED).** The `[TARGET] GetLockedTarget` log showed `tickMs` FROZEN
  between target changes → the target nameplate render (`FUN_002bfd20`) is event-driven, not per-frame, so
  any cache-age window ages out. Fix: `GetLockedTarget` now gates on the LIVE `DAT_0209be80` state
  (`gate=PtrAt(P,0x10F78)`, `liveHandle=*(u32)(P+0x9FD8)`) at press time (input-thread, SEH-guarded) and
  re-resolves a fresh pos from the cached `bc`. Removed the age window. Confirm: press `p` repeatedly on a
  held target → keeps routing.
- **Exits still 0 (FIXED).** `EnumerateExits` missed a dereference: use slot 0, `mapData=*(u64*)(container
  Base+0)`, `tag=*(u16)(mapData)>2`, `tableOff=*(u32)(mapData+0x54)`, `exitBase=mapData+tableOff+reloc`.
  Prior code used `containerBase` directly → garbage → 0. Auto per-area `[NAV-DIAG] exit-table slot0` dump
  now fires on area load (no `'` needed). Confirm: dump shows `mapData≠0 tag>2` + sane count.
- **Only enemies navigable (FIXED).** Scanner now also lists **named** objects (gates/doors/field-signs/
  NPCs, categories 1-4) via `ResolveObjectName`, not just TALK/ACTION/gimmick. Confirm: `rescan: N` rises;
  `]` cycles named objects.

### Session 39 fixes (SHIPPED, pending runtime confirmation)
Two Session-38 runtime bugs, root-caused from the live log + already-documented RE:
- **`p` "No target" even with a target selected (FIXED).** The target reader announced the target
  (`[TARGET] "Imperial Swordsman..."`) but the `p`-cache write is gated on `havePos`, and
  `havePos = ReadSceneObjectPos(target sceneObj)` returned false — the battle target's `sceneObj+0xB8`
  transform node is null during attack-menu selection (works for combatants in real-time combat, so
  state-dependent). Fix: `NameForBtlChr` falls back to `actor+0xE0/E4/E8` (documented field-actor pos,
  `GameArchitecture.md:728`) when the scene node fails/reads (0,0,0); freshness 300→1000 ms; baked
  `[TARGET]` log records both sources + cache age. Confirm: `p` in a battle → `src=1/2 havePos=1` and a
  route; if both sources are 0 during selection, escalate to gating on live `DAT_0209be80+0x10F78`.
- **No routable fortress objects → wired the never-populated `Category::Exit`.** Documented follow-up
  (`plan.md` marked exits `[x]` prematurely; nothing populated it). Exits = walk-into map-jump zones,
  invisible to the `FLAG_TALK|FLAG_ACTION|gimmick` scanner filter; read the per-map exit array behind
  `getmapjumpposbyindex` (`MapQuery::EnumerateExits`, offsets ~0.85) behind a strict sanity gate;
  surfaced as fixed-position `Category::Exit`. Confirm via the `'` exit-table dump in a fortress
  corridor (sane `count`+positions → canonicalize offsets; else the dump shows which term is off).

### `p` = route to locked battle target (SHIPPED Session 38, pending runtime confirmation)
`p` (VK_P / DIK_P 0x19) routes to the game's locked/selected battle target via the same
`PathPlanner::Request` pipe as `\`, sourcing the target from `battle_target_reader`'s live cache
(`DAT_0209be80` → `+0x9FD8`, gate `+0x10F78`, refreshed every flagged-target render; `GetLockedTarget`
returns it if <300 ms old). Built + deployed, UNCOMMITTED. Two open confirmations, both answered by the
baked `NAV-ROUTE` log (no guess):
1. **PRE-SHIP CHECK** — press `p` under **Lock-On (`2`) with NO command menu**. Routes ⇒ Lock-On drives
   `DAT_0209be80+0x9FD8` (persistent target, ≥0.98); "No target" ⇒ `p` only works during command
   target-selection. Record the verdict in `GameArchitecture.md`.
2. **Nav-safe in battle** — the drain line proves whether the field stays nav-safe in battle-state mode.
   Expected yes (battle is on-field; `FIELD_ACTIVE2 0x1F69300` is the "field/battle-active" flag). If a
   persistent "*not nav-safe*" appears, relax the single battle-cleared gate bit in `IsFieldNavSafe` — do
   NOT loosen the gate pre-emptively without that log evidence.
Also Session 38: `PlanRoute` target-cell walkability snap (off-mesh goal → nearest walkable cell) and
`entity_list` cursor lock-by-identity (`CursorId`) — both pending the same runtime pass.

### Turn-by-turn routing polish (FIX SHIPPED Sessions 34+37, pending runtime confirmation)
Routing WORKS (SQEX walkmap, `plan=Route`) but had three quality problems reported by the
user. Detail + hypotheses in `sessions_001_current.md` Session 33. **Resolution:** #1 (directions point
away) was the world-cardinal-vs-camera-relative mismatch — FIXED by Session 37's camera-forward egocentric
model (`ReadCameraForward`); #2 (distance cap) ELIMINATED and #3 (through-walls) FIXED by Session 34.

**Session 34 rebuilt the planner on a whole-map walkability OVERLAY read directly from the SQEX
walkmap grid** (`nav_grid.{h,cpp}` + `map_query` grid reads; the walkmap is a uniform ≤32767-cell
grid — `FUN_0022ffe0`/`FUN_00233050`/`FUN_00231890`, see GameArchitecture "Walkmap grid").
- **#2 (40 m cap) ELIMINATED** — no `kMaxRange`; A* searches the whole map at native resolution,
  per-cell floor sampling is now a zero-raycast overlay read.
- **#3 (through walls) FIXED** — `MapQuery::SegmentTraversable` dense validator (1 m samples: floor-
  continuity + `|ΔfloorY|≤kMaxStep` + per-substep `SegmentClear` mask=4) replaces the single long
  ray in the string-pull, so a detour is never straightened through a wall.
- **#1 (points away) — directions kept WORLD-ABSOLUTE per user directive** (no egocentric/facing
  mode; camera only moves the view). Chiefly a symptom of #3; `NAV-ROUTE` now logs yaw°/target/raw+
  smoothed polyline/spoken text to catch any residual first-waypoint geometry bug.
- **Confirmation = baked C++ self-diagnostic** (user's choice, no Frida): the direct grid read is
  0.92 offline; the `'` key logs the grid header + cross-checks `ReadCellFloor` vs the confirmed
  `GroundAt` oracle (`grid xcheck walkAgree %`). Ships behind that; `NavGrid::SetDirectRead(false)`
  falls back to a GroundAt bake if agreement is low. **Documented straight-to-C++ exception** (no new
  RE crash surface — tuning already-confirmed primitives + a struct read confirmed live in-C++).
- **Move to Solved once the tester confirms** `walkAgree ≥~90%` + `\` routes past 40 m + a detour
  around the reported wall. The three items below are the original open reports (kept for context).

1. **Directions point away from the objective (intermittent).** Holding a route direction
   moves the player *further*; re-route reports a *growing* distance ("East 15"→"17"→"20").
   User: not a constant axis flip ("works sometimes"). Prime hypothesis: **world-cardinal
   directions vs. camera-relative movement** — the route speaks WORLD cardinals (north=-Z,
   east=+X) but the stick is camera-relative, so "east"=="right" only at some camera angles
   → likely needs an EGOCENTRIC route mode (off `PlayerState::ReadPlayerYaw`) or a facing cue.
   Also rule out a geometric first-waypoint bug (see #3). Next: log raw + smoothed polyline +
   target + yaw.
2. **Distance cap too small.** `path_planner.cpp kMaxRange=40m` → targets past ~53 steps get
   "Too far to route". User wants routing to ANY map entity regardless of distance. Raise/
   remove `kMaxRange` AND raise A* budgets (`kMaxRays=2000`/`kMaxExpand=500` — a 27×15 route
   already used 1266 rays/157 expands; long routes will blow them → need bigger caps or a
   coarser long-range pass). Verify `entity_list` enumerates distant objects (no distance
   filter, but confirm the handle table / actor pool holds far map objects).
3. **Routes cut through walls/rooms.** A route said "East 15, South 17" (a single straight
   diagonal) to a target unreachable by going east/south (walls between). Walkability is
   UNDER-detecting walls, or the LOS string-pull straightened a valid detour into an
   impassable line (opposite of the old 0xffff over-block). Next: log the mask=4 seg-test
   result along the reported line; add the deferred smoothed-segment validation (denser
   sample + floor-continuity + `|ΔfloorY|≤kMaxStep`); check `GroundAt` isn't returning floor
   across gaps/room boundaries. Related to #1.

### Enter key intermittently drops mid-game (diagnosed — Session 33; upstream, NOT our mod)
The `GetDeviceState` diagnostic captured it: `INPUT-DIAG keyboard GetDeviceState FAILING
hr=0x8007001E` = **DIERR_INPUTLOST** → the keyboard DirectInput device went UNACQUIRED (game
sees no keys). This is the game's own acquisition / a focus loss, **not** our read-only hook
(and the redundant `WH_KEYBOARD_LL` hook is now retired once DInput latches). Optional future
mitigation: a mod-side re-`Acquire()` nudge when we detect the failure — defer unless it
becomes a real blocker (it touches the game's device).

## Session Log

Index of session log files (split into `sessions_*.md` every 50 sessions).

- `sessions_001_current.md` — sessions 1–N (current)

**KEYWORDS: FUN_003112f0 call volume 1275 vs 20 real damage applier not one call per hit tick
actionId 0xFFFF Tier 2 source filter per-frame hook risk FUN_00310db0 FUN_00233f70** (Session 49,
tester-reported) — **`FUN_003112f0` fires FAR more often than there are damage events.** A live
messages run counted **1275+ calls against only ~10-20 actual damage instances**. Do NOT report a
raw count of this function as "damage events" — an earlier note in this session did exactly that
and the conclusion drawn from it was wrong.

This matters because `FUN_003112f0` is the designated **Tier-2 source** for the mod's own attack
lines (`combat_system.md` §9.1.3b) — the game composes no message for a basic attack, so the log
must synthesise one from this call. If the function fires ~100x per real hit then:
1. the filter is load-bearing, not cosmetic — see the **F2** correction (filter `actionId ==
   0xFFFF` for ticks, NOT `attacker == 0`, because `FUN_00310db0` makes two real calls with a null
   attacker); and
2. hooking it may approach the **no-per-frame-hooks** rule and needs a deliberate decision.

Suspected driver: `FUN_00310db0(actor)` does per-actor periodic work and is one of the three tick
callers; its own caller `FUN_00233f70` sits in the battle-update band. **Not yet proven** —
`probe_combat_damage.js` now buckets every call (real hit / tick / zero-delta / invalid / null
attacker) and prints the ratio, which answers it directly. Settle before building Phase 5.

**KEYWORDS: FUN_003112f0 is PER-FRAME FUN_00310db0 FUN_00233f70 callback +0x38 frame loop
FUN_00265a20 FUN_0026ce60 status tick actionId 0xFFFF attacker 0 Tier 2 source per-frame hook rule
combat log damage line** (Session 49, tester-caught) — **`FUN_003112f0` is called PER ACTOR PER
FRAME, not once per damage event.** `combat_system.md` §9.1.3b presents it as "one call = one
fully-resolved outcome", which is true of its *content* but badly wrong about its *rate*.

Chain, confirmed: the frame loop (`FUN_0026ce60` → `FUN_00265a20`) registers **`FUN_00233f70` as a
per-object update callback** (`*(code **)(obj + 0x38) = FUN_00233f70`). That callback runs a
battery of per-actor updates including **`FUN_00310db0`**, which does:

```c
memset(local_1a8, 0, 0x148);
FUN_00385df0(bc, local_1a8);
FUN_003112f0(local_1a8, 0, bc, 0xffff);      // attacker = 0, actionId = 0xFFFF
```

Live evidence: a session with only ~10-20 real hits logged **1275+** applier calls, essentially all
with a **null attacker**, climbing steadily with playtime (~20/sec). Confidence 0.97.

**Consequences:**
1. The tick filter (`actionId == 0xFFFF`, per the **F2** correction — NOT `attacker == 0`, because
   the loop just below makes real calls with a null attacker) is **load-bearing**, not cosmetic.
2. Hooking `FUN_003112f0` means a detour on the game's per-frame path, which engages the
   **no-per-frame-hooks** rule. It is not on §5.7's never-hook list, but it belongs in that
   conversation. Cost per call is small (trampoline + 2 reads + compare) and there is precedent —
   `battle_target_reader.cpp` already hooks the per-render `FUN_002bfd20` with a cheap early-out —
   but this must be a **deliberate, recorded decision**, not an assumption.
3. Any claim that "N applier calls" means "N damage events" is wrong. Do not repeat it.

**KEYWORDS: never ask the user to read the screen blind tester visual confirmation invalid probe
design reaction words sprites result+0x04 backtrace processing code FUN_00384e50 equipment mask
FUN_00389370 gate FUN_003896b0 roll ladder parry block evade** (Session 49) — **a probe step that
required the tester to read an on-screen word was authored, and it is invalid by construction: the
tester is blind.** Probe P3 and the reaction-word instructions in `probe_combat_damage.js` both did
this and are struck.

**The correct method, and the one to reach for by default: backtrace the processing code to the
condition that produced the value** (the approach used on DQ7R). Applied here it resolved the
question completely and offline:

- `FUN_00384e50(bc)` → equipment mask: bit0 = main-hand (`bc+0x50 != 0x1000`), bit1 = off-hand
  (`bc+0x52 != 0x1000`), bit2 = animation-set hash `0x2902032b`.
- `FUN_00389370:73-81` zeroes each defensive rate unless its gate bit is set — `DAT_02aedff0` needs
  off-hand, `DAT_02aedff4` needs main-hand, `DAT_02aedfec` needs the animation set.
- `FUN_003896b0:38-70` rolls them in order and writes the result into **`result+0x04`**.

⇒ **`result+0x04` names the mechanic** (1/2 parry · 3/4 block · 5 evade · 6 no-effect ·
7 nullified · 8 reflected · 10 absorbed · 0xB avoided), identified by which equipment slot gated
the roll. Confidence 0.98. **`FUN_00328480` is dropped from the design** — one hook, not two.

**Rule going forward: if a value has no text behind it, do NOT ask for a visual reading — trace the
code path that set it.**
