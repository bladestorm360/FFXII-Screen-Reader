# FFXII Screen Reader — Project Risk Audit

**Date:** 2026-05-07
**Reason for this audit:** FFX laid solid groundwork (menus, battle,
basic pathfinding) and then hit walls that stalled the project to a
no-op. The pattern there: each individual feature looked feasible at
groundwork-design time, but the runtime data we needed to drive a
GAME-IN-MOTION (current map id, current dialogue context, current
quest state, walkable target list) wasn't reachable from any of the
hooks we placed. We don't repeat that here. Before any more menu or
navigation code, every feature in the v1 plan gets a brick-wall risk
mapping with explicit static-validation gates.

The rule applied throughout: a feature is NOT feasible until the
specific piece of runtime state it depends on has been (a) located
in the .exe statically and (b) confirmed reachable from a hookable
function call. Data tables in the VBF (planmapname.bin etc.) prove
that LABELS exist; they don't prove we can WIRE the labels into a
live screen-reader event.

---

## Categorical risk: "the runtime gap"

The thing that brick-walls JRPG screen readers is not data — it's
runtime context. Every feature needs three things wired up:

1. **Trigger.** When does the event fire? (Map changed. Cursor moved.
   Dialogue advanced. Damage applied.)
2. **State.** What was the relevant game state at that moment?
   (Which map. Which menu. Which speaker. Which target.)
3. **Label.** What human-readable name does the player hear?
   (Cloister of Distant Song. Items. Vaan. Fire spell.)

LABEL is solved for everything we've audited. TRIGGER is partially
solved (we have script-level API names, but the C++ entry points
they map to are unconfirmed). STATE is the highest-risk axis: most
JRPGs hide the "what menu am I in / what is the player doing right
now" enum behind several layers of indirection.

Audit format below: per feature → trigger / state / label status →
validation gates → brick-wall probability.

---

## v1 features (Phases 3-7)

### Phase 3 — Title + main menu

**What it needs:**
- Detect title screen open / close
- Detect main menu (Items / Equip / Save / etc.) open / close
- Detect cursor focus change
- Read the focused option's text verbatim
- Detect commit

**TRIGGER status:** PARTIAL.
- Script-side: `fsttl_initialize`, `fsttl_opencommand`, `fsttl_openconfig`,
  `fsttl_closecommand`, `fsttl_newgamestart`, `fsmenu_openmenu`,
  `fsmenu_closemenu`, `fsmenu_movechip`, `fsmenu_showcursor` all named.
- Native side: UNCONFIRMED. We need either the .ebp interpreter
  (option 17 + follow-up) or the native impls of the fsmenu_* family
  by string xref (none of those names are in the .exe as ASCII).
- Cursor-singleton write sites at `0x1E61248` (Yes/No), `0x1E5F5F0`
  (multi-choice) — write functions are UNCONFIRMED but readily found
  by xref-to-data.

**STATE status:** PARTIAL.
- Save/Load menu state machine identified at `FUN_001cd0e0`
  (74-of-74 string xrefs from prior session).
- Other menu state machines: not yet identified. Each needs the
  same string-xref pass against its anchor strings
  (`mGambitPositionOffsetX`, `mCursorRightX`, etc.).

**LABEL status:** SOLVED.
- The text spoken is whatever PTextObject is rendering at the
  cursor cell — read live from PhyreEngine, no decoding needed.

**Validation gates (must close before Phase 3 implementation):**
- G3.1: Locate cursor-singleton WRITE function via xref to
  `0x1E61248`. Five-minute Ghidra script we haven't run yet.
- G3.2: Confirm `Phyre::PText::PTextObject` exposes its source
  string in CPU-readable memory at draw time (decompile its
  setText / setString virtual method).
- G3.3: Identify per-menu state machine for at least 2 more menus
  (Items, License) via the same string-cluster xref method that
  found Save/Load.
- G3.4: At least ONE of: (a) interpreter found, OR (b) confirmed
  we can resolve fsmenu_* native impls by string anchor — so we
  have a hook surface bigger than just write-watching cursor data.

**Brick-wall probability:** LOW-MEDIUM.
The cursor singletons are concrete data anchors. Even if the
interpreter is unfindable, watching cursor writes + reading the
focused PTextObject covers menu reading. The risk is per-menu
shape: License Board is a 2D grid, Gambit Editor has condition+
action pairs, Bestiary has paginated entries — each needs its own
shape-aware reader. We've already deferred License Board grid
nav and Gambit editor to v2 in `plan.md`; that risk-shedding is
correct.

---

### Phase 4 — Field navigation (DQ7R-style crow-flies entity list, NOT auto-walk)

