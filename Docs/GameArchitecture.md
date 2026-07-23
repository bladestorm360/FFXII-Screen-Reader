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

**Where these live in the mod (Session 51).** Offsets shared by more than one module — the
field-actor pool, the BtlChr record, the scene-kind faction nibble, `BTLWORK_PTR` and the
master-data reloc base — are owned by `src/core/phyre_types.h`. Menu surface identity lives in
`src/ui/menu_state.h`; map/exit/planmapname addresses in `src/navigation/map_rva.h`; field-nav
addresses in `src/navigation/nav_rva.h`. Grep those before re-deriving an offset.

**Codec PAGE BREAK = `0x03`** (confirmed from shipped data 2026-07-21). A multi-page dialogue
message is ONE string; `0x03` separates the screens. Evidence: `tools/ebp_find_pagebreak.py` decoded
17,268 messages from 617 extracted `.ebp` scripts and found **3164 of `0x03`'s 3309 occurrences
(95.6%)** sitting exactly at a visible page boundary (sentence-end immediately followed by a capital,
no space) — nearest rival `0x0f 20` at 2.0%. Corroborated by `FUN_002ac5f0`, where `0x03` is the only
control case that returns 0 (ending the draw pass) after storing the resume position in `*param_2`.
`GameText::DecodePages` splits on it; `rbn_a16.ebp` msg 151 (hunt tutorial) has 4 → 5 pages.
Dialogue `.ebp` layout: `EBP2` magic, `MSG_BASE` at `+0x18`, self-describing `u32` offset table.

**Text capture hooks** (codec text, NOT UTF-16):
- PRIMARY `FUN_002b3050` (RVA `0x18B050`) — read **param_2 (RDX) = codec `byte*` PRE-call**;
  28 callers; covers menus, item/ability names+descriptions, panels/prompts, battle-UI text.
- FALLBACK `FUN_002af340` (RVA `0x18F340`) — param_2 codec `byte*` PRE-call (universal but
  fires multiple measure passes **per frame**, so anything hung off it needs a per-frame
  change-check — one of the two sanctioned exceptions to the no-dedup rule, and the comment must
  name this function. See CLAUDE.md "Code quality". This is why the PRIMARY hook is preferred.)
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
| `getmapjumpposbyindex` | Position of exit N on current map (`mapData+0x54`) |
| `getmapjumpanglebyindex` | Facing direction of exit N |
| ~~`getmapdestposbyindex`~~ | ~~Destination map ID for exit N~~ — **STRUCK (Session 46).** It returns an arrival **POSITION**, not a map id: it is `FUN_00264b90` with selector 1, reading the `mapData+0x84` table, and yields the same four floats (x/y/z/angle) as the selector-0 `+0x54` read. **`+0x54` = departure triggers; `+0x84` = arrival positions** — two different tables, which is why entrance indices never lined up with door positions. This line cost a session: it reads as "a getter exists that maps door → destination", and **no such getter exists anywhere in the 33,105 functions.** The destination is not in any table — see *Exit destinations = the map's own field script* below. |
| `getmapjumpmode` / `setmapjumpmode` | Map-jump mode |
| `setmapjumpgroup` / `setmapjumpgroupflag` | Exit group control |
| `mapload` / `mapdispose` | Map lifecycle |
| `mapjumpresult` / `mapjumppos` / `mapjumpstatus` | Per-jump state |

This surface gives exit **positions** and jump state. It does **not** give destination names — see below.

## Exit mechanism — CORRECTED Session 57 (read this before §55 and §46 below; both are partly STRUCK)

**`+0x54` is the party ARRIVAL table, NOT the exit/trigger set.** Explore-agent decompile trace,
0.9–0.95:
- `+0x54[K]` = x/y/z/angle only, **no destination field**. `FUN_00264b90` reads it (or the parallel
  table `+0x84`) by index; `getmapjumppos(nowjumpindex)` → party-spawn `FUN_00259b30` places the party
  at `+0x54[nowjumpindex]` **on arrival**.
- `mapjump(destMap, jumpIndex, flags)`: `jumpIndex` = **arrival slot on the DESTINATION map**
  (`FUN_003145e0` stores it as `nowjumpindex`, map-state `+0x1048`; the next map reads it). NOT the
  source slot.
- **No engine loop tests `+0x54` against the player.** The "stepped on a transition" test is a **script
  zone check** -- but **the `0x202d` id is STRUCK (S63): it is outside the mapctrl native table, and
  no `__MJ_CTRL` routine contains any touch test. See the native-table section below.**
