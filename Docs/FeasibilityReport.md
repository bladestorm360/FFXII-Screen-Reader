# FFXII Screen Reader - Feasibility Report

**Date:** 2026-05-07
**Question asked:** Before committing further to menu work, do we have enough
data on the game-side script layer to ship a navigation-grade screen reader?
The FFX project failed because we couldn't extract sufficient information; can
we avoid the same fate here?

**Verdict: GO.** The data quality is materially better than feared. We have
named functions, named data, decodable English text, and a clear hook surface.
This document explains what we have, what we still need, and what the next
data-collection steps are.

---

## Where game logic actually lives

The `.exe` is mostly PhyreEngine + a script-bytecode interpreter. **Game
logic lives in `.ebp` bytecode files inside `FFXII_TZA.vbf`** (10,045 of them,
576 MB). Four top-level controllers - `ctrl`, `evctrl`, `mapctrl`, `btlctrl` -
each ship with a paired `.dbg` file containing a full symbol table: function
names, variable names, even Square's original PS2 source-tree paths.

We had `tools/list_vbf.py` and `tools/extract_vbf.py` from a prior session.
This session adds `tools/survey_data.py` to decode FFXII's text encoding and
inventory the navigation-critical files.

---

## Navigation labels - the previous worry

You called this out specifically: *"the player needs to know where they're
going and what they're interacting with. We can't have map exit 1, map exit
9, npc04c29ab934."*

**Resolved.** The game ships per-locale data tables with plain-English names:

| File | Size (us) | Contents |
|---|---|---|
| `us/bin/planmapname.bin` | 13,824 B | Map / area / sub-area display names |
| `us/bin/npcdic.bin` | 37,545 B | NPC type dictionary (~2,232 string entries) |
| `us/bin/mapjumpgroupinit.bin` | 116 B | Map-jump group definitions (`FLAG`+`DEF1` magic) |
| `us/bin/mapjumpgroupflagrom.bin` | 856 B | Map-jump group flag table (`FLAG`+`ROM1` magic) |
| `us/bin/questbase.bin` | 700 B | Quest definitions |
| `ch/bin/navimapdata.bin` | (extracted) | Navigation map data |

Same files exist for `fr`, `de`, `es`, `it`, `kr`, `cn`, `ch`, `in` (all 12
locales the mod targets).

### Text encoding (cracked this session)

FFXII uses an offset cipher - bytes index into the font glyph table:

- `0x20`-`0x39` -> `A` + (b - 0x20)  (uppercase A-Z)
- `0x3A`-`0x53` -> `a` + (b - 0x3A)  (lowercase a-z)
- `0x1F` -> space
- `0x00` -> string terminator
- High bytes (>=0x80) likely multi-byte (Japanese)

Worked example:

    encoded:  "EHBLM>K H? #BLM:GM 2HG@
    decoded:  Cloister of Distant Song

### Sample decoded English map names

From `survey_planmapname.txt` - real strings extracted by the decoder:

> Arcade, Terrace, Molberry, Trant, Magickery, Technicks, Tsenoble,
> Aerodrome, Nilbasse, Rienna, Armaments, Requisites, Gambits, Sighs,
> Whispers, Parlour, Saloon, Deck, Light, Song, Highborn, Covenant,
> Crucible, Solace, Reason, Periphery, Catwalk, Antechamber, Lift,
> Shaft, Superstructure, Greencrag, Scarp, Sorrow, Reach, Past,
> Slumbermead, Mutters, Eternity, Strand, ..., Aries, Pisces, Cancer,
> Scorpio (Henne Mines zodiac shafts), Avaa, Pratii, Praa
> (Necrohol of Nabudis), Greenswathe (Golmore Jungle), Skygrounds
> (Phon Coast), Branchway, Cloister of the Highborn, Cloister of the
> Hallowed Light...

These are **canonical FFXII zone names**. The current decoder only finds
fragments because it stops at null bytes without parsing the file's offset
table; a structured parser (planned, simple) will give us full
`(map_id -> "Cloister of Distant Song")` rows.

### Sample decoded English NPC types

`npcdic.bin` magic = `NPC0`, ~2,232 strings. Sample:

> Rabanastran, Imperial, Awein, Aficionado, Batahn, Bandalla, Bangaa,
> Adventurer, Knight, Bucco, Sherral, Vain, Merchant, Trader, Headhunter