**Scope narrowed 2026-05-07.** v1 ships with crow-flies entity listing
plus turn-by-turn directions on keypress (DQ7R model). Auto-pathfind
moves to v1.x optional. Path VALIDATION (is the target reachable on
foot?) IS in v1 — it's how we filter the entity list and decide whether
to speak "blocked, route around" vs a clean bearing.

**What it needs:**
- Player position (X, Y, Z) — continuous read
- Player facing
- Current map id
- Current map display name
- Per-map list of NPCs / save crystals / treasures / signposts /
  exits, each with position + type label
- Distance / bearing announce on hotkey
- `[` / `]` to cycle the entity list
- AUTO-WALK to selected entity

**TRIGGER status:**
- Position: solved. DrummerIX BattleUnit struct has `+0x?` position
  field (offsets table in `community_rvas.md`).
- Map transition: PARTIAL. Script API has `mapload`, `mapdispose`,
  `mapjump`, `mapjumpresult`. Native impls UNCONFIRMED. The
  current-map-ID global (`nowmap_no` script variable) needs a
  static xref to find its in-RAM RVA.
- NPC / gimmick enumeration: UNCONFIRMED. Two paths:
  (a) interpreter hook on `setnpcname` / `setuptreasure` /
      `setsaveramsavestatus` — captures every gimmick set up by
      the script when the map loads
  (b) walk a runtime entity list in PhyreEngine's POctreeWorld
- Auto-walk: NOT ESTABLISHED. See below.

**STATE status:**
- Current map ID: PARTIAL. Variable name known
  (`nowmap_no` in mapctrl.dbg); RVA unknown.
- "On the field" vs "in menu" vs "in cutscene" vs "in battle":
  PARTIAL. Likely a global state enum somewhere. Not yet xref'd.
  Worth dedicated investigation — this is the core context the
  rest of the mod modulates on.

**LABEL status:** SOLVED for maps + entity types.
- planmapname.bin → map name
- npcdic.bin → NPC type (Bangaa Merchant, etc.)
- setfieldsign mes → "press X to ..." prompt for the gimmick

**Validation gates (v1 crow-flies + path-validation scope):**

- **G4.1: Player BattleUnit struct base + position offsets** — CLOSED.
  DrummerIX CT seed validates the offsets across 4+ community sources.
- **G4.2: Current map ID at runtime** — partial path identified.
  Direct: xref the planmapname.bin filename → `nowmap_no` global.
  BUT the filename literal does NOT appear in the binary (filenames
  are constructed via path-format templates). Indirect: hook
  `Phyre::PText::PTextObject::setText` — captures the area-name
  banner shown when entering a new map, gives us the human-readable
  name directly without needing the integer ID.
- **G4.3: Map name table** — DROPPED as a static gate. The mod ships
  its own parsed `planmapname.bin` (we have the file extracted and
  the offset cipher decoded). We map (current-area-banner-text →
  decoded names) at runtime by name match instead of ID lookup.