- **⇒ There is NO `+0x54`-slot → destination binding in the blob.** The destination is ONLY the
  `mapjump` literal inside each `__MJ_CTRL` routine; the trigger position is NOT in that routine (its
  zone test, not `+0x54`.

**STRUCK by this:** the §46 "**`__MJ_CTRL<N>` owns `+0x54` slot `N+1`**" DOOR RULE (paired two
independent tables; only held on Nalbina's 2–3-loader maps — on East End it puts "Southern Plaza" on the
Bazaar loader), and the §55 "exit = `+0x54` ∪ `+0x70`" union (pulled shop-arrival slots in → Exit=13).
Routing to `+0x54` (arrival, set back from the edge) also causes the tester's "exit lands a few steps
short", Nalbina included.

## Exit mechanism — SETTLED Session 58: the `+0x84` EDGE-PAIRING is the door binding

The S57 capture (`'` on East End 291 + Muthru Bazaar 290) was decoded. `FUN_00264b90` reads `+0x54`
(hdr word 0x2a) OR `+0x84` (word 0x42) by a flag, and the two tables relate like this:

> **`+0x84` = the `+0x54` arrival table with ONE extra "edge" record inserted per `__MJ_CTRL`
> controller, in controller order. Controller `i` owns the `+0x84` record immediately AFTER the i-th
> edge.**

- An **edge** is a `+0x84` record present in NO `+0x54` record (exact float match on x/y/z/angle — the
  arrivals are byte-identical copies). It is the transition TRIGGER's reference point: **off the
  walkable mesh, out past the map boundary** (East End: `z=−78`, `x=−39`, `x=160`, `z=255`).
- The record after it is that doorway's **arrival**: walkable, a couple of steps inside the map.
- `#edges == #controllers` on both measured maps. Implemented as `ResolveControllerArrivals()` in
  `map_script.cpp`, with a **consistency gate**: any shape mismatch falls the whole map back to
  `+0x54[N+1]` and logs it, so an unexpected map degrades instead of emitting a wrong position.

**Verified — East End, edges at `+0x84[1,3,6,8,10,12]`:** ctrl0→289 North End (125,21) north ✓;
ctrl3→290 Muthru **(33,56)** west ✓ (old rule: (107,56), mid-map); ctrl4→292 S.Plaza **(123,135)** south
✓ (old rule: (33,56), the west doorway — this *was* "Southern Plaza loads the Bazaar"). Muthru matches
too, and it reproduces walked ground truth: arriving from the Bazaar spawns you at `+0x54[2]=(26,58)`,
and the Bazaar exit resolves to `+0x84[4]=(26,58)` — the same point.

**Why the N+1 rule ever worked:** mapping a `+0x84` index back to `+0x54` subtracts the edges before it,
so the two rules AGREE on any map where every arrival has a controller — i.e. Nalbina's 2–3-loader maps.
They diverge the moment a map has a controller-less arrival (a shop spawn point). East End has several.

**The edge coordinate is NOT a route target** — off-mesh, and up to ~120 units past the arrival;
adjacency- and nearest-distance controller→edge pairing both fail on at least one real exit. **The route
target is the ARRIVAL**, full stop (Session 59; see below).

### The edge's bearing — PROPOSED S59, **REFUTED IN PLAY S60**

The edge record sits off the walkable mesh **exactly along the axis the player crosses**, on every
transition of both test maps:

| map | ctrl | arrival → edge | crossing |
|---|---|---|---|
| Muthru 290 | 000 | (36, 24.5) → (36.1, −74.2) | north |
| Muthru 290 | 001 | (48, 64) → (195.8, 64.2) | east |
| Muthru 290 | 002 | (48, 56) → (198.1, 56.0) | east |
| Muthru 290 | 003 | (42.9, 144.9) → (43.0, 258.6) | south |
| East End 291 | — | — | north / west / south |

It looked definitive, and it is **wrong**. Shipped S59 as *"At the exit. Walk east."*; on Muthru the
tester got that five presses running with a stable camera (`cross=90deg`) and **their x never passed
48.41 all session**. The relative-frame math checks out and agrees with the route legs — the direction
itself is not the crossing. `crossRad` is still computed and LOGGED (flagged UNVERIFIED) but **no longer
spoken**. Whatever the `+0x84` edge record is, it is not the trigger's heading; its ORDINAL position in
the table remains the correct doorway pairing, which is a separate claim and still holds.

**FIVE blob-derived trigger models are now refuted on the ground** (`+0x54[N+1]` S46; the `+0x54`∪`+0x70`
union S55; nearest walkmap boundary S58 pre-ship; trigger-bearing march S58; edge bearing S59). The
cause never changes: **the trigger is a script zone (id STRUCK S63) whose geometry is in no blob
table.** `NavTrace` (S60) records where transitions ACTUALLY fire — `TRANSITION FIRED: mapId A -> B` plus
the walked trail and its bounding box. Build the next model from that, and make it reproduce the
recorded crossings before shipping it.

**STRUCK — locating the seam from the walkmap, twice.** `SeamTarget()` (a march along this bearing to
where the floor ends) shipped in S58 and was refuted in play: on Muthru it found a real boundary **0/2**.
An earlier 16-heading geometric probe was discarded before shipping. Both fail for the same structural
reason — **the transition is a script touch ZONE (`istouchuc`; the `0x202d` id is STRUCK S63)**, and
an unwalkable sample is a building wall and a map edge alike. Details in `debug.md`; do not try a third.

**Arrival Y is not necessarily the floor.** Muthru's East End arrival carries `y = 0.0` while the player
walks that ground at `y = −9.0`. `exit_scan.cpp` projects the arrival onto `GroundAt(x,z)` and keeps the
blob Y only when there is no sample.

**Struck by this section:** the zone test is not needed for POSITIONS (it remains the engine's
own "player stepped on it" test, and it is why the walkmap cannot see the seam); and the S57 note above
calling `+0x54` vs `+0x84` "unresolved" — it is resolved, as stated here.

## MAP TRANSITIONS — SOLVED, Session 64: the walkmap tags its own map-jump surfaces

> **A walkmap floor poly whose flags carry a non-zero value above the 3 type bits is a MAP-JUMP
> SURFACE. `group = (flags >> 3) & 0x1F` is the map-jump GROUP id. The `__MJ_CTRL` routine that calls
> `setmapjumpgroup(K)` with `K == group` owns that surface, and that routine's own
> `mapjump(dest, entrance, 0)` literal is the destination.**

One routine supplies BOTH halves of a transition — the geometry you walk onto and where it goes — so
they can no longer be mismatched. Local, first frame, no cross-map data, no cache, nothing learned by
playing. The script sets the tag with **`setmapidmj`**, sibling of `setmapidfloor` / `setmapidwall`
which own other bit fields further up the same word.

**Evidence (both maps, against walked NavTrace crossings):**

| map | ctrl | `setmapjumpgroup` | dest | poly group | where | check |
|---|---|---|---|---|---|---|
| Muthru 290 | 000 | 1 | 289 North End | 1 | (30,**1**) north | ✓ |
| Muthru 290 | 001 | 2 | 294 NOT USED | **none** | — | ✓ nothing to walk |
| Muthru 290 | 002 | 3 | **291 East End** | 3 | (50,**61**),(52,60) | ✓✓ crossings (55.1,65.4) / (56.4,64.0) |
| Muthru 290 | 003 | 4 | 293 NOT USED | **none** | — | ✓ |
| East End 291 | 000 | 1 | 289 North End | 1 | (123,**6**) north | ✓ |
| East End 291 | 003 | 4 | **290 Muthru** | 4 | (**11**,54) far west | ✓✓ crossing (19.7,58) |
| East End 291 | 004 | 5 | 292 S. Plaza | 5 | (91,**126**) south | ✓ |

Observed low-byte values `08/10/18/20/28/30` = groups 1–6 at bit 3. Readers additionally reject any id
no controller on the map claims, so the exact field width cannot matter. Implemented as
`MapQuery::ReadMapJumpSurfaces` + `MapScript::ExitDest::group`, consumed by `exit_scan.cpp`.

### TRANSITIONS vs DOORS — two systems, never mix them

| | **TRANSITION** (district ↔ district) | **DOOR** (shop, Stair to Lowtown) |
|---|---|---|
| lives in | the **walkmap** — a floor poly tagged by `setmapidmj` | the **scene-object table** — `kind == 4` |
| fires when | you **walk onto it** | you **press Enter** on it |
| named by | the owning `__MJ_CTRL`'s `mapjump` literal | a `+0x70` field sign (`setfieldsignlocationjumpinfo`) |
| read by | `exit_scan.cpp` | `entity_scan.cpp` |

**Every refuted exit model looked for transitions in door-shaped places** (`+0x54[N+1]`, the
`+0x54` ∪ `+0x70` union, field-sign pairing, the `+0x84` edge bearing, blob table order, the boundary
and passage marches). Transitions are not in the map-control blob's position tables at all. Keep the
two readers separate and never use one as evidence about the other.

**SUPERSEDED (correct in isolation, no longer the mechanism):** the S61 passage march (walkability is
not how you find a trigger) and the S62 arrival relation (true, but needs a neighbour's data — the
group tag is local). The `+0x54` arrival table is still the party SPAWN point and nothing else.

**Also settled:** map **305 (Eastgate)** appears only in the Director's `mapjump(…, 0, 0x0A)` list —
`flags == 0x0A` is the world-map **teleport menu**, not a walk-through door. There was never a door to
find. The Director routines are that teleport list and nothing more.

## Script natives: the `mapctrl` id -> name table — Session 63

The map's bytecode calls natives as `5d <u16 id>` (CALLACTPOPA). Those ids resolve to NAMES via the
archived `.dbg` symbol table for the `mapctrl` script module,
`..\FFXII-Decompile\notes\dbg_symbols_mapctrl.csv`:

> **`dbgIndex = nativeId + 5140`** — ANCHORED on `mapjump` (native `0x8D` -> index 5281), then
> cross-validated **15/15** against a complete `__MJ_CTRL` routine, which decodes to:
>
> `reqenable(12) · setmapjumpgroup(K) · clearmapjumpstatus · sysucon · spotsoundtrans(40,0) ·`
> `fadelayer(6) · fadeprior(255) · fadeout(2,12) · setmapidmj(1,1) · ucmove x4 · wait(12) ·`
> `stopspotsound · pausesestop · fadesync · wait(2) · mapjump(dest,ent,0)`
>
> A coherent fade-out-and-jump. **Valid in this band only** — the standing "`dbg_idx - 5140` is
> BROKEN" note concerns index math across the whole symbol file, where variables and source markers
> interleave. Re-anchor before using it elsewhere.

**CORRECTIONS this forces:**

- **`0x011E` is `setmapjumpgroup(K)`**, not "an authoring-order id with no new binding information"
  (Session 58). K is `ctrlIndex + 1` in value, but it is the controller's identity in the engine's
  map-jump GROUP system, and it is the live thread for the destination binding.
- **STRUCK: "the transition trigger is VM native `0x202d`" (Session 57).** `mapctrl` native ids run
  ~`0x0000`-`0x06BA`; `0x202d` (8237) is outside the table entirely and indexes past the end of the
  symbol file. It was quoted as fact in three documents and is unsupported. **No `__MJ_CTRL` routine
  contains any zone/touch test at all** — all 15 of its natives decode, and none reads player position.
- The real trigger natives are named: **`istouchuc`** (`0x026D`/`0x0529`), **`istouchucsync`**
  (`0x0525`/`0x052A`), `settouchwh` (`0x0026`), `touchradius` (`0x0051`),
  `setnochecktouchheightflag` (`0x020F`). The trigger is a **touch volume**, tested somewhere else —
  the map's **Director** routines (`<mapcode>MapJumpDirector`, `Map_Director`), never yet dumped.
- **`setmapidmj` / `setmapidmjground` / `resetmapidmj` sit beside `setmapidfloor` / `setmapidwall`.**
  That says the WALKMAP's own polygons carry ids and one class of them is the MAP-JUMP surface. The
  poly stride is `0x20`; `ReadCellFloor` reads bytes `0x00-0x11` and uses only the LOW 3 BITS of the
  flags word at `+0x0C`. The upper 29 bits and the `0x12-0x1F` tail are unread — the leading candidate
  for a first-frame, local, no-cache trigger source.

## Destination binding — SETTLED Session 62: the ARRIVAL RELATION, and nothing local

> **Map M's doorway at `+0x54` slot S leads to map D  <=>  D's script contains `mapjump(M, S, 0)`.**

`mapjump(dest, entrance)` means "you will arrive at DEST's slot `entrance`", and the slot you arrive on
is the doorway you would walk back out of. **A map's `mapjump` literals therefore name its NEIGHBOURS'
doors, never its own.** This is S46's arrival relation; S60/S61 NavTrace confirmed both directions of
one pair by walking them:

| evidence | reading |
|---|---|
| Muthru `mapjump(291, 2)` | East End slot 2 -> Muthru. Tester spawned on East End `+0x54[2]`=(26,58), walked west, arrived Muthru |
| East End `mapjump(290, 2)` | Muthru slot 2 -> East End. Tester spawned in Muthru `+0x54[2]`=(48,-9,64), walked east, arrived East End |

Implemented as `ExitLinks` (`exit_links.h`), persisted to
`%LOCALAPPDATA%\FFXII-Screen-Reader\map_links.txt` behind a `version` header. `ExitDest::arrivalSlot`
carries the `+0x54` index (recovered by exact match against `+0x84`).

**STRUCK, do not reintroduce even as a fallback** -- every local binding rule, all refuted:
- **blob table order** (the `+0x84` pair order vs controller order): labelled Muthru's DEAD slot
  `+0x54[3]` "Rabanastre: East End" and the REAL corridor `+0x54[2]` "NOT USED".
- **the routine's `0x011E` argument**: it is `ctrlIndex + 1`, no new information.
- **`entrance` read as a LOCAL slot**: three East End controllers would all claim slot 2. It matched on
  Muthru only because that door pair happens to be co-indexed (slot 2 on both sides).

An unestablished destination leaves the exit UNNAMED. Position and crossing direction are measured and
still spoken; a wrong area name is what walked the tester into a wall repeatedly.

## (STRUCK, see Session 57 above) Exit source is the UNION of two tables — Session 55–56

The `__MJ_CTRL` section below is still the destination-NAME mechanism, but it is **not** the exit list on
its own. Measured on Rabanastre East End (291):

- **`+0x54` slot 7** has a `+0x70` field sign and **no `__MJ_CTRL`** — a real transition the controller
  reader alone cannot see. `+0x54` slot 3 has the reverse. **Exit = a slot claimed by a controller OR by a
  field sign.** (`exit_scan.cpp`, `EntityScan::ScanExits`.)
- **`+0x70` is NOT empty** — 25 records on East End (13 in group 0, 12 in group 3). The old "empty on every
  map" verdict was a calling-convention bug. `MapExits::EnumerateFieldSignRaw` returns every record as a
  `SignRec {pos, group, index, destIdx, areaId, usable, shown}`.
- **Arrival-point exclusions** (else the union over-lists shop *arrival* points): a field sign justifies its
  jump slot as an exit only when (a) no controller door already claims that sign, and (b) **no scene object
  sits on the sign** within `kSignObjectDist` (2.5 m). An interior doorway sign has the press-Enter object on
  it; a district sign does not. East End: 7 exits (slots 1-7), not 14.
- **`__MJ_CTRL<N> → slot N+1` — RESOLVED AND STRUCK (Session 58).** It was indeed wrong (Muthru Bazaar,
  west on the atlas, placed mid-map). The real binding is the `+0x84` edge-pairing — see "Exit mechanism —
  SETTLED Session 58" above. Neither candidate pairing logged by `exit_diag.cpp` (slot N+1 vs field-sign
  order) was correct; the answer came from the `'` capture's raw `+0x84` dump.

### Field signs = the `fieldsign*` script natives (mapctrl symbols)
`nameIdx = -1` on a sign means its name is the custom string at `sceneObj+0xf8`, written by `fieldsignmes` /
`fieldsignmesbyid` (mapctrl `0xa334`/`0xa338`). `setfieldsignlocationjumpinfo` (`0xae98`) binds a sign to a
map jump — which is how the doorway (has a `+0x70` jump record) is told from a same-named plain sign (has
none). The sign-twin dedup in `entity_scan.cpp` drops an untagged sign whose label exactly equals a tagged
doorway's. `FLAG_SHOW_NAME = 0x2000` (`decompile_all.txt:261682`) gates the on-screen name draw — diagnostic
only, must not enter a classifier.

