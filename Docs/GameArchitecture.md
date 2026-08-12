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
attribute `FUN_00254380` `0x134380`). **NOT USED BY THE MOD, and no longer a lead — see the
page-cursor block below.**

**PAGINATION — the message widget's own page cursor (SETTLED Session 91, 0.98, decompile only).**
Every message the game paginates is walked by **`FUN_002a8c50` (RVA `0x188C50`)**, which is **slot 0
of BOTH live entries** in the text-draw dispatch table **`PTR_FUN_009164c8` (RVA `0x7F64C8`)**
(stride 3 pointers = 0x18 B, type byte = `widget+0xA3`; dumped to
`FFXII-Decompile\output\text_dispatch_table.txt`):

| type | slot 0 | slot 1 | slot 2 |
|---|---|---|---|
| 0 (choice-capable) | **`FUN_002a8c50`** | `FUN_002a9f00` (`0x189F00`) | `FUN_002a9980` (`0x189980`) |
| 1 (plain) | **`FUN_002a8c50`** | `FUN_002aa800` (`0x18A800`) | **null** |

Widget fields (same object `FUN_002a9980` receives — both use the `+0xB0` state word):
`+0x28` text base · **`+0x8A` u16 BYTE OFFSET of the page on screen** · `+0xA3` type ·
`+0xB0` state (low byte = mode; 5 = parked at a break) · `+0x54` park reason (`3` = page break,
`0x23` = the `0F 23` wait escape) · `+0xC0` = 1 when the message ended (codec `0x00`) — **the next
call CONSUMES it** (`002a8c50:535-536` clears it), so read `+0xC0` PRE-call.

`002a8c50:77` starts its walk at `textBase + *(u16*)(widget+0x8A)`, and **`:199-200` is the write
that advances `+0x8A` past a `0x03` page break** (guarded by `+0x54 == 3 && mode == 0`) — so this
function is the cursor's WRITER, i.e. the event. It is device-agnostic by construction: on a type-0
widget the release is `002a9980:45-64`, which masks the engine's unified button globals
`DAT_02f9736a` / `_DAT_02f97362` with `DAT_01e0c2a0`. **Do not read those** — read the cursor.

Live-widget gate: `FUN_002e16b0` stores the window it builds in **`DAT_0215f200` (RVA `0x203F200`)**
— 8 slots, stride `0x68`, window pointer at `+0x00` — and the text widget is `window+0xD0`.
Membership there is what separates a paginated message from every other text block the same dispatch
slot lays out (the field menu shares the `FUN_002a6190` window class, so class identity alone is not
enough). Shipped in `src/ui/dialogue_reader.cpp`.

**⚠ `widget+0xC0` (end of message) IS A LEVEL, NOT AN EVENT** (S149). `FUN_002a8c50` sets it to 1 at
the codec `0x00` terminator (`:136-146`); the **next** call takes the skipped path at `:100` and sets
`+0xC0 = 0`, `mode = 1` (`:534-536`) — and mode 1 passes the `:139` guard, so the call after that
latches again. While a **finished box stays on screen** (a tutorial banner waiting for its dismissal
press) the field therefore reads `1, 0, 1, 0` at frame rate. Anything that re-arms on each `1` will
fire every two frames forever. Ordinary dialogue never shows it: those pages park at a `0x03` break
*before* the terminator (`:155-168`, mode 5) and the box is torn down when the script advances.

**`FUN_002e16b0(ctx, slot, textPtr, _)` (RVA `0x1C16B0`) is the page-key RE-ARM** — it is the WRITER
of the registry entry, tearing the old window out of `DAT_0215f200` and installing a freshly built
one (`:159-169`), so it is the exact "this slot got a new message" event. `param_2` is the slot,
clamped negative→0 and 7 as the ceiling. **Four parameters — do not shorten the detour.** In
`dialogue_reader.cpp` it forgets one page key and returns; it **speaks nothing**, and it fires ~18
times in 140 ms at area load (the ambient-chatter table), which is harmless for a re-arm and would
be a flood for a speaker.

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
> SURFACE. `group = (flags >> 3) & 0x1F` is the map-jump GROUP id. The routine that calls
> `setmapjumpgroup(K)` with `K == group` owns that surface, and that routine's own
> `mapjump(dest, entrance, flags)` literal is the destination.**

> **CORRECTED, Session 102 — the word `__MJ_CTRL` has been removed from the rule above, and the
> `, 0)` has become `, flags)`.** The rule was always about the CALLS a routine makes; the name and
> the zero were how the reader FOUND them, and both quietly hardened into the rule itself. See
> "Not every transition is a door controller" below for the map that proves it.

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

### Not every transition is a door controller — Session 102

**Map 313 (North Spur Sluiceway) has a staircase into a dungeon that the mod could not see, and the
mod's own log had been naming the defect all along:**

```
exits: controllers=1 surfaces=2 listed=1 | dropped: nogroup=0 notused=0 unreachable=0
surface g1: 2 polys at (32.0,17.0,-0.8) box x[30.0..34.0] z[-5.8..4.2]
                                       <== NO CONTROLLER CLAIMS THIS GROUP -- unreachable exit
surface g2: 22 polys at (182.7,9.3,56.1)          (the 315 exit -- listed, routed, walked)
```

It is an **ordinary walk-onto map-jump surface** — `setmapidmj` group 1, 2 polys, at Y=17 while the
rest of the map sits at Y≈9, i.e. up a flight of stairs — and the seam sweep **already found it**.
What was missing was the destination, because `MapScript::ReadExitDests` rejected every routine whose
NAME did not parse as `__MJ_CTRL<NNN>` **before its code was ever scanned**. Map 313's routine table
holds 31 entries and exactly one controller (the way back to 315); the rest include a routine whose
Shift-JIS name begins **イベント** ("event"). A transfer fired from an event — with a yes/no confirm
prompt — is precisely the case the name filter excluded.

**So the reader now scans EVERY routine's code span for the `setmapjumpgroup(K)` + `mapjump` pair.**
Admission for a non-controller routine requires `setmapjumpgroup` — a routine that arms no group
claims no surface, which keeps the Director's world-map teleport list and every story-move `mapjump`
out. Both S64 drop rules are untouched: no swept surface ⇒ dropped, destination that does not resolve
to a real area name ⇒ "NOT USED".

**`mapjump`'s third literal is PRESENTATION, not kind** (conf 0.99, every link read):
`mapjump` native = `FUN_00355350`, whose arg block is `[1]=dest [2]=entrance [3]=flags`, calling
`FUN_00314440(dest, entrance, flags, 1)` → `FUN_003145e0`. In `FUN_00314440`, `flags & 1` selects the
no-fade path and `(flags >> 1) & 1` is passed to `FUN_002efa70`. `0x0A` is the world-map teleport
MENU's combination (S64) and is the only value excluded. The old `flags == 0` test therefore filtered
on how a jump LOOKS, not on what it is.

**CORRECTED S126 — "door controllers keep the `flags == 0` test" COST A MAP ITS ONLY WAY ONWARD.**
That sentence stood here with "play-confirmed on every map that lists exits" behind it, which was
true and irrelevant: it confirmed the maps that already worked, and said nothing about the maps that
did not. Map 357 "Lhusu Mines: Shaft Entry" has three controllers — `__MJ_CTRL000` group 1 →
806 `flags=0x0`, and `__MJ_CTRL001` group 2 / `__MJ_CTRL002` group 3, both → 358 "Lhusu Mines: Oltam
Span" with **`flags=0x2`**, i.e. bit 1, the alternate fade. Both were refused and the player had no
exit deeper into the mine. Corroborated three ways: the census read all three; the blob's `+0x84`
edge table reported `3 edge records vs 1 controllers`; and `exit_scan` logged `surface g2`/`surface
g3` (30 polys each) as `NO CONTROLLER CLAIMS THIS GROUP`.

**The admission rule now, for both classes:** refuse `0x0A`; beyond that a controller that armed a
group (`group > 0`) is admitted on any value, and a controller with no group keeps the strict
`== 0`. This cannot add an exit to a map that is correct today — `exit_scan` still lists a dest only
when a swept walkmap surface carries that group tag, and a correct map has zero unclaimed surfaces,
so a new admission can only land on a surface already logged as unclaimed.

**A `flags` value is not a taxonomy. Do not re-derive a kind from it.**

**Structural note for anyone touching `ReadExitDests`:** `ResolveControllerArrivals` binds controller
*i* to arrival *i* **by position**. Event-bound entries are therefore held in a second vector and
appended only AFTER that pairing has run, with no arrival of their own (`posOk` false). Appending them
first would shift every existing exit onto the wrong doorway on every map.

### The engine has FIVE script containers; the exit reader reads ONE — Session 104

`MapScript::ReadExitDests` parses the blob pointed at by `DAT_02098e10 + 0x00`. That is **script
container 0 of five** (conf 0.99, every link read):

| fact | evidence |
|---|---|
| 5 containers, stride `0x288`, base `DAT_02098e10` (`NavRva::HANDLE_TABLE_BASE`, RVA `0x1F78E10`) | `FUN_00266d10` memsets `0x288` bytes five times from that base |
| each stamped with its own index at `+0x28` | same loop: `*(int *)(p + 0x18) = i` walking from `&DAT_02098e20` |
| `FUN_00263ff0(i)` → the i-th container | `return &DAT_02098e10 + i * 0x51` (0x51 qwords = 0x288) |
| **`FUN_0026c8c0(i, blob, entry)` INSTALLS a blob into container i** | writes `(&DAT_02098e10)[i * 0x51] = blob`, sets the current-context global `DAT_02099d70 = &DAT_02098e10 + i*0x51`, and special-cases `i == 0` with the full map reset |
| every container's blob shares the header layout the exit reader parses | `FUN_00264b90(idx, c)` reads `container[c]->blob + 0x54` for ANY `c` |
| a field object knows which container it came from | `obj+0x15` indexes the same array (`FUN_00263880`, `FUN_002675c0`, `FUN_00263050`) |
| natives act on whichever container is CURRENT | `FUN_002640c0()` returns `DAT_02099d70`; `setmapidmj` (`FUN_0034e5c0`) reads its index from `+0x28` |

`MapScript::LogContainerCensus` (`map_script_census.cpp`) walks all five and is **log-only** — no
consumer, no exit, no behaviour change. Its closing `MAP-JUMP GROUPS ARMED ANYWHERE:` line is what the
`NO CONTROLLER CLAIMS THIS GROUP` line must be read against.

### An event script NEVER arms a walk-onto transition — 0 of 346, Session 104

Swept every extracted event script (`plan_master/us/event/<area>/<unit>/<unit>.ebp`, 346 files) for the
two calls that make a transition:

- **`setmapjumpgroup` (`4f K K 5d 1e 01`): ZERO files.**
- `mapjump` (`4f/4f/4f 5d 8d 00`): 14 files, **13 of them `evt_t00NN`** — the developers' test-warp
  events. The one real script is `mrm_f0100.ebp` → `mapjump(dest=612, entrance=2, flags=0)`, a
  cutscene story move, not a surface anyone walks onto.

So an event-fired transfer is real, but it is never the *walk-onto* kind: the WHERE half of S64's
binding does not exist in the event domain. **Do not sweep event scripts for a walk-onto binding
again.** (Offline `.mpk` map-script analysis is separately dead: the name pool is packed on disk.)

Map 313's blob does name the event domain — routine `SAKIYOMI_grm_a0380`, "pre-read event
grm_a0380" — and `event/grm_a/grm_a0380/grm_a0380.ebp` is a real 7,104-byte file. It contains neither
call. Event `.ebp` headers also differ from `ctrl.ebp` (routine table is not at `+0x18`).

### AN EVENT-FIRED TRANSITION ARMS NO GROUP — Session 105, the answer to map 313

**Measured by the container census on one load of map 313:**

```
c0 routine[1] "__MJ_CTRL000": setmapjumpgroup(2) @+0x6
c0 routine[1] "__MJ_CTRL000": mapjump(dest=315 "Garamsythe Waterway: Northern Sluiceway", entrance=2, flags=0x0)
c0 routine[4] "?C?x???g????": mapjump(dest=567 "Royal Palace: Cellar Stores", entrance=1, flags=0x1)
CENSUS: 1/5 container(s) hold a blob | MAP-JUMP GROUPS ARMED ANYWHERE: 2
```

Routine 4 is the **`イベント…`** ("event") routine. It holds the staircase's destination — **map 567,
Royal Palace: Cellar Stores** — and **arms no group at all**, and `MAP-JUMP GROUPS ARMED ANYWHERE: 2`
proves nothing in any of the five containers arms group 1. Containers 1-4 are empty on this map.

> **The walkmap's map-jump group tag is STATIC map data. `setmapjumpgroup(K)` is what a DOOR
> CONTROLLER does at runtime; an event-fired transition never calls it, because the EVENT decides
> whether the party moves, not the map-jump group system.** So S64's "one routine, both halves" holds
> for doors and **does not** hold for this class: the surface and the destination are authored in the
> same blob but nothing joins them.

That is why the exit was never listed in **any** build: the pre-S102 `__MJ_CTRL` NAME filter rejected
routine 4, and S102's replacement `setmapjumpgroup` filter rejected it again. S102 did not delete an
exit — it failed to add one, twice over.

**The join is made by ELIMINATION** (`BindUnclaimedSurface`, `exit_scan.cpp`): exactly one swept
surface no routine's group claims, AND exactly one group-less candidate whose destination resolves to
a real area name ⇒ they are each other's. Any other count binds nothing and says so. It is arithmetic
over the game's own two lists — no proximity, no invented geometry — and it is *unreachable* on a map
that is already correct, because such a map has zero unclaimed surfaces. `flags = 0x1` here is bit 0 =
the no-fade path, consistent with an event doing its own presentation. The binding is published to
`g_claims`, so NavTrace's `CROSSING ORACLE` checks it the moment the player walks through.

### STRUCK — "the `+0x70` field-sign table's group 1 carries the destination" (Session 104, killed Session 105)

Map 313's one live group-1 field-sign record sits at `(30.16,13.00,4.25)` — inside the staircase
seam's x-range, 0.05 m off its z-edge — and carried the only non-`0xFFFF` `areaId` in any log this
project had taken (`areaId=32, destIdx=2`). It looked like the destination for exactly the surface
that had none.

**It is not. `areaId = 32` resolves to "Pharos at Ridorana"; the script says 567, Royal Palace: Cellar
Stores.** Word[5] of a `+0x8c` record is not the destination for this record class (or `destIdx` is
not its key). The positional agreement was a coincidence — the same failure mode as every refuted exit
model, caught in one log only because the diagnostic printed the RESOLVED NAME instead of the id.
**Do not revive the field-sign table as a destination source.**

### (superseded, kept for the reasoning trail) The `+0x70` field-sign table has MORE THAN ONE transition class — Session 104

`entity_postscan.cpp` treats **group 0** as "doorway" and discards every other group. Measured on two
maps in one log, group 0's live records land 1:1 on the map's ordinary walk-onto seam surfaces — and
map 313 has a live **group-1** record the reader never looks at:

```
315:  6 records, ALL group 0.  Live (10.99,4.29,116.00) / (180.00,9.21,54.26) = its two seams.
313:  g0[0] (174.00, 9.20,54.97) areaId=65535         -> the 315 exit's surface x[174..189] z[52..62]
      g1[5] ( 30.16,13.00, 4.25) areaId=32 destIdx=2  -> the UNCLAIMED staircase seam,
                                                         x[30.0..34.0] z[-5.8..4.2], centroid Y=17
```

That group-1 record is **the only field-sign record in any log this project has taken whose `areaId`
is not `0xFFFF`** — the game's own resolver (`FUN_002648f0` → `FUN_00264920`: `+0x8c` record `destIdx`,
word[5]) answered for it and declines for every other.

**NOT YET A CONCLUSION — the pairing is positional, and positional pairing is what every refuted exit
model did.** What is established is only that a live record exists in a group the reader drops, on the
one surface that has no destination. Two things must come from the field before it is acted on: the
census line above, and whether `areaId = 32` names anything — it has **no `planmapname` entry** (its
offset word is 0 in `planmapname.bin`), so either word[5] is not a planmapname map id for this record
class, or the destination has no area name. The sign diagnostic now prints the resolved name beside
the id and flags such a record `NOT GROUP 0 BUT CARRIES A DESTINATION`.

### The seam cache — ONE gated writer, pure readers (corrected Session 85)

The sweep is ~15k guarded reads and its answer cannot change while a map is loaded, so it is cached.
**How that cache is keyed is a correctness question, not a performance one**, and it was wrong from
S64 to S85:

> **`MapQuery::HasWorld()` is a LIVENESS signal, NEVER an IDENTITY signal.** It says a walkmap is
> resident. It does **not** say the walkmap belongs to the map id you are holding. **The map id flips
> BEFORE the engine swaps the walkmap**, so anything that sweeps the instant the id changes reads the
> PREVIOUS map's polygons.

The old cache did exactly that and then latched (`s_haveMap = true`) for the whole visit, so it was
permanently one map behind. In Garamsythe Waterway it served map 311's three seams to map 315 and
315's two back to 311 — mislabelling every exit, dropping the one whose group did not exist on the
wrong map, and putting another 199 steps away off the map entirely. It also fed `NavTrace`, so the
crossing oracle reported `MISMATCH — the group->destination binding is WRONG` about a **correct**
binding. `exit_scan.cpp` carried a second copy of the same latch on top.

The shape that is correct — and it is the one `NavMesh` / `NavReach` already used, measured right on
every load (139 / 1997 / 139 polys across 311 → 315 → 311):

| | |
|---|---|
| **writer** | `MapQuery::PrimeMapJumpSurfaces(mapId, epoch)` (moved to `map_seams.cpp`, S93) — GAME THREAD, called **only** from inside `PathPlanner::OnGameFrame`'s nav-safe + non-origin-position block. The only caller of `ReadMapJumpSurfaces`. |
| **why that gate** | ~~`IsFieldNavSafe()` is false for the **whole** of a transition, so it is the cheap proof that the resident walkmap belongs to the id we tag the answer with.~~ **STRUCK (S93): it is false for the MIDDLE of a transition, not the whole of one.** At the LEADING edge the map id has already flipped while the previous map's walkmap is still resident; the sweep ran on map 306's polygons and tagged them 1101, after which the crossing oracle blamed a "wrong group->destination binding" that was correct. Nav-safety is LIVENESS, not IDENTITY -- the same distinction S85 drew about `HasWorld()`, one level up. The cache is now keyed on the **teardown epoch**, the only signal that actually brackets a map, and `CondAreaId` is no longer in the gate at all. `map_query.h`'s own header already recorded the phenomenon ("the map id flips BEFORE the engine swaps the walkmap") without anyone connecting it to this line. |
| **invalidation** | `MapQuery::InvalidateMapJumpSurfaces()` from `PathPlanner::OnMapTeardown`, beside `NavMesh::Invalidate` / `NavReach::Invalidate`. Handles a map reloaded onto its own id. |
| **readers** | `CachedMapJumpSurfaces(mapId, out)` — any thread, **never sweeps**, serves only when the cached answer was swept for that same `mapId`. The two possible answers are "this map's seams" and "nothing yet". |

**The rule this generalises to: any per-map cache must invalidate on the TEARDOWN epoch and fill only
behind `IsFieldNavSafe()`. Never on `HasWorld()`, and never a private second copy in a consumer** —
two layers of latch meant fixing one changed nothing. Cost: the exit list is empty for one extra
frame after a map load (`EntityList::OnFieldFrame` runs before `PathPlanner::OnGameFrame` in
`NavHooks::HookedFieldFrame`), which is right — silence, not another map's geometry.

### TRANSITIONS vs DOORS — two systems, never mix them

| | **TRANSITION** (district ↔ district) | **DOOR** (shop, Stair to Lowtown) |
|---|---|---|
| lives in | the **walkmap** — a floor poly tagged by `setmapidmj` | the **scene-object table** — `kind == 4` |
| fires when | you **walk onto it** | you **press Enter** on it |
| named by | the owning `__MJ_CTRL`'s `mapjump` literal | a `+0x70` field sign (`setfieldsignlocationjumpinfo`) |
| read by | `exit_scan.cpp` | `entity_scan.cpp` |
| **routed to as** | its **POLY SET** (`MapJumpSurface::polys`) — see below | its single interaction point |

**A TRANSITION'S ROUTE GOAL IS THE WHOLE SURFACE (Session 98).** A transition can be 27 m across
(map 315's is 16 polys, `x[153..180] z[52..62]`; Southern Plaza's is 28), and it fires wherever you
step on it — so no single point is its destination. The exit's `pos` is the nearest tagged VERTEX to
the player, which is the right answer for **how far away is this exit** (`/`, the `[`/`]` listing,
the `kAtExitDist` check, all recomputed per scan from the live player position) and the wrong answer
for **where should the route end**, twice over:

- a triangle vertex lies ON the walkable boundary by construction, so the body can never quite stand
  there — the breach diagnostic reads `margin=-0.27m`, i.e. distance-to-border 0.00;
- straight-line nearest is not WALKING nearest. Measured on map 315: the crow-flies-nearest vertex
  was the corner the walkable approach reaches last, so the route validated 21 of 22 legs, drove
  **20 m along the exit surface** to reach it, and was spoken as "No path" 16.4 m short — while the
  frontier it discarded ended ON the surface.

Only the search can say which member is reachable, so `PathSearch::Run` takes the poly set and uses
it on the FAILURE PATH only (a route that validates today never consults it). Pick the member whose
`ClosestPointOnPoly` to a **proven-reachable** position is nearest — never the member's centroid, a
seam triangle can be 16 m long — and re-run the ordinary search there.

**Do not reverse-derive the group from a poly's EFFECTIVE flags.** The seam sweep reads RAW flags,
and bits 3-6 are simultaneously the map-jump group AND the index into `FUN_00232020`'s group override
bank (`C = ((flags >> 3) & 0xF) + 0x40`), so an override can rewrite the very bits the group would be
read from. `Entity::seamGroup` plumbs it explicitly and depends on no flag encoding.

**Every refuted exit model looked for transitions in door-shaped places** (`+0x54[N+1]`, the
`+0x54` ∪ `+0x70` union, field-sign pairing, the `+0x84` edge bearing, blob table order, the boundary
and passage marches). Transitions are not in the map-control blob's position tables at all. Keep the
two readers separate and never use one as evidence about the other.

**SUPERSEDED (correct in isolation, no longer the mechanism):** the S61 passage march (walkability is
not how you find a trigger) and the S62 arrival relation (true, but needs a neighbour's data — the
group tag is local). The `+0x54` arrival table is still the party SPAWN point and nothing else.

**Also settled — CLOSED Session 66, and the S64 wording above was half wrong.** Map **305 (Eastgate)**
does appear in East End's Director `mapjump(…, 0, 0x0A)` list, and `flags == 0x0A` is the world-map
**teleport menu** rather than a walk-through door — that part holds. But "there was never a door to
find" was too strong: **the Eastgate transition is off SOUTHERN PLAZA (292), not East End (291)**,
confirmed by the tester walking it. East End correctly has no loader to 305 because the door is on
another map, which is exactly what the reader was telling us for eleven sessions.

The lesson is the one the walkmap tag already taught: *"the map has no door to X"* is a statement about
**that map**, never about X. Six sessions of "the Eastgate is missing" were spent looking for something
on the wrong map. The Director routines are still just the teleport list and nothing more.

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

> **STRUCK (S63) — the IMPLEMENTATION only, not the relation above.** This paragraph used to read
> "Implemented as `ExitLinks` (`exit_links.h`), persisted to
> `%LOCALAPPDATA%\FFXII-Screen-Reader\map_links.txt` behind a `version` header." **`exit_links.{h,cpp}`
> and that file were DELETED in Session 63** as a no-learned-labels rule violation, and are not in
> `CMakeLists.txt`. Do not reintroduce a learned/persisted link store. Session 64 replaced it with a
> purely LOCAL binding that needs no neighbour and no cache: the controller's own `setmapjumpgroup(K)`
> against the walkmap poly tag `K` — see "Transitions are walkmap surfaces". `ExitDest::arrivalSlot`
> still carries the `+0x54` index but is diagnostics-only; `entrance` is parsed and unused.
> This entry stood as fact for 22 sessions after the code was gone. Left here because the ARRIVAL
> RELATION itself is still true and still the reasoning trail for why local binding rules were needed.

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

### The three GEOMETRIC gates on interaction — RE'd Session 73 (offline decompile)

Being a valid candidate (above) is not enough. `FUN_0025b820` rescans every field frame and
`FUN_0025bad0` / `FUN_0025be50` score the survivors; **three independent geometric tests** must all
pass, and only the single best-scoring object survives.

| Gate | Where | Rule | Conf |
|---|---|---|---|
| **Horizontal distance** | `FUN_003da5a0` | returns `sqrt(dx² + dz²) − Σradii`; must be **< 0**. **Y is excluded entirely** — interaction range is a cylinder, not a sphere. `Σradii` is fully solved — see "The interaction REACH" below; it is 4 memory reads, not an unknown | 0.98 |
| **Vertical band** | `FUN_0025bad0:77-80` | unless the target's `xform+0xDD != 0`, the player's Y must lie inside the target's vertical extent ± margins. A separate test from distance | **VALIDATED IN PLAY** |
| **Facing cone** | `FUN_003a1bb0` (abs `0x3A1BB0`) | `\|wrap(atan2(tx−px, tz−pz) − playerYaw)\| < halfAngle`. Player yaw = `playerColl+0xA8`; half-angle is per-target | 0.99 |

```c
bool FUN_003a1bb0(float facingYaw, float halfAngle, float px, float pz, float tx, float tz) {
    a = atan2f(tx - px, tz - pz);          // NOTE: (x, z) — same convention as faceNode
    a = wrap(a - facingYaw, -PI, PI);
    return fabs(a) < halfAngle;            // outside the cone => not interactable
}
```
(Ghidra shows only 4 args at the `FUN_0025bad0:133` call site — it drops the register-passthrough
pair. `FUN_0025be50:41` shows the full 6 and is what pins the signature. See
`feedback_resolve_natives_by_behaviour`.)

### The vertical band, VALIDATED in play (Session 73) — and why routing needs it

```
gates "Montblanc": dist2D=0.51 (Y EXCLUDED)
                 | band FAIL py=0.00 in [4.63,8.02] centre=6.92 sc=1.00 up=1.10 dn=0.50 pad=1.79
                 | cone PASS |d|=0.255 half=1.571
```

Both bounds reproduce exactly from the offsets below, so these are measured, not inferred:

```
centre = *(sceneObj+0xC0)+0x140  +  *(sceneObj+0xC0)+0x144  +  targetNode+0x04      // 6.92
lo     = (centre - targetNode+0x24 * targetNode+0xCC) - playerNode+0xC0 * playerNode+0x24
       = (6.92 - 1.00*0.50) - 1.79*1.00                                            // 4.63
hi     = centre + targetNode+0x24 * targetNode+0xC8
       = 6.92 + 1.00*1.10                                                          // 8.02
```

**`half=1.571` = π/2**, i.e. the cone is a **180° forward hemisphere** — facing is far less
restrictive than it looks, and a "turn to face the target" key would have fixed nothing. Distance is
a **cylinder** (0.51 passed while the player was 6.92 below), so for anything on a dais, counter or
ledge **the band is the only gate that decides where you must stand.**

That is why `PathSearch::Run` now takes the band: the goal is "a cell whose floor is inside
`[lo,hi]`", i.e. somewhere you could stand and interact — not the target's own cell. See
`InteractTarget::ReadBandFor`.

**The chosen target is ONE object, and there is NO cycling.** Per-frame reset by `FUN_0025d650`
(score = `0x501502f9` ≈ 1e10, ids = `-1`), then rescored:

| Global | Meaning |
|---|---|
| `DAT_0209a2b8` | winning object slot |
| `DAT_0209a2b4` | its container |
| `DAT_0209a2bc` | `10` = talk / `2` = action |
| `DAT_0209a2aa` | `1` = a target exists this frame |
| `DAT_0209a2b0` | best score so far (minimised) — **= `2*dist2D − reach`**, see below |

### The interaction REACH — the `Σradii` above, solved Session 74 (conf 0.97 offline, self-checking)

The distance gate's `Σradii` is the engine's horizontal interaction reach. Session 73 recorded two of
its four terms as "direction-dependent shape queries we do not replicate" and the mod shipped an
invented `kApproachRadius = 4.0f` in their place. **All four are plain memory reads.**

`FUN_0025bad0:83` calls
`FUN_003da5a0(out, playerXform+0x50, playerXform, targetXform+0x70, targetPosAdj)`, whose return is
`dist2D − reach` (accepted when `< 0`), with

```
reach = ellipse(playerShape -> target) + playerXform[+0x5C]
      + ellipse(targetShape -> player) + targetXform[+0x7C]
```

`FUN_003da730` is the ellipse radius along the direction to the other party; `FUN_003a1d30` is
`sqrtf(fabs(x))` applied to its (squared) result:

```c
w = cosf(-yaw)*dz - sinf(-yaw)*dx;   u = sinf(-yaw)*dz + cosf(-yaw)*dx;
r2 = (u*u + w*w) / ( u*u/(A*A) + w*w/(B*B) );      // 0 when the two points coincide
```

| Field | Player | Target | Meaning |
|---|---|---|---|
| shape record | `xform+0x50` | `xform+0x70` | 4 floats — **different offsets, do not collapse** |
| `+0x00` semi-axis A | `+0x50` | `+0x70` | pairs with `u` in the formula |
| `+0x04` semi-axis B | `+0x54` | `+0x74` | pairs with `w` |
| `+0x08` yaw | `+0x58` | `+0x78` | ellipse orientation (negated in the rotation) |
| `+0x0C` extra radius | `+0x5C` | `+0x7C` | added flat, no direction term |

**SELF-CHECKING — this is the important part, WITH ONE CLASS RESTRICTION (Session 76).**
`FUN_0025bad0:139` stores `DAT_0209a2b0 = (dist2D − reach) + dist2D`, where the second term is
`param_1[3]`, the distance `FUN_003da5a0:36` writes into its out-vector. The mod already reads that
global as `InteractTarget::Chosen::score`, so for whichever candidate the engine chose:

> **STRUCK as universal — the identity holds ONLY for class-3 (character) targets.** `DAT_0209a2b0`
> mixes units between the two scorers: `FUN_0025be50` (class 1, gimmicks) instead stores
> `FUN_003a1960(player, node)`, a plain distance measure. Applied to a class-1 winner the identity
> yields a confident, meaningless number. There is no flag distinguishing them; the discriminator is
> `sceneObj+0x03 >> 5`. See "Two interactable classes" below.

```
reach = 2*dist2D − score          <-- the engine's own answer, nothing replicated
```

`InteractTarget::ReadReachFor` returns replica and measured together; `NAV-PROBE` prints both. A
confirmed value must land inside the play-measured bracket **0.51 < reach < 1.70**.

**Direction dependence matters for routing.** Both shapes are ellipses evaluated along the line
between the two parties, so `reach` is only exact for the current relative position. `Reach::radiusMin`
(`min(A,B)` of each shape plus both extras) is the direction-independent lower bound — that is the one
a goal-cell admission test must use, because a cell inside it is interactable from **any** approach
angle.

### Target position offset — `xform+0x107`, applied BEFORE both distance and band

`FUN_0025bad0:72-76`: when the byte at `targetXform+0x107` has bit 0 set, the engine adds
`targetXform[0x10..0x12]` (bytes `+0x40/+0x44/+0x48`) to the target's position **before** the distance
gate and the vertical band test alike.

**`InteractTarget::ReadBandFor` does not apply it** (it reads `XFORM_POS_Y` directly), so its band —
and therefore the goal band `PathSearch` routes to — is wrong for any target carrying the flag.
`InteractTarget::ReadGatePos` applies it correctly; `ReadBandFor` is corrected in the Phase 3 rebuild.

Consumed by `FUN_00268d10` (the confirm handler). **Do not design a "cycle interaction target" key —
the engine has no such concept.** A pure-read announcement of `DAT_0209a2b8` is, however, the exact
answer to "who will I talk to if I press Confirm", and it is event-driven off `FUN_0025b820`.

## Navigation is ELEVATION-BLIND — root cause, Session 73

Recorded because it produced a story-blocking bug and the shape of the mistake is easy to repeat:
the mod announced *"Montblanc. right next to you"* and routed *"1 steps"* to an NPC **6.92 world
units directly overhead**, in the Rabanastre Clan Hall.

**Every floor query in navigation is `f(x, z) → y`. There is no `f(x, y, z)`.**

| Layer | Evidence | Consequence |
|---|---|---|
| `MapQuery::GroundAt` (`map_query.h:26`) | game fn, args are `(x, z)` only | cannot be asked "the floor nearest *my* height" |
| `ScanTopFloorAt` (`map_query.cpp:190`) | `if (!found \|\| y > bestY)` — keeps the **max** | in any stacked column (plinth, balcony, bridge, upper storey) the grid describes the surface **above the player's head**. It already visits every floor poly; it just discards them |
| `NavGrid` (`nav_grid.h:20`) | `WorldToCell(wx, wz, col, row)`, one `floorY` per cell | a target overhead maps to the player's own cell → `nearDist=0.0m`, the search "arrives" instantly |
| `nav_common.cpp:142-157` | `IsWithinReach(dist2D)` early-returns before `ElevationSuffix` | **"right next to you" is the one phrase that can never carry "(above)"** |
| `path_planner.cpp:122` | route-profile is *"diagnostic, log-only"* | the mod measured `maxStep=6.93m` and `wmBlock=1(step>max)` and gated nothing on either |

