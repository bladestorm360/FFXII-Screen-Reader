# FFXII-Screen-Reader — Feature Plan

Status: `[ ]` pending | `[~]` in progress | `[x]` complete | `[!]` blocked

## Phase 0: Scaffolding (no game interaction)

- [x] Project directory tree
- [x] CMakeLists.txt (dinput8.dll output, MinHook static lib, no SDL3)
- [x] build_and_deploy.bat (deploys to game x64\)
- [x] .gitignore
- [x] CLAUDE.md (house rules)
- [x] README.md (with ELF compatibility caveat)
- [x] Docs templates (plan.md, debug.md, GameArchitecture.md, GhidraReference.md, FridaScripts.md)
- [x] Vendor MinHook (copied from DQ7R local under include/MinHook/)
- [x] Clone FF12 External File Loader source (REFERENCE ONLY; not used in build)
- [x] Obtain DrummerIX Cheat Engine table (FFXII_TZA.CT under FFXII-Decompile/notes/)
- [x] dllmain.cpp + dinput8_proxy.cpp + dinput8.def (standalone dinput8 proxy)
- [x] logger.cpp/h (port from DQ7R)
- [x] speech.cpp/h (Tolk wrapper, silent fallback when Tolk absent)
- [x] Tolk.dll + nvdaControllerClient64.dll deployed in game x64\ (user-managed, not vendored)
- [x] First successful build — `dinput8.dll` produced in `build\bin\Release\` (2026-05-05)
- [x] First successful deploy — `dinput8.dll` copied to game's `x64\` folder
- [ ] First successful launch (mod announces via Tolk; log file created) — pending user game-launch test

## Phase 1: Reverse Engineering Bootstrap

### Ghidra setup

- [x] Ghidra 12 install verified, GHIDRA_HEADLESS_MAXMEM=8G
- [x] Author `import_analyze_decompile.java` (single-shot pipeline)
- [x] Author `dump_strings.java` (FFXII keyword set)
- [x] Author `recover_rtti.java`
- [x] Author `dump_imports.java`
- [x] Author `seed_from_drummer_ix.java` (script ready; needs `drummer_ix_seed.csv` data)
- [x] Author `find_phyre_signatures.java`
- [x] Author `xref_locale_strings.java`
- [x] Author `find_damage_candidates.java`
- [x] Author `export_function_index.java`
- [x] Author `disable_buggy_analyzers.java` (no-op placeholder)
- [x] Author `find_ebp_interpreter.java` (locate .ebp interpreter + dispatch table — EBP2 magic xrefs, fnptr-array scan, name-keyed vs index-keyed canary probe). Pending user run.
- [x] Author `run_ghidra.bat` launcher (menu-driven)
- [x] User ran `import_analyze_decompile` end-to-end (2026-05-05)
  - 33,105 functions decompiled to `output/decompile/*.c` (90 MB)
  - Project DB: 190 MB at `projects/FFXII.rep`
- [ ] User runs targeted scripts; output archived to FFXII-Decompile/output/
- [ ] RTTI confirmation reported (present? stripped? partial?)

### Frida setup

**Priority (Phase 3-5):**
- [x] Author `probe_locale.js` — hooks `GetUserDefaultLangID` and walks caller stack
- [x] Author `probe_player_struct.js` — DrummerIX `PermStatusBitsAOB` resolves active char struct; dumps offset table for HP/MP/position field discovery
- [x] Author `probe_input_keyboard.js` — hooks Phyre keyboard vtable slots at RVA 0x1B92430
- [x] Author `probe_text_capture.js` — finds menu-label strings + watchpoints to capture readers

**Deferred (Phase 7+ combat log):**
- [x] Author `probe_pause_global.js` — find time-scale / pause flag (pending CANDIDATES from Ghidra)
- [x] Author `probe_party_hp.js` — find HP write-site (pending PartyManager RVA)
- [x] Author `probe_damage_event.js` — find canonical damage event funnel
- [ ] Author `probe_entity_list.js` — needs Phyre::PHierarchy::POctreeWorld layout

- [x] Author `run_frida.bat` launcher
- [ ] User runs priority probes; findings archived to FFXII-Decompile/notes/

### DrummerIX seed translation

- [ ] User translates DrummerIX CE table absolute addresses → RVAs
- [ ] Seed `drummer_ix_seed.csv` checked in
- [ ] Default `mod_config.ini.template` populated with seed RVAs

## Phase 2: Core plumbing

- [ ] core/logger.cpp/h (port from DQ7R)
- [ ] core/memory.cpp/h (AOB scanner + byte-validator + RVA self-heal)
- [ ] core/config.cpp/h (self-healing INI; user_settings.ini split)
- [ ] core/hooks.cpp/h (MinHook wrapper)
- [ ] core/events.cpp/h (event bus)
- [ ] core/phyre_types.cpp/h (PhyreEngine type defs as discovered)
- [ ] speech/speech.cpp/h (Tolk wrapper, silent fallback if Tolk absent)
- [ ] speech/locale.cpp/h (locale auto-detection from RAM)
- [ ] speech/phrasebook.cpp/h (12-locale dictionary, ~50 entries)
- [ ] input/keyboard_hook.cpp/h (WH_KEYBOARD_LL)
- [ ] input/hotkeys.cpp/h (modal state machine)
- [ ] F1 mute toggle works
- [ ] Self-healing config writeback proven on at least 3 RVAs

## Phase 3: Title + main menu (REQUIRED before user can start the game)

User cannot start a New Game without these — this is the first playable gate.

**Validation gates (must close before Phase 3 implementation — see `Docs/RiskAudit.md`):**
- [ ] G3.1: cursor-singleton write function found (xref to 0x1E61248)
- [ ] G3.2: PTextObject CPU staging confirmed (decompile setText)
- [ ] G3.3: ≥2 more menu state machines identified by string-cluster xref
- [ ] G3.4: at least one of (a) interpreter found, (b) fsmenu_* native impls resolvable

**Phase 3 features:**
- [ ] Title screen reading (New Game, Continue, Config menus)
- [ ] Main menu navigation
- [ ] Configuration menu navigation (controls, sound, language, etc.)
- [ ] Save/Load menu navigation

## Phase 4: Pathfinding & field navigation (intro of game is all-walking)

The opening hours of FFXII are walking around Rabanastre. Without
pathfinding the user can't reach an NPC to talk to one — so this phase
must precede interactive dialogue. Cutscene / auto-narrative text reading
can land in parallel under Phase 5 (it's passive — no walking required).

**STATUS (Session 33, 2026-07-12): turn-by-turn routing SHIPPED and working** — on the SQEX
field walkmap (NOT Bullet, which the prologue never builds; NOT POctreeWorld). Three polish
issues open — see `Docs/debug.md` Known Issues: (1) world-cardinal directions vs. camera-relative
movement, (2) 40m distance cap too small, (3) LOS smoothing cuts through walls.

**Validation gates:**
- [x] G4.1: player position read (leader handle → sceneObj `+0xB8`)
- [x] G4.3: Map name lookup (mod ships parsed planmapname.bin)
- [x] G4.6: walkability — **SQEX floor/wall walkmap** (`FUN_003208c0`/`FUN_00230b60`, mask=4);
      Bullet proven absent in the prologue
- [x] G4.2: current-area name (`FUN_003778b0`)
- [x] G4.4: entity enumeration via the scene-object handle table + actor pool
- [x] G4.7: player yaw (`comp+0x100` matrix fwd; `atan2(fx,-fz)`)
- [~] G4.5: AUTO-WALK — DROPPED (announce-only design)

**Phase 4 features:**
- [x] Player character position read
- [x] Compass / facing direction
- [x] Area name announcement
- [x] Entity list (NPCs, save points, gimmicks, combatants; **exits** wired Session 39 — pending runtime confirm of the map-jump exit-array offsets)
- [x] Hotkey cycling through entity list (`[` / `]`)
- [x] Distance + direction announcement (`\`=route legs, `/`=crow-flies describe)
- [x] Basic pathfinding (A* over the SQEX walkmap; string-pulled + cardinal-decomposed legs)
- [x] **Auto-walk to selected entity** (Session 100) — ~~DROPPED (announce-only)~~ **UN-DROPPED and
      BUILT**; the mod's one authorized write to game input (DIK W/A/S/D only, via
      `AutoWalk::OnDevicePoll`), **default OFF**, `F8` menu toggle. Play-confirmed on map 315.
      Disengages on a real movement key, combat, route loss, map change, focus loss, menu open, and
      a 15 s no-progress cap.
- [x] **Audio beacon** (Session 92) — `\` drops a panned, accelerating ping on each route leg corner;
      silent leg advance, pitched-up arrival cue, silent off-route re-plan. In combat it tracks the
      committed target instead. `F11` (bare press only — Session 112 moved it off `F9`, which the
      game owns) / `F8` menu toggle. SDL3-backed. **Play-confirmed** (tester cleared S84–123 on
      2026-08-01).
- [x] **Door / Shop categories** (Session 92) — split out of Interactables; Shop is a doorway with a
      same-named text-only sign beside it. **The Shop rule's evidence is one district — validate from
      the log before trusting it** (see `debug.md`).
- [ ] Entity spatialization ("the soundscape") — every entity emitting its type's sound from its own
      position. ~~Sounds exist for 7 of the categories; `SaveCrystal`/`GateCrystal`/`Items` still
      needed.~~ **ALL TEN categories now have a sound** (2026-08-03). Designed in `debug.md` and, in
      full, in the Session 130 plan; **not built**. Blockers, both real: `AudioEngine` is a SINGLE
      voice (retrigger, no overlap, no looping, no distance gain) and needs a software mixer; and the
      mod menu is a FLAT list whose cursor IS the `SettingId`, so the per-category toggles and volume
      sliders need submenu support first. Radius 15 steps = 11.25 world units.
- [ ] Map menu reading
- [ ] Locale detection finalized via `GetUserDefaultLangID` hook
- [ ] Phrasebook live across all 12 locales

## Phase 5: Dialogue & cutscene text auto-read

Passive cutscene text and interactive NPC dialogue likely share the same
engine text-display function — one hook covers both. The cutscene path
can be tested early (no pathfinding needed; the intro auto-plays); the
NPC-trigger path needs Phase 4 first.

**Validation gates (see `Docs/RiskAudit.md`):**
- [ ] G5.1: .msb loader function via filename xref → in-RAM message table RVA
- [ ] G5.2: `setmeswincaptionid` native impl found (string-anchor xref)
- [ ] G5.3: PTextObject CPU staging confirmed (shared with G3.2)

**Phase 5 features:**
- [x] Text display function hook (cutscenes + NPC dialogue) — `FUN_002e16b0` content setter (S19),
      now read through the widget it fills (`ui/dialogue_reader`, S91)
- [x] Multi-page advance detection — the game's own page cursor `widget+0x8A`, written by
      `FUN_002a8c50` (S91). Device-agnostic: it replaced a Space/Enter keypress watch that left
      controller players hearing only page 1
- [ ] Speaker name detection
- [ ] Subtitle / battle-quote detection
- [ ] Verify across all 12 locales (templates from phrasebook are mod-emitted; game text is read from RAM)

## Phase 6: In-game menus (priority subset)

- [ ] Items menu
- [ ] Equip menu
- [ ] Save menu (in-game)
- [x] License menu (char-select + job ring + board — Session 53)
- [ ] Magic & Tech menu

## Phase 7: Combat log (real-time battle reading)

**FFXII is not narrative** — synthesize messages from event hooks. See
`memory/project_combat_log_design.md` for the message template list.

**Validation gates (see `Docs/RiskAudit.md`):**
- [ ] G7.1: DamageMod AOB resolves to a single funnel function
- [ ] G7.2: pause global RVA (or fallback to menu-active flag)
- [ ] G7.3: HP-write monitor design (delta sign for damage vs heal)
- [ ] G7.4: KO transition (HP=0 + is_active=0xFF)

**Phase 7 features:**
- ~~[ ] Modal keyboard hook (input/keyboard_hook.cpp)~~ — **STRUCK (S48/49).** The log is NOT modal.
- ~~[ ] Pause-game integration (hook menu-pause path or time-scale global)~~ — **STRUCK (S48/49).**
  The log never pauses the game. It works while the player pauses it themselves, which is better:
  no game-state mutation, and it reads back from menus too.
- [x] 100-event continuous-FIFO ring buffer (battle/combat_log.cpp) — shipped, confirmed in play 0.1
- [x] Log navigation — **non-modal**, `,` older · `.` newer · Home oldest · End newest. Works in
  menus and while paused (only gate is `GameIsForeground()`). ~~F4 open / Escape close~~ **STRUCK.**
- [x] Damage event capture — `FUN_003112f0` (`0x1F12F0`), Tier 2. ~~DrummerIX `DamageModAOB`~~ never needed.
- ~~[ ] Combo aggregation (~750ms window for same actor+target+type)~~ — **STRUCK (S49).** It was a
  workaround for synthesized text; the game's own bus already dedupes and phrases things its way.
- [x] Heal event capture — same applier, signed delta
- [x] KO event capture — party side is the game's own message `0x10`; enemy side is ours (below)
- [ ] Status apply event capture (`FUN_0030e360` mode 3/4) — the last open Tier-2 gap
- [~] Critical / element / weakness flag annotations — **enemy elemental WEAKNESS is DONE** (Session
  147): `BtlChr+0x40`, spoken by the Libra readout on `o`, Libra-gated and suppressed for the `????`
  marks and bosses. (A first pass this session wrongly called it unobtainable; struck in `debug.md`
  and `GameArchitecture.md`.) Still open: `critical`, which probably does not exist (§5.5), and the
  per-hit element ANNOTATION on a damage line, which is a different question — no element survives to
  the apply site, so a damage line cannot say which element landed.
- [ ] Combat-log message templates added to phrasebook (12 locales) — now tiny; the game supplies
  almost all of it
- ~~[ ] Critical-event auto-speech: party-member KO~~ — the game says it (`0x10`); ours would duplicate
- [x] Critical-event auto-speech: party member <20% HP
- [x] Enemy begins casting — game message `0x0D`, flipped to realtime (S72)
- [x] Enemy defeated + EXP/LP gained — one line off `FUN_00312280` (`0x1F2280`) (S72), **not yet
  play-confirmed**
- [ ] User test: smoke flow (enter battle, take hits, open log, scroll, close)
- [ ] User test: continuous FIFO across battle boundary verified

**Reported silent (Session 72)** — details in `debug.md` § "Clan / Hunt surfaces":
- [~] Multi-item reward panel — **surface FOUND in Session 147: it is `FUN_0035e070`, the function
  the mod already hooks.** S72's "different surface" claim is struck. Why it is silent is still open;
  the descriptor logging that settles it in one hunt is shipped.
- [x] Hunt notice board — built in Session 87 (`src\ui\choice_reader.{h,cpp}`)

## Phase 8: Remaining menus

- [ ] Bestiary (deferred until v1.x — complex)
- [ ] Clan Primer

## Phase 9: Polish & v1 release

- [ ] Locale QA across all 12 locales
- [ ] README finalization (incl. Tolk-supply instructions)
- [x] License decision (job ring + confirm prompt — Session 53)
- [ ] V1 release zip

## v2 (deferred)

- [ ] Gambit editor accessibility
- [x] Shop: Buy/Sell/Bazaar item name + price + inventory on highlight, quantity selector (qty/total + 1x/10x step), and `g` gil key — Session 69
- [x] License Board grid navigation (nodes, status, LP cost, `o` detail — Session 53)
- [x] World map / fast travel — the private airship destination map speaks each marker (Session 176)
- [ ] Hunts (marks)
- [ ] Bazaar combinations

---

See `debug.md` for tried-and-failed approaches and solved problems, and
`GameArchitecture.md` for the RVA / offset / struct registry.

## Session 147 additions (2026-08-10) — from `TesterReports.txt`

- [x] Status screen: L1/R1 character switch re-reads (`FUN_002c2c50`)
- [x] Enemy instance letter in the battle targeting menu (one naming path)
- [x] Libra readout on `o` — HP numbers, Level, MP, statuses, elemental weaknesses;
      `"Libra not active."` when it is down; the weakness clause omitted for Libra-proof units
- [x] Autodetail (`F7` + `F8` row, default Off) — shop comparison and Libra volunteered on highlight
- [x] Polish glyph mapping detected automatically from the loaded font atlas; `Text glyphs` row removed
- [~] Exit reachability: instrumented (`terrain=` / `strict=`), fix deferred to the measurement
- [~] Hunt-reward panel: surface found, descriptor logging shipped, silence not yet explained
- [ ] Dungeon-device ("power conduit") navigation friction — logged in `debug.md`, needs a save there
- [ ] **Killed enemies never leave the entity list** (reported in play 2026-08-10). No HP test in the
      field scan, and the grace window cannot age out a live transform. Two defects, both written up
      in `debug.md`; add the counter before changing an admission rule
