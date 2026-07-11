# FFXII Game Architecture — RVA / Offset / Struct Registry

This is the **persistent lookup** for everything we've discovered about
`FFXII_TZA.exe` internals. Session logs reference entries here; entries here are not
duplicated in session logs.

**Update this file** every time Ghidra/Frida/log-analysis confirms a new RVA, offset,
or class structure. **Read it first** before re-discovering.

## Binary Identity

- **Game:** Final Fantasy XII: The Zodiac Age (PC, Steam app 595520)
- **Engine:** PhyreEngine (PS3-era, Sony-licensed, native C++)
- **Compiler:** MSVC (specific version TBD — confirm from PE timestamp / PDB hint)
- **Architecture:** x64
- **Path:** `D:\Games\steamlibrary\steamapps\common\FINAL FANTASY XII THE ZODIAC AGE\x64\FFXII_TZA.exe`
- **Image base (default):** `0x140000000` (PE x64 default — confirm from Ghidra)
- **Companion exe:** `FFXII_TZA_GameSetting.exe` (config tool; ignored by accessibility mod)

## Adjacent DLLs

| DLL | Purpose | Hook surface? |
|---|---|---|
| `xinput1_3.dll` | Game ships its own | NO — overwriting breaks the game |
| `dinput8.dll` (post-mod) | FF12 Module Loader (ffgriever, BSD-2) | YES — our injection vector |
| `d3dcompiler_47.dll` | Microsoft redistributable | NO |
| `SharpDX.dll`, `SharpDX.DirectInput.dll`, `SharpDX.DXGI.dll` | .NET; used by `FFXII_TZA_GameSetting.exe` | NO |
| `Steamworks.NET.dll`, `CSteamworks.dll` | .NET Steam wrappers | NO |
| `steam_api.dll`, `steam_api64.dll` | Steam API | NO |

## Locale Detection — CORRECTED APPROACH (2026-05-05)

Original plan: scan the binary for `en-US`/`ja-JP`/etc. literals and find the
function that reads them. **That returned 2 unique hits** — locale isn't
string-keyed in the binary.

**Actual locale source:** `KERNEL32.GetUserDefaultLangID` (confirmed in the
imports list). The game queries the Windows LCID at startup, caches the
result as a numeric enum, and uses that enum as an index into per-locale
asset paths (`FileSizeTable_*.fst`). The lone `_US` string at data RVA
`0xC85068` is the ONLY ASCII locale tag in the binary — it's appended to
asset paths, not used for selection.

**Frida-prototype plan (replaces old `probe_locale.js`):**
1. Hook `GetUserDefaultLangID` at the import thunk — log the LCID returned
   (e.g. 0x0409 = en-US, 0x0411 = ja-JP).
2. Walk the call stack from the hook to find the function that consumes
   the result. That function reads the locale enum into a global; that
   global is what we want to read at runtime.
3. Cross-ref `0xC85068` (the `_US` string) — its xref function builds
   asset filenames; reading the index it uses is another path to the
   locale enum.

| Field | RVA | Type | Source |
|---|---|---|---|
| Locale enum global | TBD | int | hook `GetUserDefaultLangID` and walk stack |
| `_US` asset suffix string | `0xC85068` | const char[3] | strings.txt |

## Pause / Time-scale

*(TBD — Phase 1: `probe_pause_global.js`)*

Combat log requires freezing game time. Open question: dedicated time-scale variable
or shared `bIsPaused` flag with menu-active semantics?

| Field | RVA | Notes | Source |
|---|---|---|---|
| Time scale / pause flag | TBD | | `probe_pause_global.js` |

## Party / Battle Managers

*(TBD — Phase 1: seeded from DrummerIX CE table)*

| Manager | RVA | Pointer chain to leader HP | Source |
|---|---|---|---|
| PartyManager | TBD | TBD | DrummerIX seed |
| BattleManager | TBD | TBD | DrummerIX seed |
| MenuManager | TBD | TBD | `probe_menu_open.js` |
| CameraManager | TBD | TBD | DrummerIX seed |

## Title Command Menu (baked-sprite) — CONFIRMED LIVE 2026-07-02

Distinct baked-sprite menu (NOT the in-game `FUN_00241d40` system). Option labels are
pre-rendered glyph IMAGES in `title_logo.tm2` — **no text string exists** for them. Read by
the mod's `TitleReader` (`src/ui/title_reader.cpp`). **Runtime image base = `0x120000`**
(confirmed live via Frida — corrects the `0x140000000` guess above); mod hooks resolve
`GetModuleHandle(nullptr) + RVA`, RVA = Ghidra-abs − 0x120000.

| Item | RVA | abs | Role |
|---|---|---|---|
| Title command window | `0x29CE4C8` (`DAT_02aee4c8`) | 0x2aee4c8 | window ptr (0 when closed) |
| Title window handler | `0x2739B0` (`FUN_003939b0`) | 0x3939b0 | receives 0xc/0x8000 focus packet; case 1=init |
| Per-row decorator | `0x273950` (`FUN_00393950`) | 0x393950 | title-specific; source of the cell table |
| Logo / press-start handler | `0x274070` (`FUN_00394070`) | 0x394070 | msg 0x10=ready (Press Start), 0x12=destroy |
| Cell renderer | `0x17F5D0` (`FUN_0029f5d0`) | 0x29f5d0 | draws `cellTable[frame]` rect from `disp+0x18` texture |
| Focus dispatcher (generic) | `0x127510` (`FUN_00247510`) | 0x247510 | sends `{cat, msg, val}`; NOT hooked (title handler used instead) |

**Offsets (validated live):**
- `window + 0xC8` → W_LIST (list widget); `W_LIST + 0xE8` = u16 item count.
- `FUN_00393950` param_2 `+0x10` → A; `*A` → disp (shared cell node); `disp + 0x20` → cellTable; `disp + 0xa` = frame; `disp + 0x39` = palette.
- cellTable entry = 8 B: `x@0` (u16), `y@2` (u16), `(w:12|h:12|pal:8)@4`. **atlas row = y ÷ 70.**
- Notify packet (built by `FUN_00247510`): `category@0` (u32; 0xc=notify, 1=init), `msg@8` (u64; **0x8000**=focus), `val@0x10` (u64 = focus index).