These are NPC labels. Each NPC has at least two entries (probably
singular/plural or label/full-name). When the player approaches an NPC, we
will speak "Bangaa Merchant" or "Rabanastran Trader" - **never**
`npc_0x04c29ab934`.

---

## Interactable / gimmick type labels (added 2026-05-07)

Pathfinder needs to label the things the player walks up to: treasure
chests, save crystals, gate crystals, bounty boards, signposts,
switches, doors. We do not need the contents of those interactions -
only the type label spoken before interaction.

**FFXII uses a unified "gimmick" framework** with a script API that
sets the on-screen "press X to ___" prompt every time the player
enters `talkradius` of one. The prompt text the game already renders
IS the type label.

### Script API (hookable via interpreter dispatch)

| Function | Use |
|---|---|
| `setfieldsign` / `showfieldsign` / `hidefieldsign` | Show/hide prompt |
| `fieldsignmes` | Inline prompt text |
| `fieldsignmesbyid` | Prompt text by message-table ID |
| `fieldsignicon` | Prompt button icon |
| `setfieldsignlocationjumpinfo` | Map-exit prompt + destination |
| `setupTreasure` / `talktreasure` / `talktreasureafter` | Treasure pipeline |
| `setsaveramsavestatus` / `telepostatus` / `summonstatus` / `battlestatus` / `shutoralstatus` / `navimapstatus` | Six crystal subtypes |
| `getcrystalid` | Active crystal ID |
| `setnpcname` | Bind an `npcdic.bin` ID to a scene NPC |
| `gimmick_to_npc` / `npc_to_gimmick` | Convert between gimmick and NPC actor |

### Resolution path per object type

- **Treasure chest** -> `setfieldsign` text -> speak "Open"
- **Save crystal** -> `setfieldsign` text -> speak "Save"
- **Gate crystal** -> `setfieldsign` text -> speak "Teleport"
- **Map exit** -> `setfieldsignlocationjumpinfo` -> destination map ID
  -> `planmapname.bin` lookup -> speak "Travel to Cloister of Distant Song"
- **Signpost / bounty board / switch / lever** -> whatever verb the
  script set on `setfieldsign` -> speak it verbatim
- **NPC** -> `npcdic.bin` ID via `setnpcname` -> speak "[Name]"

This mirrors the universal-cursor-hook rule: never hardcode the spoken
text, read what the game already renders.

### Fallback for gimmicks with no rendered prompt

Rare case. If a gimmick is set up by a known controller function but
never calls `setfieldsign`, we fall back to a per-type label keyed by
the .dbg-symbol of the setup call (~10-15 distinct types). Those
labels live in the phrasebook (mod-emitted, allowed under the rules
because the game itself emits no text for that case).

### What this does NOT give us (and why that's fine)

- **Per-instance custom names** ("the third lever in Necrohol"). FFXII
  does not name individual gimmicks - sighted players navigate by
  type plus position, and so will we.
- **The interaction result** (treasure contents, signpost message,
  NPC dialogue). That belongs to Phase 5 (dialogue / cutscene reader)
  and Phase 6 (menus), not the pathfinder.

### Verdict on interactables

Same as the rest: GO. The label resolution path is shorter than for
maps - we don't need to parse a binary table, we hook one of three
script functions (`setfieldsign` / `fieldsignmes` / `fieldsignmesbyid`)
and speak what they set. This becomes free as soon as the .ebp
interpreter is hooked.

## Game-side native functions in the .exe (added 2026-05-07)

Earlier wording in this report implied "engine labeled, game-side
unlabeled." That was wrong. The accurate breakdown:

### Direct game-side classes labeled by RTTI (vtables known)

| Class | vtable RVA | Use |
|---|---|---|
| `GameApplication` | `0x7A5760` | Main game loop |
| `InputManager` | `0x7D3640` | Global input dispatcher |
| `SaveloadManager` | `0x7D9B80` | Save/load system |
| `ChrModel` | `0x7D9150` | Character model |
| `MapRenderObject` | `0x7E12C0` | Map renderer |
| `MapOrderedObject` | `0x7E10D8` | Map render-order |
| `CharacterRenderObject` | `0x7E3B68` | Character renderer |
| `CharacterOrderedObject` | `0x7E3940` | Character render-order |
| `ExtraUI` | `0x7E0370` | Extra UI overlay |
| `LoadingPage` | `0x7E0390` | Loading screens |
| `ExternalVoice` | `0xC4CD90` | Voice playback |