- **G4.4: Entity enumeration** — TWO compatible paths, can use both:
    (i) Reactive: hook `setfieldsign` (the on-screen "press X to talk
        to ___" prompt). When player walks within `talkradius` of
        any gimmick / NPC, we capture (type, position, label).
        Builds a list of "things I've passed" automatically.
    (ii) Engine walk: enumerate `CharacterOrderedObject` /
        `ChrModel` / gimmick instances via their managers. Gives
        the full per-area list without requiring the player to
        walk near each entity first.
- **G4.5 AUTO-WALK function** — **DROPPED from v1.** Moves to
  v1.x optional. Not blocking.
- **G4.6 walkability data** — **CLOSED.** Bullet is the engine,
  RTTI'd, ready to query. No separate NavMesh needed.
- **G4.7 Player yaw/facing offset in BattleUnit struct** — narrow
  grep against `decompile_all.txt`; same struct as position.
- **G4.8 path-validation queries (NEW for crow-flies scope)** —
  CONCRETE static targets:
    - `btCollisionWorld::convexSweepTest` RVA `0x71B780`
      (vtable slot 5, 9779 bytes) — the capsule sweep we use to
      validate "is target reachable on a straight line."
    - `btCollisionWorld::rayTest` RVA `0x71DE00` (vtable slot 4)
      — line-of-sight; cheaper than sweep when we just need
      visibility.
    - btCollisionWorld vtable at `0x1BAE610`,
      btDiscreteDynamicsWorld vtable at `0x1BAF5E0`.
    - Player physics controller is
      `Phyre::PPhysics::PPhysicsCharacterControllerBullet`
      (concrete vtable `0xC50770`, abstract `0xC50AB0`).
    - Remaining narrow runtime addresses: active world global ptr
      and player controller field offset — found via xref to those
      vtables. ~30 minutes of grep against `decompile_all.txt`.

**Brick-wall probability:** LOW.
- Path validation uses RTTI-labeled engine code that's already
  visible in `decompile_by_class/bullet.c`.
- Crow-flies bearing math is trivial in C++ once positions are read.
- No A* or NavMesh needed. The narrow gap is finding 2-3 runtime
  pointers, all closeable by grep.

**FFX comparison:** FFX brick-walled because the mod tried to drive
movement (auto-walk through 3D corridors with non-obvious branch
points). DQ7R-style crow-flies + path validation puts the player in
control: they navigate, the mod TELLS them what's reachable and
gives a heading. The player corrects course in real time the way a
sighted player would.

---

### Phase 5 — Dialogue & cutscene text

**What it needs:**
- Detect dialogue page open
- Read speaker + line verbatim
- Detect page advance
- Detect close

**TRIGGER status:** PARTIAL.
- Script-side: `evctrl.dbg` has eventlist, eventbosslist1..4,
  eventresult, eventpos. Plus `printfmenu` (ctrl), `talktreasure`,
  `talkfadein/out`.
- Native side: UNCONFIRMED. The C++ function that takes a message
  ID and renders dialogue must exist. xref the .msb files
  (`btlctrl.msb`, `mapctrl.msb`, `evctrl.msb`, `ctrl.msb` —
  all extracted) to find the loader, then walk to the consumer.

**STATE status:** PARTIAL.
- Active speaker / current message ID: not yet found.
  `setmeswincaptionid` (set message-window caption by id) is the
  natural candidate — its hook gets us the speaker string.

**LABEL status:** SOLVED.
- All localized text in .msb files (plus per-locale message bins).
- Decoded by the offset cipher we cracked.

**Validation gates:**
- G5.1: `*.msb` loader function via filename xref. Lands the in-RAM
  message table. From there, the consumer function (passes
  `(table_base, msg_id)`) is the universal text-display hook.
- G5.2: `setmeswincaptionid` native impl by string xref to caption
  literal strings, OR hook the .ebp interpreter when this script
  function is called.
- G5.3: Confirm rendered dialogue text is staged in CPU memory
  long enough for our hook to copy it. PTextObject internals.

**Brick-wall probability:** LOW-MEDIUM.
- We have all the labels and the script API names. The only
  failure mode is "rendered text never reaches CPU-readable
  buffer" — uncommon in PhyreEngine, which is text-heavy by design
  (PBitmapFont is RTTI'd and fully part of the public middleware).

---

### Phase 6 — In-game menus (Items, Equip, Save, License, Magic & Tech)

Same architecture as Phase 3. Each menu adds layout-aware code
on top of the shared cursor-watch + read-PTextObject pattern.

**Per-menu specific risks:**
- Items / Equip: low risk. List menus of the type we already have
  state machines for.
- Save / Load: solved (FUN_001cd0e0 already identified).
- License Board: HIGH layout risk — 2D grid with prerequisite
  edges. Already deferred to v2 in `plan.md`.
- Magic & Tech: low-medium. List menu with sub-categories.
- Gambit menu: HIGH — 200+ conditions × actions × per-character
  slots. Already deferred to v2.

**Brick-wall probability:** LOW for the v1 subset (Items, Equip,
Save, Magic). The deferred ones carry their own risk into v2.

---

### Phase 7 — Combat log

**What it needs:**
- Pause game during log read
- Capture damage / heal / status / KO events with actor + target
- Combo aggregation (~750 ms)
- 50-event continuous FIFO
- Modal hard input intercept (open with F4, close with Esc)
- Critical-event auto-speech (KO, <20% HP)

**TRIGGER status:**
- Damage: solved. DrummerIX `DamageMod` AOB at line 1047 of CT.
- Status apply: solved. DrummerIX `StatusEffect` AOB.
- Heal: not yet solved. Likely the same write-monitor on HP, but
  filtered to positive deltas.
- KO: not yet solved. HP→0 transition + active flag.
- Pause / time scale: not yet solved.

**STATE status:**
- BattleUnit struct: solved (community-validated offsets).
- Active battle flag: PARTIAL. There's a "we're in battle" signal
  somewhere — likely the same global as the time-scale.

**LABEL status:** SOLVED.
- Damage type, status name, character name all read from the
  same data tables we've audited.

**Validation gates:**
- G7.1: Damage event funnel: validate DamageMod AOB resolves to a
  single function in retail TZA (Frida-prototype the AOB once we
  have a build).
- G7.2: Pause global RVA via xref to a pause-related string
  (e.g., "PAUSED" if displayed, or to the menu-active flag).
- G7.3: HP-write monitor design: hook the write site, classify
  delta sign, distinguish damage vs heal.
- G7.4: KO via HP=0 + is_active=0xFF transition.

**Brick-wall probability:** LOW.
- DrummerIX's reverse-engineering covers the hottest paths.
- The pause global is the only soft spot, and even if it's
  unfindable we can fall back to the menu-active flag (combat
  log opens only when the in-battle pause menu is reachable; we
  enter that menu by simulating its key, freezing the game by
  proxy).

---

## Cross-cutting risks

### CC1: The interpreter hunt

The .ebp interpreter is currently the single largest unsolved static
question. Every phase except 3 (cursor-watch) and 7 (HP write-watch)
benefits from it. Status:

- v1 (option 14): EBP2 magic absent → not a literal-magic compare
- v2 (option 15): no controller filenames, no script names as ASCII,
  no struct-of-arrays dispatch in `.rdata`
- v3 (option 16): bytecode not statically embedded
- **v4 (option 17, queued):** asset manifest pointer table + VBF
  magic SRYK reader

Even if v4 fails, the interpreter being unfindable is not a
project-killer — it just makes Phase 4-7 implementation more
expensive (per-feature native function identification instead of one
universal hook). It does NOT make features impossible. The phases
each have non-interpreter fallback paths in their gates above.

**Mitigation if v4 fails:** vtable walks of the 11 game-side classes
(`MapRenderObject`, `CharacterRenderObject`, etc.) each give us
~10-50 named virtual methods. Combined with string-cluster xrefs at
the menu-state-machine RVAs we already have, that's likely enough
labeled native code surface for v1 — at the cost of 2-3 weeks of
per-feature manual labeling vs ~3 days for a single interpreter
hook.

### CC2: Locale handling

Solved at the data layer (12-locale text decoded). Risk is at the
mod side: locale enum global is still TBD. We do NOT need it to
ship v1 in English-only; locale runtime detection can land in v0.2.

### CC3: PhyreEngine text rendering pipeline

We assume PTextObject keeps source strings in CPU memory. If FFXII
PC port changes that for D3D11 GPU-side composition, several phases
slip. **High-leverage validation:** decompile
`Phyre::PText::PTextObject`'s setText / setString method (vtable
slot known) — confirms or denies CPU staging in 30 minutes of
analysis.

This single check should land before Phase 3 starts. It's the
foundational assumption under all menu and dialogue reading.

---

## The audit's verdict

| Phase | Brick-wall risk | Hardest gate |
|---|---|---|
| 3. Title + main menu | LOW-MEDIUM | G3.2 (PTextObject CPU staging) |
| 4. Field navigation (crow-flies, NOT auto-walk) | **LOW** | G4.8 active-world ptr |
| 5. Dialogue/cutscene | LOW-MEDIUM | G5.3 (PTextObject again) |
| 6. In-game menus | LOW (v1 subset) | inherits 3 |
| 7. Combat log | LOW | G7.2 (pause) |

**Major scope revision 2026-05-07:** Phase 4 narrowed from auto-walk
to crow-flies + path-validation (DQ7R style). Risk dropped from HIGH
to LOW. Highest-risk gate G4.5 (auto-walk) moved to v1.x optional.

**The two pre-implementation must-resolves:**

1. **G3.2 / G5.3 — PTextObject CPU staging.** One Ghidra session.
   Foundational; if it fails, Phases 3 / 5 / 6 all change shape.
2. **G4.5 — Auto-walk.** Multi-day investigation. Must answer:
   does a navigate-to-target function exist in FFXII (gambit-AI
   follow being the most likely source)? If yes, Phase 4 is on
   solid ground. If no, we either build A* on Bullet (significant
   work but doable) or accept manual-only navigation in v1 with
   compass + entity cycling.

Run option 17 next as planned (it advances multiple gates: G3.4,
G4.2, G4.3, G5.1 all benefit from the asset-manifest / VBF reader
trail). After that, the next two scripts to write — in priority
order regardless of option-17 outcome:

- `walk_phyre_text_vtable.java` — decompile PTextObject's
  text-update method, confirm CPU staging. Closes G3.2 / G5.3.
- `find_gambit_ai_navigate.java` — search for the function that
  drives party AI follow / gambit "approach target" behavior.
  Closes G4.5.

Those three together (option 17 + the two follow-ups) determine
whether we proceed to v1 implementation or pause to redesign.

The plan moving forward is "validate before build", explicitly:
no Phase 3 code lands until G3.1-G3.4 are green, no Phase 4 code
lands until G4.1-G4.6 are green, etc. Each gate becomes a
checkbox in `plan.md` that has to flip before its parent phase
starts.