### planmapname PLACEHOLDER slots
Some table-A slots hold the developers' filler string `"NOT USED"` (ids 293, 294 in the Rabanastre block);
their MapRef record resolves to an unrelated region, which is where `Exit, Pharos at Ridorana: NOT USED`
came from. `MapNames::HasRealAreaName` rejects the exact token `kPlaceholderNameUS = L"NOT USED"` (US build;
the ONE locale-specific point — `map_names.cpp` logs every shared-name group so the token can be read off the
log for another locale). **Detection by name multiplicity was STRUCK** (see debug.md #5). Affected exits keep
their position and stay routable; only the destination clause drops, so they speak as `Exit 1` / `Exit 2`.

### MapRef record (`DAT_02099d88`, 8 bytes/id) — for the structural placeholder test, next build
`record = DAT_02099d88 + *(s32)(DAT_02099d88+4) + mapId*8`. Getters: `FUN_00264f10` u16@+0, `FUN_00264ed0`
u16@+4, `FUN_00264f40`/`FUN_00264fd0` = shorts of the 0x10-stride sub-table at `DAT_02099d88+8` indexed by
record+2, `FUN_00264f90` = region index (record+6). The `+0x8c` dest table: `mapData + *(u32)(mapData+0x8c)`,
record = `+4 + destIdx*0x10`, u16[8]; `FUN_00264920` returns word[5] (0xffff on every East End record), words
2/3/4 are story-progress variants (`notes/exit_dest_offline_findings.md`). `exit_diag.cpp` dumps both.

## Direction reference is CAMERA-RELATIVE and the game owns the camera — Session 56

Spoken directions use `PlayerState::ReadCameraForward`, which reads **row 2 of the movement matrix
`DAT_02aedf30`** and returns `atan2(-fx, -fz)` = "the way an UP push sends you" (`worldMove =
stickX*row0 − stickY*row2`, stick rotator `FUN_004742a0` RVA `0x3542A0`). That matrix has only two refs in
the binary: the per-frame block-copy from the active camera (`FUN_00202c70(&DAT_02aedf30, &DAT_02aed5c0)`,
`decompile_all.txt:509454`) and the rotator. **The game rewrites it whenever the camera moves**, so a route
leg can flip 180° mid-walk (measured: player stepped onto a terrace, camera swung, five legs inverted). This
is a HARD limitation, not a bug: movement is camera-relative, so no frame the stick cannot act in is usable,
and locking the matrix breaks battle lock-on (the rotator carries cross-frame targeting state, `param_1[2]` +
22.5° threshold). Accepted + documented in README; `ReadCameraForwardStable(outRad, srcOut)` logs
`ref=/src=/dref=` on every route + announce so a real camera move is distinguishable from a mod bug.
`RelativeWord` returns `kCardinal` (compass words on the relative frame) — the `kEgocentric` vocabulary
exists but is unshipped, correcting the S54 note.

## Exit destinations = the map's own FIELD SCRIPT (`__MJ_CTRL<N>`) — Session 46, SHIPPED

**The problem this solves:** an exit's destination is stored on *nothing* the mod can index. The `+0x54`
record is x/y/z/angle followed by zero bytes; the `+0x70` field-sign records carry positions with an empty
destination slot on interior maps; the `+0x8c` dest table is keyed by a field-sign byte, not a jump index;
and **no engine getter returns "destination for door N"** (every `mapData` accessor was traced). The
destination lives in the map's compiled script.

**The mechanism (one global system — prologue and non-prologue maps are identical):** the map toolchain
emits **one routine per map-jump door**, named **`__MJ_CTRL<NNN>`**, whose body calls the `mapjump` native
with its destination as a literal.

| Blob offset | Structure |
|---|---|
| `hdr+0x18` | **Routine table**: `[u32 count][count records of 0x30]`; record `{+0x00 nameOff, +0x08 codeOff}` |
| `hdr+0x4c` | **Name pool**: NUL-terminated names. A routine's name is **`pool + nameOff`** |
| `hdr+0x54` | Map-jump door table: `[u32 count][records of 0x20]`, four floats x/y/z/angle at record+0 |

- A routine's code **span** is `codeOff` → next-highest `codeOff` (the record's other fields are label /
  variable sub-tables, **not** a byte length — two controllers on one map had byte-identical sub-tables
  because they are the same compiled template differing only in the destination literal).
- `mapjump(dest, entrance, flags)` compiles to `4f <destU16> 4f <entU16> 4f <flagsU16> 5d 8d 00`
  (`0x4F` push-u16, `0x5D` CALLACTPOPA, native `0x8D` = `mapjump`). **`flags==0` = field door**;
  `flags==0x0A` = the world-map teleport menu (a long run of these sits in every map — exclude them).

**THE DOOR RULE — `__MJ_CTRL<N>` owns `+0x54` slot `N + 1`.** Slot 0 is the default/cutscene arrival and is
never an exit; a slot with no controller is an arrival point, not a door. Walk-tested across five maps
(274/275/279/280/282, routine tables of 22/25/31/24/37) and independently cross-checked by the arrival
relation — *if M jumps to D with entrance E, D's door back to M is D's slot E* — which agrees on every
measured pair with no exceptions.

**Match doors to controllers BY POSITION, not slot number.** The `+0x54` table repeats records (one map's
slot 1 is byte-identical to slot 0) and `EnumerateMapJumps` de-duplicates them, so a surviving entry's index
can differ from the owning slot while naming the same doorway. Position matching also fails safe: a mismatch
drops the exit rather than mislabelling it.

Implemented in `src/navigation/map_script.{h,cpp}` (`MapScript::ReadExitDests`), consumed by
`ScanExitsLocked` in `entity_list.cpp`. Nothing map-specific is baked in — only blob/VM format constants and
the `__MJ_CTRL` prefix — so it resolves on the first frame of any map with no cross-map data, no cache and
nothing learned by playing.

**STRUCK by this work:** the routine table does **not** have "2 entries" (word 0 is a **count**; reading it
as a record truncated a 24-routine table to 2). `nowjumpindex` (native `0x8f`) has **zero** call sites, and
`lastjumpindex` (`0x90`) returns a **map id** (compared against the teleport-list ids), not a door index —
so there is no jump-index dispatch. Operand of `0x5C` is **not** a routine index (values exceed the routine
count); it is a label/target, so "routine X calls routine Y" cannot be read off `5c <n>`.

### CORRECTION (Session 54) — "resolves on any map" had an UNSTATED 96 KB precondition

The reader above was true only for maps whose routine table and name pool happen to sit inside the first
**`0x18000` (96 KB)** of the blob. `MapScript::SnapshotBlob` copied that fixed prefix and then bounds-checked
the header offsets **against the snapshot**, so on a larger map `ReadExitDests` returned `false` — *silently,
before its first log line*. Every door then fell through as `-> no controller (arrival point)` and the whole
**Exit category was empty on every Rabanastre map**.