Plus the audio chain (`MusicAudioController`, `Seb/Sep/Shout/Stream
AudioController`, `MonoAudioController`) and the rendering-pass classes
(`RenderObject`, `RenderMapOpaqueObj`, `RenderAfterWaterObj`).

### PhyreEngine classes covering specific subsystems

- **Text panel display + rendering** -> `Phyre::PText::PTextObject`,
  `Phyre::PText::PBitmapFont`, `Phyre::PText::PBitmapFontCharInfo`
- **Input** -> `Phyre::PFramework::PInputDeviceKeyboard` @ `0x1B92430`,
  `PInputDevicePadXInput`, `PInputDevicePadDirectInput`
- **Spatial index** (entity enumeration / proximity for the pathfinder)
  -> `Phyre::PHierarchy::POctreeWorld`
- **Collision / raycast** (FFXII has NO separate NavMesh - it uses
  Bullet directly) -> full `bt*` family with RTTI labels:
  `btCollisionWorld::rayTest`, `btCollisionWorld::objectQuerySingle`,
  `btTriangleMeshShape`, `btOptimizedBvh`, etc.

PhyreEngine is publicly documented (Sony GDC slides, public middleware
docs). Labeling individual methods on these classes is clean-room - we
don't need the leaked source.

### Specific community RVAs harvested (~99 entries)

In `notes/community_rvas.csv`. Includes the BattleUnit struct shape
(validated by 4+ sources): `+0x05` type, `+0x4C` MP, `+0x54` is_active,
`+0x190` LP, `+0x1C2` level, `+0x698` BattleUnitKeep ptr. Plus gil/chain/
PRNG/steal globals and several function clusters (battle-license tick,
overhead-HUD, menu-state, world-teleport).

### Developer / debug surface still in retail

- Strings: `"render debug menu"` @ `0x7A5E40`, `"Load Debug Menu
  Settings"` @ `0x7DF0E8`, `"Save Debug Menu Settings"` @ `0x7DF108`,
  `"?JsonData/debugMenuSettings.json"` @ `0x7DCCAF`.
- RTTI class `Phyre::PFramework::PDebugUtils` is present.
- .dbg files leak Square's PS2 source paths
  (`c:/cygwin/home/katano/ps2/ff12_ps2/...`).
- Dev directories `katano/` + `myoshiok/` in the VBF are individual SE
  engineers' personal dev trees.
- Dev/QA functions still callable from script: `healall`,
  `killallfast`, `killgroup`, `btl[Get/Set]PcMutekiMode` (god mode),
  `mp_debugmenu_flg`, `mp_mtanaka_dbg`.

### What's NOT labeled (the honest gap)

- ~28,000 of the 33,663 functions are `FUN_xxxxxx`. Most are leaf
  utilities, struct accessors, helpers - but some are real game-logic
  implementations.
- No class named `PartyManager`, `BattleManager`, `Inventory`, `Gambit`,
  `Bestiary`, `License` exists in RTTI - those subsystems are either
  inlined into the main loop or implemented as plain functions called
  by the .ebp interpreter (no C++ class wrapping them).

### Highest-leverage labeling task remaining

**Find the .ebp interpreter dispatch table.** Single biggest unlocker.

The .ebp scripts call ~20,000 named functions (`fsmenu_openmenu`,
`getmapid`, `setfieldsign`, `btlAtelGetMyPosX`, etc.). The interpreter
MUST bind those names to native function pointers somewhere in the .exe
- either as a string-keyed lookup or a numeric-indexed jump table.

Path:
1. Xref the `EBP2` magic literal (0x32504245) - finds the interpreter
   dispatch function (~5 min Ghidra script).
2. The dispatch function references the binding table.
3. Pair the table entries against our 20,417 .dbg symbols -> ~20,000
   native game functions labeled in one pass.

That single pass would close most of the remaining 28k unlabeled
functions for anything we'd actually hook.

## .dbg symbol tables - the script-layer skeleton key

`tools/survey_data.py` dumped every named symbol from all four controllers:

| Controller | Symbols |
|---|---|
| `ctrl.dbg` | 2,582 |
| `evctrl.dbg` | 2,849 |
| `mapctrl.dbg` | **7,748** |
| `btlctrl.dbg` | **7,238** |
| **Total** | **20,417** |