**This is global, not per-map** — see `feedback_global_not_per_map`. The fix is a layered grid
(`col,row` → `col,row,layer`) fed by an `AllFloorsAt` that keeps what `ScanTopFloorAt` throws away,
with inter-layer edges admitted by the **existing** `kStepDiscont` test (`path_search.cpp:34-52`).
**FFXII has no climb/jump button** (tester), so inter-level connectivity is purely geometric
continuity — there are no action edges to model, and spoken legs stay 2D.

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
was tried and did NOT work; reverted.) Full game keybindings + mod keys: `Docs/Controls.md`.

**CORRECTION, Session 65 (2026-07-23): "NVDA's own key commands remain blocked under the game's grab" is
NO LONGER TRUE.** The tester confirmed NVDA commands working while the game runs. **The cause is not
established** — their read is that it was a mod-side issue since fixed. Recorded as an observation, not a
mechanism; do not cite a cause until one is traced. The `GetDeviceState` hook above is unaffected and is
still how the mod reads its hotkeys, and the mod still never swallows or injects a key.

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
  *state-selected second name*.
- ~~**In the US build it is byte-identical to the even slot** … ids 0-11 and 433-469 all give
  `even == odd`. The mod's even-slot-only read is therefore correct as shipped, and **there is no second
  name to mine** — do not re-attempt it.~~ **STRUCK (Session 80).** That rested on a **48-id sample**,
  and it sampled the only two ranges that cannot hold a personal name: low crowd filler, and the 433-469
  crystal/urn/treasure gimmick band. Decoded across **all 1141 ids, 247 have a DIFFERENT odd slot, and
  it is the character's real name**:

  | id | even (generic) | odd (personal) |
  |---|---|---|
  | 221 | Nomad | **Arjie** |
  | 228 | Nomad | **Lesina** |
  | 239 | Nomad Elder | **Elder Brunoa** |
  | 159 | Viera | **Ktjn** |
  | 220 | Cockatrice | **Agytha** |
  | 104 | Rabanastran | **Arryl** |

  **The mod was speaking the generic word for every NPC the player had already been introduced to.**
  `EntityScan::TalkNameKnown` now replicates `FUN_0032a930` and `NpcdicName(id, known)` picks the slot,
  so the mod says what the game draws. This is the game's own display string in all 12 locales —
  database resolution, not a learned label. **LESSON: a sample drawn from the ranges you already
  understand cannot falsify a claim about the ranges you do not.**

  **RVAs (verified by adding back):** the bitfield block is the static array `&DAT_02164280`
  (RVA `0x2044280`; `FUN_002ef640` returns its ADDRESS, so it is **not** a pointer to dereference), and
  the bitmap sits at `+0x200` (`FUN_002ef2b0`) `+ 0x13B4` = **`+0x15B4`**. Test:
  `byte[base + 0x15B4 + (id >> 3)] & (1 << (id & 7))`, rejecting `id < 0 || id >= 0x800`.
  The bit is written by the `settalknpcname` / `releasetalknpcname` script natives and read back by
  `istalknpcname` (`FUN_0034e980`), so it **flips mid-session** on the story beat that makes the
  introduction — read per scan, never cached.
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

**Classify** by the npcdic id band (`sceneObj+0x102 & 0xbfff`): 433–469 = gimmick objects → sub-type;
else talk-flag → Person; else Object. **`def+0x05` is 0=static/1=animated, NOT an NPC flag** — the old
`(kind==0)?NPC:Object` mislabeled every NPC (they are kind 1); the pool path is now unused.

~~435–459/467 crystals~~ — **STRUCK (Session 92).** That vague "crystals" grouping was implemented as
`435–459 → Save Crystal`, and the tester's first gate crystal (id **435**, which the game itself calls
**"Rabanastre Crystal"**) was therefore announced under Save Crystal while the Gate Crystal filter read
0. The band table above at "Object name (master data)" was **already right** — *"435–465 = area gate
crystals"* — so this is the read-before-you-dig failure in its purest form: the correct fact and a
mushier restatement of it both lived in this file, and the code followed the mush. The bands, read
directly off the game's own npcdic name table (`FFXII-Decompile\notes\npcdic_names.csv`, from
`PS2Data\...\npcdic.bin`), conf **1.00**:

| npcdic id | Game's own name | Category |
|---|---|---|
| 433 | `Anchor` | Object |
| 434, 468 | `Treasure`, `Urn` | Treasure |
| 435–459 | `Rabanastre Crystal` … `Ridorana Crystal` (25 named area crystals) | **GateCrystal** |
| 460–465 | `(Crystal 26)` … `(Crystal 31)` (unused placeholders) | **GateCrystal** |
| 466 | `Gate Crystal` (generic label) | GateCrystal |
| 467, 469 | `Life Crystal`, `Save Crystal` | SaveCrystal |

Every one of 435–465 is a per-AREA **teleport** crystal. `467 Life Crystal` is grouped with Save
Crystal — inherited, unchanged by the fix, and the one row here not confirmed against play.

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
  walls (edge type 0, type1/bit30=0) + character-only invisible walls (type 4), and skips camera-only
  occluder planes (type1/bit30=1).
  > **STRUCK (Session 93) -- two halves of that sentence were wrong, and both shipped code.**
  > (a) It does **NOT** skip "floors/ceilings (poly records)": `FUN_0022cc50:26` tests type-0 FLOOR
  > triangles unconditionally. That claim, at conf **0.98**, is what licensed `MapQuery::SegmentHit`
  > flattening its far endpoint to `from.y` -- which ran a rising portal's ray UNDER the destination
  > floor and reported the floor as a wall: a false BLOCK on exactly the geometry routing most needs to
  > cross. The engine never flattens; `FUN_0032bcc0:20-23` LIFTS instead. Fixed in `map_query.cpp`.
  > (b) "triggers/water (types 2,3,5,6,7)" is an **invented label**. Nothing in the binary ties any of
  > those type values to water or to triggers; `FUN_0022cc50` simply has no branch for them. Three
  > research passes searched for a water attribute on the strength of that phrase and there is none --
  > the engine has no water concept anywhere in the movement path. What actually refuses water (and
  > cliffs, and fences) is a body-versus-boundary test: see "The hard passability check" below.
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

### Field movement has NO slope limit — walkability = walk-type flags + a ~0.3 m step (Session 68) — conf ~0.9

Root cause of the S67 "routes through impassable elevation" bug. Decompile trace of the FIELD walkmap
movement (**not** Bullet — absent in the field):
- **`FUN_0022cc50`** (RVA **0x10CC50** -- this entry said 0x102C50, arithmetically wrong: 0x22CC50 - 0x120000 = 0x10CC50. Corrected S93) — the per-poly handler used both by the segment test `FUN_00230b60`
  and by the character move-across-walkmap — decides walkability purely from **baked walk-type flags**
  (poly+0xC low 3 bits, after a remap through `DAT_0209a3e0`/`DAT_0209a3e4`): **0 = walkable**, 1/4 =
  conditionally blocked (party/enemy side, `param_2+0x46`), walls (prim idx ≥ 0x4000) block. **There is
  NO cosine / normal.y / slope-angle threshold anywhere in the movement path.**
- `FUN_00231900` / `FUN_00231890` only guard `B > 0.001` on the plane normal — a divide-by-~0 guard, NOT
  a walkable-slope cap. `GroundAt` (`FUN_0026e3c0`, mask=1) returns only walk-type-0 floor.
- ⇒ **the player can walk any CONTINUOUS slope** (that is why stairs and hills work). The only geometric
  movement blockers are (1) **walls** (`SegmentClear` mask=4, already used) and (2) a **step-height
  DISCONTINUITY**: **`FUN_0033bc80`** (RVA 0x21BC80) samples `GroundAt` a point ahead and reacts when
  `ABS(groundY − currentY) >= 0.3` world-units (`if (0.3 <= ABS(fVar1))`, sets ±π/2 pitch). The `0.3`'s
  exact effect (block vs animate) is ~0.5 conf, but it is the engine's step-significance threshold.

**STRUCK:** a poly-normal SLOPE GATE for routing — it would wrongly reject walkable continuous slopes.
The routing fix is a **step-discontinuity** test, not a slope cap.