Evidence (mod log + `x64\logs\` archives): the `==== field-script exits: … ====` header is **absent** for
East End (291), Muthru Bazaar (290) and The Sandsea (304) while `EnumerateMapJumps` reported 14 doors on the
same maps — so page 0 was readable and only a bounds check could have failed. Every successful header logged
`blob=0x18000` (the window always truncated), and **Migelo's Sundries — a small shop interior — already had
`routineTable=+0xE350`**. Observed offsets: Overflow Cloaca `+0x24F0` (18 routines, 1 controller, worked),
Migelo's Sundries `+0xE350` (37 routines, 0 controllers).

**Fixed by removing the window entirely.** `map_script.cpp` now reads the header, the routine table and each
`__MJ_CTRL` routine's code span **directly from the live blob** through SEH-guarded `MemRead` calls, with a
sanity ceiling per offset (`OFFSET_MAX = 0x400000`) instead of a read window. Side benefit: the per-rescan
cost drops from a 96 KB copy to ~2 KB, and `ReadExitDests` no longer bails silently — every failure path logs
`field-script exits: BAILED (<reason>) mapId=… routineTable=… namePool=… count=…`, and a clean parse that
finds **0 controllers** dumps the routine names so "does this map use `__MJ_CTRL`?" is answerable from the log.

**Not answerable offline:** `__MJ_CTRL` appears as plaintext in **none** of the 20 extracted `.mpk` map
archives (`extracted/ps2data/plan_master/map_ctrl/`), *including the Nalbina ones where it is proven present
at runtime* — the name pool is packed on disk, so only the loaded blob can answer it.

## Field-object interaction — the game's OWN classifier (Session 54, offline)

`FUN_002675c0` (abs `0x2675c0`, **RVA `0x1475C0`**) is the engine's "can the player interact with this object
right now" predicate. It is the authority for two things the mod had been guessing at: what an object *is*,
and whether it is *story-gated*.

```c
if ((obj+0x0E & 0x10) == 0)                       -> false   // interaction ENABLED bit (the story gate)
if (!(obj+0x14 & 0x20) || (obj+3 & 0xE0) != 0x60) -> false   // model loaded, class 3
if (!FUN_002e9fe0(obj))                           -> false   // node visible/ready
if (obj+0x1C & 0x004) { id = obj+0xCC; return id valid && (obj+0x0E & 0xF) == 5; }   // ACTION
if (obj+0x1C & 0x400) { id = obj+0xDC; return id valid && (obj+0x0E & 0xF) == 1; }   // TALK
```

| Field | Meaning | Conf |
|---|---|---|
| `sceneObj+0x0E & 0x0F` | **object KIND** — `5` = ACTION gimmick (gate/door/switch/lever/well). See the PARTIAL REFUTATION below before using it. | 0.9 for `5`; **`1` = "a person" is REFUTED** |
| `sceneObj+0x0E & 0x10` | **interaction ENABLED** — the story gate | **0.98** |
| `sceneObj+0xCC` (u16) | action payload id (`0xFFFF` = inherit from the map's object record) | 0.98 |
| `sceneObj+0xDC` (u16) | talk payload id (`0xFFFF` = inherit) | 0.98 |
| `sceneObj+0x14 & 0x20` | model loaded | 0.98 |
| `(sceneObj+0x03 & 0xE0) == 0x60` | class 3 = an interactable object | 0.98 |

Corroborated independently by **`FUN_0025bad0`**, the near-object scanner's candidate filter (reached from
`FUN_0025b820` with `10` = talk / `2` = action): kind `1` → talk only, `4` → both, `5` → action only, `7` →
talk only, anything else rejected. It also writes the game's own "nearest interactable" globals —
`DAT_0209a2b8` = object handle, `DAT_0209a2bc` = `10`/`2` (talk/action), `DAT_0209a2b0` = score.

**The story gate is script-driven.** `FUN_0026ba60(obj, enable)` (**RVA `0x14BA60`**) is a dedicated setter
for bit `0x10`; its only caller `FUN_0034afe0` (**RVA `0x22AFE0`**) has **zero in-binary callers**, i.e. it is
a script-VM native. The map's own script opens and closes interactivity per object.

**`sceneObj+0x1C` is MODE STATE, not identity.** `FUN_0025ad10` / `FUN_0025ae00` set `0x400` and clear `0x004`
when an object enters talk mode, and clear `0x400` when its talk id is invalid. So the TALK/ACTION bits change
during play and go to **zero on a disabled object** — which is why a story-gated town gate disappeared from
the mod's list entirely. Never classify from these bits; classify from KIND.

**STRUCK:** `ClassifyByNameKey`'s `if (flags & FLAG_TALK) return NPC` **as the first test** — "talk target ⇒
person" is not the engine's rule. A gate with a confirm prompt is a talk target too, so every one of them was
filed under NPC and never reached Interactables. (The test survives as a *fallback* after the character and
kind tests, where it only classifies what they do not claim.)

### PARTIAL REFUTATION — the KIND nibble is NOT a person-vs-object oracle (same session, caught in play)

The tidy reading above — "kind 1 = person, kind 5 = gimmick" — was taken from `FUN_002675c0`'s two branches
and `FUN_0025bad0`'s filter, and it is **wrong as a classifier**. A build that tested `kind == 5` *ahead of*
the scene-character class shipped, and **every NPC was reclassified as Interactables**. That is an empirical
refutation: **field NPCs evidently do not carry kind 1**, so any kind test placed ahead of the character test
swallows them.

What survives, and what does not:

- **SURVIVES (~0.9):** `kind == 5` identifies the ACTION gimmick *among non-characters*. `FUN_002675c0`'s
  ACTION branch literally returns `(obj+0x0E & 0xF) == 5`, and that is what distinguishes a gate/switch from
  a sign. Use it only after `isCharacter` has taken the people out.
- **REFUTED:** "kind 1 ⇒ a person". Do not use the kind nibble to decide personhood at all.
- **UNCHANGED and still the reliable person test:** the scene CHARACTER class, `sceneObj+0x03 & 0x1f` in
  **5-7** (the classes carrying a char component). This was already correct before the session and is now the
  dominant test in `ClassifyByNameKey`.
- **Still open:** what field NPCs' kind actually IS. `FUN_002675c0` also demands `(obj+3 & 0xE0) == 0x60`
  (class 3), which the observed non-character gimmick `cat=0x21` fails — so that predicate may be narrower
  than "can the player interact", and its role is less certain than first written. The `'` dump now logs
  `kind=` and `en=` per object precisely so this is answered from data instead of inference.

**Category ordering that ships (do not reorder without the `kind=` data):** npcdic gimmick band (Treasure /
Gate Crystal / Save Crystal keep their own categories) → `isCharacter` ⇒ NPC → non-character `kind == 5` ⇒
Interactables → `FLAG_TALK` ⇒ NPC → Interactables.

## World MAP screen (`page+0x138` = map id) — RE'd Session 45, KEEP for map-transition speech

Found while disproving the "e5f0 dialogue" read-points (they were this screen all along). **Not
currently hooked** — the mod removed both hooks in Session 45 because they were wired up as
*dialogue*. The RE itself is sound and is exactly what a **"speak the map/area on map load or map
open"** feature needs, so it is recorded here rather than lost.

| What | Where | Conf |
|---|---|---|
| Map screen page proc | `FUN_002baf80(page, msg)` — abs `0x2baf80`, **RVA `0x19AF80`** | 0.99 |
| **Current MAP ID** | **`*(s16)(page + 0x138)`** | **0.99** |
| ...set at | proc **case 1** (birth): `*(short*)(page+0x27*8) = (short)msg[2]`; `< 0` is clamped to 0 | 0.99 |
| ...consumed at | proc **case 0xE** (build) + case `0x2A`: `FUN_003c02b0((int)*(short*)(page+0x138), &out)` — passed **unmodified**, so page id and resolver id are the **same id space** | 0.99 |
| Map-name resolver | `FUN_003c02b0(mapId, out)` — abs `0x3c02b0`, **RVA `0x2A02B0`** | 0.99 |
| Map DB | `*(u64*)(DAT_02b457e0 + 0xe8)`; table base = `db + *(u16)(db+8)`; `[u32 count][8-byte entries]`; entry `+0` = attr byte, entry `+4` = **s32 text id (`-1` = none)** → `FUN_002f9920` | 0.98 |
| Map width/height | `FUN_003be680(id, &w, &h)` (callers do `0xc80 / w` → `FUN_00402f00` zoom-to-fit) | 0.98 |
| Page creator | `FUN_002b9d30(id, mode)` → root `DAT_0209e5f0` (size `0x179e0`, proc `FUN_002ba700`); page size `0xbc08` at `root + 0x1B0 + bit*0xBC10` | 0.97 |
| Openers | `FUN_0028e630(x)` → `(0, 6)`; `FUN_0028e660(x)` → `(x, 5)`; `FUN_0028e690(p)` → `(p[1], 5)` else the **movie-subtitle** path `FUN_00550510` | 0.97 |
| Sibling proc | `FUN_002b7b80` on `DAT_0209e5c0` — near line-for-line clone that reads the **player's world position** (`FUN_00265020`) + `TrackingMapScreenRateX/Y` config globals = the position-tracking map | 0.94 |

**`out` layout (0x18 bytes)** — confirmed against the caller's own stack locals in `FUN_002baf80`
(`local_1f8`/`local_1f0`/`uStack_1e8` at +0/+8/+0x10, tested with `& 0x8000`):

| Off | Type | Meaning |
|---|---|---|
| +0x00 | u16 | id (echo of arg0) |
| +0x02 | u16 | group/attr byte (→ `FUN_003bf6c0`, capped ≤ `0x3a`) |
| **+0x08** | **ptr** | **codec text ptr** (the map NAME) — NUL-terminated, decode with `GameText::Decode`. **NULL when entry+4 == -1**, which is legitimate |
| +0x10 | u32 | flags from `FUN_003c05f0`; **bit15 = "no text"** |

Other page fields: `+0xd4` zoom %, `+0xBBD8`/`+0xBBDA` scroll x/y, `+0xBBE0` mode (5/6),
`+0xBBE4`/`+0xBBE5` cursor; case `0x27` = scroll-to-keep-cursor-onscreen; `FUN_00402db0(marker,
padDir, …)` = d-pad marker navigation.

**Caveat before building on this (0.93, not 0.98):** that `out+8` is a *map/area name* is inferred
from the surrounding cluster (map DB, zoom-to-fit, `./GameData/D3D11/ArtData/menu/localmap/` at RVA
`0x7D74E0`, the position-tracking sibling) — the string itself was never read at runtime. **Also
unproven: whether this id shares the `planmapname` id space** (`FUN_00377870`, `DAT_02add0f8`,
`notes/planmapname_areas.csv` — 808 ids incl. 275 Inner Ward / 1327 Nalbina Fortress). The map DB
here (`DAT_02b457e0+0xe8`) is a **different** blob from planmapname, so do not assume they match —
check before cross-using ids.

**For map-transition speech specifically**, the mod already has a simpler, proven route that needs
none of the above: `FUN_003778b0()` = current-area name (`CURRENT_AREA_NAME = 0x2578B0`, already
called by `EntityList::CurrentAreaName`), plus the map-jump executor **`FUN_003145e0`** (RVA
`0x1f45e0`) which writes `nowMapNo`/`nowJumpIndex` to gameState `+0x1044`/`+0x1048` (base
`FUN_002ef2b0()`, RVA `0x1cf2b0`) — getters `FUN_003148f0` (`0x1f48f0`) nowMapNo, `FUN_003148b0`
(`0x1f48b0`) nowjumpindex, `FUN_00314870` lastMapNo, `FUN_00314850` lastjumpindex. An observe-only
hook on `FUN_003145e0` is the natural "a map transition just happened, announce it" event; the
`page+0x138` path above is for reading the **map screen itself** when the player opens it (`M`).

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

> ### ⇒ SUPERSEDED 2026-07-20 (Session 48 research). See **`Docs/combat_system.md`**.
>
> The entire combat system — messaging, committed-vs-browsed target, damage/heal/MP/status pipeline,
> faction + Neutral, battle lifecycle, ATB, chain, rewards, pause — was RE'd offline in one pass and is
> documented in `combat_system.md` with per-claim confidences and a probe gate. Nothing is implemented
> yet. Read that file before touching anything below.
>
> **Headline results:** damage applier chokepoint `FUN_003112f0` (RVA `0x1F12F0`); flying-number spawn
> `FUN_003283d0` (RVA `0x2083D0`, args `(work, signedDelta, isPositive, isMP)` = all four damage
> categories); HP/MP writers `FUN_00300530`/`FUN_00300ce0` (RVA `0x1E0530`/`0x1E0CE0`) are a **closed
> set**; status+KO `FUN_0030e360` (RVA `0x1EE360`).
>
> **STRIKE — the "Replacement strategy" below is obsolete.** "Frida-watch HP writes and walk the
> callers" is unnecessary: the writer set is closed and small, and was recovered offline.
>
> **STRIKE — "no textual combat strings exist in the game"** (asserted in
> `memory/project_combat_log_design.md`). FFXII composes **full localized combat sentences** from
> `battle_message.bin` (table `DAT_02ebf018`, RVA `0x2D9F018`) and keeps its **own scrollable battle
> log** at `*(u64*)(P + 0x9FF8) + 0x4098` (`P` = `DAT_0209be80`). Finished string hookable at
> `FUN_0028e110` (RVA `0x16E110`). The string-grep failed because the text is codec-encoded master
> data, not ASCII in the binary — the observation "numeric IDs, not strings" below was right, the
> conclusion drawn from it was wrong. **The mod reads this text; it does not synthesize combat lines.**

**STRATEGY UPDATE (2026-05-05):** the static "find functions referencing
'damage'/'miss'/'critical' strings" approach in
`find_damage_candidates.java` returned only 3 hits. FFXII uses **numeric
IDs** for damage / status events, not strings. String xrefs are not
viable for finding the funnel.

~~**Replacement strategy:** Frida-watch HP writes via memory access monitor,
walk callers (per `probe_damage_event.js`). Seed pointer chain from
DrummerIX's CE table (the table's PermStatusBitsAOB resolves to the active
character's struct base). Once we have leader's HP write site, walk the
caller stack to find the damage funnel.~~ — **STRUCK, see the box above.**

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

### Camera-relative movement + egocentric direction reference (Session 37, 2026-07-13) — CONFIRMED (ground truth)
FFXII field movement is **camera-relative** (proven, ≥0.98, code + live calibration) — "up on the
stick" walks along CAMERA-forward, NOT world-north. Spoken directions are therefore EGOCENTRIC
("North" = forward = where UP takes you) anchored to the live camera-forward.
- **Locomotion driver `FUN_00358cb0` (RVA `0x238CB0`):** reads the raw stick via `FUN_003594e0`
  (RVA `0x2394E0`; `stickX = rawX−128`, `stickY = 128−rawY` inverted, so UP ⇒ `stickY>0`), calls the
  camera-rotate `FUN_004742a0` (RVA `0x3542A0`), then writes facing = `atan2(moveX,moveZ)` via
  `FUN_0026a0d0` (RVA `0x14A0D0`, node+0xA4) **only while moving** (skipped when stopped).
- **Camera-rotate `FUN_004742a0`:** `worldMove = stickX·row0 − stickY·row2`, rows copied from the
  movement camera matrix `DAT_02aedf30` (indices `{0,2}` in `DAT_00d22690` = right, forward; Y zeroed
  + normalized). Output move vector `DAT_022c7fd0/4/8` (RVA `0x21A7FD0/4/8`). Since UP ⇒ `stickY>0`,
  `worldMove = −stickY·row2`, so **the direction UP takes you = `−row2`**.
- **EGOCENTRIC "FORWARD" REFERENCE (what the mod reads):** movement matrix `DAT_02aedf30` row 2
  (+0x20): `fwd.x = DAT_02aedf50` (RVA `0x29CDF50`), `fwd.z = DAT_02aedf58` (RVA `0x29CDF58`).
  **up-direction yaw = `atan2(−fwd.x, −fwd.z)`** (same `atan2(x,z)` convention as faceNode → drop-in).
  Writer `FUN_003820c0` (RVA `0x2620C0`, per-frame camera update). Read live by
  `PlayerState::ReadCameraForward` — valid idle, after a camera rotate, and in combat.
  **Calibration (position-delta ground truth):** W leg `camFwdNeg==moveVec==faceNode` (168.9°),
  `camFwdRaw` the 180° opposite ⇒ sign `(−,−)` correct; D leg right = forward−90 ⇒ ego `+90 = East`
  (right→East, no handedness flip). `nav_common`'s egocentric transform (`ego = BearingDeg(target) −
  (180 − facingDeg)`) was already correct; only the reference vector changed from faceNode.
- **DO NOT use as the forward reference:**
  - `faceNode` (node+0xA4, char facing) — written only while walking; in combat the battle action /
    target-steering subsystems (`FUN_00307300` case `0x1c7` / `FUN_0037b4d0` case `0x16` /
    `FUN_0031adb0`) turn it to face the TARGET. Kept for the `'` diagnostic only (`ReadPlayerFacing`).
  - Scalar `DAT_02aedf94` (RVA `0x29CDF94`) — `FUN_003820c0` builds it from the view/sibling matrix
    `DAT_02aede70` (rows sign-flipped `^0x80000000`), so its offset from the move heading is not
    constant. Diagnostic cross-check only. (This corrects the older nav_rva note that called it the
    camera-forward yaw.)

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
  nibble `(*(u8)(sceneObj+3)>>5)∈{1,3}`). (`comp=*(sceneObj+0x30)`, valid `*(u32)comp & 8`.)
  Controller `+0xD0` matrix was only a *writer*.