**Label catalog** (title_logo.tm2, atlas row = y÷70; baked glyph art, read visually):
`0=New Game, 1=Load Game, 2=Trial Mode, 3=Credits, 4=Press ⊗ to Start (prompt), 5=Exit`.
Live: 5 selectable rows at `cellTable[0..4].y = {0,70,140,210,350}` = New Game/Load Game/Trial
Mode/Credits/**Exit** (note index 4→y350→row5→Exit; the `y=280` "Press Start" cell is the logo
prompt, not a command row). Per-option HELP text IS readable pipeline text via `FUN_002f9860`
(id `0xd44`/`0xd45`/`0xd3d`). Details: `..\FFXII-Decompile\notes\title_menu_labels.md`.

## Universal Menu & Text Reading — static spec (2026-07-03)

Offline decompile mining for the universal reader (menus, pop-ups, panels, prompts,
dialogue, battle text). Full detail + evidence line numbers:
`..\FFXII-Decompile\notes\text_pipeline_menu_dialogue_spec.md`. RVA = Ghidra-abs − 0x120000.

**Universal focus signal (title + in-game menus):** `FUN_00247510` (RVA `0x127510`) —
`FUN_00247510(owner, msg, index)`; msg `0x8000`=focus move (index=param_3), `0x8001`=confirm.
One hook here, filtered on `0x8000`, is the universal list-menu focus signal.

**Focus INDEX** (computed on sub-widget `w = *(menu_obj+0xc8)`):
`index = (i16[w+0xf4] + i16[w+0xf2]) * u8[w+0xec] + i8[w+0xee] + i8[w+0xed]`;
`item_count = u16[w+0xe8]`. Sub-widget ctor `FUN_002d14e0` (0x1B14E0), mover `FUN_002d1db0`
(0x1B1DB0). **2-choice pop-ups** (`menu_obj+0xc8==0`, `+0x3c4` bit0) emit `0x8100`=Yes /
`0x8101`=No / `0x8102`=Cancel instead of an index.

**Menu registry** `DAT_0208ea60` (RVA `0x1F6EA60` — the correct value; `0x216EA60` is stale
and appears nowhere in the decompile). Controller `FUN_00241d40` (`0x121D40`). Factory
`FUN_00241c90` (`0x121C90`) → `FUN_002465f0` (`0x1265F0`). The **title confirm pop-up**
`FUN_00394380` (`0x274380`) builds through this factory ⇒ it's a normal registry menu the
universal reader reads for free; Yes/No ids `0x3e8`/`0x3e9`, footer help `0x4b42`.

**Text capture hooks** (codec text, NOT UTF-16):
- PRIMARY `FUN_002b3050` (RVA `0x18B050`) — read **param_2 (RDX) = codec `byte*` PRE-call**;
  28 callers; covers menus, item/ability names+descriptions, panels/prompts, battle-UI text.
- FALLBACK `FUN_002af340` (RVA `0x18F340`) — param_2 codec `byte*` PRE-call (universal but
  fires multiple measure passes → needs dedup).
- SAFETY NET `FUN_002f9860` (RVA `0x1D9860`) — id→leaf resolver; read **RETURN (RAX) POST-call**.
  st2e section table `DAT_02ec3d80` (`0x2DA3D80`), record table `DAT_02f973c0` (`0x2D973C0`).
- Names/desc record `FUN_0035d330` (`0x23D330`) → `&DAT_022ca520`. Codec = `tools/st2e_decode.py`.

**Dialogue (message-window) — separate back-end, NOT `FUN_002f9860`** (`.msb` names are
PS2-legacy, absent from the PC port). Field-dialogue resolver `FUN_00377870` (RVA `0x257870`)
from per-map pack `DAT_02add0f8` (`0x298D0F8`), loader `FUN_00378540` (`0x258540`). One
message-window class (0x179E0 B), 3 singletons: field-talk `DAT_02b47760`/handler `FUN_003cb650`
(`0x2AB650`), system `DAT_0209e5c0`/`FUN_002b7590` (`0x197590`), log `DAT_0209e5f0`/`FUN_002ba700`
(`0x19A700`). Fields: body id `+0x179D0`, flags `+0x179D2` (0x2000=open, 0x80=page-done),
type `+0x179D8`, decoded-text buffer `+0x1B0 + ((flags>>2)&1)*0xBC10`. Open=handler case 1,
advance=case 0x20, draw=case 0x12. **Speaker/caption field: none found (UNCERTAIN**; candidate
attribute `FUN_00254380` `0x134380`).

**Battle text:** `battle_message.bin` is an st2e section (via `FUN_002f9860`); names via
`FUN_002b58b0` (`0x1958B0`). **Flying damage numbers are a per-digit sprite HUD, NOT codec
text** (blitter `FUN_0028aaa0` `0x16AAA0`) — the text hook will not catch them (combat-log
scope, deferred).

**Runtime-only / to confirm in Phase B:** which text hook covers dialogue vs. needs the
message-window path; st2e section-loader RVA (see `find_text_backends.java`) + boot-zeroed
section→file binding; dialogue speaker id source; confirm-pop-up prompt id; new-game settings
creation fn (script-driven, ids 0xd44/0xd45); decoded-buffer format at msgwin `+0x1B0`.

## Navigation Data Tables — DECODED 2026-05-07

`tools/survey_data.py` decoded FFXII's text encoding (offset cipher: bytes
`0x20-0x39` -> `A-Z`, `0x3A-0x53` -> `a-z`, `0x1F` -> space). All twelve
locales have a `bin/` directory containing:

| File | English size | Magic | Contents |
|---|---|---|---|
| `planmapname.bin` | 13,824 B | `PLMN` | Map / area / sub-area display names |
| `npcdic.bin` | 37,545 B | `NPC0` | NPC type dictionary (~2,232 strings) |
| `mapjumpgroupinit.bin` | 116 B | `FLAG`+`DEF1` | Map-jump group definitions |
| `mapjumpgroupflagrom.bin` | 856 B | `FLAG`+`ROM1` | Map-jump group flag table |
| `questbase.bin` | 700 B | (TBD) | Quest definitions |
| `navimapdata.bin` | (varies) | (TBD) | Navigation map data |

Sample decoded English map names: "Cloister of Distant Song", "Alley of Low
Whispers", "Sandsea Bluff", "Greenswathe", "Skygrounds", "Cloister of the
Highborn", Henne-Mines zodiac stones (Aries / Pisces / Cancer / etc.).

Sample decoded NPC labels: "Rabanastran", "Bangaa Merchant", "Adventurer",
"Knight", "Headhunter", "Sherral".

Survey outputs:
- `notes/survey_planmapname.txt` (every locale, raw decoded strings)
- `notes/survey_npcdic.txt` (every locale, raw decoded strings)
- `notes/survey_dbg_symbols.csv` (20,417 symbols across 4 controllers)
- `notes/survey_mapjumpgroup.txt` (raw inspection of jump-group tables)

**Pending:** structured parsers for `PLMN` / `NPC0` headers so each string
gets a stable `(id -> name)` row. The current scanner finds fragments
because it stops at null without following the offset table. ~30 LOC each.

## Map-jump Script API (mapctrl.dbg, 2026-05-07)

The script-side API for map transitions is fully named:

| Function | Use |
|---|---|
| `getmapid` / `getmaptype` / `getmapgroundtype` | Current map info |
| `getmapjumpposbyindex` | Position of exit N on current map |
| `getmapjumpanglebyindex` | Facing direction of exit N |
| `getmapdestposbyindex` | Destination map ID for exit N |
| `getmapjumpmode` / `setmapjumpmode` | Map-jump mode |
| `setmapjumpgroup` / `setmapjumpgroupflag` | Exit group control |
| `mapload` / `mapdispose` | Map lifecycle |
| `mapjumpresult` / `mapjumppos` / `mapjumpstatus` | Per-jump state |

This is exactly the surface a screen reader needs to enumerate exits, get
their positions, look up destination names, and announce them.

## .ebp Bytecode + .dbg Symbols (Game-Script Layer) — UNLOCKED 2026-05-05

`FFXII_TZA.vbf` (29 GiB compressed, 55 GiB uncompressed) contains the
game's true script logic in **10,045 `.ebp` files** (576 MB). Each
controller `.ebp` ships with a paired `.dbg` file containing **full
symbol info** — variable names, function names, even Square's original
PS2 source paths.

### Format
- **.ebp** magic = `EBP2`. Custom bytecode for FFXII's "Athena" DSL
  (source extension `.ath`).
- **.dbg** magic varies per controller; contains string table of
  symbol names, source paths, global flag indices.
- **VBF** format documented in `third_party/ff12-module-loader/VBFReader.h`.
  Header: magic 0x4B595253 ("SRYK"), MD5s, file index (32B/entry,
  with embedded filenames via filenameOffset), string table, block
  index (uint16 zlib-compressed-block-sizes), then 64 KiB blocks.

### Tools (already written)
- `tools/list_vbf.py` — lists every entry in the VBF, summarizes by
  extension. Output: `notes/vbf_contents.txt`.
- `tools/extract_vbf.py` — fnmatch-glob extractor with zlib block decompression.
  Output: `extracted/<original_path>`.

### Key controllers and their original source paths

| Controller | .ebp size | .dbg size | Original source |
|---|---|---|---|
| `ctrl` (system) | 11 KB | 83 KB | `c:/cygwin/home/katano/ps2/ff12_ps2/katano/systemctrl.ath` |
| `evctrl` (event) | 38 KB | 126 KB | `c:/plan_work/in/event/header/systemctrl.ath` |
| `mapctrl` (map) | 40 KB | 301 KB | `C:/image/FF12/mapctrl/mapctrl.src` (refs `mapctrl_mj.src`) |
| `btlctrl` (battle) | 32 KB | (TBD) | TBD |

**Note:** these top-level controllers exist only in `ch/` and `in/`
locale dirs in the VBF — those are the "common" / "champion" baselines
used at runtime. Per-locale UI strings live elsewhere (myoshiok/, etc.).

### Title-screen related symbols found in evctrl.dbg + mapctrl.dbg

| Symbol | Meaning |
|---|---|
| `fsttl_newgamestart` | Title screen "New Game" handler |
| `set_title_flag` | Sets title-screen state |
| `get_title_flag` | Reads title-screen state |
| `random_push_menu` | (in ctrl.dbg) — possibly random menu push |
| `printfmenu` | (in ctrl.dbg) — menu text output? |
| `dispcursor` | (in ctrl.dbg) — cursor display? |

These give us **named hooks** for the title-screen logic — once we
have a proper .ebp/.dbg disassembler (or once the FF12 VM Script
Decompiler from Nexus mod 124 is available), we can read these
functions in source-level form.

### Next steps (script layer)
1. Write `.ebp`+`.dbg` disassembler (Python; has enough format info from
   strings and offsets — pair with bytecode opcode discovery).
2. Find the .ebp interpreter function in `FFXII_TZA.exe`. It will
   reference the `EBP2` magic at startup.
3. Hook the interpreter from the C++ mod — every script call becomes
   observable, including `fsttl_newgamestart` for the title screen.

## Damage / Heal / Status Event Funnel

**STRATEGY UPDATE (2026-05-05):** the static "find functions referencing
'damage'/'miss'/'critical' strings" approach in
`find_damage_candidates.java` returned only 3 hits. FFXII uses **numeric
IDs** for damage / status events, not strings. String xrefs are not
viable for finding the funnel.

**Replacement strategy:** Frida-watch HP writes via memory access monitor,
walk callers (per `probe_damage_event.js`). Seed pointer chain from
DrummerIX's CE table (the table's PermStatusBitsAOB resolves to the active
character's struct base). Once we have leader's HP write site, walk the
caller stack to find the damage funnel.

DrummerIX's `DamageModAOB` pattern (`49 8B C8 44 8B F2 E8 ?? ?? ?? ?? 48 8B
CE 48 8B E8`, line 1047 of FFXII_TZA.CT) is the **direct candidate** — it
hooks the damage modifier path used by his god-mode / hero-multiplier
cheats. Frida-prototyping this AOB to find its concrete RVA + arg layout
is the highest-value Phase-1 task.

| Event | RVA | Args | Source |
|---|---|---|---|
| Damage applied | TBD (DamageModAOB) | (target, amount, type, source) | DrummerIX CE table line 1047 |
| Heal applied | TBD | TBD | TBD |
| Status applied | TBD (StatusEffectAOB) | TBD | DrummerIX CE table line 1379 |
| KO | TBD | TBD | TBD |

If multiple parallel paths exist (physical / magical / status / item), each gets its
own row.

## Text-Rendering Pipeline — Phase-A audit (2026-05-11)

Audit of the 33,105-function Ghidra decompile confirmed the PhyreEngine
text-render path FFXII actually uses on PC. Two-layer architecture:

### Layer 1: per-frame dispatch

| RVA | ABS | Symbol / role |
|---|---|---|
| `0x5F850` | `0x17F850` | `FUN_0017f850` — per-frame "display : draw Font" entry. Single call site; calls the singleton getter and the vertex dispatcher. |
| `0x95FA0` | `0x1B5FA0` | `FUN_001b5fa0` — font-scene singleton getter. **28 callers** in the decompile = candidate set of text producers. |
| `0xC8580` | `0x1E8580` | `FUN_001e8580` — 3,572-byte vertex dispatcher. Iterates `font-scene-ctx + 0x70` (31 slots × 56 bytes each), allocates `DynGeoFontText*` instances for non-empty slots, feeds them per-glyph vertex data. |

### Layer 2: per-instance render

The `DynGeoFontText*Instance` family is **post-rasterization geometry submission** — by the time these methods run, the source string has already been converted to glyph quads. Five classes:

| Class | vftable ABS | vftable RVA |
|---|---|---|
| `DynGeoFontTextInstance` (base) | `0x7E6DC8` | `0x6C6DC8` |
| `DynGeoFontTextSpecialInstance` | `0x7E6DE8` | `0x6C6DE8` |
| `DynGeoFontTextSpecialShadowInstance` | `0x7E6E50` | `0x6C6E50` |
| `DynGeoFontTextSpecialFinalInstance` | `0x7E6EB8` | `0x6C6EB8` |
| `DynGeoFontTextSpecialShadowFinalInstance` | `0x7E6F20` | `0x6C6F20` |

Base class virtuals (3 slots; ctor + dtor not in vtable):

| RVA | ABS | Symbol | Size | Role |
|---|---|---|---|---|
| `0xF30E0` | `0x2130E0` | `DynGeoFontTextInstance::DynGeoFontTextInstance` | 44 B | ctor — zero-inits 7 pointer fields after vftable; 152-byte object |
| `0xF3110` | `0x213110` | `DynGeoFontTextInstance::scalar_deleting_destructor` | 43 B | dtor |
| `0xF3140` | `0x213140` | `DynGeoFontTextInstance::vfunction2` | 644 B | **Draw** — sets render state, loads identity matrix, calls `FUN_002133d0` (vertex-batch submit, RVA `0xC33D0`) up to twice (shadow + main pass) |

Per-instance field layout observed from `vfunction2` reads:

| Offset | Likely role |
|---|---|
| `+0x00` | vftable |
| `+0x08` | main texture (font atlas) |
| `+0x10` | shadow texture (or null) |
| `+0x18` | main vertex buffer |
| `+0x20` | shadow vertex buffer |
| `+0x28` | vertex count |
| `+0x30, +0x38` | style/color refs (passed to `FUN_001ee650`, `FUN_001ee7c0`) |
| `+0x40..+0x4F` | color or position (4 uint32s) |
| `+0x50+` | beyond — may hold source-string pointer if the engine retains it |

### Where the source string lives

NOT directly in `vfunction2`'s parameters. The string was already rasterized
to vertex quads before the dispatcher allocated the instance. Two hypotheses
to test in the Frida probe:

1. **String retained on the instance** (offset > 0x50) for re-rasterization or debug. Find via field walk in probe.
2. **String only in caller frame** — `FUN_001e8580`'s caller (`FUN_0017f850`'s upstream) holds the strings on the font-scene context. Find via vertex-slot scan at `font-scene-ctx + 0x70`.

The probe `probe_text_draw.js` tests both layers simultaneously. See `frida/probe_text_draw.js` and `Docs/plan.md` validation gate G3.2.

### Sanity hook

| RVA | ABS | Symbol | Role |
|---|---|---|---|
| `0x5ACC10` | `0x6CCC10` | `Phyre::PText::PUtilityText::PUtilityText` (ctor) | Fires once at game boot. Used as "module/RVA math OK" smoke test for the Frida probe. |

`PUtilityText`'s 3-slot vftable (ABS `0xC69C68`, RVA `0xB49C68`):

| Slot | RVA | ABS | Role |
|---|---|---|---|
| 0 | `0x480CB0` | `0x5A0CB0` | dispatcher |
| 1 | `0x5ACD60` | `0x6CCD60` | (Phase-A: small) |
| 2 | `0x4A5230` | `0x5C5230` | stub |

None of these slots looks like a `setText` — confirming that `PUtilityText`
is the *service* (init / per-frame tick / shutdown) singleton, not a
per-string text holder.

## Menu State Machine

**LEAD (2026-05-05):** `strings.txt` revealed dense parameter clusters at
known RVAs that name menu sub-systems. The functions that xref these
strings are the menu rendering / state-machine functions.

| Cluster | RVA range | What it is |
|---|---|---|
| Battle command UI | `0x7DB690`–`0x7DBC18` | mCommandTitleFontHeight, BattleCommandNumPosX, BattleCommandMistPosX, etc. (~37 strings) |
| Gambit board | `0x7DB700`–`0x7DC698` | mGambitPositionOffsetX/Y, GambitIconOffsetY, GambitTutorialActionOffsetY, etc. (~15 strings) |
| Cursor positions | `0x7DB7A8`–`0x7DBBF8` | mCursorRightX/Y, mCursorOpenX, m2DSideCursorCenterX/Y |
| Map/scene config | `0x7DA328`–`0x7DAFB0` | TrackingMapScreenRateX/Y, EnableMapNormalMap |

**Phase-1 task:** for each cluster, run an xref pass on its anchor strings
(e.g. "BattleCommandNumPosX") to find the consuming function. That function
is the battle-menu state machine. Frida-prototype-then-hook.

| Function | RVA | Trigger | Source |
|---|---|---|---|
| Menu open | TBD | Pause menu, shop, etc. | xref `mGambitPositionOffsetX` etc. |
| Menu close | TBD | TBD | TBD |
| Cursor move | TBD | TBD | xref `mCursorRightX` |
| Selection commit | TBD | TBD | TBD |
| Save/Load menu state machine | `0xAD0E0` (`FUN_001cd0e0`) | save / load | xref pass landed 74 of 74 "Yes"/"No"/"Load"/"Save" xrefs here (2026-05-05). **Useful as evidence, NOT as a hook target — see Universal Menu Hook design below.** |
| Save/Load menu's 7 callers | TBD | Title→Continue, Pause→Save/Load, etc. | walk callers in Ghidra |

### Universal Menu Hook (architectural — see `memory/feedback_universal_menu_hook.md`)

Per the rule against per-menu hardcoded hooks: we hook the **cursor
controller** that every menu in the game uses, not individual menu state
machines. When focus changes anywhere — title, save, options, inventory,
shop, etc. — the cursor controller's focus method fires; we walk to the
focused cell in the parent widget tree and **read the text the game is
already rendering there**, then vocalize that text verbatim.

Per-menu **layout-aware** code is allowed and expected (e.g. inventory
also reads the description pane; gambit reads condition+action pairs;
license board has 2D grid context). What's NOT allowed is hardcoding the
*text* the mod speaks — text is always read from the rendered widget at
runtime.

**Search target:** the function(s) that read the cursor parameter cluster
at `0x7DB7A8`-`0x7DBBF8` (`mCursorRightX/Y`, `mCursorOpenX`,
`m2DSideCursorCenterX/Y`). Add these to `xref_targets.txt` and re-run the
xrefs Ghidra script.

| Field | RVA / Offset | Source |
|---|---|---|
| Cursor controller class methods | TBD | xref `mCursorRightX` etc. |
| Cursor focus-change event | TBD | likely a method on the cursor controller |
| Cell index at cursor | TBD | field on cursor-controller instance |

### Cursor singletons identified (2026-05-05, from FUN_001cd0e0 decompile analysis; revised 2026-05-11)

FFXII tracks cursor state in **multiple per-menu-shape singletons**, each
accessed via a getter function. The values at these offsets are written
by input handlers and read by renderers / formatters.

**IMPORTANT — pointer-vs-object indirection.** The "Data RVA" column below
points to a **POINTER VARIABLE** in the data section, NOT to the object itself.
The cursor field offset applies to the **heap object** after dereferencing
the pointer. Correct read pattern:

```c
// WRONG:  read at (base + data_RVA + cursor_field)
// RIGHT:  read at (*(u64*)(base + data_RVA)) + cursor_field
```

`probe_cursor_focus.js` (2026-05-11) was authored against the wrong reading
of this table, watched the static data-section bytes instead of the heap
objects, and consequently captured zero writes. Any future probe MUST
dereference first.

| Singleton | Getter (RVA) | Static pointer var (RVA) | Cursor field offset (in heap object) | Type | Notes |
|---|---|---|---|---|---|
| Yes/No confirm | `FUN_001bdab0` (0x9DAB0) | `0x1E61248` (deref) | `+0xf8` | char (0/1) | binary cursor for Yes/No prompts |
| Multi-choice | `FUN_00193d50` (0x73D50) | `0x1E5F5F0` (deref) | `+0x19c` | int (0..N) | indexed cursor for grid / list menus. Heap object size `0x4a8`. |
| Primary UI singleton | `FUN_001a9930` (0x89930) | `0x1E61168` (deref) | TBD | — | 251 callsites — top-level manager, offsets unmapped |
| UI manager (render config) | `FUN_001b1c00` (0x91C00) | `0x1E611E8` (deref) | `+0x3afc` (anim frame); `+0x3b68..+0x3bc0` cursor offsets | mixed | populated by FUN_001ca600 at boot from config strings |
| State array index | `FUN_001ad440` (0x8D440) | `0x1E611C8` (deref) | `+0x40` | array stride | menu state holder |

**Image base is `0x120000`** — to translate Ghidra absolute (e.g.
`0x1f81248`) to RVA: `RVA = absolute - 0x120000`.

**Key insight (architectural — REVISED 2026-05-11):** the 5 cursor singletons
above are for **in-game menus**. The title menu does **NOT** use any of them
(empirically confirmed by `probe_menu_writers` 2026-05-11 — no candidate writer
fired on title arrow presses). The title menu has its own state path, likely
script-side via `.ebp` bytecode (`fsttl_*` symbols), and a separate strategy is
needed. See `Docs/MenuArchitecture.md` for the full in-game-menu architecture
including the menu registry at `DAT_0228ea60`.

## Debug Menu Surface — UNEXPECTED RETAIL FIND

`strings.txt` contains:

| RVA | String |
|---|---|
| `0x7A5E40` | `render debug menu` |
| `0x7DCCAF` | `?JsonData/debugMenuSettings.json` |
| `0x7DF0E8` | `Load Debug Menu Settings` |
| `0x7DF108` | `Save Debug Menu Settings` |

Plus the RTTI-labeled class `Phyre::PFramework::PDebugUtils`.

**Implication:** SE shipped the dev/QA debug menu in retail. If we can
reach an enable path, the debug menu becomes a backdoor: it almost
certainly exposes party-state inspection, item-give, encounter spawning,
etc. — which means the rendering path for that menu is a known-working
example of a menu state machine we can pattern-match against the real
game menus.

**Phase-1 task (low priority, high payoff):** xref `JsonData/debugMenuSettings.json`
to find the function that loads it, see what flag enables the menu.

## NPC Dialogue

*(TBD — Phase 1: `probe_dialogue.js`)*

| Function | RVA | Trigger | Source |
|---|---|---|---|
| Dialogue page show | TBD | NPC interact | TBD |
| Dialogue close | TBD | TBD | TBD |

## Entity / Actor Manager

*(TBD — Phase 1: `probe_entity_list.js`)*

PhyreEngine doesn't have a global "all objects" array equivalent to UE4's
GUObjectArray. Likely: per-area actor list owned by a scene-graph manager.
RTTI confirms `Phyre::PHierarchy::POctreeWorld` is present — that's the
spatial index. Entity enumeration likely walks the octree.

| Manager | RVA | List head | Item type | Source |
|---|---|---|---|---|
| EntityManager | TBD | TBD | TBD | TBD |
| `Phyre::PHierarchy::POctreeWorld` vftable | TBD | — | spatial index | rtti_classes.txt |

## Function Index — Top Targets to Label First

From `functions.csv` (33,662 functions, 87% are `FUN_xxxxx` — Ghidra didn't
label them):

| RVA | Heuristic | Why label |
|---|---|---|
| `0x4b1190` | Largest function (32 KB), 41 calls, 0 callers | Almost certainly the main game loop / dispatcher |
| `0x4c8320` | 28 KB, 7 calls, 0 callers | Update tick or render frame |
| `0x6ff900` | 23.5 KB, 131 calls, 2 callers | Utility hub — mapping its callees gives subsystem entry points |
| `0x59a030` | 144 calls (most-calling), 884 bytes | Master orchestrator |
| `0x2628b0` | 124 calls, 1.7 KB | Subsystem manager |
| `0x22a770` | 92 calls, 2 KB | High-level dispatch |
| `0x633390` | 1,981 callers (most-called), 500 bytes | **Memory allocator or critical helper — DO NOT hook by accident** |
| `0x812960` | 1,568 callers, 83 bytes | Possibly logger / event dispatcher |

## Keymap

`ConfigData\keymap.ini` is a known on-disk file. We may also need the in-memory
keymap struct to know what's bound to back/cancel (combat log close key).

| Field | RVA | Notes |
|---|---|---|
| Keymap struct | TBD | TBD |

## RTTI Status — CONFIRMED FULL (2026-05-05)

- [x] **RTTI present** — 2,371 classes, 2,269 vftables (96% polymorphic).
- Class-prefix breakdown: `Phyre::` 1,802 / `SQEX::Sd::` 227 / `PPFX` 46 /
  `std` 40 / Bullet `bt*` ~10 / Steam `CSteam*` few / game-specific 0.
- **Critical finding: zero game-logic classes recovered.** No Battle,
  PartyManager, Inventory, License, Gambit, Bestiary class names exist in
  the RTTI. Game state machines are either Lua-driven, hand-coded jump
  tables, or aggressively inlined into the main loop. Implication: we
  cannot identify game managers by class name; must trace via UI string
  xrefs and DrummerIX-seeded RVAs.
- Inheritance graph file: `..\FFXII-Decompile\output\rtti_classes.txt`.

### High-value Phyre classes (already RTTI-labeled, ready to hook)

| Class | Use |
|---|---|
| `Phyre::PFramework::PInputDeviceKeyboard` | Keyboard input source |
| `Phyre::PFramework::PInputDevicePadXInput` | XInput gamepad |
| `Phyre::PFramework::PInputDevicePadDirectInput` | DirectInput gamepad |
| `Phyre::PInputs::PInputAction` | Semantic input mapping |
| `Phyre::PCamera` + perspective/orthographic subclasses | Camera state |
| `Phyre::PText::PUtilityText`, `PBitmapFont` | UI text rendering service. **NOTE:** earlier notes referenced `Phyre::PText::PTextObject` — that class does NOT exist in this build (zero hits in rtti_classes.txt, 2026-05-11 audit). The Phyre text-system class is `PUtilityText` (vftable ABS `0xC69C68` / RVA `0xB49C68`) but it's a service singleton, not a per-string holder. Per-string drawing goes through the `DynGeoFontTextInstance` family — see Text-Rendering Pipeline section below. |
| `Phyre::PAnimation::PAnimationEventController` | Animation callbacks |
| `Phyre::PFramework::PApplication` | Main loop |
| `InputManager` (untyped namespace) | Global input dispatcher |
| `SaveloadManager` | Persistence — locale settings flow through here |
| `AudioController` | Game-level audio wrapper |

## PhyreEngine Symbol Recovery

`find_phyre_signatures.java` searches the binary's strings for `phyre::`, `Phyre::`,
`phyrescene::` and labels referencing functions. Findings here are CANDIDATES only —
do not promote without runtime validation.

| Symbol | RVA | Confidence | Source |
|---|---|---|---|

## DrummerIX CE Table Translation

Seed file: `..\FFXII-Decompile\notes\drummer_ix_seed.csv`.

Translation rule: `our_rva = drummer_absolute - drummer_image_base`. Confirm
`drummer_image_base` matches our `FFXII_TZA.exe` image base (default
`0x140000000` for x64 PE). If DrummerIX used a different base, document the
delta here.

| DrummerIX address | Our RVA | Symbol | Validated? |
|---|---|---|---|

---

## Pathfinder / Field Navigation (Phase 4) — RE 2026-07-06

RVA = Ghidra-abs − 0x120000. Confidence + validation state noted; anything <0.98 is
**Frida-pending** and must NOT enter C++ until confirmed.

### CALLACT action-binding table (RE-0) — CONFIRMED ~0.98 (read firsthand)
- Interpreter `FUN_0025e4c0` (RVA `0x13E4C0`) CALLACT case → dispatcher
  `FUN_002621d0` (RVA `0x1421D0`): `selector=idx>>12`, `slot=idx&0xFFF`; enter/exec/poll
  triad; nonzero exec return suspends as coroutine.
- Resolvers `FUN_003dc240/270/2a0` (RVA `0x2BC240/270/2A0`): `table=(&DAT_02b57ef0)[sel];
  if(slot<table[0]) return *(entry + slot*0x20 + K)`; count@+0x00, enter@+0x08,
  exec@+0x10, poll@+0x20.
- Registration `FUN_003dc2d0` (RVA `0x2BC2D0`): real tables at selectors 0/3/5/7 →
  `DAT_01eed700`/`01f281b0`/`01f29418`/`01f29440` (RVA `0x1DCD700`/`1E081B0`/`1E09418`/
  `1E09440`); counts **1496/147/1/104**. `DAT_02b57ef0` (RVA `0x2A37EF0`) is
  runtime-populated (zeros in image) — read the module tables directly.
- Dump: `FFXII-Decompile/output/action_binding_tables.txt` (script
  `ghidra/dump_action_binding_tables.java`). VM glue: arg-fetch `FUN_00267e10`,
  value-return `FUN_0026b4e0`; getters call the latter. NAMING of slots is unresolved
  (no static name table; needs .dbg-ordinal↔slot alignment — see plan Track 1).

### Player position + facing (RE-1) — struct layout CONFIRMED 0.97; runtime handle Frida-pending
- `Phyre::PPhysics::PPhysicsCharacterControllerBullet` size `0x190`; base
  `PPhysicsCharacterControllerBase` size `0x130`. Reflection registrar
  `FUN_006949a0` (RVA `0x5749A0`).
- Base fields: `m_startPosition@0x08` (spawn, NOT live), `m_targetNode@0xC8` (`PNode*`),
  `m_targetWorldMatrix@0xD0` (`PWorldMatrix*`), `m_world@0xD8`, `m_rotate@0xE0` (float,
  yaw candidate 0.55), `m_right@0xE4`, `m_forward@0xE8`, `m_velocity@0xEC`,
  `m_isOnGround@0x105`.
- **Live position** = translation of the 4x4 at `*(controller+0xD0)`
  (`PWorldMatrix.m_matrix@0x00`); translation likely `+0x30/0x34/0x38` (**0.6 — VERIFY**).
  Alt path `controller+0xC8 → PNode+0x18 → PWorldMatrix+0x00`.
- Manager `PPhysicsWorld+0x50` = `m_characterControllers` list (`m_next@0x00`), 0.95.
- **LEADER ANCHOR (STATIC, ≥0.98, verified 2026-07-07):** current player-controlled
  character scene handle = **`DAT_022c7fe0` (RVA `0x21A7FE0`)**, returned by universal
  accessor **`FUN_003590d0` (RVA `0x2390D0`)** (~250 callers). Resolve:
  **`FUN_003588b0(handle)` (RVA `0x2388B0`)** = generation-checked handle table → scene
  object → **`+0x30`** (`FUN_00263e30`, RVA `0x143E30`) → char component (valid `*comp&8`).
  Field-active gate **`DAT_02089340 & 0x10`**. Source-of-truth: party mgr `*DAT_02ebf190`
  (RVA `0x2D9F190`), leader index `mgr+0x5aa4`, control index `mgr+0x5ad5`; leader-change
  refresh `FUN_00326500` promotes the handle + retargets camera (⇒ field leader, not menu).
  Remaining pin: char component → position field (its controller `+0xD0`/`+0x30`, or a
  direct matrix) — confirm live. Corollary: leader→controller `+0xD8` = `PPhysicsWorld`
  (RE-4 context) → `+0x60` = raycast world, so this anchor closes RE-4's handle too.
- **CORRECTION — camera globals are STALE for our build:** `CameraLookAtPointPtr`/
  `CameraPositionPtr` (community RVA `0x20955F0`/`E0`) do NOT exist here (0 occurrences,
  verified). Community RVAs may target a different build — validate each before use.

### Bullet walkability (RE-4) — CONFIRMED ~0.95 (verified firsthand); runtime handle Frida-pending
- `btCollisionWorld::rayTest` = **`FUN_0083fc70` (RVA `0x71FC70`)**, vtable slot 6, called
  via world vtable offset `0x30`. (`0x71DE00` is `debugDrawWorld`, NOT rayTest.)
  `convexSweepTest` RVA `0x71B780` (slot 5). Vtables: `PTR_FUN_01cce610` (btCollisionWorld)
  / `PTR_FUN_01ccf5e0` (btDiscreteDynamicsWorld).
- **Active world = `*(physicsContext + 0x60)`** (no static global). Built by `FUN_006a0310`
  (RVA `0x580310`, stores at `:57`); stepped by `FUN_0069f070` (RVA `0x57F070`, vtable
  slot `0xa0`). Root singleton `DAT_02e4aff0` (RVA `0x2D2AFF0`) holds shared
  dispatcher/broadphase only.
- **Ready-made ray cast:** `FUN_006a1a70` (RVA `0x581A70`) `(context, from*, to*, out*,
  filterGroup)` — reads world at `context+0x60`, builds `ClosestRayResultCallback`
  (vtable `PTR_FUN_00d7ab10`), calls rayTest, writes hit point+normal to `out`. btVector3
  = 16 B `{x,y,z,pad}` by pointer. Call this directly with a cached context.
- **Runtime handle (G4.8):** hook `FUN_006a0310` (map-load, once) or `FUN_0069f070` to
  capture `context`; world null on title/between maps — guard.

### C++ M0 self-diagnostic (2026-07-07) — implemented in `src/navigation/`
Straight-to-C++ exception (user-approved): the leader/physics chain is confirmed **in-mod
via a logged read-only dump** (`\` key) instead of a standalone Frida probe. Firsthand
decompile facts wired into `src/navigation/nav_rva.h` + `player_state.cpp`:
- **RVA CORRECTION:** field-active gate `DAT_02089340` → RVA **`0x1F69340`** (= abs
  `0x2089340` − `0x120000`). The pathfinder plan's table listed `0x2089340` un-converted —
  do NOT use that. Read as a byte, `& 0x10`. (`DAT_022c7fe0`→`0x21A7FE0` and
  `*DAT_02ebf190`→`0x2D9F190` were already correct.)
- **Handle-table resolve (replicated inline, memory-only)** from `FUN_003588b0` +
  `FUN_00263ff0`: `handle` is a **uint** (not a ptr); `selector=(h>>0x10)&0xf` (must be <5),
  `slot=h&0xFFFF`, `generation=(h>>0x14)&0x7FF`. Table = `&DAT_02098e10` (RVA **`0x1F78E10`**)
  `+ selector*0x288` (`0x51`×8). Table fields: guard@+0x00 (≠0), entries(int*)@+0x08,
  active-bit@+0x10 (`&1`), capacity@+0x20. Object ptr = `*(entries + 0x08 + slot*8)`; count
  at `entries+0x00`. Generation check: `*(u16*)(obj+0x16) == generation` else stale.
  Scene obj → char component = `*(sceneObj+0x30)`, valid when `*(u32*)comp & 0x08`.
- **Game's own consumer chain = `FUN_00317e60`** (leader→component, gated on
  `DAT_02089340 & 0x10`). ⚠️ It also does `*comp |= 0x100000000` — a WRITE the mod must
  **never** replicate (announce-only / read-only).
- **Unpinned hop (what M0 discovers):** component→controller→matrix is NOT a plain getter —
  `FUN_0033c9b0(comp+0x80)` *constructs* a transform. M0 dumps the component tree and flags
  any pointer field whose `+0xD8 == cached physics ctx` (definitive controller anchor) and/or
  whose `*(+0xD0)+0x30/34/38` reads as world coords. Result feeds `PlayerState::ReadPlayerPos/
  Yaw` (currently stubbed `false` until the offset is recorded here).
- **ctx capture:** hook on `FUN_006a0310` caches its RCX arg (the physics context) →
  `BulletQuery`; `FUN_006a1a70(ctx,...)` reads world at `ctx+0x60` (guarded before every call).

### Full C++ implementation (Session 22) — canonical addresses
The M0 "dump-and-hunt" was retired: position/yaw are STATIC (no controller), and classification/names
are OFFLINE master data. Implemented in `src/navigation/`.
- **Player pos** = `*(sceneObj+0xB8)` floats `+0/+4/+8` (engine getter `FUN_00265020`; guarded by type
  nibble `(*(u8)(sceneObj+3)>>5)∈{1,3}`). **Yaw** = `atan2(fwd.x,fwd.z)`, fwd = `comp+0x100/+0x108`
  (`comp=*(sceneObj+0x30)`, valid `*(u32)comp & 8`). Controller `+0xD0` matrix was only a *writer*.
- **Field-actor pool** (enumeration): base ptr `DAT_0208e688` (RVA `0x1F6E688`), stride `0xF50`, count
  `DAT_0208e6a0` (`0x1F6E6A0`). Per actor: pos `+0xE0/+0xE4/+0xE8`, yaw `+0x160`, def `*(actor+0x698)`,
  `sceneObj *(actor+0x10)`; def: kind `*(s8)(def+5)` (0=NPC,1=gimmick), id `*(u16)(def+4)`.
- **Object name (master data, live/localized):** `ctx = FUN_0035d380(1, defId)` (RVA `0x23D380`) → codec
  string at `ctx+0x08` (NPC) / `ctx+0x10` (gimmick) → `GameText::Decode`. Classifier = npcdic def id:
  **469=Save Crystal, 466=Gate Crystal, 435–465=area gate crystals, 434=Treasure**. Bank resolver
  `FUN_002f9860` (`0x1D9860`); empty sentinel `DAT_01ceb638` (`0x1BCB638`). Track-1 naming formula:
  selector-0 slot = `mapctrl.dbg_index − 5140`.
- **Area name:** `FUN_003778b0()` (RVA `0x2578B0`, current area, no arg) → decode. (`getmapid`
  `FUN_002e9890`/`FUN_00348100`; `mapjumpgroup*` = story-flag tables, NOT exit geometry.)
- **World scale = METERS** (offline, definitive): Bullet default gravity `-10` (`FUN_00854980` `0x734980`);
  char-controller defaults `FUN_00690fc0` (RVA `0x570FC0`): `m_height 0.8`, `m_radius 0.6` (~2 m humanoid),
  `m_velocity 10`, `jumpHeight 1.5`, self-gravity `-19.6`; collision margin `0.04`. ⇒ units-per-step
  **0.75**, grid cell **~0.5**.
- **Obstacle guidance:** `BulletQuery::HorizontalClear` (one ray) + a ≤4-ray fan → "clear"/"blocked, bear X".
  Full A* occupancy grid deferred (thousands of rays would race the physics step → needs game-thread run).

### Keyboard input capture (Session 23) — the game grabs the keyboard exclusively
**FFXII acquires the keyboard via DirectInput in EXCLUSIVE mode**, which installs a swallowing low-level
keyboard hook: `WH_KEYBOARD_LL` hooks (the mod's hotkeys) AND NVDA's own commands are starved (only a few
unbound keys like Tab/Space leak through). Tolk *speech* still works (it's an API call, not a keypress).
**Solution that works — ride the game's own DirectInput poll (not a mode change):** the `dinput8.dll` proxy
patches `IDirectInput8::CreateDevice` (COM vtable **index 3**) to identify the keyboard device
(`GUID_SysKeyboard`), then patches that device's `IDirectInputDevice8::GetDeviceState` (**index 9**). Each
frame the game polls the 256-byte DIK scan-code buffer; our hook reads the *same* buffer and feeds the mod's
hotkeys (`InputTracker::FeedDInputKeyboard`) — edge-detected, dispatched on rising edges. Exclusivity is
irrelevant and the game's behavior is unchanged. (Forcing the keyboard non-exclusive via `SetCooperativeLevel`
was tried and did NOT work; reverted.) NVDA's own key commands remain blocked under the game's grab — separate
issue; the mod doesn't depend on them. Full game keybindings + mod keys: `Docs/Controls.md`.

### Name resolver + Layer-3 turn-by-turn (Session 24, 2026-07-08)
**Compass frame:** FFXII world **north = -Z**. `nav_common::BearingDeg` = `atan2(dx, -dz)`;
`ReadPlayerYaw` = `atan2(fx, -fz)`; the entity_list obstacle-probe uses the same (base
`atan2(dx,-dz)`, offset `p.z - cos(a)*probe`). E/W (dx) not flipped.

**Object name resolver (memory-only; replaces the broken `FUN_0035d380(1,def+4)` party resolver).**
The game's own `FUN_00263990(sceneObj)` (RVA `0x143990`) reads a name key at **`sceneObj+0x102`**
(s16), where `sceneObj = *(actor+0x10)`:
- `idx >= 0` → global **npcdic** dictionary. `npcdic.bin` (`NPC0`) is loaded at boot (resource cat
  9 / id 0x1f) into **`DAT_02b5e0d8` (RVA `0x2A3E0D8`, holds the blob base)**. Lookup `FUN_003eac10`
  (RVA `0x2CAC10`): `id &= 0xffffbfff`; `slot = id*2`; if `slot < *(int)(base+8)` then codec ptr =
  `*(s32)(base + 0xc + slot*4)` (relocated low-mem pointer, sign-extend). Even slot = name, odd = yomi.
- `idx < 0` → per-map custom string at `*(sceneObj+0xf8)` (set by the map's `fieldsignmes` script).
- Decode with `GameText::Decode`. Offline-verified via `..\FFXII-Decompile\tools\parse_npcdic.py`
  (real codec): **469=Save Crystal, 466=Gate Crystal, 434=Treasure, 468=Urn**. Area names =
  `planmapname.bin` (`PLMN`), `tools/parse_planmapname.py`: 1328=Rabanastre. Runtime reads the game's
  OWN loaded copy (per-locale); nothing shipped (SE IP).

**Layer-3 turn-by-turn = A* on the GAME THREAD** (`src/navigation/path_planner.{h,cpp}`; key `/`
= `VK_OEM_2`/DIK `0x35`). Lazy-sampled walkability A* (cell 1 m, ray budget 2000, range 40 m via
`bullet_query`); output = turn-by-turn legs or **"No path"/"Too far to route"/"Route unavailable"**
(no crow-flies fallback — that is `\`). **Crash-safety (ported from DQ7R post-mortems):**
- Drain hooked at **`FUN_00314020` (RVA `0x1F4020`)** — the once-per-frame field sim step (mode 0 =
  field), at ENTRY (before its teardown driver `FUN_0025bfb0` runs later in the same call). Returns
  **bool** — match it (a wrong return type clobbers RAX).
- World-invalidate + **monotonic map-epoch bump** hooked at **`FUN_002695a0` (RVA `0x1495A0`)** — the
  field-global teardown, at START (before it zeroes the leader ptr / clears 0x10 / frees the world).
  Returns **void**.
- Hard gate `PlayerState::IsFieldNavSafe()` — the `0x10` field-live bit is stale-valid on teardown /
  premature on load, so it is paired with: area collision **`DAT_02b5e0c0` (RVA `0x2A3E0C0`)** ≠ 0,
  area id **`DAT_02b5e0b8` (RVA `0x2A3E0B8`)** ≠ 0xFFFFFFFF, live `*(ctx+0x60)`, leader
  **`DAT_0209a1f0` (RVA `0x1F7A1F0`)** ≠ 0, and the leader resolves. A request captured before a
  transition is un-revivably dropped by the epoch. Map lifecycle: load sets `0x10` early
  (`FUN_002342f0`) before collision/world ready; teardown clears it late (`FUN_002341c0`) after they
  are freed — hence the extra liveness terms + the invalidation hook.

### Field-object scanner CORRECTED + drain fix (Session 25, 2026-07-09)
Runtime test (Reks prologue) exposed three defects; RE of the game's OWN interaction scanner
(`FUN_0025b820`) fixed them. **Supersedes the actor-pool enumeration and the `FUN_00314020`
drain above.**

**Enumerate the scene-object HANDLE TABLE, not the BtlWork actor pool.** `DAT_0208e688` holds
characters/combatants only — it never contained the field gimmicks (the old scan found just the 2
NPCs, never the iron gate). The registry of EVERY live field object (party, NPCs, and static
gimmicks: gates/doors/switches/treasure/crystals), populated en-masse at MAP LOAD
(`FUN_002679f0`/`FUN_0026ce60`) and walked each frame by the interaction scanner `FUN_0025b820`
(RVA `0x13B820`), is `DAT_02098e10` (RVA `0x1F78E10`), **5 containers × 0x288**:
- active = `*(u8)(base + c*0x288 + 0x10) & 1`; entry array = `*(void**)(base + c*0x288 + 0x08)`
- count = `*(int)entries` (== block+0x20); object i = `*(void**)(entries + 0x08 + i*8)` (0 = empty).
Skip the leader. Gimmicks are NOT proximity-spawned — only the floating Action Icon is proximity-driven.

**Position (universal): `node = *(sceneObj+0xB8); XYZ = node+0x00/+0x04/+0x08` (float).** Valid for
every world-present object (obj+0x03 low-5 category 1–7; category 0 = pure trigger → node null).
**Do NOT apply `FUN_00265020`'s class-nibble guard** (`(*(u8)(sceneObj+3)>>5)∈{1,3}`) — it zeroes a
gate whose class isn't 1/3, dropping it. `PlayerState::ReadSceneObjectPos` now guards only on
`node != 0`. (Node layout `FUN_00266ad0`: cats 1–7 → transform node at obj+0x120; +0x00 = world pos.)

**Interaction flags @ `sceneObj+0x1C`** (from `FUN_0025b820`/`FUN_0025bad0`/`FUN_0025be50`): bit
`0x400` = talk target (NPC/person), bit `0x4` = action target (gate/door/switch/item). The game's
chosen interaction target: container id `DAT_0209a2b4`, object id `DAT_0209a2b8`, min-dist
`DAT_0209a2b0` (RVA 0x1F7A2B4/B8/B0); leader scene object `DAT_02099d78` (RVA `0x1F79D78`).

**Classify** by the npcdic id band (`sceneObj+0x102 & 0xbfff`): 433–469 = gimmick objects (434
Treasure, 468 Urn, 466 Gate Crystal, 469 Save Crystal, 435–459/467 crystals) → sub-type; else
talk-flag → Person; else Object. **`def+0x05` is 0=static/1=animated, NOT an NPC flag** — the old
`(kind==0)?NPC:Object` mislabeled every NPC (they are kind 1); the pool path is now unused.

**Route drain moved to `FUN_0022a770` (RVA `0x10A770`)** — the no-arg per-field-frame tick
(walking-state `DAT_02064ad3==2`), returns u64. Replaces `FUN_00314020` (0x1F4020): its `mode==0`
path hides behind a fixed-timestep accumulator AND an else-branch bypass (`FUN_001800e0()!=0` →
`FUN_002f1770`) a scripted tutorial holds open → the route request was never drained (silent `/`).
Teardown hook (`FUN_002695a0`) + `IsFieldNavSafe` gate unchanged. `entity_list` now walks the handle
table, rescans fresh on every cycle/describe, and `LogDiagnostic` dumps all named/interactive scene
objects per container (cat byte, flags, npcdic key, name, pos).

---

## Message / Dialogue / Panel Text (2026-07-07) — decompile-exhausted, ≥0.98; Frida-pending

Full spec + evidence: `..\FFXII-Decompile\notes\message_text_readpoints_spec.md`. Four distinct
surfaces; two readable, two are baked assets (not readable via codec).

**A. NPC dialogue + in-engine cutscene captions — message window `e5f0`.**
- `DAT_0209e5f0` (RVA `0x1F7E5F0`, pointer global) = primary live dialogue surface (all openers
  drive it; owns `mini_face_c` portrait). `e5c0` redundant twin; `b47760` chrome. None is a backlog.
- Page obj: `page = DAT_0209e5f0 + 0x1B0 + ((*(u16*)(root+0x179d2)>>2)&1)*0xBC10`. Page proc
  `FUN_002baf80` (RVA `0x19AF80`); builder `FUN_002b9d30` (RVA `0x199D30`). msgId = `*(short*)(page+0x138)`.
- **Body text:** observe `FUN_003c02b0(msgId,out)` (RVA `0x2A02B0`) → `*(out+8)` = codec ptr;
  `*(u16*)(out+2)&0xff` = speaker attr. No game call. (Body is glyph-nodes at `page+0xBBC0` — no
  memory-only string.)
- **Speaker name (memory-only):** nameplate `DAT_02b62d78` (RVA `0x2962D78`, ptr global):
  `*(char**)( *(void**)( *(void**)(DAT_02b62d78+0x60) ) + 0x18 )` = rendered caption (draw chain
  ends `FUN_002dd680:40`→`FUN_002b0280`). Visibility gate `(*(u32*)(DAT_02b62d78+0x40) & 0x405)==5`
  (FUN_00245e60 draw gate) — read the name only when visible, else a stale name can persist.

**B. Item / treasure / battle-system / yes-no confirm — one memory-only buffer.**
- Hook `FUN_0057c480` (RVA `0x45C480`), message==1 (surface birth); text = `param_1(surface)+0x1B0`
  (0x400-byte codec buffer, written once by `FUN_00254f30`). No global needed — the surface is the
  hook's `param_1`. (Also reachable as `*(*(DAT_0209ac30 RVA 0x1F7AC30, ptr→&DAT_0209ac60)+0x328)`.)
- **Classify by surface fields (the ≥0.98 gate; cmd id `+0xdf8` DISCARDED — battle-global, not per-surface):**
  `surface[0x636]` = choice count, `surface[0x630] & 0x8` = passive-text flag.
  count==0 && passive → INFO (item/treasure/battle-system, producer `FUN_002ce370`) = SPEAK.
  count==2 → yes/no confirm (`FUN_002cdf20`, choices via `FUN_0057c320`→`FUN_0057c140` at `+0x5b0/+0x5b8`).
  count≥2 other → multi-choice (`FUN_00566680`). Confirm/choice classes are a DIFFERENT window class than
  the menu-registry confirms `FUN_00241d40` (distinct obj[0]) → muted in the reader, not double-spoken.

**C. Guided-tutorial banners + on-screen telop text ("TUTORIAL / Try using … to adjust the viewing angle").**
Live-CONFIRMED in play-test (Session 19). The game's on-screen **telop overlay**, a THIRD surface distinct from A/B,
the help-bar (`FUN_00291d80`) and the Handbook image viewer. Content setter **`FUN_002e16b0(ctx, slot, textPtr, _)`
(RVA `0x1C16B0`)** — `param_3` = full `HEADER\x02BODY` codec string (0x02→newline), once per set (null = clear).
Downstream: `FUN_002a35b0` (0x1835b0) → `FUN_002a3250` (0x183250, caption/body split); header ptr `DAT_0209c988`
(RVA `0x1F7C988`); fed by script natives `FUN_00348df0`/`FUN_0034ada0`/`FUN_0050eab0`. Hooked in `message_reader`
(decode+speak). **Follow-up:** button-icon inserts (0x0f escapes = ↑←↓→ glyphs) currently decode to nothing → map
0x0f button selectors to names so key prompts read.

**NOT readable (baked assets — discarded):** pre-rendered FMV movie subtitles (movie-embedded
glyph runs; the "Subtitles" toggle gates this, `DAT_0209be80+0x10f68` bit0xc → `FUN_00550510` →
overlay `DAT_02ca8f38`); tutorial panels (Handbook images `menuhandbook_tutorialNNN.dat`). Battle
multi-line detail builder `FUN_00293310` family (unresolved `.rdata` dispatch) — header line still
captured by B. Target-select window `DAT_0209be80`/`FUN_00552250` = battle targeting UI, out of scope.

**SHIPPED in C++** (Session 18): `src/ui/message_reader.{cpp,h}` hooks the three functions above via
`Hooks::InstallTyped`; reads memory-only + SEH-guarded (`src/core/mem_read.h`); `r` = re-read last line.
Built + deployed, pending integrated play-test. Optional aid: `..\FFXII-Decompile\frida\probe_message_text.js`.

---

## Battle Command Menu + Targeting (Session 30, 2026-07-10) — CONFIRMED

**Battle command menu (the seamless-combat ATB list: Attack / Magicks & Technicks / Items / …).**
It routes cursor moves through the **SAME `FUN_00247510` msg `0x8000`** dispatch the field menu uses —
it was just an unmapped owner class (showed as `[focus] UNKNOWN owner obj[0]=+0x15ad70`).
- **Owner (focus target) = the command PANEL, window class `FUN_0027ad70`** (RVA `0x15AD70`). It is
  built in place inside the container `FUN_002778c0` (RVA `0x1578C0`) at `container+0xf0`, so the
  `owner` seen at `FUN_00247510` IS the panel (no container hop).
- Highlighted command id = **`*(u16)(owner + 0x510 + index*8)`** (entries stride 8); count =
  `*(int)(owner+0x500)`; command type/category = `*(int)(owner+0x4c0)`. `index` = the 0x8000 `val`.
- **Name:** the top-level per-row draw **`FUN_00276be0`** (RVA `0x156BE0`) resolves each command name
  into `panel+0x1578` via `FUN_0035d330(0x15, cmdId)` (RVA `0x23D330`) → `FUN_002b58b0(*(rec+0x18),0)`
  (RVA `0x19B8B0`). Mod caches the DECODED name per cmdId from that draw (memory-only). Verified:
  0x0=Attack, 0x12="Magicks & Technicks" (codec `0xa0`='&'), 0x3=Items.
- **Sub-lists (Session 31) — SHIPPED, USER-CONFIRMED.** The mod picks the resolver by the panel's per-row
  draw callback `*( *(panel+0x1510) + 0x120 )` (exact-match guarded → wrong guess is SILENT, never wrong):
  - **Items** = draw `FUN_0027e530` (RVA `0x15E530`) → resolver `FUN_00272cb0(id)` (RVA `0x152CB0`) returns
    the name codec directly. (Earlier "FUN_0027d5c0/cat 2" claim was WRONG — retracted.)
  - **"Magicks & Technicks" is TWO-LEVEL** (trace: top cmdId 0x12 → `FUN_0027c3d0` returns kind 2 →
    `FUN_0027e050` case 2 type 8; then category → kind 0xa-0xf → type 0xb): (1) category **chooser** draw
    `FUN_0027d240` (RVA `0x15D240`), resolver `FUN_0035d330(cat, id)` with `cat = (panel+0x513+row*8 & 4) ?
    0x18 : 0x15`, name at `panel+0x1578` (not overwritten); (2) spell/technick **list** draw `FUN_0027ce70`
    (RVA `0x15CE70`), `FUN_0035d330(0x14, id)`, `panel+0x1578` OVERWRITTEN by MP-cost → re-resolve.
- **NOT** `FUN_002c2320` (that's the FIELD Equipment screen, opened via pause menu `FUN_00281ed0` cmd
  0x4b6). **NOT** `FUN_002b7590`/`DAT_0209e5c0` (that's the message/dialogue framework). Both retracted.

**Battle targeting (Foes/Party/Allies highlight select) — SHIPPED Session 32.**
⚠️ The Session-28..31 model (window `FUN_00552250` / reticle `FUN_005528c0` / gate `DAT_02ca8f38`,
`node+0x48`/`+0x54`, `DAT_0209be80+0x10fa2` mode) is **DISPROVEN** — a live probe showed those NEVER
fire for normal foe/ally selection; that set is the free-aim/AREA-target mode only. **Do not reuse it
for the highlight menu.** The reader that hooked it spoke nothing.
- The highlight target selector is a SEPARATE object on the battle-HUD context `DAT_0209be80`
  (RVA `0x1F7BE80`, a pointer `P`), **NOT** the command controller `DAT_0209ac30` (whose `+0xde0` is the
  ACTING character, not the target). **Current highlighted target handle = `*(int)(P + 0x9FD8)`**
  (nameplate/target-info mgr `0x8fa0+0xac0+0x578`); **active while `*(P + 0x10f78) != 0`** (single-target
  selector obj — absent during plain command navigation). Committed by `FUN_002be300` (RVA `0x19E300`),
  which plays the cursor-move SE `FUN_00249c60(1)` only on a real change.
- Handle decode `FUN_003588b0` (RVA `0x2388B0`) = `(list=bits16-19, slot=low16, gen=bits20-30)` —
  UNRELIABLE to call directly from a hook (returns garbage). Instead capture the real BtlChr indirectly:
  the nameplate render `FUN_002bfd20` (RVA `0x19FD20`) resolves the handle and passes the BtlChr to the
  vitals builder `FUN_00329220` (RVA `0x209220`). The reader hooks BOTH: flag the `FUN_002bfd20` call
  whose `panel+0x288 == *(P+0x9FD8)` (gate on `+0x10f78`), then grab `bc` in the nested `FUN_00329220`.
  Name = actor pool (`*(actor+0x698)==bc` → `actor+0x18`); **real** HP `bc+0x48`/`bc+0x24`; faction =
  scene-kind `*(u8)( *(actor+0x10) + 0x0e ) & 0x0f` (`3`=ally, else enemy). Speech: **enemy** = HP
  percentage (no Libra HP-visible flag found — BtlChr status bit `0x10000` was WRONG, read 0 for the
  un-Libra'd enemy); **ally** = HP numbers. Src: `src/ui/battle_target_reader.{h,cpp}`.
  (The older `notes/battle_target_vitals_2026_07_10.md` id→`FUN_002367a0`→actor path is SUPERSEDED.)

**Field-menu "Select a character" (Status) reader — DEFERRED (Session 31, NOT working).** Chooser
controller `FUN_00285290` (RVA `0x165290`, POLLED, not a 0x8000 owner); cursor-set `FUN_00285a10(slot)`
(RVA `0x165A10`). Per-slot read (mirrors portrait draw `FUN_00283e40`): `ctx = *(DAT_0209ac30)` (RVA
**`0x1F7AC30`**) → `ctrl = *(ctx+0xf8)` → `portrait = *(ctrl+0xc0+slot*8)` → `block = *(ctx+0xac8 +
*(int)(portrait+0xc0)*8)`; charId `*(i16)(block+0x60)`, curHP `+0x20`, maxHP `+0x24`, Lv `+0xba`, MP
`+0x2c/+0x30`; name `FUN_0035d330(2,charId)`. **Blocker:** `FUN_00285a10` does NOT fire for the entry
highlight (case 1 sets `ctrl+0x117` directly); with a one-character tutorial party it never fires at all →
silent. Needs a "speak initial focus on menu entry" hook. Labels (LEVEL/HP/MAX/MP/MAX) source unresolved.
See `notes/status_char_select_labels_2026_07_11.md`.

---

**Last updated:** 2026-07-11 (Session 32) — Battle target-selection readout SHIPPED: real path is a
SEPARATE selector on `DAT_0209be80` (`P+0x9FD8` handle, gate `P+0x10f78`), NOT the reticle set
(DISPROVEN) nor `DAT_0209ac30+0xde0` (acting char). Two-hook reader (`FUN_002bfd20`+`FUN_00329220`),
name/HP via actor pool, faction via scene-kind; enemy=HP%, ally=HP numbers. `src/ui/battle_target_reader.*`.
Session 31 — Magicks & Technicks two-level sub-lists SHIPPED (chooser
`FUN_0027d240`, spell list `FUN_0027ce70`; items `FUN_0027e530`→`FUN_00272cb0`); targeting NAME gate fixed
(dropped the wrong mode-3 bail); Status field-menu char-select reader DEFERRED (hook doesn't fire on entry).
Session 30: Battle command menu located (`FUN_0027ad70` via `FUN_00247510` 0x8000) + SHIPPED; targeting
`node+0x48`/`+0x54` confirmed; corrected the `FUN_002c2320`=field-Equipment and `FUN_002b7590`=dialogue mislabels.
Prior: 2026-07-07 (Session 18) — Message/dialogue/panel reader SHIPPED in C++
(`message_reader`): e5f0 dialogue body+speaker; `FUN_0057c480+0x1B0` panel gated by `surface[0x636]`/
`[0x630]` (INFO spoken, confirms muted); `r` re-read. Built/deployed, play-test pending.
Prior: 2026-07-07 (S17) message-text read-points decompile-exhausted; 2026-07-06 Pathfinder RE.