**Mod fix (`path_search.cpp` `passable()`, Session 68):** keep `kMaxStep = 1.5` as the coarse cliff gate
(so continuous slopes survive), then sub-sample `GroundAt` every `kEdgeSubStep = 0.25 m` along any edge
with `|Δ| > kStepTrigger = 0.15 m` and reject a sub-step jumping more than `kStepDiscont = 0.35 m`
(provisional; bracketed at runtime by `LogRouteProfile`). Same cap in the string-pull `SegmentTraversable`
call. Diagnostic helper `MapQuery::GroundInfoAt(x,z,&y,&cosSlope)` returns the floor Y + poly slope cosine
`B/|(A,B,C)|` (topmost type-0 floor at an arbitrary XZ; shares `ReadCellFloor`'s `ScanTopFloorAt`).

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
> use it for map-screen / map-transition speech. ~~The real field dialogue window is STILL UNKNOWN …
> Unverified lead: `FUN_003cb650` (RVA `0x2AB650`) case 1 vs case `0x20` …~~
>
> **SUPERSEDED (Session 91).** The dialogue path was never unknown — it is the telop
> `FUN_002e16b0`, and the piece that was missing was the PAGE, not the window. It is
> `widget+0x8A` on the widget that setter fills, written by `FUN_002a8c50`; the `FUN_003cb650`
> lead is struck (see the top-of-file pagination block). Speaker/caption is still open, and when
> it is hunted, hunt it against **that** widget.

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
overlay `DAT_02ca8f38`); tutorial panels (Handbook images `menuhandbook_tutorialNNN.dat`) — **but
PARTIALLY AMENDED (Session 106): only the page BODIES are baked art** (`hdatg` graphics container).
`menuhandbook_tutorial.bin` (`hctgf`) carries per-panel TITLE STRING IDS (`0x116e9`-`0x116f7` for
tutorials 000-008) — real game text a reader can speak on panel open. Extracted to
`..\FFXII-Decompile\extracted\ps2data\image\ff12\myoshiok\us\handbook\`; viewer class unidentified
(probe authored: `frida\probe_controls_overlay.js`); work DEFERRED per user 2026-07-31. Battle
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
  - ~~**Items** = draw `FUN_0027e530` (RVA `0x15E530`) → resolver `FUN_00272cb0(id)` (RVA `0x152CB0`)~~
    **STRUCK S146 as the LIVE battle path** — the battle Items list draws through `FUN_0027ce70` like
    the magick list, so its name comes from `FUN_0035d330(0x14, id)`. The entry stays as a valid
    handler; it is simply never the one that fires. (Earlier "FUN_0027d5c0/cat 2" claim was WRONG — retracted.)
  - **"Magicks & Technicks" is TWO-LEVEL** (trace: top cmdId 0x12 → `FUN_0027c3d0` returns kind 2 →
    `FUN_0027e050` case 2 type 8; then category → kind 0xa-0xf → type 0xb): (1) category **chooser** draw
    `FUN_0027d240` (RVA `0x15D240`), resolver `FUN_0035d330(cat, id)` with `cat = (panel+0x513+row*8 & 4) ?
    0x18 : 0x15`, name at `panel+0x1578` (not overwritten); (2) spell/technick **list** draw `FUN_0027ce70`
    (RVA `0x15CE70`), `FUN_0035d330(0x14, id)`, `panel+0x1578` OVERWRITTEN by MP-cost → re-resolve.
- **NOT** `FUN_002c2320` (that's the FIELD Equipment screen, opened via pause menu `FUN_00281ed0` cmd
  0x4b6). **NOT** `FUN_002b7590`/`DAT_0209e5c0` (that's the message/dialogue framework). Both retracted.
- **THE SECOND COLUMN the sub-lists draw beside the name (Session 146) — SHIPPED.** Every sub-list
  that shows a number to the right of a row draws it from **ONE** place — the **u16 at
  `panel+0x512 + row*8`**, via `FUN_0029cb80(0x4694, word)` (`FUN_0027ce70:119-125`). What the word
  MEANS depends on which builder case filled it:
  - **`panel+0x50C` SAYS WHICH** — `FUN_0031eb20`'s **case 0xB** sets it in the **same `if` that writes
    the word**, which is what makes it the only trustworthy discriminator:

    | `rec+0xC` | word written | gate at `panel+0x50C` |
    |---|---|---|
    | `& 0x20000000` (MP-costing action) | `FUN_002f95d0(actor, id)` — the cost **as this character pays it** | `FUN_002fa140` → `0→1`, `1→4`, `2→2` |
    | bit 31 (item-consuming action) | `FUN_00309ec0(FUN_003093b0(rec), 0)` — `rec+0x22` is the item spent, the call is its **OWNED COUNT** | **3** |
  - ⚠ **TWO OTHER DISCRIMINATORS LOOK RIGHT AND ARE NOT** — both shipped and were measured away in
    S146. **The per-row DRAW CALLBACK:** the battle Items list shares **`FUN_0027ce70`** with the
    magick list, so every item read "Potion, MP 31". **The LIST KIND at `panel+0x4C0`:** the Items list
    is **case 0xB too** — measured `listKind=0xB costGate=3` (Items) against `listKind=0xB costGate=1`
    (White Magicks). *Identify a shared surface by the branch that WROTE the field, not by what renders
    it or what container it was built into.*
  - Collateral: ~~"Items = draw `FUN_0027e530`"~~ is **STRUCK as the live battle path**, so the item
    NAME has always come from `FUN_0035d330(0x14, id)` (the action table covers item actions) rather
    than from `FUN_00272cb0`.
  - **`FUN_0027e530` (RVA `0x15E530`) is a real draw with a real count source** —
    `FUN_00272c80(entry)` (RVA **`0x152C80`**), the count sibling of `FUN_00272cb0`: same
    `FUN_003588b0` lookup, then `FUN_00263a10` → the stack size at **`rec+0x100`**, but only when
    **`rec+0x102 < 0`** (a stack), else **0** (a single equipment instance). **No play pass has ever
    landed on that draw**, so the mod does not read it; the resolver entry for it in
    `BattleCommandName` stays as the correct handler if it is ever the live one.
  - ⚠ **THE SILENCE TEST IS NOT "is the number non-zero", and must not be replaced by one.** The game's
    own display test is `(*(u32)(panel+0x50C) & ~2) != 0` (`FUN_0027ce70:61`). Gate **0** = no cost
    (Technicks): case 0xB **never clears `+0x512` per row**, so the stale word from a previous list is
    still sitting there. Gate **2** = the **mist-charge count** the draw spends on icons
    (`FUN_0027ce70:91-100`) — speaking it would be a wrong number, not a missing one.
  - The **gate is per-LIST, not per-row** — the builder writes one field inside a per-row loop, so it
    ends up holding the LAST row's kind. That is the game's own behaviour (these lists are
    homogeneous); do not "fix" it into a per-row lookup.
  - `[INGAME] second column: draw RVA=… listKind=… costGate=… -> owned count|MP cost|nothing` names
    each distinct surface once per session, so a log says which branch owned a row. It is what
    refuted the list-kind model.

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
  percentage, **ally** = HP numbers. Src: `src/ui/battle_target_reader.{h,cpp}`.
  (The older `notes/battle_target_vitals_2026_07_10.md` id→`FUN_002367a0`→actor path is SUPERSEDED.)
  > ⚠ **PARTLY STRUCK (Session 147):** ~~"no Libra HP-visible flag found"~~. The flag exists, and an
  > enemy now reads real NUMBERS while Libra is up — `*(u32*)(P + 0x10F68) & 2`, the game's own
  > per-frame mirror of `FUN_0030c300`. The rejection of `0x10000` was RIGHT; only the search was
  > wrong. **Libra is a status on a PARTY member (bit 30 of `bc+0x64 | bc+0x3C`), never a bit on the
  > enemy** — which is why looking at the enemy could not have found it. Full chain in the Session 147
  > section at the end of this file.
  > Also corrected there: the name on this path now carries the **instance letter** ("Dire Rat B").
  > This file's `actor+0x18` decode was a SECOND naming path that never grew one; there is exactly one
  > now, `BattleState::DisplayNameForActor`.

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
  icon/glyph family (`FUN_002aeb20`, RVA `0x18EB20` — **1 param; returns 2 on every path**, shipped in
  `game_text.cpp` Session 49; the Session 45 "pressing Touching" corruption is FIXED).
  The old "skip every following byte >= 0x80" rule is only an approximation and **corrupts live text**:
  digits are `0x85`-`0x8e` and punctuation `0x99/0x9a/0xa0`-`0xaf`, so a 0-param escape followed by a
  number ate it ("Obtained 3 Potions!" -> loses the 3 and the !).
  **The selector→button mapping IS decoded (Session 106, offline; probe-pending before shipping speech):**
  `idx = selector - 0x40`; `FUN_002b5b60` (abs `0x2b5b60`) rebuilds the KEYBOARD glyph table
  `DAT_01e0ce30` (RVA `0x1CECE30`) from the LIVE bindings — fixed action list
  `idx 0..13 -> {0x0d,0x0c,0x0a,0x08,0x0b,0x09,0x01,0x02,0x0f,0x0e,0x07,0x05,0x04,0x06}`; Confirm
  Type (`*(u32*)(DAT_01f82d20{RVA 0x1E62D20, qword ptr}+0x64)`, `FUN_0017e210`) nonzero swaps ONLY
  idx 0<->9 (actions 0x0d/0x0e — the game's own "Type A/B applies to O/X"). The LIVE binding bank is
  `DAT_01f80f90` (RVA `0x1E60F90`): byte DIK codes, `[col*0x1c + action]`, col 0 = keyboard Main
  (`FUN_00197710` kind 0; kind 1 bank `+0x40`; kind 2 u32 bank `+0x60`). DIK→glyph-slot tables
  `DAT_01df1030`/`DAT_01df0e80` (0x6a entries, `FUN_00197790`); selectors `0x60`-`0x6b` alias down
  via `FUN_002ab8f0` (cases 0x20/0x21/0x23/0x27 config-dependent on `DAT_01e0c2a0/2a4`); controller
  table `DAT_01e0ce10` built by `FUN_002b5c90` with pad-style variants `DAT_009165f0/00916670`
  indexed by `DAT_0208f574 >> 4 & 3`. GROUND TRUTH: the 568 palace call prompt (rrp_a02.ebp msgs
  23/24/25/28) is `0f 48 80` -> idx 8 -> action `0x0f` (predicted key: the Battle Menu binding, F).
  Key name from a DIK code: the shipped `KeyCodeToStringId` arithmetic (`config_reader.cpp`,
  replicating `FUN_001e0b00`) + string id. Confirmation probe: `frida\probe_key_bindings.js`.
- **The `nav_rva.h:247` formula `sel-0 slot = mapctrl.dbg_idx - 5140` is BROKEN** — not a constant
  offset (`getmapjumpposbyindex` implies 5142, `getmapjumpanglebyindex` implies 5140), because the
  .dbg symbol list interleaves variables/source-markers with actions. Resolve natives **by behaviour**,
  never by index arithmetic.
- **`distance` is native `0x0290` → `FUN_003448f0` (RVA `0x2248F0`), NOT `0x028f` (Session 106).**
  Worked example of the rule above: the generated name table pairs "distance" with index `0x028f`
  (`FUN_003482f0`, a two-line wrapper round the bitmask setter `FUN_00379010` — plainly not a
  distance), while map 568's `rrp_a02.ebp` contains **8 `CALLACT 0x0290` sites and zero `0x028f`**,
  and the `0x0290` handler measures exactly what the script needs. **Off by one SLOT; the bytecode
  and the handler body outrank the name table.** Body: pops two coords + an actor id (`FUN_00267db0`
  ×2, `FUN_00267e10`), resolves the actor via `FUN_00264010` → `FUN_00265060` (returns the actor's
  `+0xB8` transform — the same offset the entity scan reads), measures with **`FUN_004686d0` =
  `sqrtf(dx*dx + dz*dz)`, a HORIZONTAL distance**, and stores the result through **`FUN_0026b4c0`**:
  `base = *(u64*)(ctx+0xA8)`, `idx = *(i8*)(ctx+0x11)`, stride `0x28`, **value at `+0xC`, type tag
  `3` at `+0x15`**. Whether that word holds float BITS or a converted int is not settled by the
  decompile; `sneak_assist.cpp` therefore decides per call from the stored value's own magnitude and
  logs which reading it saw. Consumed by SNEAK ASSIST (`src/navigation/sneak_assist.h`) — the mod's
  second write-category exception, user-authorized, ~~default OFF~~ **always on, danger-table maps
  only (Session 115; the toggle and its `F10` key were removed after S113 was play-confirmed).**

- **`FUN_002677f0` (RVA `0x1477F0`) — "is the party LEADER inside THIS object's volume?" — is the
  SHARED CHOKE POINT under every trigger native (Sessions 113 + 115).** Leader via `FUN_003590d0` /
  `FUN_003588b0`, party-slot bit `1 << leader[0x12]`, tested against a mask at
  `*(object+0xB8) + 0x60 | 0x100 | 0x228`. **`object` is param_1**, which is what lets the mod answer
  it per OBJECT instead of per map. **THREE distinct native handlers funnel into it, and none of them
  has any caller of its own — they are dispatched from the native table:**
  `FUN_0033fa40` (`0x26D`, the instant touch test), `FUN_003407c0` / `FUN_00340bc0` (`0x525`, the
  waiting one), and **`FUN_0033f680`** (`FUN_00267e10` → `FUN_002677f0` → `FUN_0026b4e0`, added
  S115). Hooking `FUN_002677f0` therefore covers every native that reaches it, whichever slot the VM
  dispatches — ~~which is why map 569 needed only a table row and no new mechanism.~~
  > **STRUCK (Session 117).** It covers every NATIVE, and map 569 calls none of them: `rrp_a03`
  > contains zero `0x26D` and zero `0x525`, and S116's play produced zero touch lines of either kind
  > there. **A choke point is only a choke point for the paths that reach it.** 569's catch is the
  > ENGINE-side trigger update below, which never enters the script VM at all.

- **`FUN_0025c830(container, object)` (RVA `0x13C830`) — THE PER-OBJECT TRIGGER-VOLUME UPDATE, and the
  writer of the mask `FUN_002677f0` reads (Session 117).** Confidence 0.98. Single caller
  (`FUN_0025c230`'s object loop); return value used nowhere. It zeroes `*(u32*)(*(object+0xB8)+0x60)`
  at entry, walks the FOUR party actors at `DAT_0209a1f0` against the volume (OBB math via
  `FUN_0025a8e0` / `FUN_003da5a0`), ORs each occupying slot's bit back into that word, and then fires
  the object's own routines through `FUN_003dbb60`:
  | trigger | condition | routine slot on the object |
  |---|---|---|
  | kind **4** — ON ENTER | mask was 0, is now non-zero | `+0xD0` (fallback: object data `+0x0A`) |
  | kind **2** — ON LEAVE | mask was non-zero, is now 0 | `+0xD2` (fallback `+0x0C`) |
  | kind **3** | leader inside, mode bit 8 of `object+0x1C` | `+0xD8` / `+0xCE` |
  | kind **6** | leader outside, mode bit 13 | `+0xE2` (fallback `+0x1C`) |
  Gates read off the volume: **`object+0x8` bit 5 = LEADER ONLY** (native `0x26E`), **`object+0xC`
  bit 5 = only while an event is active** (native `0x3DF`), `object+0xB` bits 0/1 (natives `0x40A` /
  `0x409`). The ENTER branch continues into `FUN_00227420` / `FUN_002e1cd0(0,0xd)` / `FUN_00268530(2)`
  — handing the field to a scripted scene. **That is map 569's capture.**
  **CLASS `+0x18 == 1` (script-created objects — rects) NEVER REACHES `FUN_003dbb60` (Session 119,
  confidence 0.95):** the ENTER branch returns before the call and the kind-3/6 branches guard it
  out. For those objects the update's only outputs are the inside-mask, the `object+0xC` bits, and
  the notification registers **`FUN_003df760`** writes (`DAT_02b59c40/c80/cc0/ce0`, 4 parallel
  arrays indexed by slot, slots 1/2 here) for the script to poll. **Any suppression aimed at the
  fire path cannot reach that class — suppress at this function (the writer), which every read path
  shares.** Consumed by SNEAK ASSIST S119: guard-object and capture-named volumes are skipped here.

- **`FUN_003dbb60(object, kind, routineIdx, mode, flag) -> int` (RVA `0x2BBB60`) — START A SCRIPT
  ROUTINE ON AN OBJECT (Session 117).** Confidence 0.98. Builds an 8-byte event record
  `{mode, 1, kind, routineIdx:u16, 0x8000, flags}` and hands it to `FUN_003dbcf0(object, &rec, 1)`.
  ~~`FUN_003dbcf0` bounds the index against the container's ROUTINE COUNT, which establishes that
  `routineIdx` indexes the routine table.~~
  > **STRUCK (Session 118) — the S117 half-sentence above cost a play session. THE INDEX IS
  > OBJECT-LOCAL.** The bound `**(u32**)(object+0x48) <= idx` is against the count of the object's
  > OWN EVENT TABLE at `object+0x48`: `[count:u32][8-byte records]`, whose entry
  > `*(u32*)(tbl+4+idx*8)` is a **NAME-POOL OFFSET** — fed to `FUN_00263e40(blob, x) =
  > blob + x + *(u32*)(blob+0x4C)`, with the blob resolved per object via `FUN_00263ff0(obj[0x15])`
  > = `HANDLE_TABLE_BASE + id*0x288`. The dispatch (`FUN_003db7a0`) matches the fired index against
  > the object's active-slot table at `+0xA0`, same index space. Play evidence: objects fired
  > CONSECUTIVE SMALL indices (1,2,3 / 2,3,4 / 3,4,5) whose routine-table "names" were `setup` and
  > the map's resident director — impossible for trigger volumes. Resolve a fire's name ONLY via
  > `MapScript::FiredRoutineName`.
  Returns 1 when the caller should continue (`FUN_0025c830` bails on anything else); **2** is the
  engine's own "no event slot free". **~20 call sites and they are NOT all trigger volumes:**
  `FUN_00269640` / `FUN_00269860` start conversation events, and `FUN_00269a90` / `FUN_00269ba0` /
  `FUN_00266530` / `FUN_00266c50` are script-side event calls reached with a `param_5` the volume path
  never passes. The `kind` byte does not separate them — it is a per-object event-slot selector and
  the ranges overlap. **The only sound discriminator is the CALLER**, which is why sneak assist hooks
  `FUN_0025c830` as a scope marker rather than filtering on `kind`. Consumed by SNEAK ASSIST, which
  declines a trigger-volume fire whose routine the map named `捕獲`; see
  `src/navigation/sneak_assist.h`.

- **WHICH NATIVE TABLE DUMP TO BELIEVE (Session 117, and one wrong claim came out of the other).**
  `..\FFXII-Decompile\output\action_binding_tables.txt` is the **VALIDATED** CALLACT table: selector 0,
  `0x1EED700`, stride `0x20`, `enter@+0x08 exec@+0x10 poll@+0x20`, and it independently reproduces
  every previously-established fact (`0x08D` exec `FUN_00355350` mapjump, `0x290` poll `FUN_003448f0`
  distance, `0x525` enter/exec `FUN_003407c0`/`FUN_00340bc0`, `0x26D` poll `FUN_0033fa40`).
  `script_native_table.txt` is a DIFFERENT table (base `0x1eee448`, stride 8) whose `.dbg` name join
  runs through a measured delta, and whose own self-check prints
  `VERDICT: NOT COHERENT -- do not use any id above`. **Do not quote a name or a handler from it.**

- **The ROYAL PALACE capture census (Session 115) — the palace's danger table is COMPLETE.** Maps
  567–572 are scripts `rrp_a01`..`rrp_a06`; each `.ebp` scanned for capture-routine name strings and
  for `CALLACT` operands. **Only 568 (`rrp_a02`: 1 capture routine `ヴァン捕獲`; `0x290` ×8, `0x26D`
  ×4, `0x525` ×21) and 569 (`rrp_a03`: 12 capture routines incl. `捕獲レクトＡ/Ｂ/Ｃ`,
  `捕獲レクト兵士０１..０７`, `捕獲監視監督`; `0x290` ×4) run a capture sequence at all.** 567, 570,
  571 and 572 contain zero. Confidence 0.99 (two independent signals agreeing; no inference between
  them), and the counts were **independently reproduced from the `.ebp` files in Session 117** — 568's
  1 capture name and 569's 12, and every CALLACT count above.
  > ~~569 also uses `seteventwakerect` (`0x3DF` → `FUN_0034d470`) ×70 and an unnamed `0x26E` ×77,
  > neither yet resolved to a handler — those are the first two places to look.~~
  > **STRUCK (Session 117): BOTH THE HANDLER AND THE NAME WERE WRONG, and the lead was a dead end.**
  > `0x3DF`'s CALLACT handler is `FUN_0034dca0` (→ `FUN_0026a660`), not `FUN_0034d470`; the name
  > "seteventwakerect" came from `script_native_table.txt`'s `.dbg` join, which that file's own output
  > declares incoherent. `0x3DF`, `0x26E` (`FUN_00341000`) and the two that appear ×70 beside them,
  > `0x409` (`FUN_0033f1e0`) and `0x40A` (`FUN_0033f650`), are **all four flag SETTERS on the volume**
  > — `object+0xC` bit 5, `object+0x8` bit 5, `object+0xB` bits 1 and 0 — read by `FUN_0025c830`
  > above. None of them tests anything. **A name from an unvalidated join is a guess wearing a label**,
  > and this one was quoted across three documents as the mechanism to hook.

~~**STILL OPEN:** the `meswin` field dialogue window + multi-page pagination … Best unverified lead:
`FUN_003cb650` (RVA `0x2AB650`) case 1 vs case 0x20 … **Unverified — do not ship.**~~
> **STRUCK (Session 91) — the lead was never needed, and pagination is SOLVED without it.**
> `FUN_003cb650`'s window is a *different* singleton (`DAT_02b47760`, 0x179E0 B) whose text is a
> pre-compiled glyph resource we cannot decode (sessions_001_050.md, Session 13), so even a correct
> advance event there would carry no readable page. **The page is a CURSOR, not an event to catch:**
> `widget+0x8A` on the message widget the telop already fills, written by `FUN_002a8c50` — see
> "PAGINATION — the message widget's own page cursor" at the top of this file. Do not re-derive
> `FUN_003cb650`; three documents carried it as "the best lead" for six sessions while the answer sat
> on an offset `choice_reader.cpp` was already reading in play.

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

**`+0x70` GROUP SEMANTICS — measured Session 92.** The groups are not interchangeable, and treating
them as one pool is what mis-tagged a gate crystal as a door. Full table dumped on **map 306**
(Rabanastre, 4 records) with the entity list beside it, corroborated on map 12 (0 records) and map 702
(28 records):

| group | holds | evidence | conf |
|---|---|---|---|
| **0** | **press-Enter DOORWAYS** — the objects `[`/`]` lists and you open with Enter | `g0[0]` `destIdx=20` at (119.95,−10,127.00) ↔ `"South Gate"` (124.00,−10,127.00); `g0[1]` `destIdx=21` ↔ `"Lowtown"`. Non-zero `destIdx` (indexes the `+0x8c` dest table); map 702 has 4 live + 20 all-zero slots | **0.98** |
| **1** | **walk-onto MAP-JUMP surface** — the `Category::Exit` source | `g1[0]` (112.12,−10,198.00) is the **only** record on map 306 whose `areaId` resolves (**14**), and it lands inside the exit surface bbox `x[112..136] z[198..225]` | **0.98** |
| **2** | **a GATE CRYSTAL's own teleport record** | `g2[0]` (115.00,−10,151.00) sits at **0.00 m** from `"Rabanastre Crystal"` (115.00,−10,151.00), `destIdx=0` | 0.90 — **n=1** |
| **3** | **arrival markers** (where the party lands coming in) | map 702: two coincident pairs, `destIdx=0`; matches the pre-existing East End note | 0.95 |

**A group-0 record is NOT co-located with its doorway — it is 3.5–6.4 m away**, because the record
marks the "→ area" ARROW and not the thing you press: 4.05 m and 3.50 m on map 306, 3.6–6.4 m already
recorded on East End, against ~24–25 m to the next-nearest candidate. So the object↔record binding is
**nearest-wins per record**, never a radius test — a `2.5 m` radius silently missed both of
Rabanastre's gates. Group 0 is a **fixed-size array with unused slots reading exactly `(0,0,0)`**;
filter on the position, not on `shown` (a live render gate — "the arrow is being drawn this instant" —
so tagging would otherwise depend on where the camera points).

Only the group-0 row is load-bearing for the shipped Door/Shop split; groups 1–3 are needed there
merely as **"not a press-Enter doorway"**, which each row clears comfortably. The 0.90 group-2
identification is explanatory and nothing is built on it.

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
- **⭐ `cell+0x18` IS THE PURCHASABILITY WORD — it is what the Confirm handler branches on** (S149).
  `FUN_0055cd40` case `0xc` / sub-message **`0x8001`** is Confirm; `param_2[4]` is the focused cell,
  the same pointer the `0x8000` focus hands the reader. It tests only:

  | bit | meaning | set by |
  |---|---|---|
  | `0x1000` | **prerequisites met** — the cell touches a learned node | `FUN_0055e090` (0x43E090) |
  | `0x2000` | learned | `FUN_0055bff0` status-1 arm (`&0xfffff55f \| 0x3550`) |
  | `0x4000` | affordable (`memberBlock+0xB0` >= cost) | `FUN_0055bff0` status-0 arm |
  | `0x8000` | not learned AND not yet reachable | `FUN_0055bff0` status-0/2 arms |

  ```c
  if (((f & 0x2000) == 0) && ((f >> 0xc & 1) != 0)) {          // not learned AND reachable
      if ((f & 0x6000) != 0) { ...purchase confirm...; FUN_00249c60(0x25); return; }
      FUN_002ce2f0(board, 10);                                 // the not-enough-LP popup
  }
  FUN_00249c60(5);                                             // the invalid-action sound
  ```

  **`FUN_0055e090` is the adjacency flood**: for every cell with `0x2000` it visits the four
  orthogonal neighbours and, where `0x8000` is set and `0x1000` clear, clears `0x8000` and sets
  `0x1000`. It runs inside `FUN_0055bff0` **before** that builder's closing
  `FUN_00247510(board,0x8000,firstCell)`, so the bits are already correct at the first focus.
  Bits 4-7 / 8-11 are the icon's target-vs-displayed animation nibbles (the builder's second pass
  diffs them); bits 12-15 are cleared by `&0xffff0fff` before the status switch sets them.
- **Node status** `FUN_00323600(charId,node,0)` (0x203600) → `FUN_00323d10` (0x203D10):
  `1` learned · `2` not enough LP (`charBlock+0x190` < cost) · `0`/`9` can learn · `3/4/5/8` locked ·
  `6` null char · `7` invalid panel. **⚠ IT HAS NO ADJACENCY TEST** — every branch of its 68-line
  body is visible and none of them asks whether the node can be reached, so `0`/`9` means "you own
  the LP", NOT "you may buy this". Reading it as availability is what made the mod announce "can
  learn" on nodes the game then buzzed (S149). Kept in `license_reader.cpp` as a **log-only**
  cross-check beside the flags; nothing spoken depends on it.
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

## Shop — Buy / Sell / Bazaar + quantity + gil (Session 69, 2026-07-24) — SHIPPED, play-confirmed

Buy, Sell and Bazaar share ONE class pair, built by `FUN_0056dd50`: a **container** `FUN_0056e140`
(**0x44E140**) holding an **item panel** `FUN_0056d370` (**0x44D370**). The top Buy/Sell/Bazaar menu is the
shared equip class `FUN_0057b890` (0x45B890), already announced via the `ROW_CHAIN` label offset; the
per-item description already reaches `o`. Reader: `src/ui/shop_reader.{h,cpp}`.

**Item read — hook `FUN_0056e5d0` (0x44E5D0)**, the container highlight/refresh handler. It runs on EVERY
Buy AND Sell cursor move and sets the tooltip (why `o` works on Sell). NOTE: Sell's "sideways" grid
refreshes here via container **msg 0x13**, NOT via `FUN_00247510` 0x8000 — a 0x8000-only reader sees Buy
but never Sell (offline "Sell = same 0x8000 path" inference STRUCK). Reads:
`panel = *(container+0xC0)`, `rows = *(panel+0xC8)`, `scroll = *(container+0xD8)`;
`idx = (s8)scroll[0xED] + ((s16)scroll[0xF4] + (s16)scroll[0xF2]) * (u8)scroll[0xEC] + (s8)scroll[0xEE]`;
`row = rows + idx*0x20`:
- **name codec @ row+0x00, id (u16) @ +0x08, owned/INVENTORY (u16) @ +0x0E, price (u32 `&0x7FFFFFFF`) @ +0x10** (sell price pre-halved), stride 0x20.
- Buy/Sell discriminator (if needed): `container+0x194` bit0 (1=Sell). Controller option `*(DAT_02ca9790+0xC4)` reads 0xFF while browsing (unusable mid-browse). All ≥0.98 (Frida-confirmed: names decode, prices/inventory match, Potion owned=5).

**Quantity selector — panel `FUN_0056d370` (per-frame proc, 2nd hook).** After confirming an item:
`panel+0xE4` bit1 = quantity mode; **qty (u16) @ +0xDC, max (u16) @ +0xDE, gil snapshot (u32) @ +0xE0,
selected row ptr @ +0xD0** (id @ row+8, price @ row+0x10). Total = `(price&0x7FFFFFFF)*qty`. **+1/+10 step =
`panel+0xE4` bit 0x400000** (set = ×10) — **PROVISIONAL** (one live sample + play-confirmed as "1x"/"10x").

**Shop globals:** controller (top menu) `DAT_02ca9790` = **RVA 0x2B89790**; live list container
`DAT_02ca9798` = **RVA 0x2B89798**. (Earlier "0x1AA9790" was bad arithmetic: abs 0x2ca9790 − 0x120000 =
0x2b89790.)

**Gil:** getter `FUN_00253690` = `*(u32*)(DAT_02092758 + 8)`, `DAT_02092758` = **RVA 0x1F72758**. Read as a
pure two-step deref (base = *0x1F72758; gil = *(u32)(base+8)) — CONFIRMED live == 99999999. `GilReader` (the
`g` key) uses this off the game thread (memory-only, like `U`).

## Pause item lists — quantity + category tabs (Session 70, 2026-07-24) — PROBE-CONFIRMED

**The pause item lists, the Equipment list and the Shop are ONE tabbed-container family.** Same row
record, same `+0x180` bitfield, same nav primitives, same refresh. Reader: `src/ui/inventory_reader.{h,cpp}`.

Four window classes observed: `0x4436F0` (weapons/armor/accessories), `0x443930` (**ITEMS and LOOT**),
`0x443B10` (magicks·technicks), `0x443D20` (**KEY ITEMS**). **One class serves several screens**, so
membership is decided by STRUCT SHAPE — `+0xE0` row array, `+0xD8` scroll widget, `+0xE8` tab table,
sane row count — never by a class table.

**Container layout** (`FUN_00564010`, `FUN_005655f0`, `FUN_00564e10`):
- `+0x60` display-node array; caption node = `*(disp + (v180 & 0xFF)*8)`, its text ptr at `node+0x18`
- `+0xC8` tab-strip widget (`FUN_00567200`, 0x447200) — **baked sprite art, NOT text**
- `+0xD8` scroll/cursor grid (index quintet `0xEC/0xED/0xEE/0xF2/0xF4`; **row count u16 at `+0xE8`**)
- **`+0xE0` row array** (null = empty list), `+0xE8` tab table, `+0xF0 + i*8` per-tab state
  (**srcIdx s8 @ +6**, catId s8 @ +7), `+0x17C` selected row
- **`+0x180`: bits[20:16] = tab COUNT · bits[25:21] = tab INDEX · bit8 = use-on-target sub-mode**
  (idioms `(v<<0xb)>>0x1b` and `(v<<6)>>0x1b`; next `+0x200000`, wrap `& 0xfc1fffff`)

**Row record — stride 0x20, byte-identical to the shop row** (`FUN_0057dad0:19-56`). The shop merely
holds it elsewhere: `FUN_0056e410:54` does `*(shopPanel+0xC8) = *(container+0xE0)`.
- `+0x00` name codec · `+0x08` id (u16) · `+0x0A` icon · **`+0x0E` owned QUANTITY (u16)** · `+0x10` price/flags
- **`0xFFFF` is the empty sentinel; id `0x0` is a VALID item** (Potion). Confirmed live: `Potion 5`, `Rat Pelt 3`.
- **`+0x0C` — MEANING UNKNOWN. STRUCK as "equipped count"**: a Dagger *equipped to Vaan* read 0.
  Set from `master+0x20` when `param_6 & 8`, else `0xFFFF`; `FUN_00563560:54-59` draws `+0x0E − +0x0C`
  and `+0x0E`. Every row observed had `+0x0E==1, +0x0C==0`, so nothing discriminates. **Never spoken.**

**Category switching.** `FUN_00564e10` (**0x444E10**) masks the pad with **`0xCA0` = L1|R1|LEFT|RIGHT**
— **LEFT is hard-aliased to L1, RIGHT to R1** (measured: `held=0x20(RIGHT)` / `0x80(LEFT)`; pad words
`DAT_02f97368/6a/6c` = RVA `0x2E77368/6A/6C`, filled by `FUN_002498b0` 0x1298B0). Two reasons
Left/Right legitimately do NOTHING: **tab count < 2** (`:22-23`, early return) or **`+0x180 & 0x100`**
(`FUN_00564c80:13-18` routes to `FUN_00564d30`, which has no L/R). Observed: ITEMS/KEY ITEMS/LOOT/
ACCESSORIES 1 tab (silent); WEAPONS 2, ARMOR 3, MAGICKS·TECHNICKS 2 (active).

Prev/next = **`FUN_00563ec0` (0x443EC0)** / **`FUN_00564300` (0x444300)** — a **CLOSED SET**: exactly
three call sites in the whole dump — `FUN_00564e10` (items/abilities), `FUN_003fdad0` (**0x2DDAD0**,
equipment), `FUN_0056ded0` (**0x44DED0**, shop).

**Unified refresh — `FUN_005655f0` (0x4455F0)**, called on screen OPEN *and* every category change by
all three families (6 sites). **Hook it on ENTRY**: `FUN_00564010:56-70` populates the tab table and
`+0x180` before calling it, and its own `FUN_002d47c0:15-20` re-fires `FUN_00247510(child, 0x8000, idx)`
*during* the call — so announcing on entry yields category-then-item, on exit the reverse.
Category name = `FUN_002f9860( *(u32*)( *(win+0xE8) + srcIdx*8 + 8 ) )`, `srcIdx = *(s8*)(win+0xF6+tabIdx*8)`
clamped at 0. Two independent read paths (this, vs reading back `node+0x18`) **agreed 14/14** live:
`ITEMS`, `LOOT`, `KEY ITEMS`, `WEAPONS`, `ONE-HANDED WEAPONS`, `ARMOR`, `HELMS`, `CHEST PIECES`,
`ACCESSORIES`, `MAGICKS . TECHNICKS`, `TECHNICKS` (stored ALL CAPS). `TextCapture` already hooks
`FUN_002f9860`, so the cache serves the common path with no game call.

**~~Empty categories do not occur~~ — STRUCK (Session 89, 2026-07-29).** The old claim was that the
`gateId` at `entry+6` filters them out before they are tabbed (every `[cat]` had n≥1) — that was a
property of the SAMPLE, not of the game. **SHIELDS with no shield owned is tabbed, reachable, and
empty**, measured in play on equip container `0x2BCF6400`.

An empty category is built, not filtered: `FUN_0057cf20:253-257` frees its buffer and returns NULL
when the built count is 0, so `FUN_005655f0:42` stores **null into `+0xE0`**. `FUN_005655f0:49-52`
then **clamps the count handed to the scroll widget from 0 to 1**, so `scroll+0xE8` still reads **1**
and the widget reports a row at index 0 that does not exist. Anything reading that cell gets the
PREVIOUS category's paint. Reader consequence: a null `+0xE0` (with the scroll widget and tab table
still present) is the ONLY signal that a category is empty — `inventory_reader.cpp` `IsEmptyCategory`
claims that focus and stays silent so the generic painted-cell path cannot speak the stale row.

**Entering a one-item list moves no cursor**, so a 0x8000-only reader is silent there — the
`FUN_005655f0` hook is what covers it.

### The equipment CANDIDATE list, and the OFF-HAND's cursor HOST (Session 151, 2026-08-11) — PLAY-CONFIRMED

**The candidate-item list** ("which weapon / shield / helm?") is class **`0x2DDFE0`
(`FUN_003fdfe0`)**, parked at `menuCtx+0x150`, one instance serving every slot. It is a normal member
of the tabbed family above (`+0xE0` rows, `+0xD8` scroll, `+0xE8` tab table). Its `0x8000` branch
indexes **`val * 0x20 + container[+0xE0]`** and reads the item id at **`+0x08`** — the same record
the readers walk. Slot index is `container+0x17E`; `FUN_003fd360` sets `category = slot + 0x40`, so
the off-hand is **`0x41`**. The 5 equipped ids live at `DAT_0209ac30 + 0xD48 + slot*0x20`.

**⚠ SLOT 1 (OFF-HAND) IS BUILT DOWN A DIFFERENT BRANCH, AND ITS CURSOR IS NOT ITS OWN.**
`FUN_003fdfe0:31-36` dispatches `slot == 1` → `FUN_003fd860`, everything else → `FUN_003fd6b0`:

| | cursor widget comes from | notify target (`widget+0xC8`) |
|---|---|---|
| every other slot (`FUN_003fd6b0:38-40`) | the container's own scene subtree | **the container** — the pane holding the cursor |
| **off-hand** (`FUN_003fd860:33-38`) | an intermediate HOST created by `FUN_00244f50(200, FUN_003fd1d0, 0)` and parked at **`container+0xC0`** | **that host** |

`FUN_002d47c0:15-16` (and its siblings `FUN_002d4650`/`46f0`/`4840`) send `FUN_00247510(widget+0xC8,
0x8000, cell)`, so **the off-hand's focus messages are addressed to the host, never to the list**.
`FUN_003fd1d0:41-47` forwards every category-`0xC` message to its parent's handler **as a direct
call, not another `FUN_00247510`** — hence exactly ONE observable focus message per move, carrying
the host, with `val` unchanged.

Reader consequence: an active-pane gate keyed on the message owner drops every off-hand cursor move,
and a menu-entry stash keyed on the owner can never be replayed against the entered pane. Both were
live defects. `inventory_reader.cpp` `IsCursorHost(cursorPane, host)` pairs them: class `0x2DDFE0`
**and** `cursorPane[+0xC0] == host`. **`+0xC0` is written only by the off-hand path**, so nothing
else in the family matches.

**STRUCK as the cause of the off-hand's silence: `FUN_0057cf20` case `0x41`'s two-pool
shields+ammunition merge.** The merge is real — it is why one list holds both pools — but it decides
which ROWS the list holds, never who is told about the cursor. Also **STRUCK: "empty vs equipped"**
(an unequipped helm reads correctly; the off-hand fails with a shield equipped) and **"a different
window class"** (one instance serves `SHIELDS` and `WEAPONS`).

### The SHOP's route into that refresh (Session 88, 2026-07-29) — LOG-CONFIRMED

`:2178` says "all three families (6 sites)" but never named the shop's route, which made the shop
look uncovered and nearly bought a second, duplicate category reader. It is:

```
FUN_0056ded0:82 (shop L/R handler)  ->  FUN_0056e410:50  ->  FUN_005655f0   <- category announced here
FUN_0056ded0:83                     ->  FUN_0056e5d0                        <- shop row announced here
```

So a shop tab change announces the category and then, **~0.2 ms later on the same call stack**, the
highlighted row. Both are ours; see the interrupt note in `debug.md`. Confirmed live for six
categories on shop container `0x2BED9000`: `WEAPONS`, `ARMOR`, `ACCESSORIES`, `ITEMS`, `LOOT`,
`AMMUNITION`, plus `ARMOR`/`ACCESSORIES`/`AMMUNITION` from `probe_shop_category.js` on the Buy side
(`+0x194 & 1` = sell; help string = `FUN_002f9860(0xC6B + sell*2)`, `FUN_0056e5d0:20`).

**TRAP — the per-tab record at `+0xF0 + tab*8` holds TWO different source indices** (`:2155`):
`s8 @ +6` = **label** index into the `+0xE8` tab table (`FUN_005655f0:19`), `s8 @ +7` = **category
code**, which the shop pushes through `FUN_0056dc30` into `container+0x191` (`FUN_0056ded0:78-80`).
They are not interchangeable. In a shop stocking every category they happen to equal `tabIdx` and
each other, which is exactly how a probe can "confirm" the wrong one. Use `+6` for the name.

**Not a pointer:** `container + (pos + 0x1E)*8` *is* `container + 0xF0 + pos*8` — the address of the
per-tab record itself, handed to `FUN_005674b0` (`FUN_0056ded0:76-77`) to restore that tab's saved
cursor. Dereferencing it yields the record's own bytes and looks like a garbage pointer.

## Status screen — `FUN_002c2320` (0x1A2320) (Session 71, 2026-07-24) — SHIPPED, PLAY-CONFIRMED

**KEYWORDS: status screen attributes reader FUN_002c2320 0x1A2320 cmd 0x4b4 0x4b6 equipment shared
container menuCtx+0x140 menuCtx+0x138 FUN_003fe5d0 attribute labels 0x4A90 member block 0xAC8 EXP LP
next FUN_0057edb0 0x45EDB0 save load STRUCK**

### STRUCK: `FUN_0057edb0` (RVA `0x45EDB0`) is **NOT** the status screen — it is the SAVE/LOAD detail pane

`src/ui/status_reader.cpp` was written against `0x45EDB0` on the belief that it was the status
Attributes page. **It is not.** Evidence (confidence **0.99**):

- `00583040_FUN_00583040.c:37-53` scans a **200-entry save-slot table** (`FUN_003bb3e0`), gated on a
  save-vs-load flag at `+0xC1`, and only creates `FUN_0057edb0` (`:90`) when a usable slot exists —
  as a sibling of the slot list `FUN_0057fe80` (`:100`).
- Its seven sub-panels read a **4-byte-per-member packed preview record**
  (`0057e6d0_FUN_0057e6d0.c:20,39,46`: member id / `0xFF` = hide / level clamped to 99 / two nibble
  gauges / flag bits) — save-file preview shape, not stat-page shape.
- `FUN_0057edb0` handles only categories `1`, `0xf`, `0x12`, default. **There is no `0x13` case** —
  the `CAT_SHOW = 0x13` constant in `status_reader.cpp:36` was copied from `FUN_00280de0` (the field
  pause command column) and never applied to this function.

Do not re-derive this. Anything hooking `0x45EDB0` reads the save screen.

### The real chain

| Fact | Value | Conf |
|---|---|---|
| Pause command id | Status `0x4b4` (label `0xcc7`), Equipment `0x4b6` — `00281ae0_FUN_00281ae0.c:38-71`, `labelId == cmdId + 0x813` | 0.98 |
| Dispatch | `FUN_00281ed0` (0x161ED0) `:40-58` — both ids → `FUN_002c2280` → `FUN_002c2320` | 0.98 |
| **Controller** | **`FUN_002c2320`, RVA `0x1A2320`**, parked at `menuCtx+0x140` | 0.98 |
| **Status-vs-Equip gate** | `*(int*)(ctrl+0x160)` = the command id; `*(u32*)(ctrl+0x168)` bit0 SET ⇒ Equipment, CLEAR ⇒ Status (`002c2320_FUN_002c2320.c:63-64`) | 0.98 |
| Selected character | `*(i16*)(menuCtx+0xDE0)`, written by `FUN_00285f20`; same source the Equipment screen uses | 0.99 |
| Member block | `*(u64*)(menuCtx+0xAC8 + member*8)` (`FUN_00282df0`); blocks are inline at `menuCtx+0x408 + n*0xC0` (`FUN_00282dd0`), so the pointer is type-validatable by matching the array | 0.99 |
| Attribute panel | `FUN_003fe5d0` (0x2DE5D0) at `menuCtx+0x138`; fill `FUN_003fead0` (0x2DEAD0); rows `FUN_003fe490` (0x2DE490) | 0.99 |
| Ailment grid | `FUN_002c59d0` (0x1A59D0) at `menuCtx+0x110`, created on the status branch of cat `0xe` | 0.98 |

**Category map of `FUN_002c2320`** — `1` CREATE (sets `menuCtx+0x140`, `ctrl+0x160`, and calls
`FUN_00285f20(packet[2])`) · `8` focus lost · `9` focus gained (equip only) · `0xa` **PER-FRAME input**
(L1/R1 party cycling via `FUN_002c2240`/`FUN_002c2200` — never hang an announcement off this) ·
`0xc` child list event, sub-code at `packet+8` (`0x8000` cursor move, `0x8002` cancel tears the screen
down) · `0xe` ACTIVATE · `0x11f` enable/disable · `0x12` DESTROY · `0x13` suspend/resume.

**Member block fields** — all confirmed by two independent paths, the portrait draw `FUN_00283e40`
(`:170,198,205,207,209`) and the block fill `FUN_00329220` (`:225-229`):

```
blk+0x20 curHP(i32)  blk+0x24 maxHP(i32)  blk+0x2C curMP(i32)  blk+0x30 maxMP(i32)
blk+0x90 EXP(u32)    blk+0x94 Next(u32)   blk+0xB0 LP(u32)     blk+0xBA level(u8)
```

`Next` is already computed for us: `FUN_00329220:226-228` stores `FUN_002f8f20(level) - EXP`, so the
mod never needs to reimplement the threshold curve. The underlying BtlChr record
(`*(u64*)DAT_02ebf190 + 8 + idx*0x1C8`) holds EXP `+0x18C`, LP `+0x190`, level `+0x1C2` — but the
menu block already caches all of them, so **the status reader needs no save-record access at all**.

**The nine attributes** — value at `panel+0xC8 + row*4`, equip-preview at `panel+0xEC + row*4`
(`003fe490_FUN_003fe490.c:17-26`). On Status both are equal, so no change arrow is drawn.

**LABELS ARE GAME-SUPPLIED: `FUN_002f9860(0x4A90 + row)` for rows 0-8** (`003fe490:21`). Nothing about
the attribute names needs hardcoding. **All nine decoded live and matched the screen exactly:**
0 `Attack Power`, 1 `Defense`, 2 `Magick Resist`, 3 `Evade`, 4 `Magick Evade`, 5 `Strength`,
6 `Magick Power`, 7 `Vitality`, 8 `Speed`. (The pre-probe order guess was right; rows 5-8 were only
0.92 offline and are now 1.0.) Values matched too — Vaan Lv1: `14 5 5 5 0 23 22 24 24`, HP 128/128,
MP 30/30, EXP 31, Next 20, LP 99999.

**Categories, measured.** A Status visit delivers `0x6, 0xe, 0x8, 0x1, 0xb, 0x9, 0x13` **exactly once
each**, then repeats `0x2/0x19/0x3/0x4` per frame, and ends with **`0x12` on close** (confirmed on the
second run). **Cat `0x1` is the activation edge** — the one one-shot at which `menuCtx+0x138` is
already built; at `0xe` the panel is still NULL. Anything hung off `0x2/0x3/0x4/0x19` is per-frame.

### The other two pages — Magicks and Technicks (Session 71)

The Status screen has **three** pages selected by the mode byte **`menuCtx+0xDE7`**: `0` = Attributes,
`1` (or `3`) = Magicks, `2` = Technicks/Quickenings/Remedy Lore/Espers. Measured cycling 0→1→2→0.

**PAGE-CHANGE EVENT — `FUN_002c1a80` (RVA `0x1A1A80`).** This is the overlay page state machine and
the **only writer of `menuCtx+0xDE7` in the whole binary**, so every page change passes through it:
`:24` sets 1 (Magicks opened), `:42` sets 2 (Technicks page), **`:87` sets 0 — closed BACK to the
Attributes page**. Its `int` return classifies the event, which is what makes it usable as a hook
without any per-frame work:

| ret | meaning |
|---|---|
| `1` | opened / switched page, or the open page consumed the input |
| **`2`** | **closed back to the Attributes page** (the "dropped back to the status screen" event) |
| `0` / `-1` | nothing happened (page-1 idle / overlay idle) |

Signature `int FUN_002c1a80(block, edge, mask, held, rpt)`. It is **shared with the license board's
`F` overlay** (reached via `FUN_0055c740`), so any handler must gate on "the Status screen is open".
Note `:64` returns `-1` every frame while an overlay is open, so gate on `ret == 1 || ret == 2`, not
on `ret != 0`.

> **STRUCK (Session 71, same session):** *"there is no event for mode 2→0; the reader can only report
> position when a nav key is pressed."* Wrong — the function above fires on exactly that transition.
> The claim was made without checking the probe trace already in hand, in which the mode-transition
> lines were emitted from inside the container's own message handler. A game does not return from a
> submenu and wait for input to refresh; do not assume it does.

Pages 2 and 3 are the **same two page objects** the license board's `F` overlay uses, **and they
behave identically here — a real browsable in-game cursor**. So `ability_summary_reader` reads them
on this screen exactly as it does from the board, and `status_reader` covers only page 1, declining
every key while `menuCtx+0xDE7 != 0`.

> **STRUCK (Session 71):** ~~"on the Status screen pages 2/3 are static displays with no browsable
> cursor, so the reader must enumerate every slot into a virtual buffer"~~. That enumeration was
> built and then removed — the pages are cursor lists. Do not rebuild it.

Both objects are parked in menuCtx (useful for reading them without a hook):

| Page | Parked at | `obj[0]` class | Entries | Count |
|---|---|---|---|---|
| Magicks | **`menuCtx+0x120`** | **`+0x1A3560`** | `obj+0x0C8` | 81 |
| Technicks etc. | **`menuCtx+0x128`** | **`+0x1A4460`** | `obj+0x0E0` | 54 |

Entry stride `0x20`: name codec `+0x00` (the game's own `?` when unlearned), description `+0x10`,
flags `+0x18` (bit `0x20000` = learned/bright). Section headings read from
`*(*(obj+0x60) + {0x28,0x40,0x58,0x70}) + 0x18` and decoded live as **`Technicks`, `Quickenings`,
`Remedy Lore`, `Espers`** — note **`Quickenings`, not "Mist"**, which the old
`ability_summary_reader.h` comment claimed.

**The current section is `obj+0x7A4` (u16, 0..3) — read it, do not partition the array.** The entry
records carry no section field, and a measured slot partition `{0,24,27,41}` was derived for the
removed enumeration; the game's own index is authoritative and survives content changes, so readers
follow it. **The heading must be announced on every section CROSSING**, not only on a page switch:
without that, moving from Technicks into Remedy Lore just says "Blind" and reads as the mod reporting
the wrong thing (tester, Session 71). Observed slot layout on a fresh file, for reference:
`named[0-0] placeholder[1-26] named[27-40] placeholder[41-52] hidden[53-53]`.

**Status effects.** Grid `FUN_002c59d0` at `menuCtx+0x110`; slot *i* holds an id at
`grid+0xC8+i*4` (i8, `<0` = unused) and a bright flag at `grid+0xCA+i*4`. **Name =
`FUN_0035d330(0x1A, id)` -> record, codec `+0x18`** (`002c5900_FUN_002c5900.c:32-33`). The
**"(No status effects.)" sentence is LAYOUT ART, not a message id** — `FUN_002c5900` merely hides the
rows when nothing is set (`filter(+0x148)=0`, 0 live slots measured), so it cannot be read back and
the reader omits the group instead of fabricating a line.

**STRUCK (measured):** the row-chain entry `{ 0x1A2320, 0xC8 }` at `ingame_menu_reader.cpp:29`,
commented *"inventory category tab bar"*, does **not** fire on the Status screen — across a whole
session the only `FUN_00247510` msg `0x8000` seen with Status open came from owner class `+0x1a59d0`
(the ailment grid), never from `+0x1a2320`. So the generic row-chain reader does not claim this
window and there is no double-speak. The comment is still a misnomer worth correcting if that entry
is ever revisited.

## Ground loot — the drop pool (Session 72, 2026-07-24) — SHIPPED

**Dropped loot is NOT in the scene-object handle table.** That is the whole reason the pathfinder
never saw it. The engine keeps ground drops in their own pool, with positions in a second, parallel
table. Confidence ≥0.98 — every field below is read directly out of four agreeing function bodies
(roll, spawn, award, expire).

| What | abs | **RVA** | Notes |
|---|---|---|---|
| Loot record pool `DAT_02ec0fa0` | `0x02EC0FA0` | **`0x2DA0FA0`** | 10 slots, stride `0x60` |
| Position marker table `DAT_022be7f0` | `0x022BE7F0` | **`0x219E7F0`** | 10 slots, stride `0x20` |
| Active-list head `DAT_02ec1360` | `0x02EC1360` | `0x2DA1360` | next ptr at slot `+0x18` (unused by the mod — the flat 10-slot walk is bounded and cannot loop on a corrupt link) |
| Drop roll | `0x003180F0` | `0x1F80F0` | `bool(Actor* dying, short* out)` |
| **Spawn (the drop event)** | `0x00319CA0` | **`0x1F9CA0`** | `(lootBuf, worldPos)`; called from `FUN_00312280` |
| **Award on touch** | `0x00319920` | **`0x1F9920`** | ⚠ mutates inventory — hook only, **never call** |
| **Discard / expire** | `0x003197B0` | **`0x1F97B0`** | non-party picker *and* timeout |
| Engine's own nearest-loot search | `0x00319850` | `0x1F9850` | reference for the shape only |
| Engine's own position getter | `0x002FB310` | `0x1DB310` | replicated memory-only by `item_scan.cpp` |

**Loot record:** `+0x00` u32 slot index · `+0x04` u32 **state (0 = free**; 1 arcing in, 2–6 blinking
out, 7 expiring — any non-zero means something is on the ground) · `+0x10`/`+0x18` prev/next ·
`+0x20`…`+0x57` = **7** × `{i16 itemId, i16 pad, i32 count}`, `itemId == -1` = empty · `+0x58` visual
handle.

> **STRIKE:** `combat_system.md` said the payload was **4 slots**. It is **7** — 5 normal + 2 rare.
> Both `FUN_003180f0` and `FUN_00319920` loop seven times.

**Marker record:** `+0x00` u8 alive · `+0x10` float x, y, z. The alive flag is the engine's own
validity test (`FUN_002fb310` refuses the slot without it), so a record whose marker is not up yet is
unplaced and must not be listed at a garbage coordinate.

### `FUN_00272cb0` is NOT an item-id → name resolver — read this before reusing it

`ingame_menu_reader.cpp` labelled it "item name codec". Read firsthand it is
`FUN_00272cb0 → FUN_003588b0 → FUN_00263990`, where `FUN_003588b0` decodes its argument as a **pooled
record handle** (`>>0x10 & 0xf` pool selector < 5, `& 0xffff` index, `>>0x14 & 0x7ff` generation vs
`rec+0x16`) and `FUN_00263990` reads `rec+0x102` / `rec+0xf8` with the `0xffffbfff` npcdic mask — the
**scene-object name chain** `EntityScan::ResolveObjectName` already replicates memory-only.

Ghidra dropped the register-passthrough argument on the inner call, so the input semantics are **not
established offline**; the call is nevertheless play-confirmed in the battle item sublist. It is
therefore used **as a game call, from the game thread only** (`src/core/item_names.{h,cpp}`), rather
than reversing the 14-class dispatch behind `PTR_FUN_01eebd08` (stride `0xb` qwords, class =
`id >> 12`; per-class count array `DAT_01eebcfc`, stride `0x16` ints; same `0x58`-byte descriptor) on
an inference below the confidence bar. If a memory-only item name is ever needed on the input thread,
that dispatch is the thing to reverse — `BattleState::MasterRecord` + `PoolString` is the ready-made
idiom (`hdr+0x04` count, `+0x08` stride, `+0x0C` records offset, relocated via `FUN_0020e600`).

## Battle rewards — EXP / LP per kill (Session 72) — SHIPPED

**There is no end-of-battle results screen and no victory event.** Rewards are granted per enemy
death and shown as floating `+EXP`/`+LP` **sprite digits** — no text exists for them anywhere.

| What | abs | **RVA** | Notes |
|---|---|---|---|
| **Reward batch, per enemy death** | `0x00312280` | **`0x1F2280`** | `FUN_00312280(BtlChr* killer, Actor* dying)`; sole caller `FUN_0030e360` case 0 (the KO funnel, any cause) |
| EXP formula | `0x002F8BB0` | `0x1D8BB0` | divides by living-member count |
| LP formula | `0x002F90C0` | `0x1D90C0` | **not** divided; 0 in Trial Mode |
| `+EXP`/`+LP` HUD popup | `0x0028FB80` | `0x16FB80` | `(actorId, exp, lp)` — args are the drawn numbers, but their identity is only **0.85** (dropped register args). **Not used**; the mod diffs instead |
| Level up | `0x0030C650` | `0x1EC650` | already covered by game message `0x04` |
| Gil gained | `0x00469E80` | `0x349E80` | already covered by game message `0x26` |

**BtlChr progression fields** (stride `0x1C8`, array at `Work()+0x08`): `+0x18C` u32 EXP (cap
99,999,999) · `+0x190` u32 LP (cap 99,999) · `+0x1C2` u8 level. Corroborated by `license_reader.cpp`
(already reads `+0x190`) and the status-menu member block above.

**Enemy reward rows:** `actor+0xE70` base, `actor+0xE78` per-level; `+0x2F` u8 LP, `+0x30` i32 gil,
`+0x34` i32 EXP. Value = `(enemyLevel − levelFloor) × perLevel + base`.

**Shipped method:** snapshot EXP/LP across roster list 3 slots 0–8 — the same list the function walks
(`FUN_00320ab0(i, 3)`, capped at 9) — call through, take the largest delta. Every living member gets
the same share, so the max IS the share, while a KO'd or absent member reads 0 and cannot drag it
down.

⚠ **0.95, not 0.98:** that `FUN_00312280` is reached for *every* enemy death rests on a static
"sole caller" xref, not observation. Failure mode is a kill that announces nothing.

---

## The field walkmap is a NAVMESH — Session 75 (2026-07-27)

**Every walkmap floor triangle carries the index of its neighbour across each of its three edges. That
adjacency IS the routing graph, and it is what the engine's own character mover walks.**

### Poly record, stride 0x20 — complete

| off | type | meaning |
|---|---|---|
| 0x00 | float | plane A |
| 0x04 | float | plane B (divisor; gate `0.001 < B`, STRICTLY positive) |
| 0x08 | float | plane C |
| 0x0C | u32 | flags — type in bits 0-2, map-jump group in bits **3-6**, material group in bits 13-17 |
| 0x10 | s16 | vertex index 0 |
| 0x12 | s16 | vertex index 1 |
| 0x14 | s16 | vertex index 2 |
| **0x16** | **s16** | **neighbour poly across edge v0->v1; `< 0` = none** |
| **0x18** | **s16** | **neighbour poly across edge v1->v2** |
| **0x1A** | **s16** | **neighbour poly across edge v2->v0** |
| 0x1C | u32 | second flags word (bit 0x400 observed) |

There is **no plane D**: the plane passes through vertex 0, and `FUN_00231890` evaluates height as
`y = v0.y + ((v0.x - px)*A + (v0.z - pz)*C) / B`. Conf 1.00 (read directly).

### VERTEX WINDING — and therefore portal left/right, for free (Session 86)

**The triangle interior always lies to the RIGHT of each directed edge `v[e] -> v[e+1]`.** This is not
an inference about the data; it is forced by the engine's own containment test, and it settles the
funnel's portal orientation without any geometric test at all.

`FUN_002324f0` (replicated as `MapQuery::PolyContainsXZDetail`) rejects a point on edge `v[i]->v[j]`
when `crossY = ez*(px-vx[i]) - ex*(pz-vz[i])` is `<= -eps`. Expand the standard 2D cross product
`TriArea2(a,b,c) = (b.x-a.x)(c.z-a.z) - (c.x-a.x)(b.z-a.z)` for `a=v[i]`, `b=v[j]`, `c=p` and the two
are identical up to sign: `crossY == -TriArea2(v[i], v[j], p)`. So an interior point satisfies
`TriArea2 <= 0` on all three directed edges — interior is on the `TriArea2 <= 0` side, i.e. the RIGHT.

**Consequence for routing.** Crossing from a parent poly into its neighbour across edge `e` takes you
from the right side of `v[e]->v[e+1]` to its left, so relative to the direction of travel:

> **`left = v[e]`, `right = v[(e+1) % 3]`. Always.**

`path_search.cpp` used to derive this per portal by asking which side of the centroid-to-centroid line
`v[e]` fell on. That test only separates the two endpoints when the triangle pair forms a convex
enough quad; on this mesh a single triangle is often an entire corridor, and obtuse/sliver pairs put
**both** endpoints on the same side, returning an arbitrary answer. Conf 1.00 (arithmetic).

**A global polarity flip cannot repair a per-portal error** — and a shortest-path tie-break actively
*prefers* the corrupted result, because a funnel that accepts a bound on the wrong side cuts through
the wall and is therefore shorter. See `debug.md`, "Funnel polarity".

**Enforced in code since S124:** `PathFunnel::BestPolarity` returns the as-labelled polyline
unconditionally — this winding fact is the authority, never the length comparison. The mirrored
funnel still runs as an instrument; a mirrored-shorter measurement on a real corridor logs
`MESH LABELLING ANOMALY` (it caught map 315's south bank shipping a 61 m flood-crossing chord as
"shorter"). See `debug.md`, Sluiceway table row 15.

### The mover — `FUN_002327d0`, conf 0.99

Carries a CURRENT POLY INDEX across frames. When `FUN_002324f0` reports the position left the triangle
across edge *e* (it returns the rejecting edge index, or `-1` while inside):

```c
sVar9 = *(short *)(param_1[2] + 0x16 + ((longlong)param_5 * 0x10 + (longlong)iVar8) * 2);
if (iVar8 < 0 || sVar9 < 0 || FUN_00230a40(param_1,sVar9,*(undefined2 *)(param_2 + 0x50)) == 0)
    { blocked }   else   { param_5 = sVar9; }