- **Yaw / facing — CORRECTED Session 35 (was frozen at ~129deg).** `comp+0x100` is NOT a forward row —
  it is an internal point-vector lane, so `atan2(comp+0x100, comp+0x108)` never tracked turning. The
  **live facing** is the steering heading: **`comp+0x15C`** (current, radians) — use this; alts
  `comp+0x160` (target/cached, = actor-pool `+0x160`) and `comp+0xAC` (byte brad, deg = ×1.40625). A
  world `atan2` of an (x,z) delta; the exact convention (0-axis/sign vs `BearingDeg` atan2(dx,−dz)) is
  being pinned live by the `'` diagnostic's facing candidates + a route calibration. Conf 0.85 pending.
  Used by the `;` readout and the facing-relative direction mode ("North" = forward).
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
  (RVA `0x2CAC10`): `id &= 0xffffbfff`; `slot = id*2 + flag`; if `slot < *(int)(base+8)` then codec ptr =
  `*(s32)(base + 0xc + slot*4)` (relocated low-mem pointer, sign-extend).
- **The odd slot is NOT a yomi (Session 54, offline).** `FUN_00263990` computes the slot as
  `id*2 + (FUN_0032a930(id) != 0)`, and `FUN_0032a930` (RVA `0x20A930`) reads a **per-id bitfield at
  `FUN_002ef2b0() + 0x13B4`** — a live game-state flag, not a phonetic reading. So the odd slot is a
  *state-selected second name*. **In the US build it is byte-identical to the even slot**: decoded
  straight out of `extracted/ps2data/image/ff12/us/bin/npcdic.bin` (2282 slots / 1141 ids), ids 0-11 and
  433-469 all give `even == odd`. The mod's even-slot-only read is therefore correct as shipped, and
  **there is no second name to mine** — do not re-attempt it. `tools/parse_npcdic.py`'s "odd slot is the
  yomi/reading" comment is corrected in place.
- **The duplicate-name problem is the game's own data.** Of 1141 ids / 554 distinct names, **109 ids all
  render "Rabanastran"** (85 "Archadian Gentry", 51 "Bhujerban", 42 "Imperial", …). No engine path gives
  a townsperson a finer name, so the mod numbers same-label entities instead of inventing descriptors
  (`EntityScan::NumberDuplicateLabels`).
- **HYPOTHESIS, UNVERIFIED (do not build on it):** npcdic **362 = "Imperial Guard"** *is* a distinct
  dictionary entry (separate from id 1 and the 41 other ids that render "Imperial"), but **nothing
  confirms any object at the Rabanastre town gates carries it.** It was found by grepping the
  extracted dictionary for gate/guard/soldier terms — a string existing in a table says nothing about
  which scene object stamps it at `+0x102`. To confirm: stand at a gate, `'`, and read the `nameIdx`
  on the objects near the logged player position. No code keys off 362; if the guard is a plain
  "Imperial" the mod says "Imperial" (or "Imperial 1/2/…" when several are listed).
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

### Walkability = SQEX field walkmap, NOT Bullet (Session 33, 2026-07-12) — SHIPPED, routing works

**Bullet is a dead end for field walkability.** The route grid was first built on the Bullet raycast
world (`FUN_006a1a70`, world `*(ctx+0x60)`); runtime-proven the **Nalbina prologue builds no Bullet
world** — `FUN_006a0310` is the sole Bullet-world constructor and runs only inside master physics tick
`FUN_00698c80`'s per-region loop, skipped when region count==0. Four ctx-capture hooks all install but
never fire. See debug.md Tried & Failed.

**The real oracle = the SQEX floor/wall mesh** (the collision the field + AI NPCs walk on; loaded with
every map, independent of Bullet, so live in the prologue). Shipped `src/navigation/map_query.{h,cpp}`:
- **Ground-at-(X,Z):** `FUN_003208c0` (RVA **0x2008C0**) `bool groundAt(float x, float z, float* outY)`
  — walkable-floor-exists + height; uses cached ctx `DAT_02ec1370` (no ctx arg). MS x64: x=XMM0, z=XMM1,
  outY=R8. Internally `FUN_0026e3c0`→`FUN_00231900`/`FUN_00231890` (topmost floor tri + plane height).
- **Segment/wall test:** `FUN_00230b60` (RVA **0x110B60**)
  `int seg(void* ctx0, float out16[4], const float from[4], const float to[4], u16 mask, u32 flags)`
  → hit index **>=0 BLOCKED / <0 CLEAR**; from/to = {x,y,z,1}. Traversal = 2D DDA grid walk
  `FUN_0022f430`, per-cell `FUN_0022cc50`. Reentrant, stack-local, no shared writable scratch → safe
  for thousands of calls/route.
- **ctx0** = `*DAT_0209a678` (RVA **0x1F7A678**), valid only when gate `DAT_0209a670` (RVA **0x1F7A670**)
  != 0 (`FUN_0026e500(0)` = the same). Memory-only read; no capture hook needed.
- **mask is a query-CLASS enum (compared `==4` in `FUN_0022cc50`), NOT a bitmask.** Pass **mask=4,
  flags=0 (WALK class)** — the exact query the PLAYER leader's own per-frame wall feelers use
  (`FUN_002593a0`→`FUN_00259990(1,0,…)` slot-0=leader→`FUN_003d9930`→…→`FUN_0032cf50`→
  `FUN_003d97e0(…,4)`→`FUN_00230b60(…,4,0)`; NPCs share the funnel byte-for-byte). Class 4 blocks real
  walls (edge type 0, type1/bit30=0) + character-only invisible walls (type 4), and **skips** camera-only
  occluder planes (type1/bit30=1), floors/ceilings (poly records), triggers/water (types 2,3,5,6,7).
  `0xffff/1` is the CAMERA/occlusion class — doubly wrong for routing (blocks camera planes → phantom
  detours; passes character-only walls). flags: 0 = nearest-blocker (movement), 1 = first-hit (occlusion).
  Material override tables `DAT_0209a3e0/…3e4` are benign (applied identically to every caller, zeroed at
  load). Confidence 0.98 (+runtime `'` self-test logs walk vs camera per cardinal).

**Planner now:** A* over a 1m grid, cell walkable iff `MapQuery::GroundAt`; edge passable iff
`|ΔfloorY|≤kMaxStep` AND `MapQuery::SegmentClear` (mask=4). Reconstructed polyline is string-pulled
(greedy line-of-sight via SegmentClear) and each leg decomposed into primary+secondary cardinals
("North 16, East 2"). Known open issues (range cap, world-vs-egocentric directions, LOS-through-walls):
see debug.md Known Issues (Session 33). **SUPERSEDED by the whole-map overlay below (Session 34).**

### Walkmap GRID structure — direct read = whole-map overlay (Session 34, 2026-07-12) — SHIPPED, confirm-pending

The SQEX walkmap `GroundAt`/`SegmentHit` above sit on a **uniform staggered ("brick") grid over a
floor-triangle + wall-segment mesh** — a per-map structure we can read DIRECTLY to bake a whole-map
walkability overlay with **no raycasts**, and to get exact map bounds for free. Model = the engine's own
full-grid enumerator **`FUN_0022ffe0`** (RVA **0x10FFE0**). World→cell **`FUN_00233050`** (0x113050),
plane height **`FUN_00231890`** (0x111890). Read memory-only, SEH-guarded (`map_query.cpp`).

**`ctx0 = *DAT_0209a678` (RVA 0x1F7A678) sub-fields:**
| off | field |
|---|---|
| `+0x00` | grid header ptr |
| `+0x08` | vertex array (stride 0x10: x@0, y@4, z@8) |
| `+0x10` | floor-poly array (stride 0x20) |
| `+0x18` | wall-segment array (stride 0x90) — phase 2 (fully zero-raycast A*) |
| `+0x20` | CSR cell→list offset table (u16, `nCols*nRows+1` entries) |
| `+0x28` | primitive index list (u16) |
| `+0x38` / `+0x3C` | int originX / originZ |

**Grid header (at `*ctx0`):** `+0x08` int nCols (X), `+0x0C` int nRows (Z), `+0x10` int cellSizeX,
`+0x14` int cellSizeZ (world units/cell, **integer meters**).
**Floor-poly entry (0x20):** planeA/B/C @ +0/4/8 (B divisor, guard |B|>0.001), flags @ +0xC
(**walk type = low 3 bits**; 0 = walkable floor), baseVertIdx s16 @ +0x10.
**Primitive encoding (per-cell list):** `<0x4000` = floor-poly index; `0x4000–0x4FFF` = wall segment
(idx−0x4000); `≥0x5000` = empty.
**World→cell:** `col=(int)(originX+wx)/cellSizeX`; `gz=originZ+wz; if(col&1) gz-=cellSizeZ/2` (**integer**
stagger); `row=(int)gz/cellSizeZ`; `cell = nCols*row+col`. **Cell height** = `vy + ((vx−x)*A+(vz−z)*C)/B`
at the cell center, over `baseVert`.
**Map extent (free, per map):** `nCols*cellSizeX × nRows*cellSizeZ` meters, min-corner `(−originX,−originZ)`.
The 16-bit cell index hard-caps every map at **`nCols*nRows ≤ 32767`** ⇒ a few hundred meters/side.