Output: `notes/survey_dbg_symbols.csv` (controller, locale, symbol).

Highlights for each phase of the plan:

### Phase 3 (title + main menu) - already named

From `ctrl.dbg`:

- `fsttl_initialize`, `fsttl_opendatacheck`, `fsttl_opencommand`,
  `fsttl_closecommand`, `fsttl_openconfig`, `fsttl_getconfigstatus`,
  `fsttl_newgamestart` - the title-screen state machine, by name.
- `fsmenu_openmenu`, `fsmenu_closemenu`, `fsmenu_paddisable`,
  `fsmenu_showcursor`, `fsmenu_hidecursor`, `fsmenu_registrect`,
  `fsmenu_getrectchip`, `fsmenu_islist`, `fsmenu_isactionchip`,
  `fsmenu_isthumbnail`, `fsmenu_movechip`, `fsmenu_movelist`,
  `fsmenu_selectchar`, `fsmenu_changelicense`,
  `fsmenu_getconfirmwinstatus`, ... - the universal menu API.
- `dispcursor`, `printfmenu`, `random_push_menu` - cursor / menu render.

### Phase 4 (pathfinding + field navigation) - already named

From `mapctrl.dbg`:

- `mapjump`, `mapjumpresult`, `mapjumppos`, `mapjumpstatus`,
  `mapjump_menu_flg` - map-transition state.
- `mapload`, `mapdispose`, `mapsoundplay` - lifecycle.
- `setmapjumppos`, `getmapjumpposbyindex`, `getmapjumpanglebyindex`,
  `getmapdestposbyindex` - **enumerate every exit on the current map by
  index, get its position + facing + destination map ID**. This is
  exactly the API a screen reader needs to cycle exits with `[`/`]`.
- `getmapid`, `getmaptype`, `getmapgroundtype` - current-map info.
- `setmapjumpgroup`, `setmapjumpgroupflag` - exit groups
  (corresponds to `mapjumpgroupinit.bin` / `mapjumpgroupflagrom.bin`).

The pathfinding story: get player position from `btlAtelGetMyPosX/Y/Z`,
enumerate exits with `getmapjumpposbyindex`, look up names in
`planmapname.bin`, compute distance + bearing in our code, announce.

### Phase 5 (dialogue + cutscene text) - already named

From `evctrl.dbg`:

- `eventlist`, `eventlistjyoutyu` (joutyu = "ordinary"),
  `eventbosslist1..4` - event tables.
- `eventresult`, `eventpos` - per-event state.
- `event_global_flag[N]` - story flag array.

### Phase 6 (in-game menus) - already named

From `ctrl.dbg`'s `fsmenu_*` family (above) plus:

- `fs_stonewinopen`, `fs_stonewinclose` - License Board ("stone window").
- `fs_foodwinopen`, `fs_foodwinclose` - some food/menu window.
- `fs_setclanrank` - Clan Primer.
- `fsroll_*` family - credit roll / rolling text.

### Phase 7 (combat log) - already named

From `btlctrl.dbg`:

- `btlAtelGetCharIdFromPartySlot`, `btlAtelGetHpMaxFromPartySlot`,
  `btlAtelGetHpNowFromPartySlot`, `btlAtelGetMpMaxFromPartySlot`,
  `btlAtelGetMpNowFromPartySlot`, `btlAtelGetNowStatusFromPartySlot`,
  `btlAtelGetCharacterKindFromPartySlot` - per-slot party reads.
- `btlAtelGetMyPosX/Y/Z` - actor position.
- `btlAtelGetChainCount`, `btlAtelGetRespawnTime` - chain / respawn.
- `btlGetPcMutekiMode` / `SetPcMutekiMode` (god mode), `healall`,
  `killallfast`, `killgroup` - dev/debug functions still callable.
- Per-dungeon flag arrays: `mp_rbn_flg` (Rabanastre), `mp_gil_flg*`
  (Giruvegan), `mp_rwg_flg*` (Ridorana), `mp_rui_flg` (Raithwall's
  tomb), `mp_mnt_flg` (Mosphoran), `mp_rbl_flg*` (Bahamut sky fortress).

20,417 named symbols beats hunting opaque `FUN_xxxxx`. This is the single
biggest data find of the project.

---

## What we still need (concrete, scoped)

### Required before Phase 3 (title/menu) can start