```
`param_1[2]` = `ctx+0x10` (poly array); `param_5*0x10*2 == param_5*0x20`. Visited set at
`DAT_02088fe0` (count `DAT_020891e0`), capped at 127 per move step.

**There is NO step-height or slope test anywhere in this path.** The engine's only geometric blockers
are walls/volumes and the flag test below.

### Walkability — `FUN_00230a40`

Movement class is arg5 of `FUN_00230c10`, stored at `moveCtx+0x50`. Both actor movers
(`FUN_0032bcc0`, `FUN_0032ca70`) pass **4** normally, `0xffff` only as an unstick mode when already
inside a volume or jammed within 0.27 units of a wall.

Class 4 matches none of the 0/1/2/3/5 branches, so it falls through to walkable. Therefore:

> **For the party, floor walkability is exactly `(effectiveFlags & 7) == 0`.** Conf 0.97.

(For the record: bit 23 blocks class 0, 24 class 5, 25 class 1, 26 class 2, 27 class 3.)

**CONFIRMED IN PLAY, Session 96 — and this entry survived an attempt to overturn it.** That session
replaced the test with `FUN_00230a40(poly, class 0)` on the reading that the leader's floor class is 0
(from `FUN_002681d0`, which writes 0 to `holder+0x153`) rather than the 4 the movers pass. On map 311
it refused **399 of 690 floor prims** and made a previously routable exit unreachable; the tester walks
that ground — Garamsythe's water is ankle-deep and the game has no swimming. Reverted.

> **The lesson is about which derivation to trust.** This entry traced the value `FUN_00230a40`
> RECEIVES, through the callers that pass it (`FUN_0032bcc0`, `FUN_0032ca70` → `FUN_00230c10` arg5 →
> `moveCtx+0x50`). The overturning claim traced what a different function WRITES to a field, and never
> showed that field is what reaches the callee. **Prefer the call site over the writer: only one of
> them says what the callee is handed.**

Bit 23 is still read, via `NavMesh::TerrainRefused`, and **nothing routes on it**. `NavTrace` checks it
every metre against the poly the player is standing on (`STANDING-ON-REFUSED`); it earns the right to
be priced only once that check stops firing.

### Effective flags — `FUN_00232020`

```
A   = (flags >> 13) & 0x1F           material bank, entries 0x00-0x1F
C   = ((flags >> 3) & 0xF) + 0x40    group bank,    entries 0x40-0x4F
entry i = { u32 mask; u32 value; }   at DAT_0209a3e0 (RVA 0x1F7A3E0) + i*8, 0x50 entries
in  = (flags & ~mask[A]) | (value[A] & mask[A])
eff = (value[C] & mask[C]) | (in & ~mask[C])
```

**Floor FINDING bypasses this** (`FUN_00231900` uses raw bits). **MOVEMENT does not.** Two different
questions; the mod now replicates each where it belongs. This is also how a script opening a gate
changes walkability without touching geometry.

### CSR is four layers

`index = layer * (cellCount + 1) + cell`, layer 0..3, per `FUN_0022f830`. Layer 0 = floor polys,
1-2 = volumes, 3 = attribute/region polys. `cellCount` is the s16 at `ctx+0x40`. Layer 0's index is
just `cell`, so a floor-only reader is unaffected — but the array is 4x longer than previously noted.

### Prim index space — THREE ranges (`FUN_00232160`)

| range | meaning |
|---|---|
| `[0x0000,0x4000)` | floor polygon, `polyArr + idx*0x20` |
| `[0x4000,0x5000)` | static volume, `volArr + (idx-0x4000)*0x90` |
| `[0x5000, …)` | **dynamic obstacle** — doors, moving platforms (`mgr+0x1D8` enable, `mgr+0x1A0` transform) |

**STRUCK:** `">= 0x5000 => empty/sentinel"`. Consequence: blockers are NOT in floor adjacency, so the
floor under a closed gate is still adjacent to the floor before it — a poly-graph route must test the
shared edge with a walk-class segment.

### Two collision contexts

`FUN_0026e500(0)` -> `DAT_0209a678` (the WALKING one, what we read); `FUN_0026e500(n!=0)` ->
`DAT_0209a680` (camera/LOS). Exactly two, per `FUN_0026e960` and `FUN_0026edb0`. Conf 1.00.

### STRUCK — `MAP_GROUND_AT` returns "the floor height at (x,z)"

`FUN_003208c0` -> `FUN_0026e3c0` is two-stage. Stage 1 is the topmost type-0 floor plane. **Stage 2
climbs from it in 1-unit steps (up to 30) to the first point outside any collision volume, then casts
a segment back DOWN with mask `0xFFFF` and flags `0`; if that hits anything, the returned Y is the
HIT's Y.** Conf 0.97.

It can therefore return a wall, a ceiling or a rooftop. Usable as a rough "is there floor here";
**never** as the height a destination or a route is anchored to.

### STRUCK — the map-jump group is 5 bits

It is **four**: `(flags >> 3) & 0xF`, verified in both `FUN_00232020` and `FUN_00230a40`. With a 5-bit
mask, a seam poly carrying bit 7 computed as `group + 16`, matched no `setmapjumpgroup(K)`, and was
silently dropped — which is why Upper Apartments' Highhall seam read as a 0.3 m-deep sliver in the
wrong place. Session 64's "the exact width of the field cannot matter" is struck.

---

## TWO interactable classes — Session 76 (2026-07-27)

`FUN_0025b820` runs **two** loops over two index ranges of the same container, scored by two different
functions. **The two classes do not share field offsets**, and reading one class's layout on the other
returns plausible-looking floats that are simply wrong — the failure mode that survives review.

Discriminator: **`sceneObj+0x03 >> 5`**.

| | class 3 — characters / NPCs | class 1 — gimmicks / volumes |
|---|---|---|
| scorer | `FUN_0025bad0` | `FUN_0025be50` |
| interaction point | `pos + node[+0x40/+0x44/+0x48]` when `node+0x107 & 1` | plain `pos` |
| band lo/hi | `centre ± node+0xC8 / +0xCC`, scaled by `node+0x24` | `node.y + node+0x68` / `node.y − node+0x6C`, **no scale** |
| band skip byte | `node+0xDD` | `node+0x5C` |
| cone half-angle | `node+0xBC` | `node+0x50` |
| cone aim point | the interaction point | **`node+0x10` (x) / `node+0x18` (z)** |
| score into `DAT_0209a2b0` | `2*dist2D − reach` | `FUN_003a1960(player, node)` |

Conf 0.98 — read from `FUN_0025be50` and `FUN_002646c0` directly.

### `FUN_002646c0` (RVA `0x1446C0`) — the engine's own interaction-point getter

```c
bVar2 = *(byte *)(param_1 + 3) >> 5;
if (bVar2 == 1) { pfVar1 = *(float **)(param_1 + 0xb8);
                  *param_2 = *pfVar1; *param_3 = pfVar1[1]; *param_4 = pfVar1[2]; }
else if (bVar2 == 3) { /* same, then + pfVar1[0x10..0x12] when (pfVar1+0x107)&1 */ }
else { *param_2 = *param_3 = *param_4 = 0.0; }
```

`InteractTarget::ReadGatePos` replicates this, and `PlayerState::ReadSceneObjectPos` applies it so
every entity position the mod reports is the point the engine measures interaction from.

**Unidentified, deliberately:** `FUN_0026bb00`'s class-1 setter writes bytes `+0x10/+0x14/+0x18`, but
the getter above does **not** read them — it returns the plain position for class 1. A subagent
reported `+0x10` as class-1's interaction point; that is refuted by the getter. `+0x10/+0x18` is where
`FUN_0025be50` aims the facing cone. What the `+0x10` field is for remains open — do not build on it.

## Scene-object PRESENCE + the shop-name chain (Session 79, 2026-07-27)

### Presence vs. mode — the inclusion test for a field object

Two different questions, two different reads. Conflating them hid an entire class of NPC on every map.

| question | read | meaning |
|---|---|---|
| does it EXIST / is a body standing there | `*(u8*)(sceneObj+0x14) & 0x20` (`READY_MODEL_BIT`) | model loaded |
| may the player act on it RIGHT NOW | `*(u32*)(sceneObj+0x1C)` FLAG_TALK / FLAG_ACTION | **MODE state** — `FUN_0025ad10` clears both on a disabled object |
| is the story gate open | `*(u8*)(sceneObj+0x0E) & 0x10` (`INTERACT_ENABLE_BIT`) | |

**The `+0x1C` flags are NOT an existence test.** An enabled, placed, model-loaded CHARACTER whose
script has not armed its talk hook reads `flags = 0`. That much stands, and the table above is still
the correct reading of the three fields.

**STRUCK (Session 84) — the conclusion drawn from it: "Include by KIND (`+0x0E & 0xF`: 1 = TALK
person, 5 = ACTION gimmick) plus a loaded model."**

The premise is true and the conclusion does not follow. A loaded model says a body EXISTS; it does not
say the body is anything the mod can tell a player about. Shipped as `present` in `entity_scan.cpp`,
that rule produced three separate defects at once — bare `NPC n` shadows beside real NPCs, three
unnamed bodies stacked on one coordinate on every map, and **enemies classified as NPCs** (a field
enemy is a character with a loaded model, so the handle-table walk claimed it before `ScanCombatants`,
which skips anything `AlreadyListed`, could apply the actor pool's faction test).

**STRUCK with it — the sentence "Conflating them hid an entire class of NPC on every map," and the
object it rested on.** The unnamed woman on Nomad Village at (46.00, 0.00, 57.70) was NOT invisible;
she was listed under her own npcdic name as `Nomad N` the whole time. What the widening added was her
SHADOW. That premise was never checked, and Sessions 79–83 were all built on it.

**The rule now:** the game names it (`+0x102` npcdic id, or the `+0xF8` custom string), or the engine
is offering an interaction on it (`+0x1C` TALK/ACTION). Otherwise it is not listed. Basis, from the
tester: the engine has no nameless interactable — every interactable object draws an icon reading
`"Action: <name>"`. Verified against 130 dumped objects on four maps.

**`Category::Enemy` has exactly ONE producer** — `ScanCombatants` in `entity_scan.cpp`, faction from
the actor pool (`kind == KIND_ALLY ? NPC : Enemy`). `ClassifyByNameKey` has no enemy branch at all.
**The handle-table walk runs FIRST and wins every tie**, because `ScanCombatants` skips anything
`AlreadyListed`. Any future widening of handle-table inclusion must be checked against that ordering
or it will silently re-file enemies as NPCs. `inclusion:` now counts the overlap between the two pools.

**`cat` in the object dump prints in HEX.** `cat=66` is `0x66`: low-5 = 6 -> scene class 3 (character).
Reading it as decimal gives class 2 and a wrong object model.

### Shop name — `shopId` -> master table -> npcdic (decompiled, NOT yet probe-confirmed)

Chain from `FUN_0057c010` -> `FUN_002fae40` -> `FUN_003eabe0`:

```
shopId = *(u8*)(DAT_02ca9790 + 0xC0)          // RVA 0x2B89790  shop controller (top menu)
tbl    = DAT_02ebf158                          // RVA 0x2D9F158  shop master table
  count  = *(u16*)(tbl + 6)                    // shopId must be < count
  stride = *(u16*)(tbl + 4)
  data   = *(u32*)(tbl + 8) + _DAT_01f83530    // RVA 0x1E63530 — FUN_0020e600 is just this add
nameId = *(u16*)(data + stride * shopId)
name   = npcdic slot (nameId*2 + 1)            // FUN_003eabe0 — the ODD slot
```

**Note the ODD slot** — and note that the reason given here for shrugging at it was wrong.
`FUN_003eabe0` asks for `nameId*2 + 1` unconditionally, which is a *different* thing from
`FUN_00263990`'s state-gated `id*2 + known`.

~~"the odd slot is byte-identical to the even one in the US build"~~ — **STRUCK (Session 80): 247 of
1141 ids differ, and the odd slot is the personal name** (see the npcdic section above). So a shop
title taken from the odd slot is a deliberate choice by the game, not a harmless alias, and
`probe_shop_name.js` printing both is now the point rather than a formality. `NpcdicName(id, known)`
takes the slot selector as a parameter, so the shop path can request the odd slot directly without
disturbing the field-object path.

`FUN_0020e600(x)` is `x + _DAT_01f83530`, a plain base-relative resolver — replicable as a memory read,
so nothing in this chain requires calling a game function.

**STRUCK: "`FUN_0057c010` is the shop-OPEN event."** It is a **dialog callback** — `FUN_0057a4e0`
stores it as `local_18` and hands it to `FUN_003f47e0`. The shop-open hook point is UNESTABLISHED.
Confidence: the id/table/npcdic chain 0.97 (decompile only, probe pending); the hook point, none.

### `phyre_types.h`'s `KIND_DEAD = 5` — still a misname, still counted not flipped

`entity_scan.cpp`'s actor-pool skip still fires on it. The pre-registered falsification test stands:
**a non-zero `KIND_DEAD` tally on a field map means the skip must go.** It has never printed in any
log, so the premise (the pool holds no gimmicks) still stands.

### CORRECTION — the shop binding is reachable by the EXISTING script reader

The Session 79 note above called the NPC->shop binding unreachable because it lives in compiled map
script. **Struck.** `map_script.cpp` already walks that blob live on every map: routine table
`hdr+0x18`, name pool `hdr+0x4C`, code span `[codeOff, next codeOff)`, pattern
`4f <lit:u16> 5d <nativeLo> <nativeHi>`. Recovering a shop id is the same scan with a different native
id, and it is DATABASE RESOLUTION (present before the player acts), not a learned binding.

**Also struck: "the shop-open native's id cannot be resolved by index arithmetic."** True but
irrelevant — S63 resolves natives by symbol/behaviour (`dbgIndex = nativeId + 5140` against the
archived .dbg symbol table), which is how `setmapjumpgroup = 0x011E` was named.

Open, all answerable offline: the shop native's id; whether an NPC's `+0xDC` talk index reaches its
routine by name convention or by direct table index; whether `shopId` is a literal. Check the
interact-icon field first — a discriminating icon would be one memory read.

---

## `sceneObj+0x1C` IS AN 18-BIT INTERACTION-MODE MASK — Session 80 (2026-07-27)

**This supersedes the reading of `+0x1C` as "a flags word with a TALK bit and an ACTION bit".** It is a
bitmask over the engine's **18 interaction MODES**, and **the mode index IS the bit index**. The mod has
been reading two of eighteen bits.

Conf **0.99** — read from four independent functions that all index the same way:

| function | what it shows | RVA |
|---|---|---|
| `FUN_00269a90:21` | `(*(u32*)(obj+0x1c) >> (mode & 0x1f)) & 1` — the mode arrives as a parameter and indexes the bit | `0x149A90` |
| `FUN_00269ba0:11` | same test, same shape | `0x149BA0` |
| `FUN_00269ad0:20` | same test, over every object in a container; guarded by `mode < 0x12` | `0x149AD0` |
| `FUN_0026b4a0(obj, mode)` | **ARM**: `obj+0x1c \|= 1 << mode` | `0x14B4A0` |
| `FUN_0025d5e0(obj, mode)` | **DISARM**: `obj+0x1c &= ~(1 << mode)` | `0x13D5E0` |

The two constants already in `nav_rva.h` fall straight out of it and are now *derived*, not observed:

```
mode  2 = ACTION  ->  bit 2  = 0x00000004  ->  event index at +0xCC
mode 10 = TALK    ->  bit 10 = 0x00000400  ->  event index at +0xDC
```

### `FUN_002652d0` (RVA `0x1452D0`) — the engine's per-mode event-index getter

```c
uint FUN_002652d0(longlong *container, longlong obj, uint mode) {
    if (0x11 < mode) return 0xffffffff;                       // 18 modes, 0..0x11
    if (mode == 0xe && (*(uint*)*container & 0xffff) < 6) mode = 7;   // one aliased mode
    u16 v = *(u16*)(obj + 0xC8 + mode*2);
    if (v != 0xffff) return v;
    // 0xFFFF == INHERIT: fall back to this object's record in the map data blob
    rec = container[0] + FUN_0020e600(*(u32*)(*(void**)(obj + 0x40) + 0x18));
    if (mode < *(s16*)rec) return ((s16*)rec)[mode + 1];      // rec[0] is the entry count
    return 0xffffffff;
}
```

`FUN_0020e600(x)` (RVA `0xEE600`) is nothing but `x + _DAT_01f83530` (RVA `0x1E63530`).

**So `sceneObj+0xC8` is a `u16[18]` parallel to the mask** — one event index per mode, `0xFFFF` meaning
"inherit from the map's own object record". The mod's `SCENEOBJ_ACTION_ID = 0xCC` and
`SCENEOBJ_TALK_ID = 0xDC` are slots 2 and 10 of that array.

### The mode/offset/bit correspondence is confirmed at EIGHT distinct modes

Not inferred from two — every row below is a separate function reading a specific bit and the matching
array slot, including the same `mode 0xE -> 7` alias `FUN_002652d0` carries:

| mode | bit mask | inline slot | record index | read by |
|---|---|---|---|---|
| 0 | `0x00000001` | `+0xC8` | 1 (`rec+2`) | `FUN_00269640:18` |
| 2 | `0x00000004` | `+0xCC` | 3 (`rec+6`) | `FUN_0025b820:60`, `FUN_00268d10:36` — **ACTION** |
| 7 | `0x00000080` | `+0xD6` | 8 (`rec+0x10`) | `FUN_00269860:48` |
| 10 | `0x00000400` | `+0xDC` | 11 (`rec+0x16`) | `FUN_0025b820:44`, `FUN_002675c0:25` — **TALK** |
| 11 | `0x00000800` | `+0xDE` | 12 (`rec+0x18`) | `FUN_00269640:48` |
| 14 | `0x00004000` | `+0xE4` | 15 | `FUN_00269860:22` — **carries the `0xE -> 7` alias too** |
| 16 | `0x00010000` | `+0xE8` | 17 (`rec+0x22`) | `FUN_00268ea0:35` |
| 17 | `0x00020000` | `+0xEA` | 18 (`rec+0x24`) | `FUN_00268ea0:21` |

### `FUN_00266bd0` (RVA `0x146BD0`) — the DEFAULT mask is a pure function of the scene CATEGORY

This is the correction that keeps the mask honest. An object does not choose its modes; it is *born*
with the set its scene category dictates, and the script adds or removes from there:

```c
switch (*(u8*)(obj + 3) & 0x1f) {           // scene CATEGORY (the mod's `sceneCat`)
  case 0:              obj+0x1c = 0x00000000;  break;   // no modes at all
  case 1: case 2:      obj+0x1c = 0x00000038;  break;   // modes 3,4,5
  case 3: case 4:      obj+0x1c = 0x00000040;  break;   // mode 6
  case 5: case 6:      obj+0x1c = 0x00030004;  break;   // modes 2(ACTION),16,17
  case 7:              obj+0x1c = 0x00034c85;  break;   // modes 0,2(ACTION),7,10(TALK),11,14,16,17
}
```

**Category 7 is the only category born with TALK.** Categories 5 and 6 get ACTION and nothing else.

This **directly corroborates the Session 79 diagnosis from the other side**: the Nomad Elder read
`flags=0x00030004`, which is *exactly* the cat-5/6 default, and the woman behind his tent read
`0x00030000` — the same default with bit 2 cleared by `FUN_0025ad10`. She was never a different kind of
object; she was a standard character whose one default mode the script had switched off.

**So the MASK is not a rich classifier** — its resting value is category, which the mod already reads.
What is per-object is the **`+0xC8[18]` array**: static map data, present before the player touches
anything, and the only part of this that can distinguish two objects of the same category. Any
classifier must be built on the array (or on what its event indices resolve to), never on the mask.

### What is NOT established — do not build on these

- **The meaning of modes 0, 1, 3-9, 11-15.** Sixteen of eighteen modes are unnamed. Nothing here names
  them and nothing may be shipped that assumes one.
- **Modes 16 and 17** are read by `FUN_00268ea0` (RVA `0x148EA0`) from `+0xE8` / `+0xEA` and auto-fired
  through `thunk_FUN_003dbbf0`; they are also the only two with special teardown in `FUN_0025d5e0`.
  Conf 0.98 that they are auto/ambient event slots — **but the Nomad Village dump shows `0x00030000` on
  ordinary NPCs, i.e. bits 16+17 set on objects with no other mode**, so they are common, not
  discriminating. `flags=00030004` (the Nomad Elder) is bits 16, 17 and **2 = ACTION**.
- **`reqenable` / `reqdisable` (dbg indices 5182 / 5183) as the arm/disarm natives — HYPOTHESIS ONLY,
  conf ~0.6, BELOW THE BAR.** The shape fits: `FUN_00355540` and `FUN_003558f0` are both
  `(ctx, obj, ret, args)` natives that do `mode = popArg(); arm/disarm(obj, mode)`, and the dbg list puts
  `reqenable`/`reqdisable` immediately beside `talkang`/`talkradius` — the interaction-geometry natives.
  **But the addresses contradict the id math**: `mapjump` is native `0x8D` at handler `0x355350`, so
  `dbgIndex - 5140` would make `reqenable` native 42, whose handler should sit *below* `0x355350`, not
  at `0x355540`. Either the delta does not hold in that band or these are not those natives.
  **Settled by `ghidra\dump_script_native_table.java`, not by argument.**

### STRUCK — "the delta now has TWO anchors" (claimed and withdrawn the same session)

It was claimed that `setmapjumpgroup` (native `0x011E` = 286, dbg index 5426, 5426 - 286 = 5140)
independently confirms the delta. **It does not — the reasoning is circular.** Session 63 obtained the
id `0x011E` from bytecode and then *named* it via the 5140 delta; re-deriving 5140 from that name is the
same fact twice. `mapjump` remains the only true anchor, because its handler was identified by
BEHAVIOUR (it calls the transition-loader chain `FUN_00314440`) independently of any delta.

### Why the exit-chain method does NOT close this one offline by itself

The map-exit chain was solved by reading map DATA the mod can walk. This is different: the fact needed
is **native id -> handler function**, which lives in the exe's `.data` (the pointer to the `mapjump`
handler `FUN_00355350` sits at abs `0x1EEE8B0`, found by `dump_mapjump_native.java`). The decompile
export contains function bodies only — **no `.data` bytes** — so no amount of reading `output\decompile`
can produce it. Three cheaper routes were tried and are dead ends, recorded so they are not retried:

- ~~**Address order.** ... **Handlers are not laid out in native-id order globally.**~~ **STRUCK — the
  refutation was itself built on the bad stride-8 assumption** (see the run-1 result below). Handler
  addresses are in fact *mostly* ascending with slot order (895 of 1179 adjacent pairs), so ordering is
  **unresolved**, not refuted. It is simply not needed: `dump_native_slots.java` measures the stride.
- **The extracted `.mpk` map controllers** (`extracted\ps2data\plan_master\map_ctrl\`, 20 files)
  **contain no `EBP2` magic** — they are map data, not bytecode. Verified by scanning all 20.
- **`output\mapctrl_ebp_disasm.txt` is mis-based** — its code base is wrong and it decodes data as
  instructions (routine 0 opens on `PREQ`/`LABEL`/`LABEL`). It cannot be read as a native-call listing.

**So the offline route is a Ghidra script — the same class of artifact as `dump_mapjump_native.java`,
which is how the exit chain got its anchor in the first place.** No play session is required.

### RUN 1 RESULT — `dump_script_native_table.java` refuted its own assumption. Do not trust its ids.

It validated the anchor (a pointer to `FUN_00355350` does sit at `0x1EEE8B0`) and then **assumed a dense
qword array with `mapjump` at index `0x8D`**. Its own output kills that:

- **1197 `.text` pointers over 4096 slots, with 776 gaps of EXACTLY 3** — a pointer every 4th qword. A
  dense qword array has no such period.
- **The names it emitted are nonsense against handlers we know by behaviour.** `FUN_00355830` (disarms
  interaction mode 2) came out as `@SWCOD_000162`, a compiler switch label. `FUN_00346020` came out as
  `sin`, but its body is a wait-poll structurally identical to the one named `waitv`.
- **`FUN_00355540` (ARM mode), `FUN_003558f0` (DISARM mode) and `FUN_00351f40` were not in the window at
  all** — yet all three are certainly natives: four-argument native signature and **zero references
  anywhere in `.text`**, so only a dispatch table can reach them. Wrong window -> wrong base -> every id
  wrong.

**`output\script_native_table.txt` is retained as evidence, but NOTHING in it may be cited except the
anchor.** No exe-side native name table exists at the layouts probed, so naming still depends on the
`.dbg` join — which makes getting the stride right the whole ball game.

### The artifact that actually measures it (authored Session 80, USER-RUN)

- **`ghidra\dump_native_slots.java`** — assumes no layout. A native is identifiable *without* its id:
  four-arg signature and zero `.text` callers. The script finds the `.data`/`.rdata` slot holding a
  pointer to each of ~26 such handlers, **takes the GCD of the sorted slot deltas as the stride**,
  anchors the base on `mapjump` (id `0x8D`, from bytecode, handler identified by behaviour), and then
  **self-checks**: `FUN_00355540` and `FUN_003558f0` provably arm and disarm the same bitfield, so their
  names *must* form a matched enable/disable pair. If they do, the mapping is confirmed and the natives
  that call arm/disarm with a CONSTANT name the interaction modes. If they do not, the script says
  **NOT COHERENT** and the ids stay unusable. It also dumps the raw record bytes around the anchor so
  the field layout is read rather than inferred.
- `ghidra\dump_script_native_table.java` — superseded by the above; keep for its reverse-lookup section,
  which is layout-independent (it lists which functions call the arm/disarm/event primitives).
- `frida\probe_interact_modes.js` — **secondary, not required for the chain.** Dumps the mask and the
  full `+0xC8[18]` array for every scene object, once per map. Useful for correlating a mode with an
  object whose identity is already known, if the native names leave a mode ambiguous.

### RUN 2 RESULT — the native table is a 32-BYTE RECORD array, and the self-check passed at that stride

`dump_native_slots.java` reported `stride=8 self-check=NOT COHERENT`. **The verdict is right and the
stride is wrong**, and its own slot list says why. Three independent samples land exactly `0x20` apart:

| sample | slots | spacing |
|---|---|---|
| ARM `FUN_00355540` / DISARM `FUN_003558f0` | `0x1eedc60`, `0x1eedc80` | **0x20** |
| fire-mode-event `FUN_0034e5c0` / fire-all `FUN_0034f380` | `0x1eeec60`, `0x1eeec80` | **0x20** |
| `FUN_003537b0` registered **four times** | `0x1eeed30/50/70/90` | **0x20, 0x20, 0x20** |

**The quad is the giveaway**: one handler serving four *adjacent* natives is exactly the shape of the
`keyscan`/`keyscanr`/`keyscant`/`keyscantr` and `keywait`/`keywaitr`/`keywaitt`/`keywaittr` families in
the `.dbg` list. And ARM/DISARM sitting **one record apart** is precisely the `reqenable`/`reqdisable`
adjacency the self-check was looking for — **it passed at stride 32 and was only reported as failing
because the script measured stride 8.**

**Why the GCD gave 8:** the handler's offset *within* a record is not constant across the samples —
`0x00` for ARM/DISARM/fire/fire-all, `0x10` for `mapjump` and the quad, `0x08` for `FUN_00351f40` and
`FUN_00355830`. So a record holds **more than one function pointer**, the samples hit different fields,
and a GCD over mixed-field addresses collapses to the pointer size. **LESSON: a GCD of address deltas
only measures stride when every sample is the SAME field.**

### The record's second field is a POLL function — which is why the id-join read as nonsense

Three functions the join named as natives are not natives at all:

- `FUN_00346020` (named `sin`) and `FUN_003453d0` (named `waitv`) are the **same wait-poll shape**:
  `if (cond) { if (FUN_0035a0a0()) { FUN_00314d40(1); return 1; } }`.
- `FUN_00342f50` (named `settalkiconstatus`) **takes no arguments at all** and just polls
  `FUN_0037cb80()` then yields via `FUN_00314d40(1)`.

A `set…` native that takes no argument is impossible. These are the **continuation / "is it still
running" field** of the record — the Athena VM's blocking-native mechanism — so reading them as natives
was guaranteed to produce unrelated names. Also refuted by this: `FUN_00356990` (named `lastjumpindex`)
*pops an argument and writes*, which no `last…index` getter would do.

**So: nothing about the id numbering is established yet, and `native_slots.txt`'s PROBE IDS table is as
unusable as run 1's.** What IS established: 32-byte records, multiple function-pointer fields, and the
arm/disarm adjacency. `ghidra\dump_native_raw.java` (authored, USER-RUN) dumps three known windows as
raw 32-byte-aligned rows with every qword resolved, asserting nothing — the field order is to be READ.

## SOLVED — the Athena script-native table (Session 80, run 3)

`dump_native_raw.java` read the bytes and the layout fell out. **A native has up to three
implementations because Athena natives can BLOCK** (the script waits for them):

```
BASE      = abs 0x1EED720   (RVA 0x1ECD720)   -- the SIMPLE slot of native 0
stride    = 32
simple[k] = BASE + 32k        non-blocking: does the whole job in one call
init[k]   = BASE + 32k - 24   blocking: pops the arguments into a state block
poll[k]   = BASE + 32k - 16   blocking: returns 1 while still running
name      = dbg_symbols_mapctrl.csv[ k + 5140 ]
```

**The `-24`/`-16` is what defeated two earlier scripts.** A native's init and poll sit in the physical
row *below* its simple slot, so one 32-byte row holds `simple[k]` next to `init[k+1]` and `poll[k+1]`.
Any reader that treated a row as one native blended two natives and produced confident nonsense.

### Why it is believed — 13 behavioural matches, not one anchor

Every handler below was identified from its **code** before any name was looked up:

| id | field | handler | what the code does | name |
|---|---|---|---|---|
| 42 | SIMPLE | `FUN_00355540` | pops a mode, `obj+0x1c \|= 1<<mode` | **`reqenable`** |
| 43 | SIMPLE | `FUN_003558f0` | pops a mode, `obj+0x1c &= ~(1<<mode)` | **`reqdisable`** |
| 86 | INIT | `FUN_00351f40` | arms mode 11 | `setupbattle` |
| 140 | SIMPLE | `FUN_00352db0` | pops 1 arg, releases a voice slot | `voicedispose` |
| 141 | INIT | `FUN_00354d00` | pops **three** args (dest, entrance, flags) | **`mapjump`** |
| 141 | POLL | `FUN_00355350` | polls; calls the transition loader `FUN_00314440` | **`mapjump`** |
| 170 | SIMPLE | `FUN_0034e5c0` | fires a mode's event on ONE object | **`sysreq`** |
| 171 | SIMPLE | `FUN_0034f380` | fires a mode's event on EVERY object | **`sysreqall`** |
| 177-180 | POLL | `FUN_003537b0` ×4 | one sync poll shared by four adjacent natives | `voicepan`, `voicevolume`, `voicechangevolume`, `voicechangepan` |
| 332 | POLL | `FUN_00342f50` | a no-argument poll | `partystdmotionrecover` |
| 374 | SIMPLE | `FUN_00355a00` | sets display NAME + style + position + cone; arms 8 & 13, disarms 3 | **`fieldsign`** |
| 703 | INIT | `FUN_00355830` | disarms mode 2 (ACTION) on hand-off | **`talktreasure`** |

`mapjump = 141 = 0x8D` reproduces **Session 63's id read straight from bytecode**, independently. The
`FUN_003537b0` quad landing on four adjacent `voice*` natives is a second independent confirmation.
**The delta 5140 now holds across ids 42..703** — a 660-id span, against the single anchor it had.

### What this gives the interaction-mode question

- **`reqenable(mode)` = native 42 (`0x2A`) and `reqdisable(mode)` = native 43 (`0x2B`)** are the natives
  that arm and disarm the 18 interaction modes. A map script's literal argument names the mode.
- **Session 63's decoded `__MJ_CTRL` routine opens with `reqenable(12)`** -> **mode 12 is the map-jump /
  transition mode.** First mode named from script rather than from engine code.
- **`fieldsign` arms modes 8 and 13 and sets the object's display name** -> **mode 13 is the name-label
  mode**, which is exactly the `& 0x2000` test in `FUN_00268d10` that renders `FUN_00263990`'s string.
- **`talktreasure` disarms mode 2** -> treasure chests use the ACTION mode.
- `sysreq` / `sysreqall` fire a mode's event — the script-side counterpart of the `+0xC8` array.

### Shop natives — ids derived, handlers NOT yet checked

`openfullscreenmenu` = dbg 6278 and 6305 -> natives **1138 (`0x472`)** and **1165 (`0x48D`)**;
`setshopname` = dbg 6333 -> native **1193 (`0x4A9`)**. **These are EXTRAPOLATIONS past the validated
band (42..703)** and are below the 0.98 bar until their handlers are seen to be menu-openers.
`dump_native_table_v2.java` prints them with their handlers for exactly that judgement.

`ghidra\dump_native_table_v2.java` (authored, USER-RUN) asserts this layout, **re-proves all 13 checks on
every run and refuses to emit anything if one fails**, then dumps the full table.

### The object's GAME-DRAWN LABEL — two fields the mod does not read (Session 80)

This is the practically useful part of the mode work, and it needed none of the native-table research.
From `FUN_00268d10` (the confirm-press handler), the engine's own label render:

```c
if ((*(u32*)(obj + 0x1c) & 0x2000) != 0 &&                 // mode 13 armed
    (name = FUN_00263990(obj))[0] != '\0') {               // and a name resolves
    style = *(u8*)(obj + 0xF4);
    FUN_003df760(handleOf(obj), 3, style, <map ctx>, name); // draw it
}
```

| field | meaning | mod today |
|---|---|---|
| `sceneObj+0x1C & 0x2000` (mode 13) | **the game itself labels this object** | **not read** |
| `sceneObj+0xF4` (u8) | label STYLE — `FUN_00267cc0` sets `2`, `FUN_002659a0` sets `0`, `fieldsign` sets it from the map record via `FUN_003df790` | **not read** |
| `sceneObj+0x102` / `+0xF8` | the name itself | already read (`ResolveObjectName` mirrors `FUN_00263990`) |

**`FUN_00269fe0(obj, kind)` ties bits 8 and 13 to KIND 4**: it arms both when the kind nibble is set to
`4` and clears both otherwise (`& 0xffffdeff` = clear `0x2100`). So kind-4 objects are the ones the game
gives a drawn label — matching `FUN_0025bad0`'s filter where kind 4 is the "talk AND action" class.

**Why this matters more than the native ids:** mode 13 answers *"does the game show this NPC a label of
its own?"* with one bit the scan already fetches, and the string is the game's own text in all 12
locales — never a mod-invented or learned label. It distinguishes an object the game names from one it
leaves anonymous, which is exactly the line the F6 manual-labelling workflow should respect.

Confidence 0.98 — read directly from `FUN_00268d10` and `FUN_00269fe0`. **Not yet probe-confirmed and no
C++ written.**

### CORRECTION (Session 81) — the mod prefers the ODD slot ALWAYS, not only after the introduction

The Session 80 entry above is right about the engine: `FUN_00263990` picks `id*2 + FUN_0032a930(id)`,
so the game shows the generic word until the story introduces a character. **The mod deliberately no
longer follows that half.** It takes the odd slot whenever the dictionary carries a distinct one.

Why: on Nomad Village ids 223/228/229/231/225 all read "Nomad" / "Nomad Youth" in the even slot and
**Dania / Lesina / Masyua / Nanau / Jinn** in the odd one. Following the engine exactly left five
entities sharing one label, which the mod then had to disambiguate with invented numbers — and those
numbers are what leaked (see below). Reading the personal name deletes the problem instead of managing
it, and the words are the game's own, in all 12 locales, present in the map's dictionary before the
player touches anything. `TalkNameKnown` is kept and demoted to the `inclusion:` tally, which now
reports `spoke the PERSONAL name / of them already introduced / newly revealed`.

Implementation: `EntityScan::ResolveObjectName` -> file-local `NpcdicDisplayName`, which compares the
two slot POINTERS before decoding — identical offsets are identical bytes, so that is exactly
equivalent to comparing the decoded strings and keeps the common case (894 of 1141 ids) at one decode.

### Scene CATEGORY 5 / 6 / 7 — an OBSERVATION with a counter, not an established fact

`sceneObj+0x03 & 0x1f`. One map's `'` dump (Nomad Village, 2026-07-27) separates cleanly:

| value | what it held on that map |
|---|---|
| **5** | the party/roster bodies — the leader Vaan, plus three unnamed bodies stacked at one point 6 m above the floor |
| **6** | every map NPC |
| **7** | the creatures (also the only entries in the combatant pool) |

**This is below the 0.98 bar and is used for one narrow thing:** `entity_scan.cpp` drops a category-5
object *that resolves no name*, because those three bodies were being announced as "NPC 1..3" and made
six indistinguishable `NPC n` entries out of three real anonymous townsfolk. Named category-5 objects
still list, so if the split is wrong elsewhere the cost is bounded. The engine does **not** corroborate
the 5-vs-6 half — `FUN_00266bd0` gives categories 5 and 6 the same default mode mask — which is exactly
why the skip ships with a per-scan tally *and* a per-map dump (`party body dropped:`) naming every
object it removed. A dropped body that sits on the walkable floor at its own position, rather than
stacked well above it, falsifies the rule.

### STRUCK (Session 82) — both Session 81 claims above

Two entries written the previous session are wrong and are struck here rather than left to be built on.

**1. ~~"the mod prefers the ODD slot ALWAYS"~~ — REVERTED.** It shipped, and in play it told the player
"Dania" for someone the game still calls "Nomad" — a name the game had deliberately withheld, and a
divergence from the label on screen. The tester had already confirmed the gated behaviour was the
correct one (*"was Nomad 2 before, then Dania once interacted with"*) and reverted it on sight.
`NpcdicDisplayName` is back to the engine's own rule, `id*2 + FUN_0032a930(id)`.

The Session 80 half — reading the odd slot at all, for characters the player HAS met — was a real fix
for a real bug and stands. **LESSON: reading the wrong slot and choosing a different policy for the
slot are two changes. The first was a correctness fix; the second was a behaviour change nobody asked
for, and bundling them let the second ride in on the first's evidence.**

**2. ~~"Scene CATEGORY 5 / 6 / 7"~~ — REFUTED.** Category 5 was read as "party/roster bodies" from one
map, where it held the leader plus three unnamed bodies. The tester: *"these are not party members, I
have no other party members currently."* The exclusion's own falsification dump said the same thing —
**every object it removed sat at `pos=(0.00,0.00,0.00)`**, the world origin, i.e. an unplaced reserve
slot that the existing unplaced guard already drops. It was built on a wrong premise *and* was a no-op.
The three bodies that inspired it shared one position 6 m above the floor: that is a fact about
POSITION, and it was written down as a fact about ROLE. Constants deleted from `nav_rva.h`.

**What the dump did right:** it named what it removed, on the first map that was not the one the theory
came from, and that is what killed the theory inside one play session.

### The `en` bit is now in the object dump

`LogObjectDump` prints `en=` (the `+0x0E & 0x10` story gate) alongside `flags`/`avail`. It was the one
field the dump lacked, and its absence is why nineteen bare `NPC n` entries on Rabanastre could not be
told apart from real story NPCs without a `'` dump that had not been taken. `flags` is mode state and
`avail` folds four tests together; `en` is the engine's own single "has the script switched this object
on" bit, set by the dedicated setter `FUN_0026ba60` from map script.

## Action category byte `row+0x1E` — the full table (Session 90, 0.99)

Master data: `action_data.bin`, runtime header `DAT_02ebf138` (RVA `0x2D9F138`). Row layout:
`row = FUN_0020e600(*(u32*)(hdr+0x0C)) + actionId * *(u16*)(hdr+0x08)`; `row+0x34` u16 = name index,
`row+0x1E` u8 = **action category**. Read by `BattleState::AbilityCategory`.

Resolved OFFLINE against the shipped asset — no probe. The file is `32,612` bytes and
`0x20 + 543*0x3C = 32612` **exactly**, which validates base, stride and count before any field is
read. Histogram of `row+0x1E` across all 543 rows:

| cat | rows | meaning | cross-check |
|-----|------|---------|-------------|
| 0 | 1 | basic Attack | exactly one Attack row exists |
| 1 | 81 | Magick | |
| 2 | 24 | Technick | **FFXII has exactly 24 technicks** |
| 3 | 51 | Item | |
| 5 | 13 | Esper summon | **FFXII has exactly 13 Espers** |
| 6 | 18 | Quickening | **6 characters x 3 each** |
| 7 | 235 | Enemy ability | |
| 8 | 16 | enemy internal | |
| 9 | 26 | Quickening concurrence | |
| 10 | 16 | Esper attack | |
| 13, 14, 16, 17 | 6 total | unidentified — left at default by the mod | |
| 255 | 56 | Reserve placeholder rows | matches the `row+0x00 = 4000` Reserve family |

Spot-checks: `0x000` Cure → 1, `0x00B` Curaja → 1, `0x096` Attack → **0**, `0x0A9` Steal → **2**,
`0x1ED` Megaflare → **7**. `nameIdx` equals the action id on every row sampled.

The 24 / 13 / 18 counts are three independent hard facts about FFXII landing exactly, which is what
carries this from the archived probe's 0.9 to 0.99. Supersedes the partial note in `battle_state.h`
("1 for every magick, 2 for every technick").

## The hard passability check — a BODY vs BOUNDARY test (Session 93)

**This is the engine's "you cannot walk here", and it is not a terrain attribute.** Three research
passes searched the walkmap flags word for a per-type or per-class bit that distinguishes water, a
cliff or a steep slope, and there is none — the record is exhausted, `+0x1C` has zero readers in all
33,128 functions, and no script override can even write bits 0-2. The refusal lives in the MOTION path.

| what | RVA | conf | notes |
|---|---|---|---|
| `FUN_0022f9b0` border clearance | `0x10F9B0` | 0.99 | The refusal itself. Replicated in `nav_footprint.cpp`. |
| `FUN_0022ef20` footprint transform | `0x10EF20` | 0.99 | Builds the ellipse matrices from `moveCtx+0x80`/`+0x84`. **Writes globals — this is why the border test cannot be called.** |
| `FUN_00230c10` body sweep | `0x110C10` | 0.98 | (start,to) → achieved position + blocked. **PURE**: 29-function closure, zero game-memory writes. Wrapped as `MapQuery::BodySweep`. **Iterates CSR layers 0,1,2 — it sees volumes; see below.** **ANATOMY CORRECTED S100 (0.98): it is NOT a swept capsule — ONE zero-radius centre ray (exact grid-cell marcher, no distance cap or precision loss) + ONE 0.27 m sphere at the DESTINATION only; the ±30° side rays fire ONLY when that sphere hits. It never reads walkmap adjacency, so cliff lips / mesh-boundary jogs / unwalkable neighbours are invisible to it at ANY length — the mod-side adjacency march (`path_march.cpp`) exists for exactly that class. Returns `int` 0/1, NOT a fraction: the achieved/requested fraction is computed by callers (`FUN_0032beb0:52-58`) from the out-position, which is what `MapQuery::BodySweep` already does.** |
| `FUN_00231400` can-stand-here | `0x111400` | 0.98 | Class-walkable floor AND not inside a volume. Pure; 11-function closure. Volume half is byte-identical to `FUN_00232490` and inherits its coarseness. **Still unused, and should stay that way for routing.** |
| `FUN_00232490` point-in-volume | `0x112490` | 0.98 | CSR layers 1,2. Pure. Wrapped as `MapQuery::PointInVolume` (Session 96). **COUNTER ONLY — never a verdict; see the section below for why.** |

### `FUN_00232490` IS THE COARSEST VOLUME TEST IN THE ENGINE (Session 97)

It was wired in during Session 96 as the router's wall predicate, and in `path_validate.cpp` it was
**fatal**. It refused **16 of 16 routes on map 315** — every breach on that map `why=wall` — while
`MapQuery::BodySweep` objected to not one leg. Both halves of the premise behind it were wrong.

**1. The body sweep already sees volumes.** `FUN_00230c10` runs two ellipsoid push-out passes through
`FUN_0022f830` with **layer mask 7 — layers 0, 1 AND 2** — and callback `FUN_0022de60`, over the same
`0x4000`-tagged, `0x90`-stride volume array `FUN_00232490` reads. Conf **0.97** (the mask is written by
a `CONCAT16` byte placement into the overlaid struct at `+0x16`, unambiguous but decompiler-shaped). Its
segment-march halves use `FUN_0022f430` + `FUN_0022cc50`, whose layer list lives in `DAT_00908df8` — data
with no other code reference, so **that half's layers are not recoverable from the decompile**; the
mask-7 passes alone carry the conclusion.

**2. `FUN_00232490` cannot tell a wall from a region.** It installs `FUN_0022f8b0`, whose entire
per-primitive filter is a tag-range check plus **one bit — bit 31 — of the merged flags word.** No class
test, no query class, no material. The engine's own movement collision (`FUN_0022cc50`, and the
class-aware cast `FUN_0022d4b0`) reads `merged_flags & 7` against the mover's query class at `+0x46`:

| `flags & 7` | behaviour | conf |
|---|---|---|
| 0 | always solid | 0.99 |
| 1 | bit 31 set ⇒ ignored; bit 31 clear & bit 30 clear ⇒ solid for all; bit 31 clear & bit 30 set ⇒ solid only for `queryClass != 4` (party passes) | 0.98 |
| 4 | ~~solid only when `queryClass != 4`~~ **STRUCK S100 — INVERTED. Solid ONLY for `queryClass == 4`: the party-only invisible walls** (zone gates, story barriers). Verified 0.99 by direct read of BOTH callbacks: `FUN_0022cc50:92-95` and `FUN_0022d4b0:73-76` — `bVar9 = (class != 4)`, intersect only when `!bVar9`. No behaviour change for the mod (sweep passes class 4, so sweep and mover always agreed) | 0.99 |
| 2, 3, 5, 6, 7 | fall through every arm — **never collide** (exact for FLOOR prims in both callbacks, and for volume prims in `FUN_0022d4b0`; **`FUN_0022cc50`'s volume-prim branch guards only type 1** — other volume types intersect unconditionally there, S100 read) | 0.99 |

plus **bit 23** = a "soft" hit, recorded as result code 1 instead of 2 with no hit point written, i.e.
detected and not blocking (0.90); **bit 26** gated on `ctx+0x3c` (0.90); **bit 31** = disabled/passable
(0.97); **bits 13-17** a 5-bit group id indexing the runtime override table at `DAT_0209a3e0`
(`{mask,value}` pairs, writer `FUN_0026e880`) — the same bank mechanism as `FUN_00232020` (0.96).

**The party's movers pass class 4** (0.97, traced through the call sites above), so a class-4 volume is
one the party walks through and `FUN_00232490` calls a wall. It also **hard-excludes the `>= 0x5000`
range — the doors and moving platforms** (0.95) — which are the volumes that genuinely do shut. Wrong in
both directions.

**No pseudocode anywhere names these categories.** There is no string, enum or table mapping class 0/1/4
or bit 23 to water / trigger / camera blocker / door. Do not label them; the numbers are what is known.

> ~~**If a class-aware wall test is ever needed, it is `FUN_0022d4b0`, reached via
> `FUN_002315e0(ctx, from, to)` — not `FUN_00232490`.**~~ **STRUCK S100: `FUN_002315e0` is NOT
> class-aware — it hard-codes class `0xFFFF` (-1) into the query struct, so it misses the type-4
> party-only barriers entirely (0.96).** The class-aware ray is **`FUN_00230b60`** (RVA `0x110B60`),
> which takes the class as a parameter — `FUN_003cc970` calls it with class 4, i.e. it is the
> engine's own party-class segment test (write-free 0.98, S100 sweep). But ask first whether the
> body sweep has not already answered: same collision, party's class, whole displacement.
| `FUN_00380c40` the resolver | `0x260C40` | 0.99 | `(ctrl, delta)` → resolved position. **Writes 14 globals; do NOT call.** |

### Session 100 sweep — the findings that reframed S96-S99 (three-agent decompile exhaustion)

- **Only THREE functions in the whole binary read the walkmap adjacency array (`+0x16/18/1A`)**:
  the mover `FUN_002327d0`, the boundary check `FUN_0022f9b0`, and a single-edge push-out
  `FUN_0022eac0` (0.97). There is NO engine "can walk A→B" helper and **NO engine route planner at
  all**: exactly one goal-seeking steering routine exists (`FUN_002e45e0`, RVA `0x1C45E0`; its
  bearing helper `FUN_003a19c0` has exactly one caller), movement goals are single straight-line
  targets set by the `move*`/`cmove*` script natives, NPC routes are authored coordinate sequences
  in EBP2 bytecode (0.95), and followers seek the leader's LIVE position (no breadcrumbs). The
  mod's own A* is the only planner in the process. `planmap` = level-asset naming + the PLMN
  area-name string table; not navigation (0.99). CLOSED.
- **Dynamic obstacles (prims ≥ 0x5000 — doors, sluice gates, platforms)** are tested ONLY by
  `FUN_0022d7e0`, gated on queryStruct+0x38 (set only by the mover) AND a per-obstacle state int at
  `level(DAT_0209a670)+0x1d8[idx]` (the "closed" flag, 0.97; its writer is unlocated, 0.30). Every
  ray/segment/sphere callback hard-excludes ≥ 0x5000, so the sweep, the point-in-volume test and
  the mesh are ALL blind to a closed gate. The only callable dynamics-capable query is
  **`FUN_00231690`** (RVA `0x111690`, `nav_rva.h MAP_VOLUME_PUSHOUT`) — **conditionally write-free
  at 0.97, BELOW the bar**: safe iff the caller-supplied body object keeps `+0x80/+0x84 ≤ 2.0`,
  else it spills into the shared visit scratch (`DAT_02088fe0`/`DAT_020891e0`). The `'` probe's
  `dynprobe` block snapshot/diffs that scratch around a safe-path call to settle it from C++.
  Nothing routes on it until the ≥0.98 record exists.
- **Bit-26 volumes** are admitted by the mover only when ground-locked and have NO code path in the
  sweep's sphere callback — a second sweep blind spot, also covered by `FUN_00231690` (0.96).
- **Water depth exists but never refuses** (0.96): layer-3 effective-type-4 water polys
  (`FUN_002321d0`, RVA `0x1121D0`), surface Y at `moveCtx+0x90`, "in water" bit 3 of `+0x60` — and
  no depth term anywhere in the movement refusal. Confirms S96: bit 23 is not walkability;
  deep-water boundaries are mesh boundaries. No step/slope/drop/one-way gate for class 4 anywhere
  (0.97) — S68/S75 strikes re-confirmed.
- **Layer table** (0.96): FOUR CSR layers — 0 floor polys, 1 primary volumes, 2 secondary volumes,
  3 attributes (water surfaces, region tints, trigger zones; never in any collision mask). Mover
  uses mask 2 (not ground-locked) or 3 (ground-locked, +dynamics +bit-26); the sweep's spheres use
  7; `FUN_00231400`/`FUN_00232490` use 6. The segment casts' layer list is `DAT_00908df8` (.rdata,
  contents unrecovered).
- **The engine mover's footprint ellipse converges toward ~0.5 m semi-axes** (`FUN_00380b80` seeds
  `+0x64/+0x68 = 0.5`, `FUN_003808a0` converges `+0x80/+0x84` toward them; authoring site of
  per-character values unresolved, 0.60) vs the sweep's 0.27 — a ~0.6 m gap passes the sweep and
  jams the body. The march's 1.0 m graze allowance is sized from this.
- **OPEN, deliberately NOT acted on in S100 (live code impact):** the sweep found THREE override
  banks (`+0x00-0x1F` and `+0x40-0x4F` for walkability via `FUN_00230a40`/`FUN_00232020`;
  `+0x20-0x3F` for collision via the segment callbacks). `nav_mesh.cpp EffectiveFlags` reads two.
  If a third bank is confirmed to affect walkability, `Walkable` may under-apply script overrides —
  verify in a dedicated session; changing it blind could alter working maps.

**The corollary that drove the S100 build: the sweep and the refusal barely overlap.** Adjacency
walls stop the party and no sweep at any step size can see them; the mod-side march
(`path_march.cpp MarchLeg`, replicating the 0.99-established accept rule `neighbour ≥ 0 &&
Walkable(neighbour)`) is the instrument for that class, and the sweep remains the instrument for
volumes. Neither replaces the other.

**The mechanism.** For each of the current triangle's three edges, `FUN_0022f9b0` takes the neighbour
across it and — at `:85-88` — **DEMOTES a neighbour that fails `FUN_00230a40` for the movement class to
`-1`, making it indistinguishable from a map edge.** One branch, two causes. The character is an ellipse
normalised to a unit circle, so the test is literally `if (distance < 1.0)` at `:120`; on violation it
pushes the position back to exact **tangency** along the edge perpendicular (`:129-142`), accumulates the
correction at `moveCtx+0x40`, and sets **`moveCtx+0x60 |= 0x10`** — the "was blocked" bit. It then
recurses into every walkable neighbour the body overlaps (`:152-167`).

**Why that produces the observed behaviour** ("you can walk against a cliff, you just make no progress,
and you can slide along angles"): the push removes only the component along the edge NORMAL, and
`FUN_002327d0:233-238` zeroes that normal's Y whenever the actor is ground-locked — which
`FUN_00380b80:8` makes the default. So head-on cancels entirely; oblique keeps its tangential part.

**THE COROLLARY FOR ROUTE VALIDATION, and it was got wrong once (Session 95).** Because the refusal is a
*push to tangency* and not a stop, **a point that fails the border test is not a point the player cannot
reach — it is a point the engine nudges them off.** The two questions are different and need different
instruments:

| question | instrument | verdict it may give |
|---|---|---|
| can the body traverse this leg? | `FUN_00230c10` body sweep (achieved/requested fraction, depenetration included) | **walkable / not walkable** |
| is this exact point one the body can rest on? | `FUN_0022f9b0` border clearance (`NavFootprint::Clears`) | tight / clear — **never** "unreachable" |

`path_validate.cpp` conflated them: a taut corner failing the footprint test failed the whole route. In
the tester's Session 94 log that was **25 of 57 breaches, every one of them on a route whose legs into
and out of the corner had both swept clear** — and because the caller then banned the portal on a leg
that had swept fine, each retry detoured around a good opening and the route got *longer* every attempt
(211.6 m → 230.9 m → 233.0 m → 233.5 m on one Garamsythe route) before the run gave up. Corrected: the
sweep decides walkability, the footprint test is counted and reported as a *tight corner*, and validation
walks past one to ask the question that actually matters — does the next leg sweep?

**Why cliffs need no height test, and why the step/slope gate was struck twice.**
`FUN_00380c40:24-28` PINS the actor's Y to the poly plane. There is no gravity on the walkmap and
nothing to fall off: a cliff is an edge whose neighbour index is `< 0`, refused by the identical branch
that refuses a wall. Water, a fence line, the map edge and ground the party's class cannot stand on are
all that same branch reached by different routes. **One mechanism** — which is why S68 and S75 were
right to strike a height/slope threshold, and why looking for a terrain bit could never have worked.

**Two things called "class 4", and conflating them cost a research pass.** The `4` the actor movers pass
is the **SEGMENT** class (query struct `+0x46`, compared `== 4` in `FUN_0022cc50`). The **FLOOR** class
that `FUN_00230a40` actually receives is a different field on a different object:
`*(u16*)(walkCtrl + 0x50)`, read at `FUN_002327d0:267`. `FUN_00380b80:12` initialises it to `0xffff`,
which falls through `FUN_00230a40`'s 0/1/2/3/5 branches identically to 4 — so the conclusion
`(effectiveFlags & 7) == 0` survives, but its stated REASON in `nav_rva.h` was wrong. Named apart now as
`MAP_CLASS_PARTY_SEG` versus the runtime-read floor class.

**Body radius `0.27f`** (`0x3e8a3d71`), the literal at all three `FUN_00230c10` call sites
(`FUN_0032bcc0:52-55`, `FUN_0032beb0:36-38`/`:66-68`, `FUN_0032ca70:68-70`). **Not** the interaction
ellipse at `XFORM_PLAYER_SHAPE` — that is a reach envelope for the `;` target test, a different
quantity. The engine additionally shapes the collision body as an ellipse from `moveCtx+0x80`/`+0x84`
(reached via actor → `+0xC0` → `+0x138` → `+0x30`, getter `FUN_00265970`); those two half-extents are
**not read** by the mod, because that offset chain is unconfirmed against the live process and
`FUN_002327d0:254` compares them against 2.0, so they are plainly per-actor. A circle of the confirmed
radius is the honest approximation until a probe settles the pair.

## Area resource manifest — NOT "area collision", and NOT a readiness signal (Session 93)

`DAT_02b5e0b8` (RVA `0x2A3E0B8`, area id) and `DAT_02b5e0c0` (RVA `0x2A3E0C0`) are the per-AREA streamed
resource manifest, **not** collision data, and navigation never reads either. The area loader
`FUN_003ea820` (RVA `0x2CA820`) looks the resource up and, when the lookup returns `< 1`, frees the
previous blob and writes exactly `DAT_02b5e0b8 = -1; DAT_02b5e0c0 = 0` — and **nothing retries for the
rest of the visit.**

So `failMask = 0x0C` is the engine's TERMINAL "this area has no such resource", not "not loaded yet".
Ridorana/Pharos (map 1101) published it on all ~3,530 field frames of a 2m36s visit while the other six
nav-safe conditions passed, which killed `\`/`p` routing, the audio beacon, the seam sweep (hence the
whole Exit list), the `NavReach` flood and the `NavTrace` trail there — only direction/distance announces
survived. Both bits are now **log-only**; `IsFieldNavSafe()` keeps the six conditions that describe
something navigation actually dereferences. Labelled `areaId(log)` / `areaManifest(log)` in the fail-mask
formatter so the old "areaColl" misnomer cannot be read as collision again.

## Field-menu character chooser + the Party screen (Session 93)

One controller, four commands. `FUN_00285290` (RVA `0x165290`, parked at `menuCtx+0xf8`) serves Party
`0x4b3`, Status `0x4b4`, Equipment `0x4b6` and Gambits `0x4b9`; only the mode the field pane arms it in
differs. The active command is `**(int**)(*(u64*)(menuCtx+0xd8) + 0x268)`.

**Party `0x4b3` is a membership TOGGLE.** `FUN_00284c90` (RVA `0x164C90`) is the writer:

| field | meaning |
|---|---|
| `row+0xfc` bit 3 (`0x08`) | in the active party — the bit the toggle XORs |
| `row+0xfc` bit 4 (`0x10`) | party LEADER (`FUN_002830a0` migrates it when the leader leaves) |
| `row+0xc0` | index into `menuCtx+0xac8` for this row's member block |
| `menuCtx+0xb10 + charId` | u8 mirror of bit 3, written by the toggle from the value it just set |
| `block+0x60` | charId (i16; `< 0` = empty portrait) |
| `block+0x00` bit 1 | GUEST — the toggle refuses to change it |

`FUN_002830a0` iterates **9** rows, matching `BattleState::kRosterSlots`. A refused press takes the
guard path and plays `FUN_00249c60(5)` (error SE) instead of `0x51` (accept), leaving both witnesses
unchanged — so a reader that re-speaks the state after the call reports the truth either way.

**THERE IS NO PARTY-SIZE CLAMP IN THE TOGGLE (Session 94, conf 1.00).** `FUN_00284c90` XORs bit 3
unconditionally; its guards test only an invalid charId, the GUEST bit and two `menuCtx+0xd3c` mode
bits. Nothing counts members. The size rule is enforced on menu EXIT, which surfaces the panel message
*"The party cannot contain more than three characters."* and returns the player to the field menu —
tester-confirmed, and visible in the live log going through the ordinary message reader, so it needs no
announce of its own. Consequence for readers: **bit 3 is a STAGED selection, and five or six characters
reading "In party" at once is the game's own state, not a mod fault.** Do not "fix" it.

Also settled live: the mod's `row+0xc0 -> menuCtx+0xac8 -> block+0x60` chain is **byte-for-byte the
one the game uses** (`*(short*)(*(ctx+0xac8 + *(int*)(row+0xc0)*8) + 0x60)`), so its charId is right.
Internal roster order is **not** story order — the log has cmd-`0x4b3` toggles logging charId 2 while
the cursor sat on Fran, and 1 and 3 for Ashe and Balthier. Do not assume 0..5 = Vaan..Penelo.


## Gambit setup screen — `FUN_005691e0` (RVA `0x4491E0`) (Session 94) — PROBE-CONFIRMED

Pause command `0x4B9`, reached from the shared chooser above. **Needs no new hook:** every focus
arrives as msg `0x8000` through `FUN_00247510`, which `menu_reader.cpp` already owns, and `owner` IS
the panel object. Three instances exist (one per gambit set) in a 3-way carousel with `DAT_02ca9700`
(RVA `0x2B89700`) holding the visible one; `menuCtx+0x160` also holds a panel but is **not** updated on
a page flip — do not use it.

Display records at `panel + 0x160 + i*0x20`, 13 of them (the array is `memset` `0x1A0`):

| field | meaning |
|---|---|
| `rec+0x00` | codec\* — CONDITION name; on record 0 this is the CHARACTER name |
| `rec+0x08` | codec\* — ACTION name; **null on record 0** (the header has no second column) |
| `rec+0x10` / `+0x12` | u16 condition / action id, `0xFFFF` when unset. Condition ids are biased: the master table index is `id - 0x6000` |
| `rec+0x14` | u8 enabled; on record 0 this is the gambit MASTER toggle |
| `rec+0x15` | u8 class, `2` = empty row |
| `panel+0x0F0` | i32 the character's BtlChr index (not a scene handle) |
| `panel+0x124` | u16 per-row enable mask, **bit `i-1` for display row `i`** |
| `panel+0x126` | u8 row count (max 12) |
| `panel+0x33E` / `+0x33F` | u8 COLUMN cursor (see below) and its saved copy |

**The three columns are ON/OFF · condition · action — column 0 is the per-slot CHECKBOX, not "the
whole row".** There is no cursor position that selects a whole row. Left/Right cycle `+0x33E`
`0→1→2→0` and `2→1→0→2`; the game's own help handler (`case 0xc`) then picks the description by that
cursor, and those ids resolve in **`help_menu.bin` (section 3)**:

| `+0x33E` | help id | the game's own words |
|---|---|---|
| 0 | `0xCF1` | *"Toggle slot ON/OFF."* |
| 1 | `0xCEE` | *"Change the conditions under which an action is performed."* |
| 2 | `0xCEF` | *"Change which action is performed."* |
| — (no row) | `0xCF2` | *"Toggle gambits ON/OFF."* — record 0's master toggle |

**STRIKES this file's own first version of this table (written earlier in Session 94), which called
column 0 "whole row" at 0.97.** It was an offline inference; the tester heard the whole row read out
when they arrowed onto the checkbox, and `case 0xc` settles it. The lesson is that the game already
had a name for each column and the first pass invented one instead of looking for it.

**All THREE carousel panels receive the entry `0x8000`**, not just the visible one — the live log has
one keypress producing three identical utterances at the same millisecond from three owners. Gate on
`DAT_02ca9700` (RVA `0x2B89700`), which holds the visible panel.

Names are stored **already variant-selected** — decode directly, no `SkipVariantPrefix`.

**The dispatch `val` IS the record index** (0 = header, 1..`panel+0x126` = rows). Confirmed, no
off-by-one. Two measured hazards any reader must handle:

1. **`0x8000` repeats for an unchanged state** — seven identical `val=1 col=1` messages on entry,
   because the per-frame cat-`0xA` handler reconciles `+0x33E` against `+0x33F` and re-sends via
   `FUN_002d1ac0`. A change-check is therefore the sanctioned per-frame exception here, not a dedup.
2. **A row-0 crossing sends TWO `0x8000` for one keypress and the FIRST carries the STALE column**
   (`#31 val=0 col=1` then `#33 val=0 col=0`; again at `#42`/`#44`). Forcing column 0 whenever
   `val == 0` — which record 0's null action pointer independently justifies — collapses the pair.

Corroborating detail, not needed by the reader: msg `0x8005` accompanies a ROW change only, never a
column-only move.

**The PICKER (`FUN_0056b4d0`, RVA `0x44B4D0`, rows at `picker+0x0E0 + i*0x20`) is UNMEASURED.** The
probe run never confirmed on a row, so it captured no picker messages at all. Do not build a reader
against that row layout until it has been.

**STRIKES the `ROW_CHAIN[2] { 0x445E00, 0xC8 } // gambits` label** in `ingame_menu_reader.cpp`: that
class is command `0x4B8`, not this screen. The entry is structurally valid; only the name was wrong.


## Gate-crystal teleport list — substitution rows (Session 94) — PROBE-CONFIRMED

Not a new surface. Touching a crystal opens the field dialogue / choice window `FUN_002a6190`
(RVA `0x186190`) as a select list, and its cursor moves arrive on the `0x8000` dispatch the mod already
hooks. The destination rows were silent for one reason: **a row can be nothing but its substitution.**

Option-block layout, measured against a 26-destination list:

- Block header `0e <count | 0x80> …`, so `count = buf[m+1] & 0x7F` — read `0x9a` ⇒ **26**, matching the
  widget's own capacity byte. The mod had been passing the constant `MAX_OPTIONS` (32) as the total,
  which is why its log read `choice[25/32]` and why the hide-mask walk scanned six slots past the end.
- Entries are length-prefixed `[len | 0x80][len bytes]`. A destination row is `84 | 0f 2e <0x80+idx> 90`
  — `len = 4`, body `0f 2e 80 90`. `DecodeRow` yields nothing for it, because the escape contributes no
  characters.
- The label lives in the window's **inline** argument table at `window + 0x1B8 + idx*0x10`, entries
  `{i32 type, i32 pad, u64 codec*}` with `type == 1` for a string. `window == widget - 0xD0`, confirmed
  against the live log (widget `0x2C2BEE50`, window `0x2C2BED80`).
- Row 25 is a **literal** ("Cancel"), which is exactly why that one row spoke and 0..24 did not.

The real row count is available without new offsets at `window + 0xD0 + 0xA2` — the widget field the
per-frame choice tick already reads.


## Character-name master table — `DAT_02ebf130` (Session 94, conf 0.98)

The pure-read path to a party character's name, needed because `FUN_0035d330(0x02, id)` is a **game
call** that stages its arguments in the static record `DAT_022ca520`, and the party-status keys run on
the input thread. From `FUN_0031c5d0` **`case 1`** (the arm category `2` takes):

```
base   = Reloc(*(u32*)(DAT_02ebf130 + 0xc));
row    = base + *(u16*)(DAT_02ebf130 + 8) * id;   // the standard st2e header shape
poolId = *(u16*)(row + 0x30);                     // -> the shared pool DAT_02ebf170
```

That header shape is exactly `BattleState::Internal::MasterRecord`, so `CharacterName(charId)` is
`PoolString(*(u16*)(rec + 0x30))` and nothing more. The sibling arm for category `3` reads its own
table identically at `+0x08`, so the layout belongs to the table, not the category.

Category `0x0B` (gambit condition) takes **`case 10`**: table `DAT_02ebf0d8`, index `id - 0x6000`, name
pool index at `row + 0x14`, plus a menu-text id at `row + 0x04` for `FUN_002f9860`.


## Text codec — the comparison operators (Session 94, conf 1.00)

`0xA6` `=` · `0xB2` `<` · `0xC4` `≥`. Dropped until now, so every threshold gambit spoke without the
operator that carries its meaning ("Foe: HP  90%").

Pinned from the game's own data, which is the only way these can ever be settled — the font atlas *is*
the character map, so the byte order carries no ASCII relationship to interpolate from. Surveying the
9,518 NUL-separated strings in `us/binaryfile/word.bin` puts each byte in exactly one syntactic slot:
`0xA6` only in `status = <name>` (86×) and `HP/MP = 100%`; `0xB2` only in `HP/MP < 10%..100%` and
`< 500..100,000`; `0xC4` in those same thresholds **minus** 100%. Then `listhelp_targetchip.bin` — the
help line for these very chips — states two of them **in words**:

- *"Target any ally with **less than** 10% HP."* ⇒ `0xB2` is `<`
- *"Target any foe with HP **greater than or equal to** 1,000."* ⇒ `0xC4` is `≥`

That is what separates `<` from `≤` and `>` from `≥`; structure alone could not. `0xB2` pairing with
`100%` independently rules out `≤`, which would be a tautology. `0xA6` rests on the survey: between
"status" and a status name only equality is meaningful, and `0xAA` is already `:` in the same strings.

Also added: `0x81` = `ú`. Its only use anywhere in the pool is "Cúchulainn" / "Cúchulainn, the Impure",
which without it said "Cchulainn". Not generalised beyond that one observation.

Still unmapped, deliberately: `0xA3` (only in dev strings marked `NOT USED`, e.g. `all<a3>cancel`, so
`/` vs `-` cannot be chosen) and `0xAD` (a single dev string, `Team wanted an <ad>ark<ad> immage`,
which looks like a double quote on one witness — not enough).

> **SUPERSEDED 2026-08-04 (Session 130) — but every conclusion above SURVIVED.** The whole table is
> now generated from the game's own glyph table (`font00.dat`; see the section at the end of this
> file), which gives all 224 single-byte slots as data instead of one witness at a time.
>
> **This section is the reason to trust that.** The generator cross-checks its output against all 80
> mappings derived here, by hand, from shipped text — a completely different method — and refuses to
> emit if one disagrees. **None does.** `0x81` = `ú` in particular is reproduced exactly.
>
> The two bytes left deliberately unmapped here are now settled, and one of them was going to be
> guessed wrong: **`0xA3` is `_`**, not the `/` or `-` this section was choosing between — which
> makes the dev string `all<a3>cancel` read as `all_cancel`, an identifier, exactly as it should.
> **`0xAD` is `"`**, confirming the single witness `Team wanted an "ark" immage`. Declining to guess
> was the right call twice over.
>
> What this section could never have reached: the accented Latin block `0x54`-`0x84` (**`0x72` is
> `ñ`**), which no US-locale string exercises, and roughly forty punctuation marks that were being
> dropped silently. `game_text.cpp` used to end with `default: break; // unmapped extended glyph:
> drop` — so an accented byte emitted **nothing**, and "Señor" spoke as "Seor".


