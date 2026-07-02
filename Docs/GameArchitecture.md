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

**Last updated:** Phase 0 scaffolding (no game interaction yet).