1. **Find the `.ebp` interpreter in `FFXII_TZA.exe`.** Search for a
   reference to magic `EBP2` (0x32504245 LE). The function that reads
   that magic and dispatches opcodes is the universal hook target.
   Once hooked, every script call (including `fsttl_newgamestart` etc.)
   is observable by .dbg-symbol name. *Ghidra script work, not yet
   written.*
2. **Translate DrummerIX CE-table absolute addresses to RVAs into
   `drummer_ix_seed.csv`.** Already on the plan; gates first deploy
   self-heal.

### Required before Phase 4 (navigation) can start

3. **Write a structured `planmapname.bin` parser.** The encoding is
   solved; the file format (`PLMN` magic + offset table) needs ~30 lines
   of Python to walk properly so we get
   `(map_id -> full-localized-name)` rows instead of word fragments.
4. **Write a structured `npcdic.bin` parser** (`NPC0` magic, similar
   shape). Trivial once planmapname is done.
5. **Find where the .exe loads `planmapname.bin` and `npcdic.bin`.**
   Ghidra string xref pass on the literal filenames will land it. The
   in-RAM struct holds the (id -> name) lookup we read at runtime.
6. **Frida-hook `getmapjumpposbyindex` family** in the script
   interpreter. Confirm we can enumerate exits on a real map.

### NOT required (good news for scope)

- **A full `.ebp` bytecode disassembler is NOT necessary for the mod.**
  We do not need to read the script source - we need to know *which*
  named function is currently executing. The .dbg symbol table plus a
  hook on the interpreter dispatch gives us that directly. A
  disassembler would be helpful for deep RE later but is not on the
  critical path.
- **The leaked PhyreEngine 8 source is still off-limits** (clean-room
  rule from CLAUDE.md). We do not need it; .dbg + RTTI cover us.

---

## Risks (smaller than they look)

| Risk | Status |
|---|---|
| FFX-style data starvation | **Resolved.** 20k+ named symbols + decodable text tables. |
| Opaque NPC IDs | **Resolved.** `npcdic.bin` has ~2,232 plain-text labels. |
| Opaque map exit IDs | **Resolved.** `getmapjumpposbyindex` family + `planmapname.bin`. |
| Text encoding unknown | **Resolved this session.** Empirical decoder works on real strings. |
| Custom file formats | Mild. `PLMN`/`NPC0`/`FLAG-DEF1`/`FLAG-ROM1` headers are simple; ~30 LOC each. |
| .ebp interpreter unfindable | Low. Magic `EBP2` is a literal in the .exe; standard string xref. |
| Multi-byte JP support | Low priority - Phase 4+ is English-first; JP support deferred. |

The main remaining unknown is **interpreter dispatch shape** (calling
convention, stack layout). That is one Frida session away.

---

## Recommended next data-collection steps (in order)

1. **Ghidra: xref the `EBP2` magic** to find the .ebp interpreter.
   Single `find_ebp_interpreter.java` script. Output: candidate RVA
   for the dispatch function.
2. **Ghidra: xref the literals** `planmapname.bin`, `npcdic.bin`,
   `mapjumpgroupinit.bin`, `mapjumpgroupflagrom.bin`, `questbase.bin`
   to find their loaders. Each loader populates an in-RAM struct we
   can read at runtime.
3. **Python: structured parsers** for `planmapname.bin` and
   `npcdic.bin`. Output: `notes/map_names_us.csv`,
   `notes/npc_dictionary_us.csv`. ~60 LOC total.
4. **Extract `in/` (Japanese baseline) `.dbg` files** to confirm symbols
   are identical across locales (they should be - .dbg is debug data,
   not display text).
5. **Frida (after #1 lands): hook the dispatch function**, log every
   script call by .dbg-symbol name during a 60-second walk in
   Rabanastre. Output: a real timeline of which named scripts fire on
   menu open, NPC approach, area transition. This is the single most
   valuable diagnostic for the rest of the project.

After step 5, Phase 3 (title menu) starts with concrete hook RVAs and
no guesswork.

---

## Bottom line

We are not in FFX territory. FFXII gave us its dev-tree symbols by
accident (the `.dbg` files were never stripped from the retail VBF), and
its display text is in plain offset-cipher tables we can decode. The
labels the player needs to hear - "Cloister of Distant Song", "Bangaa
Merchant", "Sandsea, Bluff" - are all already present, in English, in
the shipped game data.

Proceed.