## The announce is SUPPRESSED ON REPEAT — `FUN_00304850` (Session 90, 0.97)

`FUN_00469af0` (the `0x0D`/`0x0E`/`0x0F` charge announce) has **exactly one caller**, and that caller
guards it:

```c
if ( param_1[0xed] == 0                                  // no repeat latch
  || *(short*)(actor+0x714) != *(short*)(actor+0x744)    // OR action differs from the previous one
  || param_1[0xe2] != param_1[0xe8] )                    // OR target differs
    FUN_00469af0(actionId, actor+0x698 /*BtlChr*/, ...);
```

`+0x714` is the ACTIVE action id, `+0x744` the PREVIOUS one. **An actor repeating the same action on
the same target announces once and then never again.** Measured in play: an Urstrix's Slap landed
twice and announced once; a whole session produced 4 Tier-1 messages against 8 damage events, with
`HookedSprintf calls=1` in the PERF block.

This is a **third** suppressor, alongside the two already recorded — the `~24`-unit distance cull in
`FUN_00469570` (ids `0x0D`/`0x0E` carry render style `0x01`, which is not cull-exempt) and the
10-slot dedup ring in `FUN_0046ab10`. It is also the largest. Corroborated independently by
`combat_re_2026_07_20_battle_state.md:300`, which already said the emitter "self-dedupes, so it is
*not* a complete log".

**Consequence for the mod:** reading the game's sentence is correct and keeps the wording verbatim in
all 12 locales, but "the mod speaks every enemy cast" is not achievable through the message bus. The
announce fires when the game decides to announce. Do not attempt to compensate by hooking
`FUN_00469af0` — the emitter is precisely what the gate above declined to call.

## Element sprites, damage type, no-list pop-ups (Session 125, 2026-08-02)

**KEYWORDS: element sprite escape 0F 3F 81 attribute_data word.bin chunk 4 pool 0x2017 ElementName
action row+0x13 AbilityElements no-list popup win+0x3C4 win+0x1B0 FUN_00241d40 game-over bitfield
DAT_022c83e8 leader select FUN_00298250 0x4C6 compare panel FUN_002cc4f0 FUN_002ca7c0 FUN_002cc780**

### The inline ELEMENT sprite — `0F 3F 81 <XX>` (SHIPPED, conf 0.99)

| Fact | Value |
|---|---|
| Escape | `0F 3F 81 <XX>`, `XX` = `0x8A`..`0x91`, 4 bytes total (selector `0x3F` already takes 2 params) |
| Bit order | `XX - 0x8A` = 0 Fire · 1 Lightning · 2 Ice · 3 Earth · 4 Water · 5 Wind · 6 Holy · 7 Dark |
| Name source | shared pool index **`0x2017 + bit`** = `(4 << 11) | (23 + bit)`, i.e. `word.bin` chunk 4 idx 23..30 |
| Binding authority | `attribute_data.bin` — st2e, 8 records x u16, values literally `0x2017..0x201E` in bit order |
| Emitter in-game | `FUN_00293ce0:41` -> `FUN_002f9860(0x4B27 + bit)` per set element bit; those 8 strings each hold `0f 3c c1 fe \| 0f 3f 81 <8A+bit> \| 0f 3c 81 80` |
| Corroboration | `help_action.bin` prose names each sprite in the adjacent word ("Deal `<0F 3F 81 8A>` fire damage to one foe.") |
| Coverage | across all 27 US st2e master-data files, `0x3F` is used ONLY for these 8. One stray `0F 3F 46 00` in `battle_pack.bin` fails the `0x81` test |

Mod side: `BattleState::ElementName(bit)` / `ElementNames(mask)`; `GameText::SetElementSpriteResolver`
(registered in `dllmain.cpp` — `core/` must not include `battle/`). The decoder emits a private-use
marker and resolves it once per page, suppressing the name when the adjacent word already spells it.

**Equipment detail panel row labels** (`FUN_002f9860` ids, from `FUN_00293310`): `0x232A` License
Needed · `0x232E`/`0x232F`/`0x2330`/`0x2331` the four element-affinity rows (masks at equipment
record `+0x3F`/`+0x3D`/`+0x3E`/`+0x3C` respectively) · `0x2332` list separator · `0x2333`/`0x2335`
immune (<4 / >=4 statuses) · `0x2334`/`0x2336` equip · status masks u32 at record `+0x44` (immune)
and `+0x48` (equip). Statuses in that panel are already WORDS via `FUN_0035d330(0x1A, bit) + 0x18`,
which is why they always read and only the element was silent.

### STATUS EFFECTS ARE NOT SPRITES IN TEXT — with one gap (Session 125)

A census of every `0x0F` escape across all shipped US st2e text found exactly **two** glyph-inserting
families: `0x3F` (the eight elements) and `0x40..0x6B` (controller/keyboard button prompts,
`GameArchitecture` "button-icon inserts"). **There is no status-sprite escape.** Statuses reach the
player as words everywhere text is involved, and where they genuinely ARE icons — the Status
screen's ailment grid `FUN_002c59d0` and the battle HUD — the mod already resolves them from the
status-bit table via `BattleState::StatusName`. So the element fix has no status counterpart to make.

**THE ONE REAL GAP — the game itself hides them past a threshold.** `FUN_00293310:139-212`:

```c
... popcount the mask ...
uVar10 = 0x2333; if (3 < iVar35) uVar10 = 0x2335;   // immune: <4 -> label, >=4 -> "Various"
if (iVar35 < 4) { ...list each name via FUN_0035d330(0x1a, bit)... }
```

With **4 or more** statuses set the panel prints `"Immune: Various status effects"` (id `0x2335`) or
`"Equip: Various status effects"` (`0x2336`) and **lists no names at all** — sighted players lose
them too. An accessory that blocks many ailments therefore tells you nothing specific.

**FIXED (S125) — `src/ui/equip_detail.cpp`.** The mask is still at record `+0x44` / `+0x48`, so the
names are recoverable. `ExpandCollapsedStatuses` mirrors the game's own selection
(`FUN_00293310:139-185`: a non-zero IMMUNE mask wins outright and the granted mask is not shown;
only when immune is empty does granted appear), popcounts it, and when the game collapsed the list
**replaces the collapsed phrase with the full one**. Every word is game-supplied — the row label
(`0x2333`/`0x2334`), the separator (`0x2332`) and each status name.

Substitution, not appending: the needle is the game's own `0x2335`/`0x2336` string, so it is
locale-correct by construction and the result reads `"Immune: Sleep, Confuse, Silence, Blind"`
rather than saying "Immune" twice. If the phrase is not found the text is left exactly as the game
wrote it and a `DESC` line records the disagreement — never patch a string you cannot locate.

Item id / category at the formatter hook: `params[0]` = category (**3** = equipment, before
`FUN_00292b70` remaps it per slot) and `params[1] >> 16` = the id. Note the record getter wants
`FUN_0035d330(6, id << 16)` — `FUN_0031c5d0` case 6 reads the id as `*(u16*)(arg + 6)`, i.e. the
HIGH half of the second argument, which is why every caller shifts.

`BattleState::StatusName` gained `includeSuppressed`: KO / Invisible / HP Critical / X-Zone carry
`rec+0x02 == 0xFF` and are hidden on the battle HUD as noise, but the item panel lists them (it
reads the master name with no suppression check) and an accessory that blocks KO is exactly what a
buyer needs to hear.

**STILL HIDDEN, a separate gap:** when BOTH masks are non-zero the game shows only the immune one
and silently drops the granted one. Not expanded — that would invent a row the panel never had.
`equip_detail.cpp` logs it when it happens so the decision can rest on evidence.

### listhelp_common string ids — base `0x2328` (conf 1.00)

`FUN_002f9860(0x2328 + i)` is entry `i` of `listhelp_common.bin`. Verified against the shipped file
on **all 18 ids the panel builders use**, 18/18:

| id | i | text | | id | i | text |
|---|---|---|---|---|---|---|
| `0x232A` | 2 | `License Needed: ` | | `0x2333` | 11 | `Immune: ` |
| `0x232C` | 4 | `On Hit: ` | | `0x2334` | 12 | `Equip: ` |
| `0x232D` | 5 | `None` | | `0x2335` | 13 | `Immune: Various status effects` |
| `0x232E` | 6 | `Immune: ` (element) | | `0x2336` | 14 | `Equip: Various status effects` |
| `0x232F` | 7 | `Absorb: ` | | `0x233B` | 19 | `Attack Power ` |
| `0x2330` | 8 | `Half Damage: ` | | `0x233C` | 20 | `Defense ` |
| `0x2331` | 9 | `Weak: ` | | `0x233D` | 21 | `Magick Resist ` |
| `0x2332` | 10 | `, ` | | `0x233E`/`0x233F` | 22/23 | `Evade ` / `Magick Evade ` |
| | | | | `0x2340` | 24 | `Element: None` |

This also settles which element mask is which row: `+0x3F` → `0x232E` Immune · `+0x3D` → `0x232F`
Absorb · `+0x3E` → `0x2330` Half Damage · `+0x3C` → `0x2331` Weak.

### Action record `+0x13` = ELEMENT MASK (SHIPPED, conf 0.99)

`BattleState::AbilityElements(actionId)` — same table and technique as `AbilityCategory`'s
`row+0x1E` read (`Internal::MasterRecord(RVA_ACTIONTBL, id)`), different offset.

Derivation against the shipped `action_data.bin` (543 rows, stride 60): 15/15 named elemental spell
families match; Cure / Shock / Scathe / Bio are all `0x00`; and **every row holds zero or exactly
one bit** — only `{0,1,2,4,8,0x10,0x20,0x40,0x80}` occur, 447 non-elemental then 19/11/10/8/13/13/9/13.
A multi-bit mask therefore means the offset is wrong, and `ElementNames` logs one if it ever sees it.

**No element survives to the damage-apply site** (`combat_system.md` §5.5) and `battle_message.bin`
carries zero `0F 3F` escapes, so the action record is the ONLY source. `DamageLine` consumes it.

> **NOT AVAILABLE: the element at the Tier-1 charge announce.** `HookedSprintf` on `FUN_00536410`
> receives the message id (`argBlock+4 & 0x7FFF`) and the finished string, **not the action id**, and
> the arg-block layout past `+4` is undocumented. Establish that layout before attempting to annotate
> the charge announce. Do not infer it.
>
> **KNOWN GAP:** a basic Attack (category 0) takes its element from the WEAPON, not the action
> record, so weapon-elemental hits report no element.

### NO-LIST confirm pop-ups — why Game Over was silent (SHIPPED, conf 0.99)

`FUN_00241d40` builds two shapes, and the game's own branch at `:88` decides which:

```c
*(uint *)(param_1 + 0x3c4) = uVar3;              // from the creation packet's +0x2C
if ((*(byte *)(param_1 + 0x3c4) & 1) == 0) {     // bit 0 CLEAR -> build the button list
    *(longlong *)(param_1 + 200) = FUN_002d14e0(...);   // win+0xC8
}
```

- **bit 0 CLEAR** — list at `win+0xC8`, cursor lands on it, `FUN_00247510` emits focus `0x8000`.
  This is the path `menu_reader` already covers.
- **bit 0 SET** — no list, `win+0xC8` stays 0, **no `0x8000` is ever emitted**, so `OnFocus` never
  runs and `PopupReader::BodyText` is never consulted. The window is completely silent.

The body is at `win+0x1B0` for BOTH shapes (`case 1` does `FUN_00254f30(win+0x1b0, *packet2, 0x200)`),
so a construction hook on `FUN_00241d40` covers every no-list prompt in the game. The two shapes are
disjoint by the game's own branch — no latch or dedup is needed, and they cannot double-speak.

Game Over reaches it via `FUN_0035c660` -> `FUN_00241c90` -> `FUN_002465f0(win, 0x3D0, FUN_00241d40, ...)`
with the constant pair `0x4FB` / `0x102`. **The `FUN_002f9860` id Ghidra dropped from `FUN_0035c660`
is NOT needed** — reading `win+0x1B0` covers it.

### Game-over state bitfield `DAT_022c83e8` — RVA `0x21A83E8`

Bit meanings taken from the bodies of the game's own getters, so these are reads of a documented
global rather than inference:

| Bit | Meaning | Getter |
|---|---|---|
| `0x01` | guest wipe | `FUN_0035c760` (`0x23C760`) `return DAT_022c83e8 & 1` |
| `0x02` | **party wipe / GAME OVER** | `FUN_0035c740` (`0x23C740`) `return DAT_022c83e8 >> 1 & 1` |
| `0x10` | **leader incapacitated** | `FUN_0035c750` (`0x23C750`) `return DAT_022c83e8 >> 4 & 1` |
| `0x40` | continue-menu variant | branch in `FUN_0035d980` |

Set on the death edge in `FUN_00300fc0` (`0x1E0FC0`) when the dying actor is the leader handle;
cleared by `FUN_0035c8e0(0x17)` (`0x23C8E0`). Set-leader is `FUN_00327850(charId)` (`0x207850`).

**"GAME OVER" itself is baked art** — `PS2Data\image\FF12\myoshiok\<loc>\Tm2_Menu\gameover_c.tm2`,
the same class of problem as the title logo. There is no codec text anywhere in the binary, so the
mod's announce is gated on bit `0x02` rather than on the picture.

### Leader-select prompt — TWO candidate surfaces, both wired (offline, NOT probe-confirmed)

| Surface | Where | Text |
|---|---|---|
| Battle HUD | `FUN_002906d0` (`0x1706D0`) opens at `DAT_0209be80 + 0xAB30`; proc `FUN_002985d0` (`0x1785D0`); **`FUN_00298250` (`0x178250`) case 1 builds the labels** | header `FUN_002f9860(0x4A49)`, body `FUN_002f9860(0x4C6)` = `menu_command` 222 "Select Leader", used nowhere else in the binary |
| Field / pause chooser | `FUN_00280b70` (`0x160B70`), gated on `!FUN_0035c760() && FUN_0035c750()`; commits the row whose `row+0xFC & 0x10` (LEADER) is set | the list rows the mod already reads |

The user declined a probe, so both are hooked and the one that never appears never fires. A `LEADER`
log line names whichever did. **`row+0xFC` bit 4 = leader is only believed when bit 3 (in party)
agrees with the independent `ctx+0xB10+charId` witness** — the pointer it is read through is the
PORTRAIT child and "portrait == row" has never been measured.

### Shop equip COMPARE panel + equip-target screen — PROBE-CONFIRMED 2026-08-03, SHIPPED

`probe_equip_compare.js` run by the user; output in `..\FFXII-Decompile\notes\
probe_equip_compare_output.log`. Every structural criterion passed:

- `clsOk=1` on every pass; **exactly 1 header call + 9 column calls** per refresh, as predicted.
- **Zero stray delta targets** across all passes — `target` is always `colA+0xD8` or `colA+0xE8`.
- Labels read straight off the header widgets (`Attack Power(on)` / second slot `(off)` for a
  weapon). *Only kind 1 was exercised — no armour/accessory pass appears in the capture, so the
  two-label kinds are confirmed by code, not yet by play.*
- `canEq`/`eq` agree with the equip screen's own notice widget on every character.
- `vis=6`, member index != char id (mem 0..5 -> char 0,3,2,4,1,5), so `CharacterName(charId)` is
  the right lookup.
- Equip-target screen fires on entry, on **every** Left/Right, and after the equip; `slotItem`
  decoded correctly every time ("Whale Whisker", "Sagittarius", "Masamune", "Golden Axe",
  "Chopper"), `remaining` counted 1 -> 0.

**TRAP, measured:** the FIRST `FUN_0057b1a0` fires while **`DAT_02ca97a0` is still null**
(`[equip] FUN_0057b1a0 fired but DAT_02ca97a0 is null`). A reader keyed on that global misses the
screen's own entry announcement — validate the window handed to the hook instead.

**POLARITY — settled at 1.00, from Ghidra's own declarations rather than the log.** The log cannot
disambiguate it (delta 80 -> 0 on equipping fits both signs), but `FUN_002ca7c0`'s locals do:
`undefined1 local_b8[10]` is followed by `byte local_ae` (= `local_b8 + 0x0A`) and
`undefined1 local_8c[10]` by `byte local_82`, with `FUN_0030a4e0(member, 0, local_b8)` filling
CURRENT and `(member, slotsWithNewItem, local_8c)` filling NEW. So `delta = cur - new` and
**NEGATIVE = the new item is BETTER**. `EquipCompare` stores `improve = -delta` and never exposes
the raw value.

Stat slots, same source: kind 1 -> slot 0 = Attack Power (`+0x0A`) · kind 2 -> slot 0 = Evade
(`+0x11`), slot 1 = Magick Evade (`+0x12`) · kind 3 -> slot 0 = Defense (`+0x0B`), slot 1 = Magick
Resist (`+0x0C`). The mod does not use this mapping — it reads the labels off the widgets — but it
is recorded because it is what makes those labels trustworthy.

Shipped as `src/ui/equip_compare.{h,cpp}` + `src/ui/equip_target_reader.{h,cpp}`, keys `4`-`9`.

| Thing | Value |
|---|---|
| Compare panel | class `FUN_002cbf80` (`0x1ABF80`), size 0x170, parked at `menuCtx + 0x2E0` (`menuCtx` = `DAT_0209ac30`, RVA `0x1F7AC30`) |
| Refresh | `FUN_002cc4f0(itemId)` (`0x1AC4F0`) -> 1x `FUN_002cb1f0` + 9x `FUN_002ca7c0` (**RVA `0x1AA7C0`**, not `0x1AC7C0`), then `FUN_002cc2a0` (`0x1AC2A0`) lays out / hides |
| Called from | `FUN_0056e5d0:44-47` (EVERY shop highlight, unless `container+0x191 == 0x0B`), `FUN_0056d370:118` (confirm), `FUN_0057b1a0:95` (equip-target screen) |
| Columns | `colA[i] = *(panel + 0xD0 + i*8)`, `colB[i] = *(panel + 0x118 + i*8)`, i = 0..8; `colA+0xD4` = member index -> `*(menuCtx + 0xAC8 + idx*8)` = block, `block+0x60` = char id (i16, <0 = absent); visible count = low byte of `panel+0x168` |
| Flags | `colB+0xE0` = 1 already wearing this exact item · `colB+0xE4` = 0 CANNOT equip (`FUN_002cb360` clears it on `!canEquip`) |
| Labels | `header = *(panel+0xC8)`, `arr = *(header+0x60)`, label k codec at `arr[k]+0x18`, live iff `*(u32*)(arr[k]+8) & 1`. **A pure read — `FUN_002cb1f0` already resolved them, so no game call and no label ids are needed.** |
| Deltas | **`FUN_002cc780(pair, delta, target, style, outBuf, outSize)` (`0x1AC780`) — SIX arguments, not four.** `target` is `colA+0xD8` (stat slot 0) or `colA+0xE8` (slot 1); draws `abs(delta)` plus an arrow glyph chosen by `delta >> 0x1f & 1` |
| **`FUN_002cc780` args 5-6 are on the STACK** | Corrected S129 after this entry's four-argument form **crashed the game** (dump 2026-08-03 17:48). x64 passes only args 1-4 in registers; 5 and 6 live in the caller's outgoing area. `FUN_002ca7c0`'s call site, disassembled: `mov dword [rsp+0x28],8` (arg6 = buffer SIZE) · `mov [rsp+0x20],rcx` (arg5 = buffer PTR) · `mov r9d,0x10` (arg4) · `call FUN_002cc780`. The callee reads arg5 back as `[rsp+0x70]` after its prologue (spills rbx/rbp/rsi/rdi into the home area, pushes r12/r14/r15, `sub rsp,0x30`) and passes it to the writer at `0x365660`: `if (size >= 8) { *(u32*)buf = 'ex00'; buf[4] = '+'\|'-'; return 8; }`. **Ghidra hides these as `local_f8`/`local_f0` in `FUN_002ca7c0` because it did not attach the callee's signature — outgoing stack args always look like caller locals. Count parameters from the CALLEE's decompile, never from the call site.** |
| **POLARITY** | `FUN_002ca7c0:115-116` computes `FUN_0030a4e0(member, 0, cur)` then `(member, slotsWithNewItem, new)`, and every delta is **`cur - new`** — so a **NEGATIVE delta means the new item is BETTER**. Same polarity as the Equipment screen's twin renderer `FUN_003fe490:31-51`. **This is the make-or-break fact; do not ship a delta announce before it is confirmed live.** |
| Stat bytes | kind 1 -> `+0x0A` Attack Power · kind 2 -> `+0x11` Evade, `+0x12` Magick Evade · kind 3 -> `+0x0B` Defense, `+0x0C` Magick Resist (kind = item record `+0x50`; slot = `+0x4C`). Labels `FUN_002f9860(0x4A90..0x4A94)` |
| Equip-target screen | class `FUN_0057ac10` (`0x45AC10`), live at `DAT_02ca97a0` (RVA `0x2B897A0`); refresh **`FUN_0057b1a0` (`0x45B1A0`)** fires on entry (case `0xE`), every Left/Right, and every equip. Selected member = `*(i16*)(menuCtx + 0xDE0)`; L/R writers `FUN_0027f360` (`0x15F360`) / `FUN_0027ed10` (`0x15ED10`); footer help `FUN_002f9860(0xC70)` already goes through `FUN_00291d80`, which `TextCapture` hooks |
| Bound it yourself | `colA+0xD4` indexes `menuCtx+0xAC8` with **no bound check in the game** — clamp to 0..8 before dereferencing |

### `TextCapture::StringById` id-cache band — a trap

The passive cache holds ONLY ids `1000`/`1001` and `0x46dc..0x4882` (`text_capture.cpp:182`).
**`0x4A90..0x4A94` (the attribute labels) and `0x4C6` (Select Leader) are outside it** and read empty
forever. Use `TextCapture::ResolveStringById(id)` (added S125), which falls back to the
`FUN_002f9860` trampoline. **Game thread only.**


### Field Equipment screen — the per-highlight attribute PREVIEW (S125, shipped)

A SECOND comparison mechanism, unrelated to the shop's compare panel. The pause menu's Equipment
screen shows ONE character's nine attributes as `current > preview` — an absolute new value, not a
signed delta. Confirmed from a tester screenshot: `Attack Power 108 > 14`, `Evade 30 > 5`.

| Fact | Value |
|---|---|
| Panel | `menuCtx + 0x138`, class `FUN_003fe5d0` (`0x2DE5D0`) |
| Current / preview | `panel + 0xC8 + row*4` / `panel + 0xEC + row*4`, rows 0..8 |
| Labels | `FUN_002f9860(0x4A90 + row)` — outside TextCapture's cache band, so `ResolveStringById` |
| Fill | `FUN_003fead0(slotOverride)` (`0x2DEAD0`) — writes **2 iterations x 9 dwords at stride 0x24** from `panel+0xC8`, so iteration 0 lands on `+0xC8` (current) and iteration 1 on `+0xEC` (preview). `slotOverride == 0` copies current into preview, which is why Status shows no arrows. |
| **Per-highlight event** | **`FUN_003fe720(cmdId, arg)` — RVA `0x2DE720`** |

**`FUN_003fe720` is the hook point and it is NOT per-frame** — established, not assumed: its call
site sits inside `FUN_002c2320` **case 0xC**, the child-list event (S71: `0xC` list event, `0x8000`
cursor move at `packet+8`), immediately after `FUN_00291d80` sets the description bar for the
newly-highlighted row. One call per highlight.

Reader: `src/ui/equip_compare.cpp` `HookedAttrFill`. Speaks **queued**, never interrupting — the
row's own name is announced first off the 0x8000 and interrupting would cut it off. Needs no dedup:
on Status `current == preview` so the composer returns empty and it is silent there by itself.

### Description text has TWO independent sources — do not let them share a slot

| Source | Hook | What it is |
|---|---|---|
| Description BAR | `FUN_00291d80` (`0x171D80`) | the pane's help line ("Change equipment.") |
| ITEM DETAIL panel | `FUN_00292b70` (`0x172B70`) | the highlighted item's own stats/description |

Both used to write `TextCapture`'s single `g_helpText`, so it was last-writer-wins. On the field
Equipment screen the bar is re-set AFTER the item panel is formatted, so `o` read the pane help on
every row and never changed — it sounded stale because it was. Now kept in separate slots with
`CurrentHelpText()` preferring the ITEM detail when both belong to the current focus.

**The DESC dump is what separated the two candidate causes:** `gated=1` fires once per highlight
with the correct per-item bytes, proving capture was fine and the loss was downstream. Staring at
the capture path would never have shown it.

### Charge announce — DECLINED 2026-08-03. Not pending work.

> **User decision, do not resurrect this as a to-do.** The element at charge time was offered and
> declined. It is NOT blocked and NOT unfinished — it is unwanted.
>
> **THE REASON THAT MATTERS: it is not actionable.** Charge windows are short, the abilities that
> matter are usually buffs and debuffs prepared in advance, and there is no response a player can
> execute in the time available. A warning nobody can act on is just more words mid-fight. That is
> the tester's judgement from actual play and it is the load-bearing argument here.
>
> Two secondary points, recorded so neither is mis-remembered:
> * **The game does NOT draw the element in the charge message.** `battle_message.bin` has zero
>   `0F 3F` sprite escapes; ids `0x0D`/`0x0E`/`0x0F` are plain `"{0} begins casting {1}."`. So this
>   was never "read a picture the game shows" — unlike the item panel, where it was.
> * The MOD could still have supplied it (the element is in the action record at charge time, so
>   `Pain Flare` would have read as fire). "The data is unavailable" is therefore NOT the reason —
>   *the data exists and the feature was still not worth having.* Do not re-open this on the
>   strength of discovering the field.
>
> The recipe below is kept only so it need not be re-derived if the call is ever reversed.

#### (Kept for reference) why the element could not be attached, and the ONE hook that unblocks it

The CHARGE ANNOUNCE is the game's own battle sentence when an actor STARTS an action, before it
lands. `battle_message.bin` ids `0x0D` / `0x0E` / `0x0F`:

```
0x0D | style=0x01 | args=[attacker, action, -, -]   "{0} begins casting {1}."
0x0E | style=0x01 | args=[attacker, action, -, -]   "{0} readies {1}."
0x0F | style=0x01 | args=[attacker, action, -, -]   "{0} uses {1}."
```

The mod reads them verbatim (Tier 1) and speaks `0x0D`/`0x0E` only in **Verbose** (`F4` / the `F8`
menu); `0x0F` stays log-only in both. Attaching the ELEMENT here is the only way the mod can warn
about incoming damage type BEFORE the hit — the damage line is log-only, so today the element is
readable after the fact but never spoken during a fight.

**Why it was blocked.** The Tier-1 hook is on `FUN_00536410`, which sees the message id
(`argBlock+4 & 0x7FFF`) and the finished string, but **not the action id**. And the args cannot
supply it either: `FUN_002b49f0` consumes the arg block as 8-byte slots (`param_4 = param_4 + 2` on
an `int*`), and for a `0F 31` STRING slot it `memcpy`s the pointed-to codec (`:214-221`) — so the
`{1}` arg is a pointer to the action's NAME, not its id. Reverse-matching that name across 543 rows
is fragile and non-unique.

**How to unblock it — pair with the charge SITE.** `FUN_00304850` is the action-start function and
already known to this project (it owns the repeat gate that makes an actor announce once per
action/target pair). It has the actor and the action in hand, and the announce is emitted from
inside its own path, so the ordering is deterministic:

1. Hook `FUN_00304850` (**RVA `0x1E4850`**) and stash the action id it is starting.
2. The Tier-1 hook consumes that stash when the next message id is `0x0D`/`0x0E`/`0x0F`, and
   appends `BattleState::ElementNames(AbilityElements(actionId))`.
3. Clear the stash on consumption so a message that arrives without a matching charge adds nothing.

Remaining work before building it: confirm `FUN_00304850`'s signature and which argument/field
carries the action id, and confirm the charge announce is emitted downstream of it rather than in a
sibling path. Both are decompile questions, no probe needed. **Do NOT hook the emitter
`FUN_00469af0`** — the repeat gate deliberately declines to call it, and hooking it would cost the
game's verbatim wording in all 12 locales.

### Equipment status masks — the game shows ONE row; the mod now restores both (S125)

All three equipment builders share one idiom, verified in **all three** (`FUN_00293310:139`,
`FUN_00293fe0:117`, `FUN_00294b50:113`) rather than inferred from one:

```c
if (immune /* rec+0x44 */ == 0) { row = granted /* rec+0x48 */, label 0x2334/0x2336 }
else                            { row = immune,                 label 0x2333/0x2335 }
```

So a non-zero IMMUNE mask wins outright and **the granted-status row is never drawn at all** — at
any count, not merely past the collapse threshold. Two separate losses, both fixed in
`src/ui/equip_detail.cpp`:

1. **Collapsed row** (>= 4 entries) — the drawn row's phrase is SUBSTITUTED with the full list.
2. **Hidden row** (both masks non-zero) — the granted list is APPENDED, since there is no phrase on
   screen to replace. Label and names are game text; only the hiding is undone.

An earlier revision of this file claimed the hidden row should be left alone as "inventing a row the
panel never had". **Struck** — both facts matter when choosing what to wear, and the data is the
game's own. The `DESC` log still records every item where both masks are set.


## Save-slot preview record + Clan Primer entry record (Session 127)

Both confirmed against a live probe (`probe_save_and_primer.js`) over six real saves and every primer
sub-screen. `abs = RVA + 0x120000`.

### Save / load slot list

| what | where | conf |
|---|---|---|
| slot list window class | `FUN_0057fe80` RVA **`0x45FE80`** | 0.99 |
| detail pane / record filler | `FUN_0057efe0` RVA **`0x45EFE0`** (`args[1]` IS the record) | 0.99 |
| row painter | `FUN_0057fb00` RVA **`0x45FB00`** | 0.99 |
| header builder (write side) | `FUN_00583d60` RVA **`0x463D60`** | 0.99 |
| preview array | `container+0x590`, **200 × 0xA0**, cached on the list window at **`+0x1C0`** | 0.99 |
| row → array index | `u8[win + 0xEF + row]` — the list is ordered by RECENCY, so row ≠ slot | 0.99 |

Record fields (offsets into the 0xA0):

| off | type | meaning | conf |
|---|---|---|---|
| `+0x10` | u32 | playtime FRAME counter (= h:m:s × 60; used only to validate the split below) | 0.99 |
| `+0x18` | u16 | playtime **hours** | 0.99 |
| `+0x1A` | u8 | playtime **minutes** | 0.99 |
| `+0x1B` | u8 | playtime **seconds** | 0.99 |
| `+0x20 + n*4` | — | party slot *n*: charId (`0xFF` empty) / **level** / HP+MP gauge nibbles / flags, **bit 0 = PARTY LEADER** | 0.98 |
| `+0x44` | u32 | **clan rank** 1..12 — rank 2 resolved "Hedge Knight", rank 11 "Knight of the Round" | 0.98 |
| `+0x4C` | s32 | **map id** — the field that produces the location the row already spoke | 0.99 |

**RESOLVED BY SCREENSHOT — `+0x08` IS GIL** (conf 0.99). The 92-hour save reads 2,782,150 there and
the panel reads `GIL 2782150G`. `+0x0C` (961,190 on the same record) was the rival candidate and is a
different counter — it is NOT gil. Do not re-open this. Note why the earlier "confirmation" of the
mod's gil reader proved nothing: it matched `99999999`, which is also the clamp constant.

**RESOLVED BY THE SAME SCREENSHOT — `+0x40` IS clan points and `+0x44` IS the rank** (conf 0.98). The
panel reads `CLAN RANK Knight of the Round` / `POINTS 13422868` for the record holding 11 and
13,422,868. **An earlier note here struck `+0x40` on the grounds that 13.4 million was "absurd for
clan points"; that was reasoning from taste, not measurement, and it was wrong.**

**`FUN_003153f0` RVA `0x1F53F0` — `(rank) -> const uint8_t*` THE RANK'S NAME, not a string id**
(conf 0.98, probe-confirmed). Two observations settle it: the argument equalled the focused record's
`+0x44` (11), and the probe's own hook on `FUN_002f9860` logged `id=1254 -> "Knight of the Round"`
**before** `FUN_003153f0` returned — i.e. the resolve happens INSIDE it and the pointer that comes
back is the resolved codec string. Pass it straight to `GameText::Decode`.

**So there is NO rank->string-id table and none should be built.** A "id = 1243 + rank" formula fitted
to two observed points (rank 2 -> 1245, rank 11 -> 1254) was nearly shipped; the function makes it
unnecessary.

**RANK 0 = "no clan yet", and the game DRAWS NEITHER ROW** (conf 0.99, screenshot). On a rank-0 save
the detail panel shows only `LEVEL` and `GIL` — no `CLAN RANK` line, no `POINTS` line. The mod
suppresses both on rank 0 to match, and never calls `FUN_003153f0(0)`.

**GIL RE-CONFIRMED at the other end of the range** by the same screenshot: the panel reads `GIL 9G`
and that record holds 9 at `+0x08` (`+0x0C` holds 492). Two confirmations three orders of magnitude
apart — 2,782,150 and 9. `+0x08` is gil; treat this as closed.

**THE DISPLAYED SLOT NUMBER IS THE ARRAY INDEX** (conf 0.99), NOT `+0x54`. The rows read 004, 005,
006, 007, `<icon>`, 008 in exactly the order the row map gives (`...04 05 06 07 00 08`), and the
record at index 8 is the one whose `+0x54` holds `3`. **Index 0 is drawn with an ICON instead of a
number** — what that icon means is not established, so nothing names it. `+0x54` numbers something
else; it happens to equal the index on most records, which is why it looked right.

**THERE IS NO SAVE DATE ANYWHERE IN THE RECORD** (conf 0.98). Two saves 77 seconds apart in one
playthrough differ only in playtime, `+0x50`, the map id and the party gauge nibbles. FFXII's save
list shows location + playtime + party, not a timestamp. Do not re-derive this.

### Clan Primer ("Handbook")

| what | where | conf |
|---|---|---|
| sub-screen select | `FUN_0056f2a0` RVA **`0x44F2A0`** — `args[1]` = mode | 0.99 |
| entry record builder | `FUN_00576300` RVA **`0x456300`** — `args[0]` = the 0x50-byte record | 0.99 |
| set page (line-breaks one page) | `FUN_00573be0` RVA **`0x453BE0`** — `args[1]` = page index | 0.99 |
| page viewer class | `FUN_005742d0` RVA **`0x4542D0`** | 0.99 |
| entry list pane | `FUN_00571a50` RVA **`0x451A50`** | 0.99 |
| category panel | `FUN_0056f810` RVA **`0x44F810`** (feeds `FUN_00291d80`, so `o` already read it) | 0.99 |
| detail pane | `FUN_005701b0` RVA **`0x4501B0`** (record at `+0xE8`) | 0.98 |

Entry record: **`+0x03` u8 page count**, **`+0x08 + i*8` `char*` page *i*`**. Each page is
`HEADER 0x03 BODY` — the same page-break byte `GameText::DecodePages` splits on. **The body is
ORDINARY CODEC TEXT**: `FUN_00573be0`'s token measurer `FUN_002b1fb0` calls `FUN_002ac5f0`, the same
escape parser `game_text.cpp` derives `EscapeParamCount` from. Decoded live on the first probe run.

Sub-screen modes, confirmed by the strings each resolved: **4 = Traveller's Tips, 5 = Bestiary,
6 = Hunts**, 7 = present but empty on the probe save (almost certainly Sky Pirate's Den).

**AMENDED, NOT STRUCK — the "baked assets / OCR-only" note above.** That measurement holds for
TUTORIAL PANEL PICTURES. It does NOT cover primer PROSE, which is codec text on a pointer array. The
note read as a red light for the whole feature and it was not one.

**`FUN_002f9920` RVA `0x1D9920` is a SECOND string resolver** (keyed `id/10000` with a linear key
match, unlike `FUN_002f9860`'s `id/1000` indexing). Every primer title comes through it — bestiary
area names, hunt names, tip names, "CLAN RANK", "POINTS". The mod hooks only the first. If a title
needs resolving by id, **EXTEND `TextCapture::ResolveStringById`; do not duplicate it.**

**`viewer+0xF8` is NOT the page start** — after line-breaking it points at the LAST line laid out
(`"with a tolerance for needles."`). Read pages from the record, never from the viewer.


## Clan Primer round 2 — Hunts, the Den, and the page offset (Session 127)

All probe-confirmed (`probe_primer_screens.js`), `abs = RVA + 0x120000`.

### The page offset that made the Bestiary read one page behind

`FUN_00573be0(viewer, pageIdx)` does **not** index the record by the displayed page:

```c
bVar9 = param_2 + 1;
if ((*(uint *)(param_1 + 200) & 1) == 0) { bVar9 = param_2; }   // viewer+0xC8 bit 0
uVar7 = *(undefined8 *)(*(longlong *)(param_1 + 0xd0) + 8 + bVar9 * 8);
```

**Record page = displayed page + 1 when `viewer+0xC8` bit 0 is set.** The Bestiary sets it, so its
record page 0 is the `CLASSIFICATION / GENUS` box — not a page you can turn to. Traveller's Tips has
the bit CLEAR, which is exactly why Tips read correctly and the Bestiary read one behind.

Also off the viewer, so nothing needs latching: `+0xD0` = the entry record, `+0x8060` = the mode.

### Hunts (sub-screen 6)

| what | where | conf |
|---|---|---|
| Hunts LIST window class | `FUN_00579420` RVA **`0x459420`** (cell cb `FUN_005792f0`) | 0.99 |
| row builder | `FUN_00577c30(kind, cells, rowIndex, ...)` RVA **`0x457C30`** | 0.99 |
| row struct fill | `FUN_0037e5b0(id, out)` RVA **`0x25E5B0`** | 0.99 |
| Quest Progress pane | `FUN_00578450` RVA **`0x458450`** (cell cb `FUN_00578310`) | 0.99 |

Row struct (`out`): **`+0x00` char\* mark name**, **`+0x08` u16 the bitfield id**, **`+0x10` char\*
petitioner + place**. Confirmed verbatim: row 1 gave `"Red & Rotten in the Desert"` /
`"Tomaj (Rabanastre)"` / id `128`, matching the screen.

**`FUN_0046b1a0(huntId)` is a PURE BITFIELD TEST, so the mod does NOT call it:**
bit `(id & 7)` of `*(u8*)(BLOCK + 0x1020 + (id >> 3))`. The probe printed BLOCK at `0x2164480`
absolute = **RVA `0x2044480`**, i.e. `FUN_002ef640`'s base + `0x200` — which is precisely what
`FUN_002ef2b0` returns, corroborating the earlier note about that pair.

**Quest Progress body: `huntsDetail+0xD0` then `+0x28`.** `FUN_00578450` stores the block it is handed
at `win+0xD0`, so reading from the WINDOW avoids depending on the caller's argument shape. There is a
second string slot at `+0x18` on the other branch and it measured **EMPTY** — dumping both rather than
picking one is the only reason that is known rather than a coin flip.

### Sky Pirate's Den (sub-screen 7) — text located, EVENT NOT located

The tooltip text is ordinary resolvable string data, through the resolver the mod already hooks:

```
[str] mode 7 id=7016  -> "Balthier"
[str] mode 7 id=24030 -> "Awarded for {v}Attacking{v} over {v}300 times{v},
                          earning you the title of {v}Assault Striker{v}."
```

**But the Den emits NO `0x8000` at all.** Its only class is `FUN_00572e10` RVA **`0x452E10`**, sending
msgs `0xE`, `0xF`, `0x12`, and it does not paint through the universal list painter either — no
`[paint]` line for mode 7. Cursor movement between characters is internal to that window, so there is
no focus event to hang a reader off. **Nothing is built for the Den until that is found; the strings
being readable is not the same as knowing when to read them.**

### Sky Pirate's Den (sub-screen 7) — SOLVED

| what | where | conf |
|---|---|---|
| Den window | `FUN_00572e10` RVA **`0x452E10`** | 0.99 |
| achievement tooltip | `FUN_00572600` RVA **`0x452600`** — created at `FUN_00572e10:81` | 0.99 |
| cursor | **`den+0xCB`** u8 achievement index; **`0x1E` = nothing picked** | 0.99 |
| tooltip's latched index | `tooltip+0xC8` u8 | 0.99 |

**The name and body are `FUN_002f9860(idx + 0x1B68)` and `FUN_002f9860(idx + 0x5DDE)`** — the game's
own arithmetic, read straight out of `FUN_00572600`'s constructor, not a fitted mapping. Corroborated
live: index 0 produced ids **7016** ("Balthier") and **24030** ("Awarded for Attacking over 300 times,
earning you the title of Assault Striker.").

**The Den sends NO `0x8000` and paints through NO list painter** — it is a picture of sprites and the
cursor is internal. So the reader watches `den+0xCB` for a change after the game's own window proc has
run. That is the sanctioned per-frame exception, naming `FUN_00572e10`, and it is reset when the
sub-screen changes so re-entering re-announces.

### Traveller's Tips list — SOLVED, and it was never a primer bug

`InventoryReader::IsEmptyCategory` claims a window by **SHAPE** — null row array, live scroll widget,
live tab table — and the primer's entry list has exactly that shape. So the inventory reader was
claiming Traveller's Tips and then going deliberately silent on it:

```
[READER] pane owner=...2CC899C0 rowOff=0x0
[INV] empty category -- claimed and SILENT (row array null, scroll count clamped to 1)
```

That log line was printing on a list that was neither empty nor its own. Fixed by
`PrimerReader::OwnsSurface`, the same stand-down it already does for the shop.

**A SHAPE TEST CANNOT TELL TWO STRUCTS APART; ONLY IDENTITY CAN.** The shape claim was correct when it
was written and stayed correct until a second family happened to match it — and its failure mode is
silence, which is the one this project cares about most. Any future shape-claimed surface needs the
same stand-down list.

### The paint replay bypassed a claiming reader

`MenuReader::OnMenuPainted` replays a focus by calling `OnFocus` **directly**, not through the
dispatch chain where `SaveReader::TryFocus` / `PrimerReader::OnHuntFocus` sit. A save slot was
therefore announced in full and then a second time, bare:

```
[SPEAK-OUT] Bhujerba: Miners' End, 26 hours 33 minutes, Vaan, Level 11, 134 gil, Hedge Knight, ...
[SPEAK-OUT] Bhujerba: Miners' End
```

`OnFocus` now declines any surface with its own reader. **Anything that claims a row in the chain must
claim it on the replay path too** — there are two ways into that function, not one.

### Traveller's Tips list — the game DOES send focus messages

`[disp] mode 4 class +0x451a50 msg=0x8000 index=1, index=2` — the same class and the same cell
callback (`+0x451900`) the Bestiary list uses and reads correctly from. So the silence is **mod-side,
not a missing game event**, and the remaining question is whether `OnFocus` is reached and finds no
captured text. **CAVEAT on the probe's `(not the focused pane)` label: it is printed from the FIRST
sample of each (mode, class, msg) triple, which is usually construction-time. It is not evidence
about later focuses and must not be read as any.**

## `font00.dat` — the glyph table, and therefore the byte → character map (Session 130, conf 0.99)

**The font atlas IS the character map.** `FUN_002ac2f0:75` computes a glyph slot as `byte - 0x20`,
and `gamedata/d3d11/artdata/font/<locale>/font00.dat` carries the character for every slot. So:

```
codec byte = slot + 0x20
```

This replaced years of empirical byte-by-byte discovery. It is DATA, not inference.

### Record layout

```
+0x00  u32   version              (1)
+0x04  u32   glyph count          (us: 1301)
+0x08  u32   texture width        (us: 2048)
+0x0C  u32   texture height       (us: 1980)
+0x10  glyph records, stride 0x24 (36 bytes), `count` of them, then 24 trailing bytes

  within a record:
    +0x00  u32    kerning-class / group id (0 for the first run, then 0x37, 0x6E, 0xA5 ... )
    +0x04  u32    0x30      cell width
    +0x08  u32    0x37      cell height
    +0x0C  u32    advance
    +0x10  u32    advance (duplicate)
    +0x14  u32    0xFFFFFFFF on most records
    +0x18  u32    SLOT INDEX  -- equals the record's own ordinal, which is the parse check
    +0x1C  u32    THE CHARACTER, as its UTF-8 BYTES packed little-endian
    +0x20  u32    atlas U offset
```

### ⚠ THE CHARACTER FIELD IS UTF-8, NOT A CODEPOINT

It reads as a codepoint for ASCII and then stops making sense, which is the trap:

```
0x00000042  ->  bytes 42        ->  'B'        U+0042
0x000089C3  ->  bytes C3 89     ->  E-acute    U+00C9
0x0000BAC3  ->  bytes C3 BA     ->  u-acute    U+00FA
0x00A789E2  ->  bytes E2 89 A7  ->             U+2267
```

### ⚠ THE MAP IS PER-LOCALE

There are **five** font directories, not twelve — `us` (shared by the seven Western locales), `jp`,
`cn`, `ch`, `kr`. All 224 single-byte slots differ between them. In the `jp` atlas slot 0 is a
mathematical symbol, not `A`:

```
us  0x20-0x2F: ABCDEFGHIJKLMNOP
jp  0x20-0x2F: (2 math symbols, a colon, a kanji, ? ! / ( ) + = < > and two more kanji)
kr  0x20-0x2F: (CJK punctuation)
```

The mod ships the `us` table (`src/core/game_glyphs.h`), which is correct for every Western locale
and is what the codec was always implicitly built for. **A locale-detection hook is what would let
the mod pick the right one**; it does not exist (`plan.md` Phase 4 still lists it).

Fan translations matter here too: the Polish patch `PL_ff12_v1.3` ships its own `us/font00.dat`,
differing from stock in **20 bytes** — a handful of remapped slots. A translation that repaints the
atlas invalidates the shipped table for that install.

### The single-byte ranges, from the `us` table

| bytes | content |
|---|---|
| `0x20-0x39` | `A-Z` |
| `0x3A-0x53` | `a-z` |
| **`0x54-0x84`** | **accented Latin** — À Á Â Ä Æ Ç È É Ê Ë Ì Í Î Ï Ñ Ò Ó Ô Ö Œ Ù Ú Û Ü à á â ä æ ç è é ê ë ì í î ï **ñ (0x72)** ò ó ô ö œ ù ú û ü ß |
| `0x85-0x8E` | `0-9` |
| `0x8F` | em-dash U+2014 |
| `0x90-0xB7` | « » ¡ ¿ „ ~ ` ! ? @ # $ % ^ & * / _ + - = , . ; : \ ' " ( ) [ ] < > { } &#124; |
| `0xB8-0xFF` | arrows, circled marks, full-width forms — the shared-atlas CJK tail |

### The 2-byte extended banks — arithmetic known, NOT implemented

`FUN_002ac2f0:25-32`, for a lead byte below 0x20:

```
bank  = (DAT_0209cc38 == 0) ? DAT_00916570 : DAT_009165b0     // 16 ints each
slot  = bank[lead & 0xF] + (trail - 0x20)
advance 2
```

Read from the exe (`pefile`, RVA `0x7F6570` / `0x7F65B0`):

```
DAT_00916570 = 224 448 672 896 1120 1344 1568 1792 | 0 224 448 672 1120 1344 1568 1792
DAT_009165b0 = 224 448 672 896 1120 1344 1568 1792 | 2016 2240 2464 2688 0 224 448 672
```

So `bank[i] = 224 * (i + 1)` for `i` in 0..7 — leads `0x10`-`0x17` — where the two tables agree.
They diverge from index 8 on the runtime flag `DAT_0209cc38`, which the mod does not read.

**Deliberately not implemented.** In the `us` atlas slots 224+ are full-width Latin, kana and kanji,
which cannot appear in Western text; shipping a 1077-entry table of them would be dead weight and,
because the map is per-locale, actively wrong for a `jp` install. `game_text.cpp` still consumes
both bytes and emits nothing, which is correct behaviour for the audience the mod serves.

### Tooling

`FFXII-Decompile/tools/parse_font_dat.py`

```
python parse_font_dat.py <font00.dat> --check       # cross-check + summary
python parse_font_dat.py <font00.dat> --emit-cpp    # generate src/core/game_glyphs.h
python parse_font_dat.py <a.dat> --diff <b.dat>     # what a fan translation remapped
```

**`--check` is the ship gate.** It re-derives all **80** mappings `game_text.cpp` had established
independently — 62 structural plus 18 hand-won punctuation marks, each originally read out of
shipped text — and refuses to emit if one disagrees. On the stock `us` table, **0 disagree**. That
agreement between two unrelated methods is what puts this at 0.99 rather than at "a plausible
struct".

Extract the input with `tools/extract_vbf.py "*artdata/font/*/font00.dat"`.

### Session 130 (follow-up) — the Polish patch: the font metadata LIES, so the mapping came from the text

The Polish fan translation `PL_ff12_v1.3` repaints ~16 accented glyph slots to Polish letters. Its
`font00.dat` differs from stock in **20 bytes**, and **not one of them is a character field** — all
ten changed records changed only their *advance width*. So the file that is authoritative for a
stock install is actively wrong for this one, and every repurposed slot still claims the stock
letter it used to draw.

**Autodetection is not available.** `instaluj.bat` repacks the archive in place
(`ff12-vbf.exe -r ff12data ..\FFXII_TZA.vbf`) and patches `FileSizeTable_US.fst`. No loose file, no
marker, no version string. Hence a mod-menu row (**Text glyphs — Standard / Polish translation**,
default Standard) rather than a probe.

**How the mapping was recovered:** decode the patch's own shipped `ps2data` text with the STOCK
table and read the Polish. A repurposed slot shows up as a stock letter standing in a position
Polish orthography forbids, and the correct letter is the one that makes the word:

```
"Jù¿RùùùJùù nie moêe dosiègnàç celu."   ->  nie może dosięgnąć celu
"zamienia siè w kamieñ."                ->  zamienia się w kamień
"PÊ czèéciowo odnowione."               ->  PŻ (Punkty Życia) częściowo odnowione
"minè¿y róêne przypad¿oéci"             ->  minęły różne przypadłości
"BROŃ JEDNORĘCZNA" / "Bezimienne Źródło" / "Pani Życia i Śmierci"
```

**L-STROKE IS THE TRAP.** Lowercase `ł` sits at `0x94`, a PUNCTUATION slot, not in the accented
block — so the block rule cannot place its capital, and the obvious guess (`0x93`, the neighbouring
inverted-exclamation slot) is **wrong**. `Ł` is at **`0x81`** — the very byte the stock table maps to
`ú`, which this project had hand-derived years ago for "Cúchulainn". Found by asking which unmapped
byte behaves like a word-initial capital: 173 hits, witnesses `Łatwo` / `Łowca` / `Łupieżca`, plus 42
item and enemy names (`Arkadyjski Łucznik`, `Cesarska Łuska`, `Deszcz Łez`).

**The in-block rule, stated over its five confirmed pairs and no counterexample:** capital = lowercase
− `0x18` (ę/Ę, ś/Ś, ż/Ż, ź/Ź, ń/Ń). `ą`→`Ą` (`0x54`) and `ć`→`Ć` (`0x59`) follow from it and are
marked **rule-derived, not witnessed** — capital A-ogonek is essentially unattested in Polish and
capital C-acute is word-initial only in rare proper nouns, so neither appears anywhere in the
corpus. That is expected, not alarming, and it is recorded so a later session does not mistake it
for a measurement.

**End-to-end validation:** decoding the patch's name pool with the shipped table yields **1,351
clean Polish item and enemy names** — `Adamantowy Żółw`, `Agatowy Pierścień`, `Anielska Pieśń`,
`Arkadyjska Armia Żołnierz` — and **zero** names still containing a stock accented letter, which is
the check that proves no repurposed slot was missed.

`ó` needs no entry: the stock atlas already carries it at `0x7C` / `0x64` and the patch left both
alone (`Podróżnik`, `Żółć`, `Ósma` all decode correctly unmodified).

**KEYWORDS: Polish patch spolszczenie PL_ff12_v1.3 fan translation glyph override font00.dat
advance width repaint l-stroke 0x81 u-acute collision block rule 0x18 text glyphs mod menu
game_glyphs_pl.h autodetection impossible VBF repack in place**

> ⚠ **THE TESTER REPORTS THIS IS NOT FIXED (2026-08-05).** The mapping work above is recorded as
> findings, not as a solved problem. Re-measure against the TESTER's build and the TESTER's own log
> before building on any claim in this section — several of them may need striking.

---

## The HUD gauge system, and the script VM's variable storage (Session 131)

Established while building the spoken infamy meter for Bhujerba's shout minigame. Everything here
is read from the decompile and cross-checked against the shipped bytecode; the two agree.

### The gauge the scripts drive

| what | where | notes |
|---|---|---|
| `setgaugecounter` native | id **0x1AE**, group 0 | shim `FUN_0034EA90` (RVA `0x22EA90`) |
| **the HUD writer** | **`FUN_004085B0`, RVA `0x2E85B0`** | **`void(int)` — ONE int arg, and it IS the new counter value** |
| getter | `FUN_00408250`, RVA `0x2E8250` | reference only — see the `+0xC0` caveat below |
| manager global | `DAT_02B62D80`, **RVA `0x2A42D80`** | a POINTER slot; same HUD cluster as the dialogue nameplate `DAT_02B62D78` |
| the gauge object | `*(u64*)(mgr + 0xC0)` | re-resolve every use; it dies with the HUD |
| value / max | object `+0xE4` / `+0xE0`, both s32 | the writer stores `+0xE4` BEFORE dispatching its redraw |
| write suppressed when | `*(u8*)(mgr + 0xC8) & 2` | log it; do not gate speech on it |
| tween state | `+0x108`, `+0x118`, `+0x11C`, `+0x120` | `changegaugecounterbyframe` (`0x1B2`) drives these |

**ARITY: the detour takes one int, counted from the CALLEE.** The shim above it, `FUN_0034EA90`,
decompiles as `void(void)` while actually reading its VM context out of R9 — hooking *that* would
have repeated S129's four-args-on-a-six-arg-function defect exactly. Hook the writer, not the shim.

**OPEN MEASUREMENT — the byte at gauge `+0xC0`.** As decompiled, `FUN_004085B0` nulls the gauge
object when `*(char*)(gauge+0xC0) != 0` and *then* dispatches its redraw on that same byte being
0/1/3, which cannot both be true; `FUN_00408250` carries the identical guard. The likely reading is
that Ghidra folded two adjacent fields (`+0xC0`/`+0xC1`). **Nothing in the mod gates on it** — the
value at `+0xE4` is what the script asked for either way — and `shout_meter.cpp` logs `+0xC0`,
`+0xC1` and `mgr+0xC8` on every meter-key press so one play pass settles it.

### Script variable storage — resolver `FUN_00262440` (RVA `0x142440`)

Module records live in the **same five-slot array the entity scan already walks**:
`NavRva::HANDLE_TABLE_BASE` = RVA `0x1F78E10`, stride `0x288`. A slot is a *controller*: a script VM
instance plus its actors. As longlong indices into the record (verified against loader
`FUN_0026C8C0`, RVA `0x14C8C0`):

| field | byte off | meaning |
|---|---|---|
| `mod[0x00]` | `+0x00` | the module base — **the file base + 0x80**, see the correction below |
| `mod[0x08]` | `+0x40` | storage-class **1** base |
| `mod[0x09]` | `+0x48` | storage-class **0** base |
| `mod[0x0B]` | `+0x58` | storage-class **4** base — the loader assigns `0x02099DF0` (RVA `0x1F79DF0`) to *every* module: the cross-script global int work array |
| `mod[0x0C]` | `+0x60` | storage-class **5** base — `0x02099FF0`, the float globals |
| `mod[0x0F]` | `+0x78` | the variable **descriptor table** (= `base + *(i32*)(base+0x28)`; **not** "filled at load" — see below) |

Class **3** is module-local: `ebpBase + *(u32*)(ebpBase + 0x40)`. Class **2** is per-actor and is
resolved by CALLING `mod[0x13]` with a live VM context — there is no address to compute outside a
running native, so the mod declines class 2 rather than guessing.

```
desc     = *(u32*)(mod[0x0F] + 4 + varIdx * 8)
elemType = desc >> 28        0=u8 1=s8 2=u16 3=s16 4=u32 5=float  (strides 1/1/2/2/4/4)
class    = (desc >> 24) & 7
address  = classBase + (desc & 0xFFFFFF)
```

**READ THE BASES FROM THE RECORD, not from the globals the loader happens to store there** — same
values, but the record is what the engine's own resolver reads, so the two cannot drift apart.
`PUSHV` = `FUN_00267FC0`, `POPV` = `FUN_00267E60`. **Do not hook the resolver**: it is on every
variable access in every script.

`DAT_02099D70` (RVA `0x1F79D70`) holds a pointer to the *currently executing* module record
(save/set/restore around every module entry point). The mod does not use it — scanning the five
slots is cheaper to reason about and does not depend on when the engine sets it.

### ⚠ CORRECTED (S137): the runtime module base is the FILE base + 0x80

**`mod[0]` does not address the file header.** It addresses the **section directory**, which the
file places at offset `0x80` and which announces itself with **`0x8000000B`**. Measured from a live
`'` dump: record `+00` pointed at bytes `0B 00 00 80`, not at `EBP2`. Three sessions of shout
features sat dark behind a module that never resolved, because the code checked for `EBP2` at `+0`.

**THIS STRIKES THE S131 CLAIM that `+0x14`/`+0x24`/`+0x28`/`+0x2C`/`+0x48`/`+0x50` are "zero in the
extracted file and filled by the loader's relocation pass".** They were never zero, and there is no
relocation pass — they were read at the FILE base, 0x80 too early. At `base = file + 0x80` every
field the loader uses is populated in the shipped file:

| at base | byu_a01 value | what |
|---|---|---|
| `+0x14` | `0x9BA0` | the count table the loader walks |
| `+0x28` | `0x2DD30` | the **variable descriptor table** (`mod[0x0F]`) |
| `+0x40` | `0x180` | the class-3 storage base |
| `+0x90` | — | the three name strings |

**A MEASURED DESCRIPTOR, from the play log that first resolved a module:** `byu_a02.src` meter
variable `0x0E` decoded as `desc = 0x000006B1` → **elemType 0 (u8), storage class 0, offset
`0x6B1`**, and the write landed. Class 0 is `mod[9]`, i.e. per-module storage rather than the
class-4 global that S131 inferred — so the meter is NOT self-evidently shared across the fourteen
street scripts. Whether `mod[9]` is itself a shared allocation is not established; do not assume
either way.

### A module names itself — the identity that replaced a mapId table

**At file offset `0x110` (runtime `base + 0x90`) every EBP2 image carries three NUL-terminated
strings: a build stamp
(`DD/MM HH:MM`), the author, and `<module>.src`.** This is FORMAT-LEVEL, not a per-map discovery:
it parses correctly on **all 809** EBP2 files in the game — map scripts and event scripts alike —
with the third string matching the file name every time, across 18 distinct author names.

That is why `shout_table.cpp` is keyed on `"byu_a01.src"` and not on a map id: the mapId to script
join is genuinely unproven for the Bhujerba streets, and this makes the join unnecessary.

### Bhujerba is `byu`

**Bhujerba's internal map code is `byu`** (Japanese katakana Byuerba). `bhm_*` is the **Sky Fortress
Bahamut** — confirmed from its own message text. A search for `bhu*` finds nothing.

### The shout minigame's own mechanics (from the bytecode)

Meter is a per-module script variable, max **100**, and all three thresholds were recovered
generically and agree with a hand decode of `byu_a01`:

* `v >= 100` — success; the story proceeds (message 9, "You. Boy. You will come with us.")
* `v >= 30` — the Imperial penalty: a **30-step `-1` loop, one step per frame** (`0x34C35`)
* `v >= 1` — idle decay, a single `-1` through `changegaugecounterbyframe`

**The increment loop updates the script variable AFTER the gauge call** (`0x34EF8`:
`PUSHV meter / PUSHII 1 / OPADD / CALLPOPA setgaugecounter`, then `POPV meter`), so at the instant
the native runs, `storage == newValue - 1`. That is a guarantee, not a guess, and it is the
falsifier `shout_fill.cpp` requires before it writes.

**THE GUARD IDENTITY IS NOT IN THESE SCRIPTS.** A full native census of byu_a01's shout region
(911 instructions, `0x34800`-`0x35200`) contains no `distance` native of either slot hypothesis,
and the "how many heeded" weights come from variables set elsewhere rather than npcdic ids pushed
as immediates. So no earshot radius and no guard npcdic id could be extracted at the 0.98 bar; the
mod ships `guardNameIdx = -1`, reports the game's own NPC names with bearing and distance instead
of a verdict, and dumps a per-map npcdic census to the log so the first play pass measures it.

### Two errata in the RE archive, both corrected

