# FFXII-Screen-Reader — Debugging Log

This file is structured for keyword searching. **Always grep before proposing solutions.**

## Tried & Failed

Approaches that were attempted and did NOT work. Each entry tagged with `KEYWORDS:` for
grep. Check this FIRST to avoid repeating failed approaches.

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

## Solved Problems

Problems that were resolved. Each entry has `KEYWORDS:` + `SOLUTION:`. Check this to
reuse known-good solutions.

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

### Turn-by-turn routing polish (open — Session 33, 2026-07-12)
Routing WORKS (SQEX walkmap, `plan=Route`) but has three quality problems reported by the
user. Detail + hypotheses in `sessions_001_current.md` Session 33.

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