**No callable engine pathfinder** (conf 0.85): AI movement = reactive local steering fan
(`FUN_0033a720`→`FUN_00335b60`) + a waypoint *follower* (`FUN_00335180`, route node chain at
`route+0x180`), not an A→B planner. We keep our own A*.

**Planner (Session 34) — `src/navigation/nav_grid.{h,cpp}` + `path_planner.cpp`:** on the game thread,
`NavGrid::EnsureBuilt(epoch)` bakes `walkable[]+floorY[]` for the whole grid via `MapQuery::ReadCellFloor`
(zero raycast), cached per map-epoch, invalidated on teardown. A* runs over the entire overlay (no
distance cap; `kMaxExpand=20000`/`kMaxRays=60000` are safety ceilings). Edge = overlay-walkable +
`|ΔfloorY|≤kMaxStep` + `SegmentClear`(mask=4). String-pull uses the dense **`SegmentTraversable`** (1 m
samples: `GroundAt` floor-continuity + `|ΔfloorY|` + per-substep `SegmentClear`) so no leg crosses a
wall. Directions stay WORLD-ABSOLUTE (no egocentric). **Direct read is conf 0.92**, confirmed live by the
`'` self-diagnostic (grid header + `ReadCellFloor`-vs-`GroundAt` cross-check `walkAgree %`);
`NavGrid::SetDirectRead(false)` = GroundAt-bake fallback.

---

## Message / Dialogue / Panel Text (2026-07-07) — decompile-exhausted, ≥0.98; Frida-pending

Full spec + evidence: `..\FFXII-Decompile\notes\message_text_readpoints_spec.md`. Four distinct
surfaces; two readable, two are baked assets (not readable via codec).

**A. ~~NPC dialogue + in-engine cutscene captions — message window `e5f0`.~~ — STRUCK (Session 45).**
> **THIS ENTIRE SUBSECTION IS WRONG. It is the WORLD MAP SCREEN, not dialogue.** `page+0x138` is a
> **MAP ID**, not a msgId; `FUN_003c02b0`'s `out+8` is a **MAP NAME**, not a body. The "decisive"
> `mini_face_c` evidence below was a misread — that symbol is a **texture-bundle name** passed to
> `FUN_0024a5a0` alongside `battle_4_p`/`s_font_c`, and the `FUN_002baf80` family never references
> it. Both hooks were REMOVED; wired up, they spoke nothing, and would have spoken a map name over
> real dialogue. The nameplate `DAT_02b62d78` went with them — it existed only to prefix these
> "dialogue" lines, so do NOT assume it belongs to the real dialogue window; re-derive the speaker
> against THAT widget once it is found.
>
> The RE is preserved (correctly labelled) under **"World MAP screen (`page+0x138` = map id)"** —
> use it for map-screen / map-transition speech. **The real field dialogue window is STILL UNKNOWN**;
> the only dialogue path that works is the telop `FUN_002e16b0` (a whole-message setter, which is why
> multi-page screens read all at once). Unverified lead: `FUN_003cb650` (RVA `0x2AB650`) case 1 vs
> case `0x20`, `DAT_02b47760`, `+0x179D0` msg id, `+0x179D2` bit `0x2000` open / bit `0x80` page-done.

**B. ~~Item / treasure~~ / battle-system / yes-no confirm — one memory-only buffer.**
> **PARTLY STRUCK (Session 45): `FUN_0057c480` is the MENU system-message window — NOT the field /
> item / treasure path.** Its case 1 does `*(longlong*)(DAT_0209ac30 + 0x328) = surface`, i.e. it
> registers into the menu manager; every caller of its producer `FUN_002ce370` is a menu screen, and
> it never fired once in a 17-minute play log. The "field-chest 'obtained X' reuses this surface"
> claim was self-tagged **0.90 — below this project's 0.98 bar** — and is wrong.
> **The real obtained-item read point is `FUN_0035e070` (RVA `0x23E070`) + `widget+0xC8`** — see the
> Session 45 block at the top. The offsets below are still correct **for menu system messages**
> ("cannot equip", "sold"), which is the only role this surface now has.
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

## Field pause-menu entry announce — SHOW message (Session 52, 2026-07-21) — CONFIRMED in play

**Problem it solves:** the field/party menu spoke its focused row on key-press, before the menu was
visible. The battle command menu did not. The difference was purely *when the stashed entry focus is
released*: the battle menu waits for its own row draw (`FUN_00276be0`); the field menu was releasing
at `FUN_00244830` (RVA `0x124830`), the focus **assignment**, which fires at the START of menu
construction. Fixed by giving the field menu its own "menu is visible" event, mirroring the battle
menu exactly. Confidence 0.98 (in-play confirmed).

**The field pause-menu command column = `FUN_00280de0`** (RVA `0x160DE0`, == `ROW_CHAIN[0]`,
`rowOff 0xD8`) — its own window/message proc. Message map (arg = `packet`, `cat = *(int)packet`,
`msg = *(int64)(packet+8)`):
- `cat 1` init · `cat 2` close · `cat 0xa` teardown · `cat 0x10` destroy
- `cat 0xc` notify: `msg 0x8000` row focus (sets the description via `FUN_00291d80`), `0x8001` confirm,
  `0x8002` cancel
- **`cat 0x13` = SHOW — the menu-becomes-VISIBLE frame.** Guarded by `*(byte)(pane+0x280) & 1` so its
  body runs once per open (each open is a fresh pane object, so `+0x280` starts clear): it creates the
  info window (`FUN_002839b0` → `DAT_0209ac30+0x318`), plays the open SE **`FUN_00249c60(4)`** once,
  and clears the hidden bit `0x80` on the menu's UI resources (battle_4_p / s_font_c / targetline_p /
  shape / mini_face_c). **This is the announce trigger.** (We trigger on the message; the SE call only
  corroborates that this branch is the visible-open moment.)
- **`cat 0x11f` ACTIVATE (`msg 0x8000`) / DEACTIVATE (`0x8001`) is NEVER SENT** — 0 occurrences in a
  full session. The decompile makes its activate branch (resets `+0x274=-1`, `+0xC0=0`, raises bit
  `0x200000` on `*(pane+0x260)+0xE0`, inits cursor) *look* like "menu is live", but it does not fire
  for a normal open. **STRUCK as a readiness signal — do not wait on it** (it shipped as silence).
- `pane+0xC0` is a state enum (0 activate/select, 1, 3, 5 cancel).

**Mod implementation (`src/ui/ingame_menu_reader.cpp`, one-to-one with the battle path):**
`IsFieldPaneOwner(owner)` = `Obj0(owner)==ResolveRva(0x160DE0)`. `MenuReader::HookedFocusSet` calls
`ArmPaneEntry(owner,rowOff,idx)` (stash `g_panePending*`) for that ONE class instead of speaking;
`HookedFieldPaneWnd` (observe-only hook on `0x160DE0`) releases it on `cat 0x13` via
`OnRowChainFocus`, consumed one-shot under the lock. Every other pane still speaks immediately in
`HookedFocusSet`. No fallback — unresolved row = silence, like the battle menu.

**Four refuted readiness signals (measured, in `debug.md` so they're not retried):** first-UI-string
drawn (next frame), the row's own text drawn (31 ms — drawing ≠ presentation), `cat 0x11f` ACTIVATE
(never sent), a 400 ms timeout fallback (spoke at the wrong time). Only `cat 0x13` is the game's own
visible-open event.

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

**Last updated:** 2026-07-15 (Session 45) — **FIVE prior conclusions STRUCK. Read this block before
trusting anything below it.**

1. **`FUN_00353490` is `getmapjumpanglebyindex`, NOT a party-placement call.** It is abs `0x353490` =
   RVA `0x233490` = the mod's own `GETMAPJUMPANGLEBYINDEX`; its body returns a jump's angle
   (`buf+0xc`). Session 43's demotion of `mapData+0x54` from Exit to "party ARRIVAL/SPAWN" rested
   ENTIRELY on this misread and is **REVERTED**. `mapData+0x54` is the **map-jump point table** = the
   intra-map "Mapjump" exits (Inner Ward -> Upper Apartments), tester-confirmed by walking into them.
   `Category::Event` is retired; those entries are `Category::Exit` again.
2. **`+0x54` records carry x/y/z/angle ONLY.** `FUN_00264b90` is the sole reader of that table in all
   33,105 functions and touches only `word[i*8+1..4]`; bytes `+0x10..+0x1f` are read by **nothing**.
   The Session-42 `destIdx@+0x1d -> +0x8c -> areaId` chain read **dead bytes** and returned
   `destIdx=0 / areaId=0xffff` on every record of every map (see the live log). `+0x1d` is real only
   on the **`+0x70` field-sign** records — a different table.
3. **`mapData+0x70` — SETTLED at runtime (Session 45 test). Do not re-litigate.** The array is
   **populated and structurally valid**, and the mod's reader is **correct**; it simply holds **no
   records** on the prologue map. Live dump: `off=0x20650 groupCount=5`, group offsets
   `0x20600`..`0x20640` spaced `0x10` — five `0x10`-byte sub-table headers packed exactly into the
   `0x50` bytes before the group table — every group `count=0`. So the map has **no field signs**,
   hence no destination name to read (the game draws none either). **Session 44's "`blob+0x70` is
   empty at runtime" is FALSE** (`blob+0x70` = `0x20650`, non-zero); its `count=0` came from the ABI
   bug below. **Session 43's "`+0x70` = the exit source" was the RIGHT table** — it just never worked
   because of that same bug. NEXT: re-dump on a map that visibly draws "→ \<area\>" arrows (town gate
   / open field). If still empty there, `+0x70` is authored-empty in TZA and destinations must come
   from the script — observe-hook **`FUN_003145e0`** (RVA `0x1f45e0`, receives `(mapNo, jumpIndex,
   flags)`) + the global map table `DAT_02099d88`.
   The original supporting analysis, still accurate:
   measurements behind it are invalid: (a) the runtime `count=0` came from `Pfn_ExitCount` declared
   `int(*)()` and called with **no argument**, while `FUN_00264ac0` passes its incoming `ecx` through
   to `FUN_00264ae0(group)` and uses it as an array index — so it read register junk on every map;
   (b) `parse_mapdata.py` reports `+0x8c` = 0 in all 550 files, yet the live log shows `+0x8c`
   populated (`destCount=2`) — it is reading the wrong blob base (Session 44's own note concedes the
   map-control blob "is a cluster child, not `mpk[+0x10]`", which is what the parser uses). **`+0x70`
   had never actually been measured.** ABI fixed + a direct-memory group-table dump added.
4. **`FUN_003c02b0` + `FUN_002baf80` are the WORLD MAP screen, not dialogue.** `page+0x138` is a MAP
   id; the resolver's `out+8` is a MAP NAME (`DAT_02b457e0+0xe8` = map DB; `FUN_003be680(id,&w,&h)`
   returns width/height for zoom-to-fit; assets under `ArtData/menu/localmap/`; sibling proc
   `FUN_002b7b80` tracks player position). The spec's "decisive" `mini_face_c` evidence was a misread
   — that is a **texture-bundle name** passed to `FUN_0024a5a0`, and the `002baf80` family never
   references it. **Both hooks REMOVED** (they would have spoken a map name over real dialogue).
   **The RE is KEPT, not discarded** — see *"World MAP screen (`page+0x138` = map id)"* below: it is
   what a "speak the map/area" feature will want, just wired to the map screen instead of dialogue.