1. **`notes\athena_opcodes.md` (and `tools\ebp_disasm.py`'s `OP` dict) map name-index to opcode
   DIRECTLY; the real opcode is name-index + 1.** The proof is internal to `ebp_disasm.py`: its
   `isize()` model, taken from the VM's own length table `DAT_01efea60` via `FUN_0025E4C0`, says
   opcodes `>= 0x48` carry an inline u16. `PUSHV` is the first operand-carrying opcode in name
   order, so `PUSHV` must be `0x48` — the direct mapping puts it at `0x47`, which would make a
   variable push carry no variable index. Correct values: `OPGTE 0x0E`, `OPADD 0x12`, `PUSHV 0x48`,
   `POPV 0x49`, `PUSHI 0x4E`, `PUSHII 0x4F`, `JMP 0x51`, `CALL 0x58`, `CALLACT 0x59`,
   `CALLPOPA 0x5D`, `CALLACTPOPA 0x5E`. **The map scripts reach natives through `CALLPOPA`**, so a
   CALLACT-only scan finds nothing at all.
2. **`output\script_native_table.txt` and `output\native_slots.txt` assume an 8-byte native record;
   the real record is 32 bytes with three function slots**, and `impl(N) = record(N+1) slot 0`.
   `output\group_tables.txt` already has it right. Group 0 base `0x01EED700`. Native names join
   from `notes\dbg_symbols_mapctrl.csv` at **`dbgIndex = id + 5140`** — anchored on `mapjump`
   (id `0x8D`, index 5281) and re-confirmed by `rand 0x29` and `setgaugecounter 0x1AE`.

**The `0x28F` vs `0x290` distance-native tension is RESOLVED in favour of S107's `0x290`.** The
22 apparent `0x28F` sites in `byu_a01` cluster in a data region at `0x3C600` on a regular 15-byte
stride and **none of them decodes as code at any alignment** — they are a table that happens to
contain the byte pair. There is no off-by-one on the native id; only on the opcode table.

**KEYWORDS: gauge setgaugecounter 0x1AE FUN_004085B0 0x2E85B0 DAT_02B62D80 gauge+0xE4 gauge+0xE0
infamy meter shout minigame Bhujerba byu byu_a01 script variable descriptor FUN_00262440 storage
class mod+0x78 module base file+0x80 section directory 0x8000000B src name 0x110 file 0x90 runtime athena
opcode off by one CALLPOPA native table stride 32 dbgIndex 5140 distance 0x290 resolved**

## Session 147 — the Libra flag, the status L1/R1 event, the font fingerprint, the reward panel

**KEYWORDS: libra FUN_0030c300 FUN_00290100 0x10F68 bit30 enemy HP numbers status screen L1 R1
FUN_002c2c50 0x1A2C50 character switch instance letter DisplayNameForActor font00.dat advance width
fingerprint DetectVariant autodetection struck reward panel FUN_0035e070 additemmes row array
quantity gil 0x833 status timer 0x17C strict reach terrain bit23**

### LIBRA — the "HP-visible" flag, found (conf 0.98). STRIKES "there is no flag to read"

Session 32 recorded *"no Libra HP-visible flag found — BtlChr status bit `0x10000` was WRONG, read 0
for the un-Libra'd enemy"*, and the enemy readout has been percentage-only ever since. The rejection
of `0x10000` was correct — it is the forced-max-display bit (`FUN_00329220:102-124`, the Disease /
Bubble path). **The hunt failed because it looked on the ENEMY. Libra is a status on a PARTY member.**

| Fact | Value | Conf |
|---|---|---|
| Libra predicate | `FUN_0030c300` (RVA **`0x1EC300`**) — walks the 9 party slots, skips the KO'd, returns 1 when any LIVING member has **bit 30** of `(BtlChr+0x64 \| BtlChr+0x3C)` | 0.98 |
| Per-frame mirror | `FUN_0028e290:58-62` writes it to **bit 1 of `*(u32*)(P + 0x10F68)`**, `P = DAT_0209be80` (RVA `0x1F7BE80`) | 0.99 |
| Getter the game uses | `FUN_00290100` (RVA **`0x170100`**) = `*(u32*)(P + 0x10F68) >> 1 & 1` | 0.99 |
| What it drives | `FUN_002bfd20:130,139-141` — show-numbers flag = (target is a party member) OR this bit; `FUN_002c0400` (RVA `0x1A0400`) draws HP as DIGITS when set and passes `0xFFFFFFFF` (blank) when clear | 0.98 |

So **"Libra is up" and "the enemy's HP is a number rather than a bar" are the same fact**, and the
mod mirrors the game's own swap rather than bypassing it. Corroboration: `FUN_002f82f0`, the
trap-visibility toggle (Libra's other documented effect), is driven by the same predicate.

Mod side: `BattleTargetReader::LibraActive()` — one guarded u32 read, no game call. `HpClause()` is
the single HP choke point; `LibraDetail()` adds Level (`bc+0x1C2`), MP (`bc+0x4C`/`+0x28`, gated on
the game's own `BC_MP_GUARD_A/B` sign test), statuses (`BattleState::StatusNames`) and the elemental
weaknesses (see the next block).

### ENEMY ELEMENTAL WEAKNESS = `BtlChr + 0x40`, ONE BYTE (conf 0.98)

> ⚠ **STRIKES this section's own first version**, written earlier in Session 147: *"ELEMENTAL
> WEAKNESSES ARE NOT AVAILABLE… the game's own Libra does not show weaknesses either, so speaking
> them would invent a fact the screen never states."* **Both halves are false.** Libra draws a
> `Weak:` row of element icons on the target panel, and the mask behind it was already arriving in
> the vitals snapshot the mod's own hook receives.

The chain, end to end:

```
BtlChr + 0x40  (u8)   bits 0..7 = Fire Lightning Ice Earth Water Wind Holy Dark
   FUN_00329220     *(char*)(snapshot + 0x89) = (char)bc[0x10]     // int* index 0x10 == byte 0x40
   FUN_002bfd20     FUN_00295d90(0x80, panel + 0x200, *(u8*)(panel + 0x149))   // panel+0xC0 = snapshot
   FUN_00295d90     (RVA 0x175D90)  emits FUN_002f9860(0x2331) == "Weak: "
                    then FUN_002f9860(0x4B27 + bit) per set bit == the eight element sprite strings
   panel + 0x200 -> widget (panel+0x60 -> +0x30 -> +0x18 -> +0x60 -> [0]); its colour word +0x24
                    carries an ALPHA ramped by panel+0x282/+0x284 — which FUN_002bfd20:151-176 drives
                    from FUN_00290100(), THE LIBRA FLAG.
```

So the row is Libra-gated by the same predicate as the HP digits, and **only weaknesses are shown**:
`FUN_00295d90` hardcodes `0x2331` and has exactly one caller. Absorb / Half / Immune
(`0x232F` / `0x2330` / `0x232E`) appear only in the three equipment detail panels.

**THE LIBRA-PROOF FLAG (the `????` marks and bosses) — extended status bit 41.** Same function:
`if ((*(u8*)(panel + 0x111) & 2) != 0) { iVar19 = 1; iVar18 = 0; }` zeroes the weakness row's alpha
**and** blanks the HP digits. `panel+0x111` is snapshot `+0x51`, and snapshot `+0x4C + i` is
`bc[0x68+i] | bc[0x78+i]` (`FUN_00329220`'s 4×4 copy loop) — so the bit lives in the extended status
mask, byte 5, bit 1. The mod honours it: `BattleTargetReader::LibraSuppressed`.

**WHY THE FIRST SEARCH MISSED IT, recorded because the negatives are still TRUE and still useless.**
There genuinely is no Weak/Absorb/Half/Immune **quartet** on the enemy — that shape belongs to the
**equipment** record (`+0x3C..+0x3F`) and its only consumers are the equip-preview scratch globals
`_DAT_02ae96a0..ac` (`FUN_00374280` / `FUN_003745c0`). There genuinely is no affinity field on the
per-actor enemy record at `actor+0xE68`. And no element survives to the damage-apply site. All three
hold; none of them is about a **single byte on the BtlChr**. The lesson is in `debug.md`: a negative
result is only as good as the shape you searched for — when one comes back empty, re-derive what the
DISPLAY reads, because the game draws it, so something reads it.

**Adjacent and deliberately NOT identified:** `bc+0x41`, `+0x42`, `+0x43`, `+0x44`, which
`FUN_00329220` copies to snapshot `+0x8A..+0x8D`. Nothing displays them. The equipment quartet's
order is a tempting fit for them and that guess is precisely what produced the struck claim above.

### `+0x42..+0x51` / `target+0x17C..+0x18A` are PER-STATUS TIMERS, not affinity (conf 0.99)

**STRIKES `notes\combat_re_2026_07_20_damage.md:302`** — *"i16 x8 resistance/affinity overrides"*,
self-rated 0.90. Producer and consumer agree: `FUN_00385570:36-56` walks the extended status bits at
`bc+0x68`/`bc+0x78`, takes the timer-slot index from `statusMasterRec+0x06` (`0xFF` = no timer),
reads `*(i16*)(bc + 0x17C + slot*2)`, decrements it, and writes the result to `result + 0x42 + slot*2`.
Eight i16 slots. Writer of the live array: `FUN_0030ea40:26`.

### Status screen — the L1/R1 character switch is `FUN_002c2c50` (RVA `0x1A2C50`, conf 0.99)

`FUN_002c2320` category `0xa` is per-frame input and must not carry an announcement (already
recorded). The cycle itself is an EVENT: `FUN_002c2240` (R1 / pad RIGHT) and `FUN_002c2200`
(L1 / pad LEFT) call `FUN_0027f360` / `FUN_0027ed10` — the writers of `menuCtx+0xDE0` — and **only
when the index actually moved** do they play SE `0x51` and call `FUN_002c2c50(ctrl)`.

`FUN_002c2c50` is the screen's own refresh: ailment grid (`menuCtx+0x110`), portrait child, then
`FUN_002c2cd0` -> `FUN_003fead0(0)`, the attribute-panel fill. **Exactly three call sites in all
33,105 functions** — those two plus `FUN_002c2320:100` inside cat-1 CREATE. Signature
`void(uint32_t)`. Hooked by `status_reader.cpp`; the CREATE site is a no-op by construction because
`g_active` is only set after `s_origCtrl` returns.

### `font00.dat` — the fan-patch fingerprint. STRIKES "autodetection is not available"

S130 concluded a fan translation cannot be detected because the patch repacks the VBF in place. True
of the DISK, and beside the point: the question is which atlas the GAME LOADED. S130 also recorded
the marker without recognising it — the patch *"adjusts ten advance widths"*.

Diffed byte for byte, stock vs `PL_ff12_v1.3` `us/font00.dat` (both 46,876 bytes): **exactly 20
differing bytes, in 10 records, every one an advance width** (stored twice per record, `+0x0C` and
`+0x10`).

| slot | 60 | 61 | 62 | 84 | 85 | 86 | 98 | 117 | 118 | 179 |
|---|---|---|---|---|---|---|---|---|---|---|
| stock | 21 | 21 | 21 | 22 | 22 | 22 | 24 | 19 | 36 | 36 |
| PL | 20 | 24 | 24 | 17 | 18 | 18 | 20 | 11 | 11 | 11 |

Runtime read: font manager `DAT_01f811f8` (RVA **`0x1EE11F8`**, via `FUN_001b5fa0`);
`FUN_0017f8c0(slot)` (RVA **`0x5F8C0`**) -> `FUN_001e9860(mgr, slot)` -> `FUN_001fdec0` — a **red-black
tree lookup returning `node+0x24`**, so the in-memory record is the LOADER's layout, not the file's.
`GameText::DetectVariantOnce` therefore does not assume a field: it walks every 4-byte offset and
matches the ten values against the stock or PL vector, so the advance field is *located* by
measurement. No match means log the values and stay Standard. The `Text glyphs` mod-menu row is gone.

### The multi-item reward panel IS `FUN_0035e070` — STRIKES the S72 "different surface" claim

`debug.md`'s S72 entry says of the hunt-reward panel: *"It is not the single-item obtained toast the
mod already reads (`message_reader.cpp`, `FUN_0035e070`) — that one is a one-line toast with no title
and no quantity column."* **Wrong on both counts.** The function has a row loop; nobody had read its
body. Descriptor at `msg+8`:

```
+0x00  i16  mode / title flag        (FUN_0035e070:53 -> local_6e8[0] = (mode == 0))
+0x04  i16  ROW COUNT                (the loop bound at :101)
+0x08  u8   layout flags             (bit 0 = the bordered multi-row panel; clear = the plain toast)
+0x0C  row array, stride 8:
         i32 kind   0 = item, 1 = gil
         kind 0: item id in the low half of +4, QUANTITY as i16 at +6
         kind 1: raw amount at +4, suffixed with message id 0x833 ("gil")
```

Rows render into three 0x180-byte slots and compose into `widget+0xC8` through a `0F 31` template
(`FUN_002b4090`, cap `0x4A0`) — which is the buffer the mod already decodes, so the reader may well
have been right all along. **Why it is silent is NOT yet established**: across all 20 archived dev
logs the init line appears 20 times and `diag: item popup proc FUN_0035e070 fired` **zero** times, so
there is no evidence the surface was ever visited in a dev session. S147 shipped the descriptor
logging instead of a guess.

### `NavReach::ReachableStrict` — a measurement, not a gate

A second flood beside the permissive one, refusing `NavMesh::TerrainRefused` polys, published
separately and read only by the exit-scan routability line (`terrain=` / `strict=`). It exists
because our own logs print `*** UNWALKABLE (bit23) ***` for a poly class and, lines later,
`walk=1 reach=1` for the exit standing on one — and `unreachable=0` in every log ever. **Nothing may
filter on it** until a log shows it going 0 on a genuinely unreachable exit while staying 1 on
Bhujerba's Travica Way, which carries the same flags and routes fine. Re-arming bit 23 itself is
struck twice over: S96 tried and reverted it, and Travica Way is a live counterexample.

## Session 148 — the summoned Esper's BtlChr, and its duration gauge

**KEYWORDS: esper summon BtlWork 0x5AD4 0x5AD8 0x5ADC 0x5B04 FUN_00306760 FUN_00320a40 FUN_003135e0
FUN_003135c0 FUN_0031b9f0 roster lists summon gauge pips key 8 party status Belias**

### The Esper is NOT in any roster slot — it has its own field on BtlWork (conf 0.98)

Keys `4`-`7` read roster list 3 (`W+0x5A7E`) and can never reach an Esper, because an Esper is not
in that list at all. From the summon-commit function **`FUN_00306760`**, action class
`DAT_022c215c == 1` — the Esper-summon class, action ids `0x106`..`0x112`, i.e. exactly the thirteen
Espers:

```c
case 1:                                                   // Esper summon
  *(u32*)(W + 0x5B04) |= 1;                               // summon-mode bit
  *(u8 *)(W + 0x5AD5) = summonerBtlChr[0x04];             // control index := the SUMMONER's charId
  *(u8 *)(W + 0x5AD4) = actionRec[0x26];                  // <-- the ESPER's BtlChr INDEX
  iVar4 = FUN_002fa0e0(summonerBc, actionRec[0x26]);      // per-Esper master record byte +0x32
  *(float*)(W + 0x5AD8) = *(float*)(W + 0x5ADC) = (float)iVar4;   // gauge: current, then max
  lVar6 = FUN_00320a40(actionRec[0x26]);
  FUN_0030c470(lVar6, summonerBc[0x1C2], 0);              // esper level := summoner level
```

| offset | width | meaning |
|---|---|---|
| `W + 0x5AD4` | u8 | **the summoned Esper's BtlChr index**; NOT cleared on dismiss |
| `W + 0x5AD5` | u8 | control index — during a summon this is the SUMMONER's charId |
| `W + 0x5AD8` | f32 | duration gauge, current (the pips beside the Esper's HP) |
| `W + 0x5ADC` | f32 | duration gauge, max — seeded equal to current at summon |
| `W + 0x5B04` | u32 | battle sub-mode bits; **bit 0 = summon active**, set here, cleared by case 2 |

**What pins `0x5AD4` as a BtlChr index** rather than a master-data id is the use directly below it:
the same byte goes to `FUN_00320a40`, whose entire body is `idx < 0x28 ? W + 8 + idx*0x1C8 : 0` —
byte for byte the BtlChr-array arithmetic `BtlChrForSlot` already uses.

**Read the mode bit FIRST.** `0x5AD4` is not cleared on dismiss (only the bit is), so reading the
index alone keeps naming the last Esper summoned for the rest of the session.

Shipped as `BattleState::EsperBtlChr` / `EsperGauge`, spoken on key `8`.

### The gauge's UNIT is not established, and must not be asserted

`FUN_002fa0e0(summonerBc, esperIdx)` returns byte `+0x32` of the per-Esper master record
(`DAT_02ebf130`, header `+0x08` stride / `+0x0C` records, relocated by `FUN_0020e600`). It is a
per-Esper constant, stored into both halves of the pair — so the decompile says how big the gauge is
and nothing about what depletes it. The spoken word therefore names the gauge and claims no unit;
`party_status.cpp` logs the raw float pair on every press so one watched summon can settle it.

### The five roster lists — `FUN_0031b9f0(slot, list)` (conf 0.99)

The resolver behind `FUN_00320ab0`. Bound check `FUN_00322c50(0x17, slot)` is `slot < 9`; each list
is **9 x u16** BtlChr indices, `>= 0x28` empty:

| list | base | note |
|---|---|---|
| 0 | `W + 0x5A48` | |
| 1 | `W + 0x5A5A` | |
| 2 | `W + 0x5A6C` | |
| **3** | **`W + 0x5A7E`** | the unmasked master party — the one the mod reads (slots 0-2 active, 3 guest, 4-8 reserve) |
| 4 | `W + 0x5A90` | |

Lists 0/1/2/4 have not been characterised. **None of them was needed for the Esper** — that hunt was
avoided entirely by reading the summon commit instead, which is the general lesson: the writer of the
state names the state, and a slot hunt is what you do when you have not found the writer yet.

### `FUN_0031b770(bcIdx)` — BtlChr index -> field actor

`idx < 0x28`, then scan the actor pool (`FUN_00236850` count, `FUN_00236820(i)`) for the actor whose
`+0x698` def pointer equals `W + 8 + idx*0x1C8`. The inverse of `BtlChrForActor`, and the reason a
summoned Esper resolves a name through `NameForBtlChr`: while it is out it has a pool actor.

### The HP DISPLAY CLAMP — `FUN_002fef30` (abs `0x2FEF30`, RVA `0x1DEF30`), conf 0.99

**KEYWORDS: HP cap 9999 clamp display Bubble HP x2 party side BC_KIND boss over 9999 DisplayHp**

The game's own rule, from the tail of that function:

```c
cVar9 = *(char *)(btlChr + 5);                 // BC_KIND -- 0 = party side
if (value < 1) out = 1;
else { cap = 1000000000; if (cVar9 == '\0') cap = 9999;
       out = (cap < value) ? cap : value; }
```

**`param_1` is a BtlChr on two independent counts:** `+0x05` is the field `phyre_types.h` already
documents as *"0 = party side"*, and a few lines above, the same function tests
`charId - 0x1B < 0xD` — the exact guest range `0x1B..0x27` that `battle_state.cpp` carries.

**The cap is PARTY-SIDE ONLY.** Anything else gets `1e9`, i.e. no clamp — which is why the mod takes
the game's selector rather than hardcoding 9999: a boss with more than 9999 HP still reports its real
number under Libra.

**Why it was needed.** Bubble (the `HP x2` icon on the Status screen) doubles **current** HP in memory
without touching the stored max, so a bubbled level-99 character reads `14638/7319` while the party
screen draws `9999/7319`. Verified against one Status-screen screenshot covering all six characters:
`+0x24` matched the MAX column **6/6 exactly**, and `+0x48` matched the HP column exactly on the three
*without* the icon (Balthier 8437, Fran 6174, Ashe 6171) and read exactly `2 x max` on the three
*with* it (Vaan 17026/8513, Basch 14638/7319, Penelo 12026/6013). **The offsets were never wrong; the
clamp was missing.** Belias likewise reads `12786/12786` and draws `9999/9999`.

Shipped as `BattleState::DisplayHp(bc, value)`. Applied to **both** halves of every spoken HP pair —
clamping only the current would have said *"9999 of 7319"* and the Esper *"12786 of 9999"*.

**Two deliberate departures, both load-bearing:**
- **The floor is NOT replicated.** `if (value < 1) out = 1` belongs to that function's max-HP
  recompute, where a max of zero is meaningless. On CURRENT hp it would turn a KO'd ally into
  "1 HP" — a number the player would act on. A dead ally must read 0.
- **Percentages and thresholds use the RAW pair.** `HpClause`'s enemy percentage and
  `CombatEvents::CheckVitals`' 20% latch and KO test are ratios, not display. Clamping a bubbled
  ally's current to 9999 against a max of 7319 computes 137%; clamping both flattens a real
  difference to 100%. The clamp is for digits the player hears, nothing else.

### `sceneObj + 0x14` bit `0x40` = PRESENT IN THE WORLD (Session 148, measured)

**KEYWORDS: stale entity pruner presence bit 0x40 READY_PRESENT_BIT defeated enemy despawn corpse
tracker sceneObj 0x14 model loaded 0x20**

Same byte as the already-known "model loaded" bit `0x20` (which the engine's own interaction
predicate `FUN_002675c0` tests). Observed values:

| value | objects | verdict |
|---|---|---|
| `0xF0` | live party members, a live enemy | present |
| `0x70` | **treasure, field gimmicks** | **always set — carries no information** |
| `0xB0` | a DEFEATED enemy; reserve slots never spawned | absent |

The entity scan applies it as a **general stale-entity pruner** and not as a kill detector — the
user's own framing, and the correct one: *"you're still tracking it as if kills matter, when what we
want is a stale entity pruner."* For combatants and NPCs that is measured and it stands.

> **STRUCK — Session 150.** This paragraph used to read: *"Treasures keeping the bit SET is the
> load-bearing observation. It means `0x40` is not 'is a live combatant' but 'is in the world', so
> one test prunes a corpse, a despawned NPC, **a consumed chest** and a cleared trigger alike."*
>
> **The treasure clause is false.** It was never measured — it was inferred from unopened treasure
> reading `0x70`. Refuted by `x64\logs\FFXII-Screen-Reader-2026-08-10_12-08-35.log:1133-1145`, which
> dumps two treasure slots the game **never placed** (world origin, `layers=0`) still reading
> `r14=0x70`, bit `0x40` **set** — while the never-spawned *enemy* reserve in the same dump reads
> `0xB0`, bit clear. On these objects the bit is set unconditionally; bit `0x80` is what separates
> the two populations. Corroborated by a flat `Treasure=5` across 30 rescans, never decrementing.
>
> **Consequence:** the pruner already runs on every treasure, in both walks, and *provably cannot
> ever fire on one*. That is the tester report of 2026-08-11 — collected treasure never leaves the
> list. Do not widen this bit to fix it; collected state lives in a different backing store
> (see **Treasure spawn and collection** below). The corpse / despawned-NPC / cleared-trigger half
> of the original claim is unaffected and remains measured.

**Measured before it was applied.** A counter on exactly this condition read `0` before a kill and
`1` across 35 consecutive rescans afterwards, while `Enemy=1` refused to fall and one defeated Hyena
stayed listed. Applied in BOTH walks — the handle table and the actor pool — because the pool walk
only sees what the handle walk did not list, so a prune in one is an admission in the other.

Every prune logs itself (`[NAV-DIAG] absent: [c:slot] +0x14=0x.. kind=.. "name" at (x,y,z)`, capped
at 8 per scan): the risk of a presence test is that it removes something still wanted, and a bare
count cannot tell one corpse from one NPC deleted by mistake.

**What this REPLACED, and why none of it was needed:** an HP gate on the actor-pool walk (S147's
proposal, struck by the user — the fix is not about death), a liveness test on the BtlChr, and a
`KIND_DEAD` skip that has measured zero in 602/602 rescans and is still inert. None of them would
have caught a departed NPC; this does. (The original sentence also claimed "a consumed chest" here —
struck above, and struck in Session 150. Collected treasure is handled separately, below.)

---

## Treasure — spawn, award, and how a COLLECTED one is detected (Session 150)

**Do not call these chests.** There is no opening animation; the game's own script natives are
`setuptreasure` / `talktreasure` / `settreasureflag`, and the mod has always used
`Category::Treasure` (npcdic 434 "Treasure", 468 "Urn").

### ⚠ FIRST: `FUN_002fa740` / `FUN_002f8060` / `DAT_02ec3ea0` are the TRAP system, NOT treasure

This cost most of a session and is the single most useful thing in this section. That family looks
exactly like treasure — a per-map presence mask, a spawn roll with a percentage, a record table with
X×10/Z×10, a "consume" that clears the mask bit — and it is traps. Five independent proofs:

1. **This file already said so** (the `sceneObj` / Libra section): `FUN_002f82f0`, which walks that
   same `DAT_02ec3ea0` mask, is the **trap-visibility toggle** gated on the 0.98-confidence Libra
   predicate `FUN_0030c300`.
2. **No button press.** `FUN_002f8060` is reached per-frame from the actor tick
   (`FUN_00233f70:45` → `FUN_00310db0:67`) and fires on distance `<= 1.3`. Treasure needs Confirm.
3. **It is an AoE.** `FUN_002fb8c0` returns *every party member* within `record+0x0A`, and the
   caller applies an effect to each.
4. **One shared model for all 32 slots** (`DAT_02b5ec94`, set once). Treasure has many models.
5. **The dbg name** — `settrapresource`, `settrapshowstatus`.

The treasure ring is **`DAT_02ec3e60`** (RVA `0x2DA3E60`), written by `FUN_002fb430` (`0x1DB430`);
3 × `{mapId @+0, presenceMask @+4, decidedMask @+8}`.

### The definition record (script-bytecode data, not a table in memory)

| off | type | meaning |
|---|---|---|
| `+0x00` | u8 | treasure index — bit position in the map's presence mask |
| `+0x04` | s16 | world **X × 10** |
| `+0x06` | s16 | world **Z × 10** |
| `+0x08` | u8 | bits 0-5 yaw in 60ths (negated); bit `0x40` randomise gil |
| `+0x09` | s8 | one-time global flag id; **`0xFF` = respawner** |
| `+0x0A` | u8 | spawn % |
| `+0x0B` | u8 | gil-vs-item % |
| `+0x0C`/`+0x0E` | u16 | common / rare item (no Diamond Armlet) |
| `+0x10`/`+0x12` | u16 | common / rare item (Diamond Armlet equipped) |
| `+0x14`/`+0x16` | u16 | gil, no armlet / armlet |

**There is no enumerable record table.** `FUN_002faed0` takes ONE def; each def reaches the handlers
as a **script-bytecode literal** decoded by `FUN_002650b0(ctx, _, encoded)` — pure segment-base
arithmetic on `DAT_02099d70`, whose `param_2` is unused. So treasure cannot be enumerated the way
the trap records can.

### The three functions that matter

| what | function | RVA | notes |
|---|---|---|---|
| `setuptreasure` INIT | `FUN_00354400` | `0x234400` | 4 params. Rolls the spawn via `FUN_002faed0`; stashes the encoded def in the VM state block |
| `setuptreasure` SIMPLE | `FUN_00355210` | `0x235210` | 3 params. **Has the def AND the scene object together** — decodes the def, `FUN_002faf60` → position, `FUN_0026af80(obj, …)` places it |
| **award** | `FUN_002fafd0` | **`0x1DAFD0`** | **3 params**, returns the item id. Sole call site `FUN_0050faf0:21` |

`FUN_002fafd0` is identified beyond doubt by the **Diamond Armlet branch** — the only place in the
binary that swaps the whole common/rare pair on `BtlChr+0x6B & 2 || +0x7B & 2`. It sets the one-time
save flag (`FUN_0032ad70`), clears the presence bit, and rolls gil-vs-item then common-vs-rare.

### Why the mod reads the AWARD and not the object — and how it identifies which treasure

**The engine never writes a treasure's identity onto its scene object.** `FUN_00355210` places the
object and discards the id; nothing on the object carries the def, the index, or the flag. Every
per-object candidate was tested against the 0.98 bar and failed:

- `+0x1C & 0x004` (ACTION) — **0.55**. `talktreasure` (`FUN_00355830`) disarms it via
  `FUN_0025d5e0(obj, 2)` for the *whole message window*, so a live treasure reads ACTION-clear
  mid-interaction; and the converse ("ACTION clear ⇒ collected") is false for story-gated objects
  too. The only setter anywhere is `FUN_0026b4a0`, and no caller passes a constant 2.
- `+0x14 & 0x20` (model loaded) — 0.35. `FUN_003ec700` writes a *model-instance* field, never a
  scene object, and that path is trap-side anyway.
- `+0x14 & 0x40` (presence) — **useless**: set unconditionally on treasure, see the strike above.

So the mod hooks the **award** instead. Position falls out of the same record the placement used
(`FUN_002faf60`: `x = (s16)(def+0x04)/10`, `z = (s16)(def+0x06)/10`, y = 0), and
`FUN_00355210` placed the object at exactly those floats — so the collected treasure is named by the
coordinates **the game itself used to put it there**. Implemented in
`src/navigation/treasure_state.cpp`; the scan consults it for `Category::Treasure` only
(`entity_scan.cpp`, after classification) and drops via `NoteFiltered`.

**Lifetime: cleared on map change.** The spawn roll shows two populations — a def with a real
one-time flag id is skipped for good once `istreasureflag(def+0x09)` is set, while a def with
`+0x09 == 0xFF` records nothing at award time and re-rolls its spawn PERCENTAGE on every map load.
How often the second kind is seen to return in play is not settled and does not need to be: clearing
is the safe direction either way. If the treasure is gone for good the engine never places it again,
so a retained record has nothing to match and clearing costs nothing; if it can return, a retained
record would hide it. **Holding it is the only choice that can be wrong.**

**Fail-safe:** the record is only ever written by a real award event, and a coordinate that matches
nothing simply leaves the entry listed — the pre-fix behaviour.

**PLAY-CONFIRMED 2026-08-11.** Collected treasure leaves the nav list. This also settles the one link
the decompile could not reach: **the scene transform reads back the floats the placement wrote**, so
the award record's `(s16)/10` coordinates identify the object exactly. `kMatchTol` (0.25 m) is slack
for the float round-trip, **not** a search radius — do not widen it; a loose radius would drop the
treasure *beside* the one collected.

---

## Frame pacing and the game-speed multiplier (Session 152, 2026-08-12)

**The canonical entry for this cluster is `Docs\combat_system.md` §7.5** — that is where the RVAs
and confidences live, and it carries a standing **read-only** prohibition on all of them. This is
the nav/per-frame-side pointer to it, recorded because the whole tier-C frame-counter class in
`Docs\PerFrameAudit.md` turns on one fact stated nowhere else:

**`FUN_0022a770` (RVA `0x10A770` — what the mod hooks as `FIELD_FRAME`) CONTAINS the sim loop.**

```
0022a770:105-109   ac4 = (gate == 0) ? 1.0 : speedTable[speedIndex]
0022a770:250       acc += ac8 * ac4        // once per call
0022a770:139       while (1.0 <= acc) {    // the sim loop
0022a770:185           acc -= 1.0
```

Because the mod detours the OUTER function, `HookedFieldFrame` and every callee it dispatches fire
**once per call at 1x, 2x and 4x alike** — game speed runs the *inner* loop more times. Conf 0.99
on the reading; runtime confirmation is what `core/frame_probe.cpp` exists to print.

**Consequence, and the reason this is here rather than only in the combat notes:** no mod behaviour
keyed to how often the field tick fires can be affected by the game-speed setting. Only frame RATE
can move it. Do not re-derive this.

`ac8` (`DAT_02064AC8`, RVA `0x1F44AC8`) is the per-frame delta **in sim ticks, not seconds**, and is
a hard `1.0f` at every reachable writer (`0022a0b0:44`, `002628b0:165`, `003601d0:13`). Its only
variable writer `FUN_00343ee0` has **zero callers**. Whether `FUN_0022a770` is itself called at
display refresh or at a paced rate is **NOT ESTABLISHED** and is runtime-only — see
`Docs\PerFrameAudit.md` §2 of the Session 152 block.

### Walkmap context writer — CANDIDATE, below the 0.98 bar, do not build on it

**`FUN_0026e960` (abs `0x26e960`, RVA `0x14E960`)** is the sole non-zero writer of `DAT_0209a670`
(the gate) and `DAT_0209a678` / `DAT_0209a680` (walk ctx0 and camera ctx1) — the globals
`MapQuery::HasWorld()` / `MapQuery::Ctx0()` read. Install sequence is its caller `FUN_0026e760`
(reset `FUN_0026e640`, allocate, then `FUN_0026e960`); unload is `FUN_0026e640` / `FUN_0026e1d0`.

**Confidence is on WHAT it writes, not on WHEN.** Its call-site timing relative to the leader-actor
install (`FUN_00269c70`) and the fade is unmeasured, so this is **not** usable as a "walkmap is
ready" event without a Frida confirmation probe. It was traced while looking for an event-driven
replacement for the planner's wait and rejected for that purpose — readiness is a conjunction
across four subsystems, so hooking one writer still leaves `IsFieldNavSafe()` to evaluate. It is
recorded because it is the right lead for the separate walkmap-IDENTITY question
`map_seams.h:46-51` raises (the map id flips before the engine swaps the walkmap).

**Also corrected here:** `NavHooks::HookedWorldStep` caches the **Bullet** context, which is a
DIFFERENT world from the SQEX one `CondWorld` tests (`BulletQuery::HasWorld()` reads `*(ctx+0x60)`;
`MapQuery::HasWorld()` reads the globals above). The existing world-pointer cache says nothing about
nav readiness — do not reach for it as one.