5. **`FUN_0057c480` is the MENU system-message window, not the field/item path.** Its case 1 does
   `*(longlong*)(DAT_0209ac30 + 0x328) = surface` — it registers into the menu manager. It never
   fired once in a 17-minute play log. The `message_text_readpoints_spec.md` claim that field-chest
   "obtained X" reuses it was self-tagged **0.90 — below this project's 0.98 bar** — and is wrong.

**NEW (Session 45), all ≥0.98 offline-derived, no probe:**

- **"Obtained \<item\>" popup — RUNTIME-CONFIRMED** (`[MSGTEXT] item: "You obtain a Potion"`):
  proc **`FUN_0035e070`** (RVA **`0x23E070`**), gate `*(int*)param_2 == 1`
  (build), read the composed codec text at **`widget + 0xC8`** (cap `0x4A0`). `FUN_002b4090`
  (codec-sprintf) fills it from a template of three `0f 31 80 80 80` slots separated by `0x02`
  (item / item / gil, max 3; truncated in-place for 1-2 entries). Widget = 0x6e8 bytes, created by
  `FUN_0035df40`, handle in `_DAT_022ca430` (RVA `0x21AA430`). Reached from BOTH `FUN_0050faf0`
  (treasure) and `FUN_002a59c0` (field). It is a **timed toast** — case 2 self-destructs when the
  animation ends — so it never paginates and fires once per popup.
- **Party vitals (combat support)**: roster **list 3** at `DAT_02ebf190 + 0x5a7e` (RVA `0x2D9F190`;
  9 x u16 = BtlChr index, `>= 0x28` means empty). BtlChr array = `base + 8`, stride `0x1c8`, `0x28`
  entries. Fields: `+0x04` u8 charId, **`+0x24` i32 maxHP**, **`+0x28` i16 maxMP**, `+0x3c` u32
  statusA, **`+0x48` i32 curHP**, **`+0x4c` i16 curMP**, `+0x64` u32 statusB, `+0x6c`/`+0x7c` i8
  MP-enabled guard (both sign bits clear => MP usable), `+0x1c2` u8 level (0.97). **HP is i32, MP is
  i16** — reading MP as i32 gives garbage in the high half. Slots 0..3 (3 active + 1 guest); test each
  for emptiness rather than trusting the `< 4` bound. **Status = `*(u32*)(bc+0x64) | *(u32*)(bc+0x3c)`**
  — which is why the old "status bit 0x10000" guess failed: bit 16 is real (max-HP-lock / Disease) but
  only reads correctly from the OR of BOTH words. Bit->name is data-driven at `DAT_022c8b08`
  (RVA `0x21A8B08`, stride `0xA8`, `rec+0x98` = bit index, name = `FUN_002b58b0(*(u64*)(rec+0x18),0)`).
  Derived from the seven `btlAtel*FromPartySlot` natives (`FUN_0050fd00`..`FUN_0050ff20`), resolved by
  fingerprint (seven consecutive fns funnelling through the slot->BtlChr resolver `FUN_00320ab0`;
  address order == .dbg order 7/7; each body matches its name 7/7). **Do NOT call them** — they are
  athena-VM natives that pop args via `FUN_00267db0`/`FUN_00267e10` (aborts on underflow) and push
  returns through the VM context. Read the data directly, as their bodies do.
- **Codec escape params are SELECTOR-dependent**, from the game's own interpreter **`FUN_002ac5f0`**
  (RVA `0x18C5F0`): `0x21` = 0 params; `0x20/0x27/0x2f/0x32/0x34/0x35/0x37/0x3a/0x3c/0x3d/0x3e/0x56`
  = 2 (`FUN_003ffab0`); **`0x31` = 3** (`FUN_003fff10`, the sprintf "%s" slot); `0x40`-`0x6b` = the
  icon/glyph family (`FUN_002aeb20`, RVA `0x18EB20` — selector->button mapping NOT yet decoded).
  The old "skip every following byte >= 0x80" rule is only an approximation and **corrupts live text**:
  digits are `0x85`-`0x8e` and punctuation `0x99/0x9a/0xa0`-`0xaf`, so a 0-param escape followed by a
  number ate it ("Obtained 3 Potions!" -> loses the 3 and the !).
  **STILL BROKEN for the icon family (open):** `0x40`-`0x6b` have an unknown param count, so they fall
  back to the legacy `>= 0x80` run and it eats the byte after the glyph. Live proof (Session 45 test):
  *"...by approaching a save crystal and pressing Touching one of these crystals..."* — the button
  glyph AND the `.` (`0xa8`) after it are both gone. **Fix = decode `FUN_002aeb20` (RVA `0x18EB20`)**,
  which yields the param count and the selector→button-name mapping together. Do NOT patch this by
  making the fallback stop at any decodable byte — a genuine param byte landing in `0x85`-`0x8e` or
  the punctuation range would then be emitted as literal text. Get the real count.
- **The `nav_rva.h:247` formula `sel-0 slot = mapctrl.dbg_idx - 5140` is BROKEN** — not a constant
  offset (`getmapjumpposbyindex` implies 5142, `getmapjumpanglebyindex` implies 5140), because the
  .dbg symbol list interleaves variables/source-markers with actions. Resolve natives **by behaviour**,
  never by index arithmetic.

**STILL OPEN:** the `meswin` field dialogue window + multi-page pagination. The working dialogue path
is the **telop** (`FUN_002e16b0`), which is a whole-message setter — it hands over speaker + every page
in one string, which is exactly why multi-page screens read all at once. Best unverified lead:
`FUN_003cb650` (RVA `0x2AB650`) case 1 vs case 0x20, with `DAT_02b47760` / `+0x179D0` msg id /
`+0x179D2` bit `0x2000` open, bit `0x80` page-done. **Unverified — do not ship.**

---

2026-07-14 (Session 44) — **(a) Mod is strictly READ-ONLY on input/memory** (audit:
no `SendInput`/`keybd_event`/`WriteProcessMemory`/mem-write; DirectInput buffer passed as `const`; only
`VirtualProtect` = the one-time vtable patch; all game calls are pure getters). The tester's speed jump
was their own `1`/`2`/`3` keypress = **Game Speed 1×/2×/4×** (the old Controls.md "Lock On / Target Group"
labels for 1/2/3 were WRONG — corrected). **(b) Naviicon "markers" REMOVED** (see below — disproven).
**(c) All-maps exit DB extracted OFFLINE** from the VBF (`tools/parse_mapdata.py`): each map ships as
`ps2data/plan_master/map_ctrl/<AREA>/<MAP>/bin/<MAP>.mpk` (~550 maps); `mld = mpk[u32(mpk,0x10):]`; the
`.mld` tables are file-relative (reloc `_DAT_01f83530`=0): `+0x70` EXITS `[u32 count][u32 recOff…]`, record
`X@+0/Z@+8/enable@+0xc/destGroup@+0x1d`; `+0x8c` DEST-IDs (`u16 areaId@+0x0a` → planmapname); `+0x54`
arrival, `+0x84` dest-pos. Accessors `FUN_00264ae0`/`FUN_00264870`/`FUN_00264920`/`FUN_00264b90`. Emits
`notes/map_exits.csv` — the whole game's exit graph in one offline pass (answers "post-boss Nalbina map:
what exits does it actually have, or is it cutscene-only" without a per-map play-through). Inner exit-record
byte offsets ~0.9 (self-validate across 550 maps: sane positions + resolvable dest names ⇒ ≥0.98).

Session 44 — **The naviicon minimap "markers" claim (Session 43 below) is DISPROVEN and the code REMOVED.**
Two decompile traces proved `DAT_02b45a80` holds ONLY character/unit dots (party / ally / enemy / neutral),
each a 1:1 duplicate of a live scene object the combatant + handle-table scans already list — there is NO
objective/waypoint marker (zero `setnaviicon`/objective code in the whole decompile), NO crystal, NO treasure
in it, and `marker[+0x04]` was the entity HANDLE, not a label id. `EnumerateMarkers`/`MarkerRec`/`NAVIICON_*`
/`MARK_*`/`ScanMarkersLocked` all deleted; nothing labelled "Marker" appears anymore. `Category::Event` (old
`+0x54` spawn/dialogue triggers) and the real exits (`+0x70`) are unchanged. **`p`-key target source**: `p`
routes to the battle target the game is currently selecting (`DAT_0209be80 + 0x9FD8`, `battle_target_reader`);
it does NOT press or depend on any keyboard "Lock On" key — there is no such binding (1/2/3 = Game Speed).

Session 43 — ~~**Map-exit source CORRECTED.**~~ **STRUCK (Session 45) — the "correction" was wrong.**
> `mapData+0x54` is **NOT** the arrival/spawn table. Its ONLY evidence was "`FUN_00353490` places the
> party from it" — that function is `getmapjumpanglebyindex` (abs `0x353490` = RVA `0x233490` = the
> mod's own `GETMAPJUMPANGLEBYINDEX`); it returns a jump's **angle** and places nothing. `+0x54` is
> the **map-jump point table = the exits**, exactly as Sessions 39-42 had it, tester-confirmed by
> walking into them. `Category::Event` is retired; they are `Category::Exit` again.
> Also struck from this entry: the `+0x54` record's `+0x1d` dest chain (those bytes are read by
> **nothing** — `FUN_00264b90` is the table's sole reader and takes only x/y/z/angle).
> The `+0x70` field-sign array below is **real and still the destination-name source** — but note the
> getters take a **GROUP index** (see the Session 45 block at the top: `FUN_00264ac0(group)` was being
> called with no argument, which is why it always reported 0 exits).

The `mapData+0x70` field-sign array — the list the game draws as
radar blips / 3D "→ area" arrows (`FUN_003f9720`/`FUN_003c34e0`). Enumerate via getters:
`count=FUN_00264ac0(group)` (RVA 0x144AC0); `obj=FUN_002649b0(group,i)` (0x1449B0, null=hidden, leader-visibility
filtered); `FUN_002648f0(obj,buf)` (0x1448F0) → `buf[0]`=story-gate USABLE, `buf+4`=u16 areaId. Record floats
off `obj`: X@+0, Y@+4, Z@+8, enable@+0xc; +0x1c vis mask, +0x1d dest-group. Name via `FUN_00377870(areaId)`.
A story-gated (disabled) exit has `buf[0]=0` (e.g. the Nalbina "no simple way through"). **Minimap/naviicon
markers** (`DAT_02b45a80` / `_DAT_02b45a70`, stride 0x20) were hypothesized here to carry objective/crystal
icons — **DISPROVEN and REMOVED in Session 44 (see above): they are unit dots only.** ~~`Category::Event` now
holds only the old +0x54 spawn/dialogue triggers.~~ — **STRUCK: `Category::Event` no longer exists
(Session 45); the +0x54 entries are `Category::Exit`.**
Session 40 — Three fixes. (1) **`p` route** now gates on the LIVE
`DAT_0209be80` selection state (gate `PtrAt(P,0x10F78)!=null`, handle `*(u32)(P+0x9FD8)`) read at
press-time — NOT a cache-age window (the target nameplate `FUN_002bfd20` redraws are event-driven, so
the cache freezes between target changes); it re-resolves a fresh pos from the cached target `bc`.
(2) **Map-jump EXIT ARRAY — corrected (≥0.95, two decompile traces agree):** container **slot 0** only;
**`mapData = *(u64*)(containerBase+0)`** (the missing dereference — `containerBase = 0x1F78E10`),
require `mapData!=0` and `*(u16)(mapData+0)>2`; jump table offset `*(u32)(mapData+0x54)` (dest table
`+0x84`); `exitBase = mapData + tableOff + reloc` (reloc `_DAT_01f83530`@0x1E63530 ≈ 0); `count=*(u32)
exitBase`; exit `i` floats x/y/z/angle at word `[i*8+1..4]` (0x20 stride). Prior recipe used
`containerBase` directly → 0 exits. Natives: `getmapjumpposbyindex`=FUN_003538f0→FUN_00264b90(0,0,idx),
dest=FUN_00353ed0→FUN_00264b90(1,…). (3) **Scanner** now also lists **named** objects
(`ResolveObjectName`: npcdic `nameIdx>0` or field-sign `+0xf8` for `nameIdx<0`) regardless of
TALK/ACTION flag (scene categories 1-4: gates/doors/signs), still gated by a readable `+0xB8` node.
Session 39 — Fixed `p` "No target" + wired `Category::Exit`.
(1) `p`-target position now falls back to the field-actor cached pos **`actor+0xE0/E4/E8`** (see :728)
when the battle target's `sceneObj+0xB8` node is null during attack-menu selection (state-dependent;
`battle_target_reader`). (2) **Map-jump EXIT ARRAY** (backs `getmapjumpposbyindex`/`FUN_00264b90`) —
PENDING `'`-dump confirmation: per handle-table container `c`, `containerBase = 0x1F78E10 + c*0x288`,
`exitBase = containerBase + *(u32)(containerBase+0x54) + *(u32)@0x1E63530`, `count = *(u32)exitBase`,
exit `i` floats `x/y/z/angle` at word `[i*8+1..4]` (0x20 stride). Read by `MapQuery::EnumerateExits`
behind a sanity gate; surfaced as fixed-position `Category::Exit` in `entity_list`. `mapjumpgroup*` are
story flags, NOT exit geometry (:736). Canonicalize these offsets (drop the ~0.85 caveat) once the `'`
exit-table dump validates `count`+positions in a fortress corridor.
Session 38 — `p` key (VK_P / **DIK_P `0x19`**, `input_tracker` free
`g_extraDown[5]`) = turn-by-turn route to the locked/selected battle target, via the SAME
`PathPlanner::Request` pipe as `\`. Target sourced from `battle_target_reader`'s new live cache: the
render hooks resolve `bc`→actor (`+0x698`)→`sceneObj` (`+0x10`)→world pos (`sceneObj+0xB8` via
`ReadSceneObjectPos`) and store `pos+label+handle+tickMs`; `GetLockedTarget` returns it if <300 ms old.
`PlanRoute` now snaps an off-mesh GOAL cell to its nearest walkable cell (ring ≤6 cells). `entity_list`
`[`/`]` cursor is now identity-locked (`CursorId` = obj+nameIdx+label+category). **OPEN (pending runtime
confirmation):** whether `DAT_0209be80+0x9FD8` stays populated outside command target-selection (⇒ `p` works
for a persistent lock) or only while the command menu is choosing a target — answered by the baked `NAV-ROUTE`
log, not yet verified. (NOTE Session 44: the keyboard `2` is **Game Speed**, not Lock-On — there is no
keyboard lock-on binding; `p` reads the selection object directly, it never presses a key.) Also confirm the
field stays nav-safe in battle-state mode.
Session 32 — Battle target-selection readout SHIPPED: real path is a
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

## License Board / Job system (Session 53, 2026-07-22) — SHIPPED, play-confirmed

Entry: Party Menu **Licenses** = command `0x4b5` → `FUN_00281ed0` → `FUN_005601b0`.
`menuCtx = *(u64*)(base + 0x1F7AC30)` (`DAT_0209ac30`).

| Surface | Proc (RVA) | Registered | Focus signal |
|---|---|---|---|
| Character-select | `FUN_00560910` **0x440910** | `menuCtx+0x158` | own proc: cat `0x13` SHOW = entry, cat `0xc`/`0x8000` = move |
| Job-select ring | `FUN_00557db0` **0x437DB0** | `menuCtx+0x320` | shared `FUN_00247510` msg `0x8000` |
| Board node grid | `FUN_0055cd40` **0x43CD40** | `menuCtx+0x320` | shared `FUN_00247510` msg `0x8000`, **`val` = POINTER to the focused cell** |

- Char-select is **NOT** the Status/Equip chooser (`FUN_00285290` @ `menuCtx+0xf8`); the deferred
  `FUN_00285a10` hook can never fire for licenses. Highlighted member: read global
  **`menuCtx+0xde0`** (set first by `FUN_00285f20`) — `ctrl+0xd0` is only filled later by
  `FUN_00560ee0`, so it is stale at SHOW time.
- Member block = `*(u64*)(menuCtx + 0xac8 + memberIdx*8)` (= `FUN_00282df0`); `charId = *(i16*)(block+0x60)`.
- Board fields: `+0x120` cell array, `+0x160` member idx, `+0x558` viewed job, `+0x559/0x55a` grid dims.
- **Cell (stride 0x38)**: `+0x00` name codec (variant-selected) · `+0x08` u16 node id (`0xFFFF` =
  locked/not-reachable) · `+0x0b` type · `+0x0c` LP cost · `+0x10` **CATEGORY codec** ("Weapon"/
  "Magick" — *not* a description) · `+0x18` flags · `+0x20/0x21` col/row · `+0x30` category id.
  `FUN_0055bff0` (0x43BFF0) builds it and **zeroes locked cells** (status 3/4/5/8) to `id=0xFFFF`.
- **Node status** `FUN_00323600(charId,node,0)` (0x203600) → `FUN_00323d10` (0x203D10):
  `1` learned · `2` not enough LP (`charBlock+0x190` < cost) · `0`/`9` can learn · `3/4/5/8` locked ·
  `6` null char · `7` invalid panel.
- **Granted-entry list** (`o` detail) — `FUN_0035d330(0x19,node)` → kind `rec+0x23`, 8 ids
  `rec+0x26..0x34`; copy them BEFORE resolving again (shared scratch `DAT_022ca520`). Resolve by
  kind: `0`→`FUN_0035d330(1, id<<16)` gear · `1`→`0x14` magick · `2/3`→`0x1d` technick. The entry
  DESCRIPTION (`rec+0x08`) is drawn **only** for kind‑1 ids in the technick block
  `(ushort)(id-0x9e) < 0x18` (`FUN_00559e30`, 0x439E30). Anything looser leaks unseen text.
- **Save record** = `*(DAT_02ebf190) + 8 + charId*0x1c8`: `+0x190` LP · `+0x1c3` job1 · `+0x1c4`
  job2 · `+0x1c5` viewed board. **Two jobs are SEPARATE boards** — `FUN_003242f0` (0x2042F0)
  toggles `+0x1c5` between job1/job2. Blank tiles are NOT "where job 2 goes".
- Job text: name `FUN_002f9860(job + 0x3ED)` (ids 0x3ED..0x3F8), description
  `FUN_002f9860(job + 0x838)` (0x838..0x843). Ring highlighted job = `ring+0x358`; `ring+0x388 & 4`
  = committing job1 vs job2.
- The board **clears** the description bar (`FUN_00291d80(0,0)` in `FUN_00561390`/`FUN_00558fb0`),
  so `o` text must be supplied by the reader (`TextCapture::ProvideHelpText`).

### Ability-summary overlay (the `F` pages) — a SHARED party-member detail screen

Not part of the license module; the board forwards pad input in (`FUN_0055c740` → `FUN_002c1a80`).
Mode byte **`menuCtx+0xDE7`**: `2` = Technicks/Mist/Remedy Lore/Espers, `1`/`3` = Magicks, `0` = closed.

| Page | Proc | Slot | Focus routine (fires on open AND every move) | Entries | Index |
|---|---|---|---|---|---|
| Abilities | `FUN_002c4460` 0x1A4460 | `menuCtx+0x128` | **`FUN_002c53b0` 0x1A53B0** | `obj+0xE0` | `obj+0x7A0` u16 |
| Magicks | `FUN_002c3560` 0x1A3560 | `menuCtx+0x120` | **`FUN_002c3b90` 0x1A3B90** | `obj+0xC8` | `obj+0xAE8` u16 |

Entry stride `0x20`: `+0x00` name codec · `+0x10` description codec · `+0x18` flags
(bit **`0x20000`** = learned/bright, clear = greyed). Section idx `obj+0x7A4`; heading children
`*(void**)(obj+0x60) + {0x28,0x40,0x58,0x70}` → codec `+0x18`. Description bar window
`FUN_002c13c0` @ `menuCtx+0x118` (`+0xC0` = published entry ptr).

**Unlearned slots** carry the game's own `"?"` placeholder (`FUN_002f9860(0x4C7)`) in `+0x00`.
**`GameText::IsMostlyPrintable` rejects it** (`alpha >= 1` fails on `"?"`), which silenced entire
pages until detected explicitly — see debug.md.

### Yes/No confirm prompt — `FUN_002cdf20` (0x1ADF20)

A SECOND confirm class, distinct from `FUN_00241d40`. Created by `FUN_002cdea0`, registered at
**`menuCtx+0x2e8`**, object `0xe0`; mode at `+0xd0`; buttons registered as string ids **1000/1001**.
It stores **no body text**: the composed prompt is handed to a `FUN_0057c480` surface (kept at
`prompt+0xc0`, also `menuCtx+0x328`) and is readable **only at that surface's case‑1 birth**
(`surface+0x1B0`). Reading it later returns nothing. `message_reader` captures it at birth;
`MessageReader::TakeConfirmPrompt()` hands it to the pop-up preamble.
