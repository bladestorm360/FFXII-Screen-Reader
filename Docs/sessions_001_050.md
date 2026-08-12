# Session Logs (1–50)

## Session 1 — 2026-05-04 — Project bootstrap

**KEYWORDS:** scaffolding, Phase 0, project bootstrap, CMakeLists, CLAUDE.md, Module Loader, dinput8

Created the FFXII-Screen-Reader project from scratch following the approved plan
at `~/.claude/plans/deep-research-time-i-atomic-sonnet.md`.

**Done:**
- Directory tree created under `D:\Games\Dev\Custom\FFXII\` (mod root +
  decompile sibling)
- `CMakeLists.txt`, `build_and_deploy.bat`, `.gitignore`
- `CLAUDE.md` — house rules (ported + adapted from DQ7R; FFXII-specific
  rules: clean-room PhyreEngine RE, RVA-first no-fishing, no Tolk
  bundling, F4 combat log)
- `README.md` — pre-alpha status note
- `Docs/` templates: plan.md, debug.md, GameArchitecture.md, GhidraReference.md,
  FridaScripts.md, sessions_001_current.md

**Pending (still in this session):**
- Stub source files (module_entry, logger, speech)
- Ghidra scripts (import_analyze_decompile, dump_strings, recover_rtti, dump_imports,
  run_ghidra.bat)
- Day-1 Frida probes (locale, pause global, damage event, party HP, run_frida.bat)

**External dependencies pending user action:**
- Translate DrummerIX CE table addresses → `drummer_ix_seed.csv` (semicolon delimited)
  (Tolk runtime DLLs already deployed by user; no vendoring planned — runtime-only.)

## Session 2 — 2026-05-04 — External deps + architecture pivot

**KEYWORDS:** vendoring, MinHook, External File Loader, ELF, dinput8 proxy, Cheat Engine table, DrummerIX, architecture pivot

Obtained external dependencies; pivoted injection model after source review.

**Done:**
- **MinHook** copied from DQ7R's local vendor (`D:\Games\Dev\unreal\dq7-r\DQ7R-Screen-Reader\include\MinHook\`) into FFXII project — identical to GitHub upstream.
- **FF12 External File Loader source** cloned from GitLab (BSD-2) into `third_party/ff12-module-loader/`.
- **DrummerIX CE table** downloaded (`FFXII_TZA.CT`, 84 KB, 100 cheat entries) into `FFXII-Decompile/notes/`. Found AOB scans for PermStatusBits, CharJobView, Inventory, DamageMod, ChainCount, MonsterDrop, StealItems, TreasureChest, ShopItem; direct module-relative RVAs for GilPtr +0x1F8B468, ChainLevelPtr +0x219BE18, ChainCountPtr +0x219BE1C, PRNGPtr +0x2D8D030, StealPtr +0x29C6CEC.

**Architecture pivot:**
- Reading ELF source revealed it's a **VBF file redirector**, not a plugin host. There is no `modules\*.dll` discovery API; the `FF12HgetName`/`FF12HgetVer` exports are just metadata. The Module Loader plugin model the original plan assumed cannot work.
- **Decision (user-confirmed):** switch to standalone `dinput8.dll` proxy following DQ7R's xinput pattern. Single DLL ships as `dinput8.dll`, forwards all six exports to System32, runs deferred init from a background thread.
- Replaced `src/proxy/module_entry.cpp/h` with `src/proxy/dllmain.cpp` + `src/proxy/dinput8_proxy.cpp/h` + `src/proxy/dinput8.def`.
- Updated `CMakeLists.txt` (output name `dinput8`, added `.def` to sources), `build_and_deploy.bat` (deploy to `x64\` directly, no `modules\`), `CLAUDE.md` (injection rules + ELF conflict warning), `README.md` (install + ELF caveat), `Docs/plan.md` (Phase 0 checklist), the approved plan file in `~/.claude/plans/`, and memory entries.
- Logged ELF source as REFERENCE ONLY in `third_party/`.

**Pending (user actions before first build):**
- Translate DrummerIX CE-table addresses to a `drummer_ix_seed.csv`.

## Session 5 — 2026-05-05 — Phase order revised, first build, priority Frida probes

**KEYWORDS:** phase order, pathfinding before dialogue, combat log synthesized messages, first build, dinput8.dll, LNK4104 PRIVATE warnings, probe_locale, probe_player_struct, probe_input_keyboard, probe_text_capture

User clarified the phase ordering and combat-log design:

1. **Pathfinding before NPC dialogue** — can't find an NPC without pathfinding.
   Cutscene / auto-narrative text reading can run in parallel since it's
   passive. New phase order: Title menu → Pathfinding → Dialogue+cutscenes
   → In-game menus → Combat log.
2. **Combat log messages are synthesized** — FFXII has no narrative damage
   strings. We hook event funnels (DrummerIX `DamageModAOB`,
   `StatusEffectAOB`) and synthesize "Vaan attacks wolf for 12 damage"
   etc. from captured args. Implication: phrasebook grows from ~50 to
   ~30 templates × 12 locales = 360 strings.

**First build attempt landed:**
- `build_and_deploy.bat` produced `dinput8.dll` (~150 KB) in
  `build\bin\Release\`, copied into the game's `x64\` folder.
- Four LNK4104 warnings about `Dll*` exports needing `PRIVATE` qualifier;
  fixed by updating `dinput8.def`.
- Build succeeds clean. Pending: user launches the game and confirms Tolk
  announces "FFXII screen reader loaded" + log file created.

**Priority Frida probes written:**
1. `probe_locale.js` (REWRITTEN) — replaces the original string-scan
   approach with a hook on `GetUserDefaultLangID`. Walks 6 stack frames
   on first call from game code to identify the locale-enum store function.
2. `probe_player_struct.js` (NEW) — uses DrummerIX `PermStatusBitsAOB`
   pattern to find the active-char struct base, then dumps 0x300 bytes
   formatted as offset/u32/float table for X/Y/Z position + HP/MP discovery.
3. `probe_input_keyboard.js` (NEW) — hooks the first 16 vtable slots of
   `Phyre::PFramework::PInputDeviceKeyboard::vftable` at RVA 0x1B92430,
   reports which slots fire on input + their callers.
4. `probe_text_capture.js` (NEW) — scans for English menu-label strings
   ("New Game", "Continue", etc.), installs MemoryAccessMonitor on each
   hit; captures the reading function the next time the menu displays.

**Updated documentation:**
- `Docs/plan.md` Phase 0 marked complete; Phase 4-7 reordered.
- `Docs/GameArchitecture.md` updated with corrected locale strategy
  (`GetUserDefaultLangID` hook), Phyre vtable RVAs (Keyboard 0x1B92430,
  Application 0x1B92508, InputAction 0x1BB65A8), menu UI string clusters,
  and the debug-menu retail surface (`render debug menu` @ 0x7A5E40 etc.).

## Session 4 — 2026-05-05 — Ghidra import + bulk decompile complete

**KEYWORDS:** Ghidra, headless, import, analysis, decompile, FFXII_TZA.exe, ClosedException, Ctrl+C, line endings, CRLF, run_ghidra.bat, menu launcher

Multi-hour Ghidra import + auto-analysis + bulk decompile pipeline ran to
completion on `FFXII_TZA.exe`. Results saved at
`D:\Games\Dev\Custom\FFXII\FFXII-Decompile\projects\FFXII.rep`
(190 MB) with 33,105 functions decompiled to `output/decompile/*.c` (90 MB).

**Import process:**
- First import attempt was Ctrl+C-aborted after the user misread a normal
  "restarting decompiler switch analyzer later" log line as a fatal error.
  Ctrl+C closed the program DB mid-analysis, causing every analyzer in
  flight to throw `ClosedException: File is closed` (see
  `memory/project_ghidra_buggy_analyzers.md` for the corrected analysis).
- After verifying the partial `.rep` was an empty shell, the user re-ran
  the import and let it complete uninterrupted. All Ghidra default analyzers
  ran successfully — `disable_buggy_analyzers.java` left as a no-op.

**Tooling fixes during this session:**
- All `.bat` files had LF-only line endings (Write tool quirk on this
  Windows + Git Bash setup); cmd.exe was splitting `setlocal`/`REM`
  commands mid-word. Converted to CRLF + ASCII via PowerShell. Saved
  `memory/feedback_bat_files_crlf.md` to prevent recurrence.
- Rewrote `run_ghidra.bat` with a numbered menu (1-11) matching DQ7R's
  pattern for ergonomic script selection.
- Removed per-handler log redirection (`> "%ANALYSIS_LOG%" 2>&1`) — Ghidra's
  headless output is a useful progress tracker and isn't excessively
  spammy.
- Made the import handler three-way detect:
  (a) no `.rep` → `-import`,
  (b) empty `.rep` shell from a crashed run → clean up and re-import,
  (c) populated `.rep` → `-process` mode preserving prior work.
- Added `-preScript disable_buggy_analyzers.java` to import handler. Empty
  DISABLE_LIST means full Ghidra defaults; ready to populate if a real
  analyzer bug is ever observed.

**Architecture pivot also recorded this session:**
The plan to ship as an FF12 Module Loader plugin was abandoned after
reading ffgriever's source — the repo is the External File Loader (a VBF
file redirector), not a plugin host. Pivoted to standalone `dinput8.dll`
proxy following DQ7R's xinput pattern. See
`memory/project_injection_dinput8_proxy.md`.

**External deps obtained:**
- MinHook source — copied from DQ7R local vendor.
- ELF source — cloned from GitLab as REFERENCE ONLY (not used in build).
- DrummerIX CE table — downloaded as `FFXII_TZA.CT` to `notes/`. Contains
  ~30 AOB scans + direct RVAs (GilPtr +0x1F8B468, ChainLevelPtr +0x219BE18,
  PRNGPtr +0x2D8D030, StealPtr +0x29C6CEC, etc.).

**Pending before Phase 2 work begins:**
- Run supplementary Ghidra scripts (strings, rtti, imports, functions, etc.)
  to extract findings into `output/`.
- Translate DrummerIX CE-table addresses to `notes/drummer_ix_seed.csv`
  (semicolon-delimited).
- First build attempt of the dinput8 proxy (`build_and_deploy.bat`).

## Session 3 — 2026-05-04 — Tolk: runtime-only, no vendoring

**KEYWORDS:** Tolk, runtime-only, no vendoring, no build dependency, LoadLibrary

User clarified: Tolk is **never** vendored. No `Tolk.h` checked into the repo, no
build-time dependency. `speech.cpp` uses `LoadLibrary("Tolk.dll")` +
`GetProcAddress` with hand-rolled typedefs (already implemented this way).
User has already deployed `Tolk.dll` + `nvdaControllerClient64.dll` to the game
folder.

**Done:**
- Removed empty `include/Tolk/` directory.
- Updated `CLAUDE.md` (Tolk: runtime-only, never vendored — no header path,
  no build link).
- Updated `Docs/plan.md` Phase 0 checklist: removed "Vendor Tolk.h", marked
  Tolk DLLs as deployed.
- Updated MEMORY entries to reflect runtime-only stance.

**Then:**
- Run `build_and_deploy.bat` from the mod root → first compile.
- Confirm `dinput8.dll` lands in `<game>\x64\` and the game still launches with input working.
- Confirm Tolk announces "FFXII screen reader loaded".

## Session 6 — 2026-05-07 — Data feasibility survey (go/no-go before menu work)

**KEYWORDS:** feasibility, planmapname, npcdic, mapjumpgroup, text encoding,
offset cipher, dbg symbols, navigation labels, mapjump API, FFX comparison

User paused before further menu work to verify the project has enough
game-side data to ship navigation-grade speech (real area names, not opaque
IDs). Their FFX project failed for lack of this; needed a survey to confirm
FFXII does not have the same problem.

**Done:**
- Wrote `tools/survey_data.py` — decoder for FFXII text encoding plus a
  symbol dumper for every `.dbg` file.
- Cracked the text encoding empirically from the planmapname.bin dump:
  `0x20-0x39` -> A-Z, `0x3A-0x53` -> a-z, `0x1F` space, `0x00` terminator.
  Verified by decoding "Cloister of Distant Song" and "Alley of Low
  Whispers" out of the raw bytes.
- Confirmed per-locale data tables exist for all twelve locales:
  `planmapname.bin` (PLMN), `npcdic.bin` (NPC0, ~2,232 strings),
  `mapjumpgroupinit.bin` (FLAG-DEF1), `mapjumpgroupflagrom.bin`
  (FLAG-ROM1), `questbase.bin`, `navimapdata.bin`.
- Dumped 20,417 named symbols across the four controller .dbg files
  (ctrl 2,582; evctrl 2,849; mapctrl 7,748; btlctrl 7,238) into
  `notes/survey_dbg_symbols.csv`.
- Identified the script-side map-jump API by name:
  `getmapjumpposbyindex`, `getmapjumpanglebyindex`,
  `getmapdestposbyindex`, `getmapid`, `setmapjumpgroup`, `mapload`,
  `mapdispose`, `mapjumpresult`/`status`. This is exactly what a screen
  reader needs to enumerate exits and announce destinations.
- Wrote `Docs/FeasibilityReport.md` (the go/no-go document for the user).
- Updated `Docs/GameArchitecture.md` with a Navigation Data Tables
  section + a Map-jump Script API section.
- Saved memory: `project_navigation_data_decoded.md`.

**Verdict:** GO. Data quality is materially better than feared.

**Still needed (in order, before Phase 3 menu work):**
1. Ghidra: xref `EBP2` magic to find the .ebp interpreter dispatch
   function (universal hook target).
2. Ghidra: xref filename literals `planmapname.bin`, `npcdic.bin` to
   find their loaders -> in-RAM struct base.
3. Python structured parsers for PLMN and NPC0 file headers (the
   current scanner finds word fragments only; need offset-table walk).
4. Frida prototype: hook the .ebp dispatch, log script calls during a
   60-second walk in Rabanastre.
5. DrummerIX CE-table -> RVA seed CSV (already on the plan).

## Session 7 — 2026-05-07 — find_ebp_interpreter.java authored

**KEYWORDS:** ebp interpreter, dispatch table, EBP2 magic, fnptr arrays,
name-keyed vs index-keyed, ghidra script

After confirming we have ~11 game-side classes labeled by RTTI plus
~99 community RVAs but most game function names still missing, the
single highest-leverage labeling task is finding the .ebp interpreter
dispatch table - it binds ~20,417 named script symbols to native
function pointers in one structure.

**Done:**
- Authored `ghidra/find_ebp_interpreter.java`. Three phases, all
  read-only: (A) every EBP2 magic occurrence + xrefs, (B) scan .rdata
  for runs of consecutive 8-byte values that are valid .text RVAs
  (function-pointer arrays >= 100 long), (C) probe 20 distinctive
  script-name canaries (fsmenu_openmenu, btlAtelGetMyPosX, setfieldsign,
  etc.) as ASCII strings to distinguish name-keyed from index-keyed
  dispatch.
- Registered in `run_ghidra.bat` as menu option 14 (`ebp`). Added to
  the INFRA exclusion list. Bumped valid-options range message.
- Updated `Docs/plan.md` Phase 1 Ghidra checklist.

**Then (user runs):**
- `run_ghidra.bat` -> option 14. Output: `output/ebp_interpreter.txt`.

**Decision tree based on output:**
- Phase C canaries hit -> dispatch is **name-keyed**. Write a follow-up
  script that, for every .dbg symbol, finds its ASCII string in the
  binary, follows the xref to the (string, fnptr) pair, and applies the
  symbol name as a Ghidra label on the resolved function.
- Phase C canaries do NOT hit -> dispatch is **index-keyed**. The
  largest fnptr run from Phase B is the table; pair table[i] against
  .dbg symbol[i] in deterministic order (cgrtl/evctrl/mapctrl/btlctrl
  segmentation likely matches the four largest runs). Write a follow-up
  script that applies labels by index.

## Session 8 — 2026-05-11 — Title-screen text-detection: Phase A audit + Phase B/C authoring

**KEYWORDS:** title-screen text-detection PTextObject PUtilityText DynGeoFontTextInstance probe_text_draw find_text_draw_path G3.2 phase-A-audit

Worked from plan `~/.claude/plans/as-i-recall-we-re-drifting-platypus.md`. Goal: get the title-screen menu speaking — by **detecting** what the game actually renders, not assuming "New Game / Continue / Config". Phases A (audit), B (Ghidra script), C (Frida probe + fix) completed; Phases D (user runs probes) + E (analyze logs, update docs) pending.

**Phase A — decompile audit findings:**

- **`Phyre::PText::PTextObject` does NOT exist.** Zero hits in `rtti_classes.txt`. The prior claim at `Docs/GameArchitecture.md` line ~395 ("ready to hook") was wrong. Corrected today: replaced with `Phyre::PText::PUtilityText`.
- **`Phyre::PText::PUtilityText`** is a service singleton, not a per-string holder. Vftable ABS `0xC69C68` / RVA `0xB49C68`, 3 slots: dispatcher / small fn / stub. Ctor at RVA `0x5ACC10` (ABS `0x6CCC10`) — used as a one-shot sanity hook in the probe.
- **Per-string drawing goes through the `DynGeoFontTextInstance` family** (5 sibling classes, vftable RVAs `0x6C6DC8` / `0x6C6DE8` / `0x6C6E50` / `0x6C6EB8` / `0x6C6F20`). Each instance is a 152-byte wrapper around a vertex buffer.
- **`DynGeoFontTextInstance::vfunction2` at RVA `0xF3140` (644 bytes) is a DRAW function — NOT a setText.** By the time it runs, the source string has been rasterized to glyph quads (each 48 bytes). The function loads identity matrix, sets render state via ~12 indirect virtuals on `param_2` (the render context), and calls `FUN_002133d0` (RVA `0xC33D0`, vertex-batch submit) up to twice (shadow + main pass). The text content is not in vfunction2's parameters.
- **Caller chain identified:** `FUN_0017f850` (RVA `0x5F850`, "display : draw Font" per-frame entry) -> `FUN_001b5fa0` (RVA `0x95FA0`, font-scene singleton getter, **28 callers** = the candidate set of text producers) -> `FUN_001e8580` (RVA `0xC8580`, 3,572-byte vertex dispatcher, iterates 31 vertex-source slots at `font-scene-ctx + 0x70` and allocates `DynGeoFontText*` instances). Only one function in the entire binary calls `FUN_002130e0` (the DynGeoFontText ctor): `FUN_001e8580`.
- Decompile quality verified: 33,105 functions are real, non-stub. Spot-reads of `vfunction2`, the ctor, and `FUN_001e8580` show clean pseudocode with conditionals, loops, named globals, indirect calls.

**Phase B — Ghidra script `find_text_draw_path.java`:**

- Authored at `FFXII-Decompile/ghidra/find_text_draw_path.java`. Registered as option 18 in `run_ghidra.bat`.
- Strategy: walk every function calling one of 9 hardcoded anchor RVAs in the text pipeline (`FUN_001b5fa0` getter, `FUN_001e8580` dispatcher, `FUN_0017f850` per-frame entry, 5 `DynGeoFontText*` ctors, `vfunction2`). For each caller, count string-typed parameters in the Ghidra signature (`wchar_t *`, `WCHAR *`, `LPCWSTR`, `LPCSTR`, `char *`). Also list one level up (callers-of-callers).
- Output: `output/text_draw_candidates.csv` with rank + depth + RVA + size + signature + n_string_params + anchors_reached.
- Read-only (`setTemporary(true)`). Ghidra 12 API rules followed.

**Phase C — Frida probe `probe_text_draw.js`:**

- Authored at `FFXII-Decompile/frida/probe_text_draw.js`. Picked up automatically by `run_frida.bat`'s dynamic discovery.
- Five hooks: (1) `PUtilityText` ctor (sanity, one-shot); (2) `DynGeoFontTextInstance` ctor (per-object); (3) `vfunction2` (per-frame draw); (4) `FUN_001e8580` vertex dispatcher (per-frame, scans 31 vertex-source slots at `ctx + 0x70`); (5) `FUN_001b5fa0` font-scene getter (per-call, unique callers).
- For each text-instance encountered, walks first `0x98` bytes in 8-byte strides looking for any pointer that resolves to a printable string (UTF-16 LE first, ASCII fallback).
- Content dedup: each unique string logged ONCE (file + console); console hard cap 200 unique. **Dedup is for discovery, not production** — explicitly commented per CLAUDE.md rule against debouncing.

**Phase C — `probe_menu_writers.js` fix:**

- Removed forbidden `setInterval` at line 137 (CLAUDE.md Frida-gotcha — `setInterval` not supported in injected scripts).
- Reworded instructions to drop hardcoded "New Game / Continue / Config" assumption — now says "the title-menu options (whatever they are)".
- Per-call counts still flow to log via `CALL #N` lines; grep `CALL #` post-run to count fires per candidate.

**Phase D + E pending — user actions:**

1. Run `run_ghidra.bat` -> option 18 (text_path). Reports back `output/text_draw_candidates.csv`.
2. Run `run_frida.bat` -> select `probe_text_draw`. Boot to title; press up/down twice each; Ctrl+C. Reports back `notes/probe_text_draw.log`.
3. Separately: re-run `probe_menu_writers` (now without the setInterval bug). Reports back `notes/probe_menu_writers.log`.

**Critical files touched:**

- `FFXII-Decompile/ghidra/find_text_draw_path.java` (new)
- `FFXII-Decompile/ghidra/run_ghidra.bat` (option 18 added; CRLF preserved)
- `FFXII-Decompile/frida/probe_text_draw.js` (new)
- `FFXII-Decompile/frida/probe_menu_writers.js` (setInterval removed, comments updated)
- `FFXII-Screen-Reader/Docs/GameArchitecture.md` (PTextObject mis-claim corrected; new Text-Rendering Pipeline section added)
- `FFXII-Screen-Reader/Docs/sessions_001_current.md` (this entry)

**Open question for Phase E:** if `probe_text_draw` captures strings but `find_text_draw_path` CSV's top-ranked candidates have `n_string_params == 0`, the source string is held on the instance/context object (field walk wins) rather than as a direct parameter. Either result is informative — we re-rank candidates and move forward.

## Session 9 — 2026-05-11 — Menu architecture lockdown (in-game menu pipeline + cursor renderer + menu registry)

**KEYWORDS:** menu-architecture cursor-renderer FUN_00241d40 FUN_00241a50 DAT_0228ea60 menu-registry pointer-indirection-bug probe_cursor_focus-no-fire MenuArchitecture.md title-menu-not-here

Probing pivoted from text capture (Phase D) to cursor-focus detection, then to menu architecture. Three probes (probe_menu_writers, probe_menu_state_diff, probe_cursor_focus) all came up empty for title-menu cursor identification. Log analysis of probe_cursor_focus revealed a **pointer-vs-object indirection bug** in the candidate addresses: the singleton RVAs in `GameArchitecture.md` are POINTER VARIABLES in the data section, not objects. The probe was watching `static_RVA + cursor_field_offset` — but the field offset applies to the *heap object*, which lives at `*static_RVA`. Correct read requires runtime dereference.

User pivoted scope: **lock in menu architecture before further probes.** Phase 1 (static analysis only) executed.

**Findings (in-game menu architecture):**

- `DAT_0228ea60` (static, absolute) is the **menu registry table**. 8-byte pointer slots indexed by menu-type byte. Slot N holds the currently-active menu of type N, or 0. Three types observed: 1 (default), 2, 4.
- `FUN_00241a50` (RVA `0x121A50`, 20 B) — `bool isMenuOpen(int type)` query.
- `FUN_00241d40` (RVA `0x121D40`, 2,160 B) — **menu state-machine + cursor renderer**. Function-pointer-dispatched (no direct C-level callers). Receives `(menu_obj, opcode_ptr)`. Registers/unregisters in `DAT_0228ea60` on init/close.
- **Cursor sprite draw call** at line 226935 of decompile_all.txt: `FUN_00243f70(widget, X, Y, 1)` where X/Y combine per-item position (menu_obj +0x3b8, +0x9e) + bob animation (+0x3c2) + global config (`_DAT_01e0c148`, `_DAT_01e0c14c`).
- **Cursor pulse animation**: 2-state machine via menu_obj fields +0x3c0..+0x3c2 (state, frame, Y offset 0↔4).
- **UI manager singleton** (`DAT_01f811e8`) holds 14 POINTERS to static config addresses (offsets +0x3b68..+0x3bc0, +0x3fb8..+0x3fc0). `FUN_001af470` (RVA `0x8F470`) installs the pointers; `FUN_001ca600` (RVA `0xAA600`) writes config values through them at boot.
- **Title menu does NOT use this architecture.** Empirically: no candidate writer fired on title arrow presses (probe_menu_writers). Structurally: no code path sets menu_obj +0x3c8 from title-screen source, and only types 1 and 2 are queried by `FUN_00241a50`. Title menu likely uses .ebp script-side state (`fsttl_*` symbols).

**Deliverables (Phase 1 of plan `as-i-recall-we-re-drifting-platypus.md`):**

- `Docs/MenuArchitecture.md` — new file (full architecture spec for in-game menus)
- `Docs/GameArchitecture.md` (line 350 section) — corrected to clarify pointer-vs-object indirection on cursor singletons; added 2026-05-11 revision note
- `memory/project_menu_registry_and_renderer.md` — new memory
- `memory/project_postprocessobj_is_menu_class.md` — retracted (data refuted earlier hypothesis)
- `memory/project_strings_not_on_text_instance.md` — earlier finding stands
- `memory/feedback_ghidra_decompile_uses_abs_addrs.md` — earlier feedback stands
- `memory/feedback_frida_attach_mode_only.md` — earlier feedback stands

**Phase 2 (probe design) is queued.** Default approach: hook `FUN_00241d40` at entry, snapshot the menu_obj + sub-widget on each frame to identify the focus INDEX field. Title menu still needs a separate strategy — out of scope until in-game architecture is verified by Phase 2 probe.

**Phase 1 verification:** `Docs/MenuArchitecture.md` answers (1) cursor sprite renderer's RVA = `FUN_00241d40` line 226935; (2) title menu uses a separate path. Specific enough to design Phase 2 probe in one session.

**RETRACTION later in same session (2026-05-11):** Claim (2) — "title menu uses a separate path" — was unsupported. User called out the same mistake pattern from FFX: assuming title is structurally different without proving it. The architectural argument is load-bearing: Square would not reinvent the menu system for one screen. The `fsttl_*` script symbols are most likely action callbacks (what happens when an option is selected), not the menu implementation itself. Updated MenuArchitecture.md to mark title-menu status as UNCONFIRMED and queue the cheapest empirical test as the first move next session: read `DAT_0228ea60` slots while on title screen. Non-null slot → title is in this architecture. Only if all 3 slots null does the `.ebp` interpreter hunt become necessary.

## Session 10 — 2026-05-20 — Title-menu vocalization + universal menu reader (Frida probes + C++ scaffolding)

**KEYWORDS:** title-menu universal-menu-reader FUN_00241d40 FUN_002a6190 text-wrapper-cluster probe_title_controllers probe_text_wrappers probe_focus_change Hooks::Install MenuObserver TextCapture MenuReader no-hardcoding no-fabricated-labels build-clean dinput8-deployed

User pivoted to building the menu reading pipeline. Two parallel streams per plan `we-are-working-on-vivid-nest.md`:
- **Stream A** (Frida discovery, user-run): identify the title menu controller — could be `FUN_00241d40` after all (prior "no fire" result may have been RVA-bug interference); a sibling controller; or, last resort, a script-driven path. Three new probes authored.
- **Stream B** (C++ universal menu reader): scaffolded against the already-mapped in-game menu architecture. Designed so the title controller (from Stream A) drops in as one more `RegisterController` call — no restructure.

**Load-bearing design constraint locked in plan:** **NO HARDCODING** of option names anywhere. The mod never stores "option N → text". Focus-INDEX (or cursor X/Y) is used ONLY for change-detection. Text comes exclusively from the live wrapper-cluster capture each frame, spoken verbatim — the mod doesn't know whether a captured string is "New Game", "Save Slot 3", or "Light Mace +1". Dynamic menus (Load Game with variable save data, Equip with inventory-dependent gear) work out of the box. The user verifies the actual spoken strings via OCR.

**Deliverables (this session):**

Frida probes (Claude authors, user runs):
- `FFXII-Decompile/frida/probe_title_controllers.js` — G-A2. Hooks 6 sibling-controller candidates from the 2026-05-20 decompile dig: `FUN_002a6190` (RVA `0x186190`, HIGH conf, same `+0x3b8/+0x3c4` field pattern as the known controller), `FUN_00228c70`, `FUN_0023bd40`, `FUN_0023ce10`, `FUN_0023fbe0`, `FUN_002289d0`. Logs obj/type/X/Y/sub-widget/caller per fire with `*MOVED*` tagging on (X,Y) change.
- `FFXII-Decompile/frida/probe_text_wrappers.js` — G-A3. Catch-all: hooks `FUN_001b5fa0` (RVA `0x95FA0`, font-scene singleton getter — called by every text-cluster wrapper) and `FUN_0017fa10` (RVA `0x5FA10`, top text-entry candidate). Burst detection: 3+ consecutive fires from same caller within 100 ms == per-frame menu draw routine. 5 such fires during idle title display = the 5 title options being drawn; their caller RVA identifies the title controller. Also decodes args 0–3 as UTF-16 LE / ASCII strings to capture source-text directly.
- `FFXII-Decompile/frida/probe_focus_change.js` — G-A4. Once a controller is known, this probe diffs the menu_obj and sub-widget bytes between consecutive controller fires to rank fields by change frequency. The field that changes ~1:1 with arrow presses (vs. cursor-bob animation noise at `+0x3c1/+0x3c2`) is the focus-change signal.
- `run_frida.bat` requires no update — dynamically discovers `.js` files via `@auto`/`@desc` tags.

C++ scaffolding (deployed in this session as part of `dinput8.dll`):
- `src/core/hooks.h/cpp` — MinHook wrapper. `Hooks::Init/Shutdown/Install(rva, detour, original_out)/InstallTyped/Uninstall/ResolveRva`. RVA-keyed map; image base resolved via `GetModuleHandle(nullptr)`; every install logs RVA + absolute address as a sanity check for the `abs = RVA + 0x120000` convention.
- `src/ui/menu_observer.h/cpp` — Registry slot read at `image_base + 0x216EA60 + {8, 16, 32}` (corrected RVA per `MenuArchitecture.md` Layer 2). Hooks `FUN_00241d40` (RVA `0x121D40`) by default. `RegisterController(rva, label)` lets us add the title controller later without restructure (4 slot static array, template-generated detour thunks because MinHook needs distinct C function addresses). Edge-trigger focus-change callback on (X,Y) change for the same menu_obj. SEH-safe field reads (`__try/__except`) because game can asynchronously destroy menu objects.
- `src/ui/text_capture.h/cpp` — Hooks `FUN_0017fa10` (RVA `0x5FA10`) as the initial wrapper. Defensive: reads args 0–3 with `_ReturnAddress()` for caller RVA, attempts UTF-16 LE decode then ASCII fallback, captures into 256-event ring buffer keyed by (timestamp, frame_id, caller_rva, decoded_text). Mostly-printable heuristic (≥80%) prevents capturing garbage pointers. `DumpRingToLog(reason)` for diagnostic when MenuReader fails to resolve.
- `src/ui/menu_reader.h/cpp` — Subscribes to `MenuObserver::SetFocusChangeCallback`. On change: queries `TextCapture::RecentEvents` for events since the prior focus change (or 200 ms back for first focus on a new menu_obj), logs all candidates, speaks the LATEST via `Speech::Output(text, /*interrupt=*/true)`. Picker is temporal-heuristic — wrong for menus where z-order ≠ visual order, but produces observable behavior to iterate from. Refinement (caller-RVA filtering, position matching) lands after Stream A G-A3/G-A4. No option-name strings in the file. On miss: logs `focus-text resolve failed` with full ring dump; stays silent.
- `src/proxy/dllmain.cpp` — Wires the pipeline into Stage B deferred init: `Hooks::Init → MenuObserver::Init → TextCapture::Init → MenuReader::Init`. Shutdown reverses.
- `CMakeLists.txt` — added the 4 new source files.

**Build & deploy:** clean compile (one transient `std::min` macro collision fixed with `(std::min)(...)`), zero LNK warnings, dinput8.dll copied to game `x64\`. Existing "FFXII screen reader loaded" boot announcement preserved.

**Verification status:**
- Static: 4 new .cpp / 4 new .h all under the 500-line / 150-line budgets.
- Static: grep'd the new files for any string that could be a fabricated option label. Zero hits. Phrasebook is untouched.
- Runtime: deferred to user. The mod is ready for the user to (a) run the 3 new Frida probes to identify the title controller and (b) run the game with the deployed dinput8 to observe in-game menu vocalization in the log.

**Next session (Phase A in plan `we-are-working-on-vivid-nest.md`):**
1. User runs `probe_menu_registry.js` on title (the corrected one, already on disk). G-A1 closes.
2. If title is in the C++ architecture, user runs game + opens in-game menus → observe `[READER]` log entries to validate the temporal picker.
3. If title is NOT in the in-game controller, user runs `probe_title_controllers.js` (G-A2), then `probe_text_wrappers.js` (G-A3) if needed. Once title controller is found, we add `MenuObserver::RegisterController(title_rva, ...)` and rebuild.
4. After any controller has reliable focus signal (G-A4), refine `MenuReader`'s picker from temporal-latest to caller-RVA-filtered or position-matched.

**Open issues / known limitations going in:**
- `FUN_0017fa10`'s actual signature is unconfirmed. Decompile callers showed pattern `FUN_0017fa10(param_1[0x17], 0x44, 1)` which looks like a property setter, not a text submit. If `probe_text_wrappers.js` shows zero TEXT_ENTRY decodes with valid strings, we need to swap to a different wrapper RVA in `text_capture.cpp`.
- Text-wrapper hook fires from the game's render thread. The temporal picker assumes text is captured BEFORE the controller fires the focus-change for that frame. If the controller fires FIRST (the focus-change signal precedes the text draw), we'll always be one frame behind — fixable with a one-frame deferred speak.
- `MenuObserver` registers per-controller focus-change callback for the FIRST observation of any new menu_obj. That means opening a menu speaks the default-focused option (correct behavior for accessibility), but it also means re-entering a menu after switching characters speaks the option again (acceptable; user can correct via "speak less").
- `kDetours[4]` cap — if we ever need >4 controllers, the template-thunk array grows.

## Session 11 — 2026-05-20 — Input correlation rewrite (probes + menu_reader gate)

**KEYWORDS:** input-correlation Phyre-keyboard-vtable WH_KEYBOARD_LL InputTracker probe_title_controllers-v2 probe_text_wrappers-v2 probe_focus_change-v2 menu_reader-input-gate animation-noise-filter title-screen-not-idle

User flagged a critical flaw in the prior session's probes: they assumed the title screen was idle when arrow presses happen. Reality: background animation, scrolling text, particles, and preload activity produce per-frame controller fires and field churn. A single arrow press is buried in that noise, indistinguishable from animation. Same problem at runtime — `MenuObserver`'s edge-trigger on (X,Y) change would fire on every animation jitter.

Fix: **input correlation as the discriminant**, not idle-vs-busy. Hook a known input function as ground truth for "the user just pressed a key" and tag every other event (controller fires, byte changes, wrapper calls) as "within window" or "outside." Animation noise is uncorrelated with input timing and self-cancels.

**Choices (asked + answered):**
- Ground-truth input source for the Frida probes: Phyre keyboard vtable hook (RVA `0x1B92430`, 16 slots).
- C++ menu_reader: also gate speech on input correlation.

**Deliverables this session:**

Frida probes (rewritten):
- `probe_title_controllers.js` (v2) — added `installInputCorrelator()` block hooking the Phyre keyboard vtable's 16 slots. For each call, `args[1]` is checked as a possible VK code (range 1–255); a `0 → nonzero` return-value transition on a given (slot, vk) = key press event. Each candidate controller fire is tagged "IN-WINDOW" (≤200 ms after a delta) or "outside." Final ranking uses XY-change-IN-WINDOW / total-fires ratio.
- `probe_text_wrappers.js` (v2) — same input correlator. Burst detection now distinguishes BURST IN-WINDOW (input-correlated, console + log) from BURST bg (background animation, log only). Per-caller stats include `burstsInWindow`; periodic ranking emits "top callers by in-window burst ratio." TEXT_ENTRY lines also tagged.
- `probe_focus_change.js` (v2) — diffs now tag each per-byte change as in-window or not. Per-offset rank sorted by `inWindow` count, with `ratio = inWindow / total`. Animation-driven offsets (cursor-bob `+0x3c1/+0x3c2`) get baseline ratio; input-driven offsets get high ratio.

C++ scaffolding:
- `src/input/input_tracker.{h,cpp}` — minimal WH_KEYBOARD_LL hook. `InputTracker::Init/Shutdown`, `LastInputTimestampMs`, `MsSinceLastInput`, `WasRecentInput(windowMs)`. Atomic timestamp updated from low-level keyboard hook thread on `WM_KEYDOWN`/`WM_SYSKEYDOWN`.
- `src/ui/menu_reader.cpp` — added Gate 2: if `MsSinceLastInput() > 200 ms` AND this isn't the first focus for a new menu_obj, log "no recent input — staying silent (animation noise)" and don't speak. First focus for a menu_obj passes the gate unconditionally (opening a menu IS a user action). Speech is otherwise unchanged.
- `src/proxy/dllmain.cpp` — `InputTracker::Init()` before `Hooks::Init()`; reverse on shutdown.
- `CMakeLists.txt` — added `src/input/input_tracker.cpp`.

**Known limitation (documented):**
- WH_KEYBOARD_LL is keyboard only. Gamepad input does not update the timestamp, so gamepad-driven focus changes will be suppressed by the gate. Tracked in `project_menu_reader_scaffolding.md` as future work; the cleanest fix is intercepting the game's DirectInput calls (we're already the DirectInput proxy) or adopting the Phyre vtable in C++ once the probes identify the right slot.

**Build:** clean rebuild + redeploy. Boot announcement preserved.

**Why this is the right pivot:**
- Animation-driven byte changes happen at a roughly fixed rate independent of input. Input-driven changes happen ONLY around input events. The ratio between in-window and total changes IS the discriminator.
- Same logic at runtime: speech-gate by recent-input means menu_reader doesn't speak during animation but DOES speak on every actual user action.

**Next step (same as before, but probes now produce usable signal):** user runs `probe_menu_registry.js` first; if title not handled by FUN_00241d40 then `probe_title_controllers.js` v2; if no candidate fits then `probe_text_wrappers.js` v2 to find the unknown controller's per-frame draw caller.

## Session 12 — 2026-05-20 — Frida directory cleanup + input-correlator try/catch fix

**KEYWORDS:** archive probe-cleanup input-correlator-skip-bad-slot uninterceptable-vtable-slot Interceptor.attach-per-slot-try-catch frida-directory-pruning

Two issues addressed:

**(1) Two of the three v2 probes errored with `Error: unable to intercept function at 000000000079D690`.** Root cause: one of the 16 Phyre keyboard vtable slots resolves to a function (abs `0x79D690` / RVA `0x67D690`) that Frida cannot intercept — probably a scalar-deleting destructor or a stub with too-short a prologue for MinHook to patch. The input-correlator block wrapped the entire 16-slot for-loop in one try/catch, so a single slot's `Interceptor.attach` throwing aborted the whole loop AND the rest of the probe's IIFE (candidate hooks, font-scene getter hook, text-entry hook, focus-change diff hook never installed).

Fix: per-slot try/catch around `Interceptor.attach`. On failure, increment `skipped`, log offending slot + RVA + error, continue with next slot. The probe now reports `hooked 15/16 ... (1 uninterceptable; see log)` instead of dying. Applied to all 3 active probes (the input correlator was hand-duplicated).

**(2) `frida/` directory had 16 `.js` files; only 4 actively drive the title-menu / universal-reader work.** Archived 12 probes to `frida/archive/` (which `run_frida.bat` hides unless you type `a`).

**Archived (6 OBSOLETE — superseded or findings digested into code):**
- probe_menu_writers.js (superseded by probe_title_controllers.js)
- probe_menu_state_diff.js (superseded by probe_title_controllers.js; 2026-05-11 results inconclusive due to RVA bugs)
- probe_cursor_focus.js (superseded by probe_focus_change.js)
- probe_text_capture.js, probe_text_draw.js, probe_text_args.js (Phase A text discovery; findings now in src/ui/text_capture.cpp and MenuArchitecture.md)

**Archived (6 DORMANT — for deferred features):**
- probe_input_keyboard.js (input-correlator idiom folded into probe_title_controllers.js)
- probe_locale.js (locale feature; deferred)
- probe_player_struct.js, probe_pause_global.js, probe_party_hp.js, probe_damage_event.js (combat log / battle features; deferred)

**Survives in frida/ (4 ACTIVE):**
- probe_menu_registry.js (G-A1: "does title use FUN_00241d40?")
- probe_title_controllers.js (G-A2: 6 sibling-controller candidates with input correlation)
- probe_text_wrappers.js (G-A3: catch-all text wrapper with input correlation)
- probe_focus_change.js (G-A4: input-correlated byte-diff on menu_obj/sub-widget)

**Verification:** `ls frida/` shows 4 .js files + `archive/` + `run_frida.bat`. `ls frida/archive/` shows 13 .js (12 newly-moved + the previously-archived probe_cursor_state.js).

**No C++ changes** — WH_KEYBOARD_LL doesn't go through the Phyre vtable; `InputTracker` is independent of the interception bug.

## Session 13 — 2026-07-02 — TITLE MENU SPEAKING (shipped + committed) + 2 follow-ups

**KEYWORDS:** title menu, TitleReader, baked sprites, title_logo.tm2, cellTable, FUN_003939b0, FUN_00393950, FUN_00247510 0x8000, atlas row, New Game/Load Game/Trial Mode/Credits/Exit, st2e codec, section-3 help_menu, initial commit 8999c5c, press-start false-fire, initial-focus announce

**MILESTONE — title menu now speaks in-game (user-verified).** Deep offline RE (many
agents) + one confirming Frida dump, then C++ port. Committed as the repo's initial commit
`8999c5c`. Full architecture in `Docs/GameArchitecture.md` (Title Command Menu section) and
`..\FFXII-Decompile\notes\title_menu_labels.md`; memory `project_title_screen_located.md` +
`project_menu_text_st2e_codec.md` current.

**Key findings (corrected several wrong turns, all before shipping):**
- Title command menu = `DAT_02aee4c8` / handler `FUN_003939b0` (RVA 0x2739B0); NOT the in-game
  `FUN_00241d40` system, NOT the logo object `DAT_02aee4c0`.
- Option labels are **baked sprite glyph-art** in `title_logo.tm2` (no game text string). Read by
  the focused cell's atlas row: `cellTable[focusIndex].y / 70` → {0 New Game,1 Load Game,2 Trial
  Mode,3 Credits,4 Press-Start(prompt),5 Exit}. cellTable cached from `FUN_00393950`; focus index
  from `FUN_003939b0`'s 0xc/0x8000 notify packet (val@0x10). Index-agnostic (index 4→y350→Exit).
- `0xd3d` (once thought to be the options) is HELP text (`help_menu.bin` entry 389 = "Display
  On-Screen Keyboard"); per-option help IS readable pipeline text (`FUN_002f9860` id 0xd44/0xd45).
- Full text pipeline validated (`st2e` + custom codec, `tools/st2e_decode.py`); the game is a
  dynamic text-render game — title is the lone baked-sprite anomaly. In-game menus use readable text.
- Runtime image base = **0x120000** (confirmed live), so RVA = Ghidra-abs − 0x120000.
- New module `src/ui/title_reader.cpp`; wired in `dllmain.cpp`; stale registry RVA fixed in
  `menu_observer.cpp` (0x216EA60→0x1F6EA60).

**FOLLOW-UPS FOR NEXT SESSION (user feedback 2026-07-02; do NOT implemented yet):**
1. **Remove the "Press Start" announcement.** `HookedLogo` (on `FUN_00394070` msg 0x10) fires FAR
   too early, and TZA has **no press-start prompt** that requires input — it's a legacy signal we
   don't need. Action: drop the press-start announcement (and likely the whole logo hook), OR
   repurpose it strictly as a "title screen appeared" trigger for follow-up #2.
2. **Announce the INITIALLY-focused option when the title menu appears.** Currently `OnTitleFocus`
   only speaks on focus CHANGE, so the first option isn't announced until the user moves the
   cursor. Action: when the command menu opens + is ready (cursor set + `cellTable` cached — e.g.
   `FUN_003939b0` case 2 build, or the first `FUN_00393950` draw where `g_cellTable != null`),
   read the current focus once (compute index from W_LIST cursor `f4+f2`, or the initial index at
   `window+0xc0`) → `cellTable[idx].y/70` → label → speak once. Watch ordering: the build-time
   0x8000 may fire before the first draw caches `cellTable`, which is why the initial option is
   currently missed. NOTE: don't add debouncing beyond the existing `g_lastRow` state-change
   edge-trigger without asking.

**Next major area:** in-game menus (Party/Status/Config/Items/Equip) via the readable `st2e`
pipeline (`FUN_002f9860`/`FUN_002b49f0` + validated codec) — should be simpler than the title's sprites.

## Session 14 — 2026-07-03 — Universal text/menu/dialogue RE (offline-first, deep), title fix C1 built, dialogue text CRACKED

**KEYWORDS:** offline-first, confidence-gates, FUN_002b3050 text hook, FUN_00247510 universal focus,
FUN_00241d40 registry 0x1F6EA60, focus-index-formula sub-widget, title 1b window+0xC0, FUN_002b0280-is-combat-numbers,
NPC-dialogue-body, PS2Data-is-LIVE, .ebp-codec-dialogue, alc_z0100.ebp, ebp_msg_decode, staged-testing,
probe_universal_reader surgical, find_text_backends, CLAUDE.md-legacy-rule, press-start-removed, initial-focus-replay

Big offline-first RE session (per user: mine the decompile DRY, ≥0.98 before stopping / ≥0.90 before any
C++; Frida is CONFIRMATION-ONLY, never discovery of resolvers/pipelines/renderers). ~11 decompile-mining
agents over our Ghidra decompile of the **PC** exe (`decompile_all.txt`, provenance confirmed 64-bit PE).

**Text/menu model — corrected & locked (decompile-certain):**
- **No single universal text hook.** Menus/item+ability names+descriptions/panels/prompts/battle-UI text →
  **`FUN_002b3050`** (RVA 0x18B050, param_2 = codec byte*, PRE-call). `FUN_002af340` = its measure pass.
  `FUN_002b0280` turned out to be the **combat damage-number** decoder (out of scope), NOT dialogue.
- **Universal focus signal = `FUN_00247510` msg 0x8000** (index=param_3), shared by TITLE + in-game menus.
  Item index computable from sub-widget `w=*(menu_obj+0xc8)`: `(i16[w+0xf4]+i16[w+0xf2])*u8[w+0xec]+i8[w+0xee]
  +i8[w+0xed]`, count `u16[w+0xe8]`. 2-choice pop-ups (quit-confirm) emit 0x8100/0x8101/0x8102.
- **Menu registry = `DAT_0208ea60` @ RVA 0x1F6EA60** (0x216EA60 was stale). Factory `FUN_00241c90`→
  `FUN_002465f0`→handler `FUN_00241d40`. **Title confirm pop-up = native registry menu** (`FUN_00394380`)
  → the universal reader reads it for free.
- **Title 1b = decompile-certain:** starting index at **window+0xC0**; initial 0x8000 fires on open before
  the first `FUN_00393950` row-draw → replay the announce once cellTable is cached.
- **Battle:** names via `FUN_002b58b0`/`FUN_0035d330`; flying **damage numbers = per-digit sprite HUD**
  (`FUN_0028aaa0`), NOT codec text → out of the text hook (combat-log scope).
- Speaker/actor/party/map NAMES = codec bytes via `FUN_00377870` (base DAT_02add0f8). SOLVED.

**"Legacy" discipline correction (user-flagged):** I twice mis-called live content "legacy PS2" —
`PS2Data\` is **LIVE** (the exe hashes those paths, e.g. loads `battle_message.bin` by literal path), and
the `.msb`/`meswin` premises were wrong (no such strings/format in the PC exe). `CLAUDE.md` updated: Frida
is confirmation-not-discovery; **"legacy/dead" must be 100%-PROVEN** (no live loader reaches it) — a dir
name / missing string literal (files load by VBF hash) / PS2-era symbol name are NOT evidence of deadness.

**NPC DIALOGUE — traced to the LIVE path, then CRACKED:**
- Live talk window (`FUN_003cb650`→`FUN_002b7b80`→`FUN_003be2d0`→`FUN_003c1a20`) RENDERS from a pre-compiled
  glyph-record resource (category 7 `0xa3`/`0xa4`) — not codec text. So an exe hook won't yield readable text.
- **BUT the SOURCE dialogue text is plain codec strings in the live `.ebp` event/map files.** Extracted `us`
  `alc_z0100.ebp` (opening) and the validated codec decoded it PERFECTLY to real English ("Doctor Cid does
  this at Draklor?", "I cannot see my own son's heart.", …). Structure = u32 offset-table + codec strings;
  runtime msg id (`window+0x179D0`) indexes it. Reader = decode `.ebp` tables offline + key by runtime id
  (nav-reader pattern). Dialogue confidence 0.55→**~0.85 (proven decodable)**. `.ebp` msg parser in progress.

**Built this session (C1 — title fix, decompile-certain, built+deployed):** `src/ui/title_reader.cpp` —
**removed the "Press Start" logo hook entirely** (`HookedLogo`/`RVA_LOGO_HANDLER`/`g_pressStartSaid`;
`AtlasRowLabel` case 4→nullptr), and **announce the initial option on menu-open** (read `window+0xC0`, replay
`OnTitleFocus` once `HookedRow` caches the cell table; edge-trigger guards single-fire). Added the missing
`TitleReader::Shutdown()` to `dllmain.cpp` detach. Clean build + deploy.

**Authored (user-run):** `frida/probe_universal_reader.js` (REWRITTEN surgical, confirmation-only: text/
focus/menu-formula/title-1b/speaker) and `ghidra/find_text_backends.java` (ran; confirmed the split +
section-loader write @0x1D7665). Full spec: `notes/text_pipeline_menu_dialogue_spec.md` + GameArchitecture.md.

**STAGED TESTING (important — the tester is blind and pre-gameplay):** the tester can currently reach ONLY
the **title screen, new-game settings menu, and quit-game pop-up**. Reaching **in-game menus requires first
(a) dialogue + text panels reading** (to follow the intro/New-Game sequence) **and then (b) pathfinding** (to
move through the intro to where menus/NPCs are). So we **port to C++ in TESTABLE STAGES**, not all at once:
1. **Stage 1 (now):** title fix (built), + universal text capture (`FUN_002b3050`) + pop-up/settings/prompt
   readers — all verifiable on the title screen / new-game menu / quit pop-up.
2. **Stage 2:** dialogue reader (`.ebp` text keyed by `window+0x179D0`) — verifiable during the New-Game intro
   cutscene (auto-plays; no navigation needed).
3. **Stage 3:** pathfinding/navigation (existing Phase 4) — required to traverse the intro and reach in-game
   surfaces.
4. **Stage 4:** in-game menus + remaining panels — only reachable/testable after Stage 3.
Plan file: `~/.claude/plans/status-check-the-session-eventual-clover.md`.

**Pending / next:** user runs `probe_universal_reader.js` (confirms Stage-1 surfaces) + tests C1 on the title
screen; finish `.ebp` message parser (dialogue id→text + runtime id confirm); build Stage-1 text-capture +
pop-up/settings/prompt readers; then Stage-2 dialogue reader.

## Session 15 — 2026-07-04 — Menu reader round 2: pop-up Yes/No SHIPPED; settings values attempted, WRONG class

**KEYWORDS:** menu_reader round2, pop-up buttons WORKING, FUN_002f9860 idCache id=1000/1001 Yes/No, confirm
body +0x1B0, per-owner item map, initial-focus replay OnMenuPainted, FUN_002d28e0 painter CellWrapper,
settings-values-FAILED, FUN_002a6190-WRONG-CLASS, DAT_0209c4d0 spinner registry, FUN_002b2d90 blob decode,
value blob +0x3e0/sel +0x3c6/mask +0x154/count +0x3c7, scene-graph child-walk +0x18/+0x20, no-spinner-owner-
0x8000, offline-first, new-game config owner 2B551C00, FUN_002a86a0 kind +0x148 (agent-3 model, revisit)

Continued the menu-focus fix. Round 1 (prior session) made list-row **names** read by true cursor index
(via `FUN_002d28e0` per-item painter, cell cb at `subwidget+0x120`, index=arg4). This session fixed the
three remaining gaps from the round-1 live log — pop-up Yes/No silent, settings values missing, entry-time
"no item text". Resolved all mechanisms **offline** (per user: no runtime-discovery loop) via deep decompile
reads, built one round-2, tested.

**Built (`src/ui/text_capture.{h,cpp}` + `src/ui/menu_reader.cpp`, clean build+deploy):**
- **Pop-up reader** — new hook on `FUN_002f9860` (RVA 0x1D9860, POST-call) caches id→string for ids
  **1000/1001**; menu_reader speaks `StringById(1000 + (idx!=0))` (code-fixed by `FUN_00241d00`: 0=Yes,1=No,
  localized) + the body at `window+0x1B0`, when `obj[0]==FUN_00241d40` (RVA 0x121D40).
- **Per-owner item map** (`g_itemsByOwner[owner][index]`) — stops pop-up-over-menu thrash.
- **Initial-focus replay** — `TextCapture::SetMenuPaintedCallback` fires after the painter fills an owner's
  map; menu_reader stashes the menu-entry focus and replays it (mirrors title `g_pendingIndex`).
- **Settings value read (attempted)** — decode from a `FUN_002a6190` (RVA 0x186190) 0x410 "spinner" row:
  blob `+0x3e0`, selected-logical `+0x3c6`, mask `+0x154`, count `+0x3c7`, ported `FUN_002b2d90` walk. On
  focus: child-walk `owner+0x18`→sibling`+0x20` for the N-th `FUN_002a6190` child → "name: value". On
  toggle: expected `FUN_00247510(rowObject,0x8000,valueIdx)` to reach our hook with `owner`=the row.

**LIVE TEST RESULT (`FFXII-Screen-Reader-Latest.log`):**
- ✅ **Pop-up buttons WORK.** "Restore Defaults" → body *"Restore all settings to their default values?"*,
  then index0→**"Yes"**, index1→**"No"** tracking the cursor. idCache + `+0x1B0` + code-fixed id rule
  CONFIRMED live. (User tested config, not the title-Exit quit pop-up, but same `FUN_00241d40` path.)
- ✅ Names read by true index; replay resolves entry rows (e.g. idx22→"Restore Defaults", idx15→"Language").
  New names seen: Sound Effects/Voice/Restore Defaults.
- ❌ **Settings VALUES did NOT read** (focus or toggle). Diagnostic: **every** focus event stays on
  `owner=2B551C00` (the outer menu) — **no `FUN_002a6190`-owner `0x8000` ever fired**, and no `value:` line
  appeared, so value-on-focus's `FindRowByIndex`/`DecodeRowValue` returned empty for every row.

**Diagnosis / next-session hypothesis (DO NOT retest blindly — resolve offline first):** the **new-game
config menu (owner 2B551C00) rows are NOT the `FUN_002a6190` 0x410 "spinner" class** I traced. That class
(reached via `FUN_002a1b00`→`FUN_002b2d90` blob `+0x3e0`, registry `DAT_0209c4d0`) is likely a *different*
menu (in-game/pause options), so its offsets + the `0x8000`-toggle-to-row assumption don't apply here. The
competing **agent-3 model — 0x150 item widget `FUN_002a86a0`, row-kind `item+0x148` (0xb=value), value
index/count at `owner+0x3c0/+0x3c1`, value formatted at draw via `FUN_002f9860(0x4690)+FUN_002b4090`** — is
now the leading candidate for THIS menu and should be traced to ground next. Key offline task: identify the
**outer cell callback** at `*(2B551C00-subwidget + 0x120)` and follow IT to the real row class + where the
value + value-change live; confirm which `FUN_00247510` message (if any) a left/right value change emits, or
whether it routes elsewhere (no `0x8000` was observed on toggle).

**Shipped/committed this session:** the round-2 build (pop-up reader confirmed working; value read present
but inert on this menu, harmless — decodes nothing so speaks nothing, no wrong speech). Stopped here for the
day at user request. Plan file: `~/.claude/plans/status-check-the-session-eventual-clover.md`.

## Session 16 — 2026-07-07 — [menu-reader] Config-menu VALUES + `o`-key tooltips SHIPPED (offline controller ID was WRONG; the probe caught it)

**KEYWORDS:** config values WORKING, new-game/config controller = **FUN_0023fbe0 (0x11FBE0)** NOT FUN_0023ce10,
offline-0.98-was-wrong-probe-corrected-it, row array `ctrl+0xE8` stride 0x18, focus index = 0x8000 val,
value-row classes by FUN_0023ed80 type switch: enum FUN_0023e770(1/2/8)/FUN_0023d6b0(3), slider FUN_0023ebe0(6);
highlighted-option label at `child+0x18` (children `*(row+0x60)`, base `row+0xd0`, sel bit `child+8&1`);
E770 sel index `row+0xd0`, cells inline `row+0xd8+i*8`, label `*(*(*(cell+0x60)+8)+0x18)`; slider gauge
`*(*(row+0x60))+0x18` val / `+0x1c` max -> number (% if range>100); Graphics ctrl **FUN_0023bd40 (0x11BD40)**
rows `+0x4E0`, value rows FUN_0023b330(slider)/FUN_0023b6f0(enum, fmt buf `row+0xCC`); Controls ctrl
**FUN_0023ce10 (0x11CE10)** rows `+0xD8`, key-binding rows FUN_0023c5c0 (0x11C5C0) — VALUES DEFERRED;
on-change: main FUN_00240750 (0x120750), Graphics FUN_0017db90 (0x5DB90); tooltip FUN_00291d80 (0x171D80);
`o` key describe; WH_KEYBOARD_LL was DEAD (init thread exits + no msg loop) -> dedicated msg-loop thread;
1-frame settle defer; IsActiveConfig via DAT_0208e6d8 (0x1F6E6D8)/DAT_0208e6d0 (0x1F6E6D0); codec 0xa4='+';
startup announce now log-only; commit f52e4b8.

Solved the 2026-07-04 blocker (settings values). Did the RE **offline first** per house rules, then confirmed
with **one Frida probe** (`frida/probe_newgame_config.js`) — and the probe **corrected a wrong offline
conclusion**: agents had "definitively (0.98)" identified the controller as `FUN_0023ce10` because it's the sole
builder of `FUN_0023db40`; the probe's `[paint]` line showed the live controller is **`FUN_0023fbe0`** (its
value rows are `FUN_0023e770/d6b0`, not db40). Lesson: even a high-confidence offline ID of a script-opened
menu must be probe-confirmed. `FUN_0023ce10`/`FUN_0023db40` turned out to be the **Controls** sub-screen, and
`FUN_0023bd40` the **Graphics** sub-screen (both reached from `FUN_0023fbe0`'s 0x8001 cases 0x21/0x20).

**Shipped (all memory-only reads + universal-write hooks; no game-function calls, no menu-specific interception):**
- **Values on focus** for enums (On/Off, Normal/Inverted, x2/x4, English…), audio + Graphics **sliders as
  numbers**, and Graphics enums (GPU name, "1920 x 1080", "MSAA 4x"…). Dispatched by row `obj[0]` class.
- **Values on change:** main screen via `FUN_00240750` (new value from arg), Graphics via `FUN_0017db90`
  (marks the row; announced on the next settled paint, since the display buffer refreshes on draw).
- **`o`-key tooltip** — reads the focused item's help/description captured from `FUN_00291d80`, gated to the
  current focus generation so a stale description is never spoken. Key is `o` (i/j/k/l are alt arrows).
- **Fixes found along the way:** the `WH_KEYBOARD_LL` hook had never fired (installed on the short-lived init
  thread with no message loop) — moved to a dedicated message-loop thread, which also revived the input gate;
  codec `0xa4` -> `+` (New Game+); startup "loaded" announcement muted to log-only.
- **Safety (per user):** value reads are gated to the ACTIVE menu instance (`IsActiveConfig`, DAT_0208e6d8/d0)
  so a closed/freed menu's widgets are never dereferenced; a 1-frame settle defer (speak config focus on the
  next paint) fixes occasional missed reads on fast scroll.

**Deferred (named in commit):** Controls **key-binding values** — the bound-key NAME isn't stored on the row
(only key codes at `row+0xd0+col*4`; the game resolves the name transiently during draw via
`FUN_001e0b00`->`FUN_002f9860`). A memory-only read needs widget-tree tracing to the binding cell's leaf; NOT
done via game calls (user: function like other menus, no main-thread calls) nor a Controls-specific draw hook.
Graphics/Controls value-on-change beyond the `FUN_0017db90` hook.

**Docs note:** config-menu RVAs/offsets are recorded HERE (this KEYWORDS block) rather than
`Docs/GameArchitecture.md`, which currently holds uncommitted pathfinding (Phase-4) work — fold them into
GameArchitecture.md once that lands. Commit **f52e4b8** = the core config-value + `o`-key work (7 src files;
GameArchitecture.md deliberately NOT staged); the Graphics on-change hook (`FUN_0017db90`) is a follow-up commit.

---
---

# ══════════ NEW SESSION BOUNDARY ══════════
# Everything ABOVE this line is the Config-menu (Phase-6 menus) work — C++ SHIPPED, commit f52e4b8.
# Everything BELOW is a SEPARATE session: Phase-4 PATHFINDER reverse-engineering only.
# No source files were touched and NOTHING was committed below. No overlap in files or scope.
# ═══════════════════════════════════════════

## Session 20 — 2026-07-07 (PATHFINDER — distinct from the Config-menu session above) — Phase-4 field-navigation RE: gates advanced, leader anchor + walkability located (RE only, NO C++, NO commit)

<!-- Numbered retroactively (was a date-only header = the log-numbering BUG). Chronologically this
     pathfinder RE session predates the message-text Sessions 17-19 above; it takes number 20 (the
     next free integer at repair time) rather than reordering the file. Session 21 (M0 C++) is the
     newest entry, appended at the end. -->


**KEYWORDS:** PATHFINDER Phase-4 RE-ONLY (no src change, no commit); planned announce-only nav (movement:
never auto-move — user chose; G4.5 auto-walk DROPPED), turn-by-turn #1 on keypress, non-interference with
gambit system by construction (read-only, never invoke CALLACT / never touch task pool), keybinds `\`=crow-
flies+next-turn, `[`/`]`=destinations, Shift+`[`/`]`=categories; categories FFXII-confirmed only (Exit/NPC/
Treasure/Save Crystal/Gate Crystal/Enemy/Interactable-FieldSign); EVENT category CONFIRMED-NEGATIVE (no
enumerable event-position getter; only `seteventwakerect` setter, unlabelable — parked v1.x RE-5).
**CALLACT action-binding table LOCATED+dumped:** interp FUN_0025e4c0 (0x13E4C0) -> dispatcher FUN_002621d0
(0x1421D0) sel=idx>>12 slot=idx&0xFFF, enter@+0x08/exec@+0x10/poll@+0x20 stride 0x20 count@+0; reg FUN_003dc2d0
installs tables at selectors 0/3/5/7 = DAT_01eed700/01f281b0/01f29418/01f29440 (counts 1496/147/1/104);
DAT_02b57ef0 (0x2A37EF0) runtime-populated (read module tables directly). Ghidra script
`FFXII-Decompile/ghidra/dump_action_binding_tables.java` (user ran it) -> `output/action_binding_tables.txt`.
**Action NAMING (Track 1, NON-OPTIONAL, OPEN):** `tools/name_action_slots.py` classifier — handlers are VM glue
(argfetch FUN_00267e10, value-return FUN_0026b4e0; getters call VALRET; real logic in callees). Behavioral
anchoring alone TOO NOISY (position-triple false positives); no parallel name table (only FUN_003dc2d0 refs
tables). FULL naming needs precise `.dbg` action sub-table parse (fix `parse_dbg.py` heuristic) ->
ordinal<->slot alignment, behavioral anchors as validation. Fingerprints in `notes/action_slot_fingerprints.csv`.
**RE-1 player pos/facing (struct 0.97):** PPhysicsCharacterControllerBase size 0x130 (Bullet subclass 0x190);
pos = translation of 4x4 at controller+0xD0 (m_targetWorldMatrix, PWorldMatrix.m_matrix@0) at matrix
+0x30/34/38 (0.97, corroborated by lookAtWorldMatrix); yaw m_rotate@0xE0 (0.55) or matrix basis; manager
PPhysicsWorld+0x50 list (m_next@0). Reflection registrar FUN_006949a0 (0x5749A0).
**RE-4 walkability (0.95, verified):** rayTest = **FUN_0083fc70 (0x71FC70)** vtable slot 6, via world vtable
**+0x30** — CORRECTION: 0x71DE00 is debugDrawWorld NOT rayTest; convexSweepTest 0x71B780; ready-made cast
**FUN_006a1a70 (0x581A70)** (ClosestRayResultCallback vtable PTR_FUN_00d7ab10, btVector3=16B{x,y,z,pad});
world = *(context+0x60); built by FUN_006a0310 (0x580310), stepped FUN_0069f070 (0x57F070) slot 0xa0.
**Physics-world chain-up EXHAUSTED (earned Tier-C):** NO static anchor — up-trace dead-ends at FUN_00698c80
(0x578C80), zero xrefs in the 1.5M-line corpus (engine-registered cluster callback); only static physics
global DAT_02e4aff0 (0x2D2AFF0) holds shared dispatcher/broadphase, never a world.
**LEADER ANCHOR FOUND + VERIFIED (STATIC, >=0.98):** current player-controlled char scene handle =
**DAT_022c7fe0 (0x21A7FE0)** returned by universal accessor **FUN_003590d0 (0x2390D0)** (~250 callers);
resolve **FUN_003588b0 (0x2388B0)** = generation-checked handle table -> scene obj -> **+0x30** (FUN_00263e30
0x143E30) = char component (valid *comp&8); field-active gate **DAT_02089340 & 0x10**; source-of-truth party
mgr *DAT_02ebf190 (0x2D9F190), leaderIdx byte mgr+0x5aa4, ctrlIdx mgr+0x5ad5; leader-change refresh FUN_00326500
promotes handle + retargets camera => FIELD leader (not menu). Corollary MASTER KEY: leader->controller+0xD8 =
PPhysicsWorld (=RE-4 context) -> +0x60 = Bullet world, so ONE anchor closes RE-1 AND RE-4.
**CORRECTIONS:** camera globals CameraLookAt/PositionPtr (community RVA 0x20955F0/E0) STALE/ABSENT in our build
(0 occurrences verified) — community RVAs need per-build validation ([[feedback_validate_community_rvas]]).
**Gate status:** G4.3/G4.6 done; RE-0 done; RE-1/RE-4 STATIC done; G4.1/G4.7/G4.8 offsets derived (Frida-pending);
G4.5 auto-walk DROPPED. Full detail in `Docs/GameArchitecture.md` "Pathfinder / Field Navigation (Phase 4)"
section + plan file `~/.claude/plans/start-planning-the-pathfinder-proud-noodle.md`.

**What this session did (all RE / planning; no mod code):** wrote the full pathfinder plan (announce-only,
turn-by-turn via A* on a Bullet-raycast occupancy grid, DQ7R idiom port); located + verified the CALLACT
action-binding table and dumped it; built the action-naming classifier; resolved the player-position struct
chain, the Bullet raycast path, and — per the user's "exhaust static before Frida" doctrine — traced BOTH
runtime handles to conclusion: the physics world has NO static anchor (earned Tier-C), but the FIELD LEADER
DOES (DAT_022c7fe0), and it is the master key that also yields the world. Every load-bearing claim was read
firsthand in the decompile (not taken from subagents).

**═══ PICK-UP POINT for next session (do these, in order) ═══**
1. **Pin the ONE remaining static hop:** char component (`sceneObj+0x30`) -> its position field. Decompile a
   leader-position consumer — the camera-follow, or any `FUN_003590d0` caller that reads the character's world
   matrix — to find `component -> controller` (or a direct matrix on the component), chaining to the confirmed
   `controller+0xD0 -> matrix+0x30`. That makes the WHOLE leader->position chain static. (In FUN_00317e60 the
   component's sub-object at `comp+0x80` is passed to FUN_0033c9b0 — a lead to follow.)
2. **Author the confirmation-only Frida probes** (Claude authors; USER runs — never Claude): 
   - `confirm_player_pos.js` — read `DAT_022c7fe0` -> FUN_003588b0 -> +0x30 -> walk to position; verify it
     tracks the on-screen leader while walking N/S/E/W; confirm yaw (m_rotate vs matrix basis). Closes G4.1/G4.7.
   - `confirm_bullet_ray.js` — from leader->controller+0xD8 = PPhysicsWorld -> +0x60 = world (or hook
     FUN_006a0310 at map-load); call FUN_006a1a70 for a downward (floor) + wall ray. Closes G4.8.
   Frida-first / prototype-before-C++; pick Frida OR C++, never both.
3. **Track 1 (name ALL action slots — NON-OPTIONAL, still open):** do the `.dbg` action-order alignment
   (fix `parse_dbg.py` sub-table split -> ordered actions per controller; align to native slots via the
   behavioral anchors) -> `notes/action_slot_names.csv`. Essential targets: `getmapjump*byindex` (exits),
   `setfieldsign` (labels). Validate >=5 anchors + spot-check >=10 before any use.
4. **Only after each gate hits 0.98 AND Frida-confirms:** begin `src/navigation` C++ (nav_common ->
   player_state (leader-anchor backend) -> bullet_query -> entity_* -> occupancy_grid/astar/path_directions ->
   nav_commands/nav_hooks). Announce-only, on-keypress, non-interfering.

**Hygiene / boundary:** NO src files changed, NO commit this session. New artifacts (all under
`FFXII-Decompile/`): `ghidra/dump_action_binding_tables.java`, `tools/name_action_slots.py`,
`output/action_binding_tables.txt`, `notes/action_slot_fingerprints.csv`, `notes/action_slot_anchor_candidates.txt`.
`Docs/GameArchitecture.md` gained the "Pathfinder / Field Navigation (Phase 4)" section (this is the
"uncommitted pathfinding work" the config-menu session flagged above — it is now populated but still uncommitted).
The config-menu session's RVAs remain in ITS KEYWORDS block; folding those into GameArchitecture.md is a
separate follow-up owned by that work, not this one.

---
---

# ══════════ NEW SESSION BOUNDARY ══════════
# Below is a SEPARATE track: MESSAGE/DIALOGUE/PANEL TEXT reverse-engineering. RE + one Frida
# confirmation probe only. No src files changed, NOTHING committed. No overlap with the pathfinder
# or config-menu work above.
# ═══════════════════════════════════════════

## Session 17 — 2026-07-07 — [message-text] Dialogue/panel text RE (decompile-exhausted, ≥0.98) + confirmation probe authored (RE only, NO C++, NO commit)

**KEYWORDS:** DIALOGUE/PANEL TEXT RE (no src change, no commit); goal = read NPC dialogue + in-engine
cutscene captions + text panels (item/treasure/battle-system/yes-no confirm); **decompile-exhausted first,
every read-point scored 0–1, anything <0.98 DISCARDED (not probe-deferred)** per user directive; Frida =
confirmation-only. **Four surfaces, not one.**
**A. Dialogue = message window e5f0 `DAT_0209e5f0` (0x1F7E5F0, ptr global)** — CERTIFIED primary (all
openers FUN_0028e660/630/690 → builder FUN_002b9d30 (0x199D30); owns `mini_face_c` portrait); e5c0
`DAT_0209e5c0` = redundant mode-3 twin (NOT hooked); b47760 `DAT_02b47760` = chrome; NONE is a backlog
(refutes old text_pipeline_menu_dialogue_spec §5). page = `DAT_0209e5f0 + 0x1B0 + ((*(u16*)(root+0x179d2)>>2)&1)*0xBC10`;
page proc **FUN_002baf80 (0x19AF80)**; msgId `*(short*)(page+0x138)`. **Body text (0.98):** NO memory-only
string (body = glyph-node list `page+0xBBC0`); OBSERVE resolver **FUN_003c02b0 (0x2A02B0)** → `*(out+8)`=codec
body ptr, `*(out+2)&0xff`=speaker attr (no game call). **Speaker name (0.98, memory-only):** nameplate
**DAT_02b62d78 (0x2962D78, ptr global)** → `*( *( *(DAT_02b62d78+0x60) ) + 0x18 )` = rendered caption
(draw chain sealed to FUN_002dd680:40→FUN_002b0280).
**B. Item/treasure/battle-system/yes-no confirm = ONE memory-only buffer:** `DAT_0209ac30 (0x1F7AC30, ptr
global → &DAT_0209ac60)`; `surface = *(*(DAT_0209ac30)+0x328)`; `text = surface+0x1B0` (0x400-byte codec
buffer, written once by FUN_00254f30). Trigger: hook **FUN_0057c480 (0x45C480)** msg==1 (read `param_1+0x1B0`);
classify via cmd `*(*(DAT_0209ac30)+0xdf8)` (0x4b3–0x4bc loot/steal/reward). Producers FUN_002ce370 (item/
loot/battle-system) / FUN_002cdf20 (confirms) / FUN_00566680 funnel here. Field-chest "obtained X" reuses it (0.90).
**DISCARD (<0.98 / baked assets):** FMV movie subtitles = movie-embedded glyph runs, NOT codec, not RAM-readable
(0.88) — the in-game "Subtitles" toggle gates THIS (`DAT_0209be80+0x10f68` bit0xc → FUN_00550510 → overlay
DAT_02ca8f38); tutorial panels = Handbook images `menuhandbook_tutorialNNN.dat` (0.9); battle multi-line detail
builder FUN_00293310 family = unresolved .rdata dispatch (header line still captured by B); target-select window
DAT_0209be80/FUN_00552250 = battle targeting UI (out of scope). Menu help/desc bar (FUN_00291d80 →
DAT_0209be80+0x8fa0+0x10d0) ALREADY hooked by text_capture.
**RVA-hygiene:** old notes had RVA typos (spec §4 called FUN_002b3050 "0x18B050"; correct 0x193050) — every RVA
here recomputed as abs−0x120000 from the FUN name; DAT_0209ac30 confirmed a POINTER global (`002800f0.c:9`
`DAT_0209ac30=&DAT_0209ac60`), not a struct base. **The old FUN_002b0280-is-just-combat-numbers claim is wrong:
it is the shared codec rasterizer; but per the no-debounce rule we do NOT hook it (per-frame) — we hook the
once-per-line semantic points above.**

**What this session did (all RE + one probe; no mod code, no commit):** ran six parallel decompile-only agents
(singleton disambiguation, dialogue text+speaker chain, panel+subtitle paths, then a second round: e5f0 coverage
map, memory-only read point + speaker-caption seal, e5c0/FMV/config, then battle+item read point, system+tutorial
locate, speaker seal). Verified every load-bearing line firsthand (`002b9d30.c`, `0057c480.c`, `002800f0.c`,
`002baf80.c`). Authored the confirmation-only probe **`frida/probe_message_text.js`** (hooks FUN_003c02b0 +
FUN_002baf80 + FUN_0057c480; decodes with the game_text codec; console-budget split; page-geometry cross-check;
emits `\xNN` for unmapped codec bytes to watch the 0x6C–0x83 gap). Full spec archived in
`FFXII-Decompile/notes/message_text_readpoints_spec.md`; RVAs folded into `Docs/GameArchitecture.md`
("Message / Dialogue / Panel Text").

**═══ PICK-UP POINT for next session ═══**
1. **USER runs `probe_message_text.js`** (run_frida.bat auto-lists it) — talk to an NPC, pick up an item/open a
   chest, trigger a battle system line + a Yes/No confirm. CONFIRM: `[dialog]` shows `"Speaker": "body"`; `[geom]`
   match=true; `[panel]` decodes item/battle/confirm text; note any `\xNN` unmapped codec bytes. Report the log.
2. **Only after the probe confirms + explicit go-ahead:** port Phase C → new `src/ui/message_reader.{cpp,h}`
   (hooks 1–3; edge-trigger on msgId / surface-birth — NOT a debounce; auto-speak interrupt + `r`=re-read-last;
   reuse GameText::Decode/Hooks/Speech; wire into DeferredInitImpl after MenuReader::Init). Plan file:
   `~/.claude/plans/before-we-pick-back-sparkling-quokka.md`.

**Hygiene / boundary:** NO src files changed, NO commit this session. New artifacts (all under
`FFXII-Decompile/`): `frida/probe_message_text.js`, `notes/message_text_readpoints_spec.md`. `Docs/GameArchitecture.md`
gained the "Message / Dialogue / Panel Text" section (still uncommitted, alongside the pre-existing uncommitted
pathfinder section).

---

## Session 18 — 2026-07-07 — [message-text] Dialogue + message-panel reader SHIPPED in C++ (straight-to-C++ exception; builds + deploys)

**KEYWORDS:** MESSAGE-TEXT C++ SHIPPED (built + deployed, NOT yet play-tested); **user-authorized exception to
Frida-first** (dialogue + pathfinding mutually block isolated testing → validate together in an integrated
new-game playthrough; probe_message_text.js kept as optional aid only). New module **`src/ui/message_reader.{cpp,h}`**
+ shared **`src/core/mem_read.h`** (SEH read helpers extracted from menu_reader, now used by both — menu_reader's
`FUN_00241d40` confirm path untouched). Three hooks (all via Hooks::InstallTyped, edge-triggered = state tracking
NOT debounce): **FUN_003c02b0 (0x2A02B0)** observe → cache body ptr `out+8` + attr `out+2` by msgId; **FUN_002baf80
(0x19AF80)** page proc → on `page+0x138` msgId edge (per-page map) speak cached body prefixed with speaker;
**FUN_0057c480 (0x45C480)** panel → case1 read `surface+0x1B0`. **Speaker** = nameplate **DAT_02b62d78 (0x2962D78,
ptr global)** `*(*(*(np+0x60))+0x18)`, gated on visibility `(*(u32*)(np+0x40) & 0x405)==5` so a stale name is never
spoken (best-effort; tune the flag if play-test shows missing/wrong speakers). **Panel classify gate (directive 2/3):**
`surface[0x636]`=choice count, `surface[0x630]&0x8`=passive flag → count==0 && passive = INFO (item/treasure/battle-
system) AUTO-SPOKEN; count!=0 = yes/no confirm (FUN_002cdf20) / multi-choice (FUN_00566680) = **preserved read-point,
classified + logged, but MUTED** (`kSpeakSurfaceConfirms=false`). This surface's confirms are a DIFFERENT class than the
already-working title/new-game confirms (`FUN_00241d40`, distinct obj[0], never alias) — so muting = no regression, no
double-speak, and the read-point is saved per user request in case in-game pop-ups use this path. **`r` = re-read last
line** (new InputTracker WM_REREAD + SetRereadCallback, mirrors the `o` path; edge-detected 'R'). Auto-speak
interrupt=true; last line stashed under a mutex (game thread writes, input thread reads). All reads memory-only +
SEH-guarded; only the dialogue body uses observe (no game calls). Wired into DeferredInitImpl after MenuReader::Init;
added to CMakeLists. **Build + deploy CLEAN** (only the 4 touched files recompiled). No commit (user hasn't asked).

**OUT OF SCOPE (proven unreadable, documented):** FMV movie subtitles (movie-embedded glyph runs; the "Subtitles"
toggle gates these — in-engine captions ARE covered via e5f0); tutorial panel bodies (Handbook IMAGES
`menuhandbook_tutorialNNN.dat` — bodies byte-identical across 9 langs = fixed-canvas textures; no text bank exists;
OCR-only). Readable-later (not this push): Handbook category titles / Clan-Primer labels via st2e.

**═══ PICK-UP / TEST POINT ═══**
1. **Integrated new-game play-test** (dialogue + pathfinding together): NPC dialogue + in-engine cutscene captions
   speak with speaker as they advance; `r` repeats last line; item pickup / opened chest + a battle system line speak
   (INFO); title/new-game yes/no still behaves as before (no double-speak). Check
   `x64\FFXII-Screen-Reader-Latest.log` MSGTEXT lines: per-line reads; muted-confirm surfaces logged-with-class but
   silent; watch for `\x`-style unmapped codec bytes (→ extend game_text 0x6C–0x83) and any wrong/stale speaker
   (→ tune the +0x40 visibility gate).
2. After play-test confirms: mark `Docs/plan.md` Phase-5 dialogue items complete; commit; consider enabling the
   muted confirm path if an in-game pop-up on the FUN_0057c480 surface is found uncovered.

**Hygiene:** New/changed src: `src/ui/message_reader.{cpp,h}` (new), `src/core/mem_read.h` (new),
`src/ui/menu_reader.cpp` (helpers → mem_read.h), `src/input/input_tracker.{cpp,h}` (`r` key), `src/proxy/dllmain.cpp`
(init wiring), `CMakeLists.txt`. RE artifacts (from Session 17, under FFXII-Decompile): `frida/probe_message_text.js`,
`notes/message_text_readpoints_spec.md`. No commit this session.

---

## Session 19 — 2026-07-07 — [message-text] Play-test: tutorial TELOP surface found + hooked + CONFIRMED reading; r→t; diagnostics (COMMITTED)

**KEYWORDS:** play-test of the Session-18 C++ reader. **Findings from the mod log** (all 3 message hooks installed
cleanly, menus read fine, ZERO message reads at the first interactive screen): the opening is FMV (movie-subtitle
path, not RAM-readable, expected silent) and the first on-screen text is a guided-TUTORIAL banner ("TUTORIAL / Try
using [↑←↓→] to adjust the viewing angle") that is a THIRD surface my dialogue/panel hooks don't cover.
**Fixes this session:**
1. **Re-read key `r` → `t`** — `r` collides with a game function; our repeat-last-text key moved to `t`
   (`input_tracker.cpp` vkCode 'R'→'T', `g_rDown`→`g_tDown`). (User: this was a key-conflict fix, NOT movement.)
2. **File-only diagnostics** in `message_reader.cpp`: first-fire markers per hook (`diag: resolver/… fired`) + a
   per-line `diag: dlg page line msgId=… havePending=…` trace, so the log shows which surfaces fire vs. capture misses.
3. **TUTORIAL/TELOP surface located + hooked (the guided-tutorial banner):** it's the game's on-screen "telop"
   overlay, **`FUN_002e16b0(ctx, slot, textPtr, _)` (RVA 0x1C16B0)** — `param_3` = full `HEADER\x02BODY` codec
   string (0x02→newline), once per set/clear. Content chain: `FUN_002e16b0`→`FUN_002a35b0`(0x1835b0)→
   `FUN_002a3250`(0x183250, splits caption/body); header ptr `DAT_0209c988` (0x1F7C988); fed by script display
   natives `FUN_00348df0`/`FUN_0034ada0`/`FUN_0050eab0`. **Confirmed DISTINCT** from e5f0 dialogue, `FUN_0057c480`
   panel, help-bar `FUN_00291d80`, and the Handbook image viewer. Host chain ~0.97; "prologue banner routes here"
   was ~0.85 offline → **the live play-test CONFIRMED it: the tutorial banner now vocalizes ("works perfectly").**
   Hooked in `message_reader` (RVA_TELOP), decode+speak, first-fire diag.
   **Known follow-up:** button-icon inserts (0x0f escapes = the ↑←↓→ glyphs) decode to nothing, so key/button
   names are dropped for now ("Try using to adjust…") — map the 0x0f button selectors → names next.
**Still pending fuller test** (needs movement, which is the PATHFINDER track's job, deferred by user): NPC dialogue
body + item/battle panels — hooks installed + diagnostic-instrumented; will confirm when gameplay is reachable.

**Sequencing (user):** text panels vocalizing first (tutorial ✓; dialogue/panels instrumented) → THEN pathfinding
+ player movement.

**Commit:** src only — `src/core/mem_read.h`, `src/ui/message_reader.{cpp,h}` (new), `src/ui/menu_reader.cpp`,
`src/input/input_tracker.{cpp,h}`, `src/proxy/dllmain.cpp`, `CMakeLists.txt`. **`Docs/GameArchitecture.md` +
`Docs/sessions_001_current.md` deliberately NOT staged** — they also hold the pathfinder track's uncommitted RE
sections (per the no-cross-track-commit rule); they'll be committed with/after the pathfinder work. RE artifacts
(FFXII-Decompile) are a separate archive, not this repo.


---
---

# ══════════ NEW SESSION BOUNDARY ══════════
# PATHFINDER track resumes (continues Session 20). This is the first C++ for field navigation.
# Straight-to-C++ exception, user-approved. src/navigation created; NOT committed this session.
# ═══════════════════════════════════════════

## Session 21 — 2026-07-07 — [PATHFINDER] Phase-4 M0: straight-to-C++ leader/physics chain self-diagnostic SHIPPED (builds + deploys, uncommitted); RVA correction; announce-only nav scaffolding

**KEYWORDS:** PATHFINDER M0 STRAIGHT-TO-C++ (user override of Frida-first, session-18-style — chosen via
AskUserQuestion; risk acknowledged); `src/navigation/` created = `navigation.{h,cpp}` (module entry + M0 dump),
`player_state.{h,cpp}` (leader chain), `bullet_query.{h,cpp}` (rayTest wrapper), `nav_hooks.{h,cpp}` (ctx
capture), `nav_rva.h` (all confirmed RVAs/offsets), `nav_types.h` (FVec3). Announce-only, event-driven,
SEH-guarded, NO gambit/action hooks. **M0 = read-only logged self-diagnostic on the `\` key** — bakes the
Frida-confirm the user skipped INTO the mod (mod log is a sanctioned read) so unconfirmed offsets + the
runtime-only world ptr are pinned in-game before any behavior builds on them.
**RVA CORRECTION (caught during impl):** field-active gate `DAT_02089340` → RVA **`0x1F69340`** (abs−0x120000);
the pathfinder plan table listed `0x2089340` un-converted. Handle-table base `DAT_02098e10` → RVA `0x1F78E10`.
**Leader resolve replicated INLINE (memory-only)** from `FUN_003588b0`+`FUN_00263ff0` (no game-fn call): handle
is a **uint** (sel=`>>0x10&0xf`<5, slot=`&0xFFFF`, gen=`>>0x14&0x7FF`); table=base+sel*0x288; entries@+0x08,
active@+0x10&1, cap@+0x20; obj=`*(entries+0x08+slot*8)`; gen check `*(u16)(obj+0x16)`; component=`*(sceneObj+0x30)`
valid `*comp&8`. Game consumer = `FUN_00317e60` — ⚠️ it does `*comp|=0x100000000` (a WRITE) which we do NOT copy.
**ctx capture:** hook `FUN_006a0310` caches RCX (physics ctx) → `BulletQuery`; `FUN_006a1a70(ctx,from,to,out8,0xF)`
reads world at `ctx+0x60`, guarded before every call. Infra: `mem_read.h` +`SafeReadF32`/`SafeReadU64`;
`input_tracker` +`NavKeyCallback`(vk,shift) + edge-detected `\ [ ] ` ` (VK_OEM_5/4/6/3) + Shift via GetAsyncKeyState
(observe/passthrough, no swallow yet); `dllmain` wires `Navigation::Init/Shutdown`; `CMakeLists` +4 sources.

**What this session did:** wrote + built + deployed M0 (clean compile, all 4 nav sources link into `dinput8.dll`;
deployed via build_and_deploy.bat). Read firsthand the load-bearing decompiles: `FUN_00317e60` (leader→component
chain + field-active gate + the write-to-avoid), `FUN_003588b0`/`FUN_00263ff0` (handle table), `FUN_00263e30`
(sceneObj+0x30), `FUN_006a1a70` (ray wrapper ABI + null-world garbage-return), `FUN_006a0310` (world builder /
ctx), `FUN_0033c9b0` (comp+0x80 is a transform CONSTRUCTOR, not a getter — so component→controller is genuinely
unpinned, which is exactly what M0 discovers). `PlayerState::ReadPlayerPos/ReadPlayerYaw` intentionally return
false until M0's log pins the offset. GameArchitecture.md gained an "M0 self-diagnostic" subsection + the RVA
correction.

**═══ PICK-UP POINT (do these, in order) ═══**
1. **USER runs the in-game M0 test:** load a save on a field map (e.g. Rabanastre); press **`\`** (backslash).
   Speech confirms state ("Nav dump logged, world ready" / "No field" / "No leader"). Then walk a few steps
   N, press `\`; walk E, press `\`; etc. (5–6 dumps while moving).
2. **CLAUDE reads the log** `FFXII-Screen-Reader-Latest.log` (tag `NAV-DIAG`) and pins:
   (a) handle/sceneObj/component resolve non-null + `valid=1`; (b) the CONTROLLER — a `CAND ... +D8 ==CTX` line
   (definitive) and/or a `COORD` line whose `m(30/34/38)` floats track the on-screen movement between dumps;
   (c) yaw — `+E0yaw` vs the matrix basis (dump shows both). Diff dumps to confirm which floats are position.
3. **Record** the pinned `component→controller` offset + position/yaw offsets in `GameArchitecture.md`; implement
   `PlayerState::ReadPlayerPos/ReadPlayerYaw`; flip **G4.1/G4.7/G4.8** to confirmed. → then **M1 (compass/facing)**:
   port `nav_common` math, wire the facing key + `\`'s crow-flies half.

**Hygiene / boundary:** NOTHING committed this session. New/changed (uncommitted): `src/navigation/*` (7 files),
`src/core/mem_read.h`, `src/input/input_tracker.{h,cpp}`, `src/proxy/dllmain.cpp`, `CMakeLists.txt`,
`Docs/GameArchitecture.md`, `Docs/sessions_001_current.md`. No cross-track files touched. The message-text track's
prior uncommitted doc sections remain untouched.

---
---

## Session 22 — 2026-07-07 — [PATHFINDER] Full announce-only field-nav SHIPPED in C++ (master-data labels, meters-calibrated); deployed, one confirmation pass pending

**KEYWORDS:** PATHFINDER Phase-4 FULL C++ (decompile-exhausted first per user, THEN port, runtime=confirm only).
`src/navigation/` complete: `nav_common` (cardinal 8-pt compass on X/Z + egocentric-optional + steps/elevation),
`player_state` (pos `sceneObj+0xB8`, yaw matrix-fwd `comp+0x100`), `entity_list`/`entity_scan` (walks actor pool
`DAT_0208e688` stride 0xF50 count `DAT_0208e6a0`; per-actor pos +0xE0/E4/E8, yaw +0x160, def=*(+0x698),
kind=*(def+5), id=*(u16)(def+4)), `nav_commands`, `nav_hooks` (ctx capture on `FUN_006a0310`), `bullet_query`
(`FUN_006a1a70` + `HorizontalClear`), `path_directions`, `nav_rva.h`, `nav_types.h`. Hotkeys: `\`=describe
(name+cardinal bearing+distance +obstacle hint), Shift+`\`=facing, `[`/`]`=cycle nearest-first, Shift+`[`/`]`=category,
`` ` ``=rescan+area, Shift+`` ` ``=diagnostic.
**MASTER DATA (offline, all maps, no per-map dumps — DQ7R model, per user):** object NAME = the game's own
localized text via `ctx=FUN_0035d380(1, defId)` (RVA 0x23D380) -> name codec* at `ctx+0x08` (NPC) / `ctx+0x10`
(gimmick) -> `GameText::Decode`. Classifier = npcdic def id (def 469=Save Crystal, 466=Gate Crystal, 435-465=31
area gate crystals, 434=Treasure). Area name = `FUN_003778b0()` (RVA 0x2578B0, current-area, no arg) -> decode.
`FUN_002f9860` (0x1D9860) is the bank resolver used internally. Empty sentinel `DAT_01ceb638` (RVA 0x1BCB638).
`mapjumpgroup*` are FLAG tables NOT exits (corrected); walking-exit dest names need per-map EBP (deferred).
**WORLD SCALE (offline, definitive): METERS.** Bullet default gravity -10 (`FUN_00854980`), char controller defaults
(`FUN_00690fc0` RVA 0x570FC0): m_height 0.8 + m_radius 0.6 => ~2 m humanoid, jump 1.5, self-grav -19.6, collision
margin 0.04. => units-per-step 0.75, grid cell ~0.5. (Caught + fixed a 30.0 placeholder = 40x off.)
**LAYER 3 = SAFE obstacle guidance (not full A*):** `\` casts <=5 rays toward the selection -> "Path clear" /
"Blocked, bear <cardinal>". Full A* occupancy grid deferred (thousands of rays would race the physics step;
needs game-thread execution) -- `path_directions` (leg aggregation) already built for it.

**What this session did:** exhausted the decompile FIRST (6 deep RE passes: position/yaw static chain, actor-pool
enumeration, Track-1 API RVAs via `slot=mapctrl.dbg_idx-5140`, master-data classification+names, and world-unit
scale), THEN ported the full announce-only nav to C++. RVA correction carried from M0: field-active `DAT_02089340`
= RVA 0x1F69340. Handle resolve replicated inline memory-only (`FUN_003588b0`+`FUN_00263ff0`). Reused
`GameText::Decode` + the `FUN_002f9860` resolver `text_capture.cpp` already calls. Builds clean; deployed via
build_and_deploy.bat (game closed). Localization: game text auto-localizes (live resolver); the mod's own words
(compass/"steps"/"Facing") are English literals for now (English tester) -- FFXII TZA does ship ~11 text locales
(per-locale npcdic sizes differ); the new-game EN/JP toggle is VOICE, not text.

**═══ PICK-UP / one confirmation pass (USER runs; fresh tutorial restart) ═══**
1. Start a NEW game (fresh map load fires the `FUN_006a0310` ctx hook so obstacle rays have a world).
2. Nav keys: `` ` `` -> "<area>. N objects"; `[`/`]` cycle -> hear names + bearing + distance; `\` -> re-describe +
   obstacle hint; Shift+`\` -> facing. `Shift+`` ` `` -> diagnostic dump to log.
3. CLAUDE reads `FFXII-Screen-Reader-Latest.log` (tag NAV-DIAG): confirm (a) `def 469 "Save Crystal"` resolves live
   (verifies the def+4->objid hop), (b) the compass matches N/E/S/W facing + yaw sign vs `FUN_004686d0`, (c)
   raw coords make 0.75 m/step feel right (tune if not). Then flip G4.1/G4.7/G4.8/G4.4.
4. Follow-ups: full A* turn-by-turn (game-thread grid), walking-exit destination names (per-map EBP), egocentric
   direction mode, mod-word phrasebook if non-English support wanted.

**Hygiene:** uncommitted. New: `src/navigation/*` (16 files) + edits to `src/core/mem_read.h`,
`src/input/input_tracker.{h,cpp}`, `src/proxy/dllmain.cpp`, `CMakeLists.txt`, `Docs/GameArchitecture.md`,
`Docs/sessions_001_current.md`, plan `~/.claude/plans/find-our-last-pathfinding-composed-pinwheel.md`. No
cross-track files. RE artifacts stay in the FFXII-Decompile archive.

---
---

## Session 23 — 2026-07-07 — [PATHFINDER] Input hijack SOLVED (DirectInput GetDeviceState) + hotkeys working; pathfinder partially up; 2 known bugs logged; COMMITTED

**KEYWORDS:** PATHFINDER input-capture fix. FFXII acquires the keyboard via **DirectInput (exclusive)**,
which installs a swallowing low-level hook that starves OS-level keyboard hooks (WH_KEYBOARD_LL) AND NVDA's
own commands — mod hotkeys were 100% dead (probe caught only Tab/Space leaking). **SOLUTION (works):** ride the
game's OWN input path — the dinput8 proxy patches `IDirectInput8::CreateDevice` (vtable idx 3) to identify the
keyboard device and patch `IDirectInputDevice8::GetDeviceState` (vtable idx 9); each frame we read the same
256-byte DIK scan-code buffer the game just polled and feed `InputTracker::FeedDInputKeyboard` (DIK 0x18=o,
0x14=t, 0x1A=[, 0x1B=], 0x2B=\, 0x29=`, 0x0C=-, 0x0D==, 0x27=;, 0x28=', 0x2A/0x36=shift). Exclusivity now
irrelevant; game behavior unchanged. Reverted an earlier attempt to FORCE the keyboard non-exclusive (didn't
work + would alter game behavior). LL hook kept for the recent-input timestamp only (gated off once DInput feed
active, via `g_dinputActive`).
**KEY SCHEME (all STANDALONE — no Shift, because game binds Left Shift = Toggle Walk/Run):** `\`=describe,
`[`=prev object, `]`=next object, `-`=prev category, `=`=next category, `` ` ``=rescan+area, `;`=facing,
`'`=diagnostic dump. All confirmed working in-game. (`[`'s earlier "not working" was transient stale-state from
the LL-hook-only builds; the DIK log confirms the game reports 0x1A fine.)
**FULL game keybindings captured -> `Docs/Controls.md`** (WASD move, arrows camera, IJKL/numpad cursor, Space/
Enter confirm, C cancel, F battle menu, R party menu, 1/2/3 speed/lockon/target, Left Ctrl escape, Esc pause,
M map, F1-3 game speed, Left Shift walk/run). NONE conflict with the mod's keys.
**Pathfinder status: PARTIALLY WORKING.** Position read, actor-pool enumeration, crow-flies bearing+distance,
and the obstacle hint all function (`rescan: 2 field objects`; spoken "N steps <dir>"). Two bugs remain.

**═══ KNOWN BUGS (fix next session) ═══**
1. **Compass N/S axis FLIPPED.** Walking NORTH toward an object is spoken as SOUTH (distance decreases
   correctly as you approach — so `hypotf` is right — but the cardinal LABEL is inverted on N/S; E/W apparently
   OK). Root: the Z-axis sign in the bearing convention. `nav_common::BearingDeg` uses `atan2(dx, dz)` with
   +Z=North; FFXII's north is the opposite Z sense. **Fix: negate dz (and `fwd.z` for facing) in `BearingDeg`
   + `CardinalOfHeading`** (`nav_common.cpp`); re-verify E/W against the minimap. Affects crow-flies, `;`
   facing, egocentric.
2. **Object NAMES all resolve to "Object" (resolver failing) -> so classification also fails.**
   `ResolveObjectName` (`FUN_0035d380(1, defId)` -> ctx+0x08 (NPC) / ctx+0x10 (gimmick) -> `GameText::Decode`)
   returns empty, so `entity_list` falls back to the `CategoryWord` "Object", and `ClassifyByName` (keyword on
   the resolved name) can't categorize. Likely the **MEDIUM-confidence hop**: `*(u16)(actor.def+0x4)` may not be
   the objid `FUN_0035d380` expects (category-nibbled objid vs raw npcdic index — flagged by the name-recipe RE
   agent), or the ctx+0x08/0x10 field / type param is off. **Fix: on a field map WITH the 2 objects present,
   press `'` (entity diag) to log their def ids + kinds, then match to npcdic / re-trace `FUN_0035d380` to
   correct the objid mapping.** (The `'` dump captured this session was pressed too early — 0 objects, pos
   unavailable — so no def ids were recorded.)

**Files this session:** `src/proxy/dinput8_proxy.cpp` (CreateDevice + GetDeviceState vtable hooks),
`src/input/input_tracker.{h,cpp}` (`FeedDInputKeyboard`, DIK dispatch, standalone keys, diag OFF),
`src/navigation/nav_commands.{h,cpp}` + `navigation.cpp` (new key map), `Docs/Controls.md` (new). Plus the full
Session-20/21/22 pathfinder src (all navigation modules) — first pathfinder COMMIT.
**Pickup:** (1) `'` diag on a populated field -> def ids; (2) fix name resolver (objid mapping); (3) fix compass
N/S (negate Z); (4) then full A* turn-by-turn (game-thread grid).

## Session 24 — 2026-07-08 — [PATHFINDER] Compass + name resolver FIXED; Layer-3 game-thread A* turn-by-turn SHIPPED

**KEYWORDS:** PATHFINDER Phase-4. Fixed both Session-23 bugs + added Layer-3 routing. Built+deployed; NOT committed (one runtime confirmation pass pending). Straight-to-C++ (standing pathfinder exception).

**1. COMPASS N/S FLIP — FIXED.** FFXII world north = **-Z**. `nav_common::BearingDeg` -> `atan2(dx, -dz)`; `player_state::ReadPlayerYaw` -> `atan2(fx, -fz)`; entity_list obstacle-probe made consistent (`base=atan2(dx,-dz)`, `p.z - cos(a)*probe`). E/W (dx) untouched — confirm live, negate dx only if E/W also inverted.

**2. NAME RESOLVER — FIXED via master data (the user's directive: "data loads from files, extract the maps").** The old `FUN_0035d380(1, def+4)` was the PARTY/ROSTER resolver fed a MODEL index (`(kind<<8)|instance`) -> range-check fail -> memset -> empty -> "Object" every time. CORRECT path = the game's own `FUN_00263990(sceneObj)` (RVA 0x143990), `sceneObj=*(actor+0x10)`: name key at **`sceneObj+0x102`** (s16), `>=0` -> global **npcdic** (`DAT_02b5e0d8` RVA 0x2A3E0D8, loaded boot cat 9/0x1f; lookup `FUN_003eac10` 0x2CAC10: `id&0xffffbfff`, `slot=id*2`, codec* = `*(s32)(base+0xc+slot*4)`); `<0` -> per-map string at `*(sceneObj+0xf8)`. Replicated MEMORY-ONLY (no game call), decode via `GameText::Decode`. Removed the FUN_0035d380 block from nav_rva.h. **OFFLINE-VALIDATED** new `tools/parse_npcdic.py` (real codec, not the stale survey_data.py 0x1F-space one): npcdic **469=Save Crystal, 466=Gate Crystal, 434=Treasure, 468=Urn** (the plan's ids were right all along); `tools/parse_planmapname.py`: 1328=Rabanastre. Runtime reads the game's OWN loaded npcdic, per-locale — nothing shipped (SE IP). Diagnostic `'` now logs `nameIdx`/`nameStr`.

**3. LAYER 3 = game-thread A* turn-by-turn (user chose option B: "be very careful hooking the game thread; DQ7R crashes on partially loaded/destroyed maps").** New `src/navigation/path_planner.{h,cpp}`: lazy-sampled walkability A* (cell 1m, ray budget 2000, range 40m via bullet_query), runs ON the game thread. Key **`/`** (VK_OEM_2/DIK 0x35) = turn-by-turn legs OR **"No path"/"Too far to route"/"Route unavailable"** — NO crow-flies fallback (that's `\`). **CRASH-SAFETY (ported from DQ7R sessions 361-378 post-mortems):** (a) drain hooked at **`FUN_00314020`** (RVA 0x1F4020, per-frame field step, mode 0, at ENTRY before its teardown driver FUN_0025bfb0); (b) world-invalidate + **monotonic map-epoch bump** hooked at **`FUN_002695a0`** (RVA 0x1495A0, field teardown START); (c) hard gate `PlayerState::IsFieldNavSafe()` = 0x10-live + area collision `DAT_02b5e0c0`(0x2A3E0C0) + area id `DAT_02b5e0b8`(0x2A3E0B8)!=-1 + live `*(ctx+0x60)` + leader `DAT_0209a1f0`(0x1F7A1F0) — because the 0x10 bit is stale-valid on teardown / premature on load. Epoch drops any request captured before a transition; hook return types matched EXACTLY (bool/void — a wrong return clobbers RAX). Running on the game thread means our reads and the game's teardown are the same thread (serialized, can't interleave) — the key advantage over DQ7R's async scanner.

**Files:** `src/navigation/{nav_common.cpp, player_state.{h,cpp}, nav_rva.h, entity_list.{h,cpp}, nav_hooks.cpp, nav_commands.cpp, navigation.cpp}` + new `path_planner.{h,cpp}`; `src/input/input_tracker.cpp` (DIK_SLASH); `CMakeLists.txt`; `Docs/{Controls.md, GameArchitecture.md}`; `..\FFXII-Decompile\tools\{parse_npcdic.py, parse_planmapname.py}` (+ `notes/npcdic_names.csv`, `notes/planmapname_areas.csv`). Built + deployed clean.

**PICKUP — one runtime confirmation pass (fresh field map), then COMMIT:** (1) `'` diag -> log shows `nameIdx = 469` for the save crystal (verifies +0x102 is on the scene object) + npcdic name resolves live; (2) face N/E/S/W -> spoken cardinal matches minimap (compass); if E/W also inverted, negate dx; (3) `[`/`]` speak real names; (4) `/` gives turn-by-turn legs to the save crystal / honest "No path"; (5) tune units-per-step (0.75) if steps feel off. Follow-ups: walking-exit dest names (per-map EBP), egocentric mode, locale-independent classification via npcdic id-ranges.

## Session 25 — 2026-07-09 — [PATHFINDER] Post-test fixes: NPC classification, gate enumeration via the HANDLE TABLE, turn-by-turn drain point

**KEYWORDS:** PATHFINDER Phase-4. Runtime test (Reks/Nalbina prologue) exposed 3 bugs; all fixed by RE of the game's OWN interaction scanner. Built + deployed + PLAY-TESTED: enumeration/classification CONFIRMED working; turn-by-turn still silent. NOT committed. 6 follow-ups handed to a duplicate instance. Straight-to-C++.

**Runtime test reported 3 defects** (log confirmed all nav hooks installed; only ONE rescan `2 objects` = the 2 NPCs fired, before the gate step): (1) NPCs classified "Object" not "Person"; (2) the iron gate + doors/switches NEVER appear in the scanner — a tutorial BLOCKER; (3) `/` turn-by-turn silent.

**Bug 3 (routing silent) — FIXED.** Drain moved off `FUN_00314020` (0x1F4020) onto **`FUN_0022a770` (RVA 0x10A770)** — the no-arg per-field-frame tick (walking-state `DAT_02064ad3==2`, returns u64). FUN_00314020's `mode==0` path IS reached while walking but sits behind a fixed-timestep accumulator AND an else-branch bypass (`FUN_001800e0()!=0`→FUN_002f1770 directly) a scripted tutorial holds open → the route request was never drained. Hook now no-arg/u64, drains unconditionally at entry; teardown hook + IsFieldNavSafe gate unchanged.

**Bug 1 (NPCs→Object) — FIXED.** `def+0x05` is **0=static / 1=animated**, NOT an NPC flag (field NPCs are kind 1) — the old `(kind==0)?NPC:Object` was inverted. Replaced by npcdic id-band + interaction-flag classification. NPCs now → Person.

**Bug 2 (gate absent) — root cause + fix; TWO tester corrections killed my first two theories** ("activates on approach" was wrong — the gate is present from LOAD; only the Action Icon is proximity-driven). The 32-slot BtlWork actor pool `DAT_0208e688` is CHARACTERS ONLY → never held the gate (why the old scan found just 2 NPCs). RE of the game's own per-frame interaction scanner `FUN_0025b820` (RVA 0x13B820) → **enumerate the scene-object HANDLE TABLE `DAT_02098e10` (RVA 0x1F78E10), 5 containers ×0x288** (active `+0x10&1`; entries `*(base+c*0x288+0x08)`; count `*entries`; obj i `*(entries+0x08+i*8)`), which holds EVERY field object (NPCs + gimmicks) from load (allocator `FUN_002679f0`/loader `FUN_0026ce60`). Positions via **`node=*(sceneObj+0xB8); XYZ=node+0/4/8`** — CRITICAL: dropped `FUN_00265020`'s class-nibble guard `(*(u8)(sceneObj+3)>>5)∈{1,3}`, which zeroes gates whose class isn't 1/3 (would've been a 3rd miss — the 2nd dig caught it). Interactivity filter = `sceneObj+0x1C` flags: `0x400`=talk(NPC), `0x4`=action(gate/door/switch) + any npcdic-band name. `entity_list` fully rewritten to walk the handle table (identity = scene object ptr); rescans FRESH on every cycle/describe; `LogDiagnostic` now dumps all named/interactive objects per container (cat byte / flags / nameIdx / name / pos).

**Files:** `src/navigation/{nav_rva.h, nav_hooks.cpp, entity_list.{h,cpp}, player_state.{h,cpp}}`. Built + deployed clean. RE from two decompile digs of the game's interaction scanner (`FUN_0025b820`→`FUN_0025bad0`/`FUN_0025be50`, winner in `DAT_0209a2b4/b8`) + the scene-object registry (`FUN_002679f0`/`FUN_00266ad0`).

**DEPLOYED + PLAY-TEST OUTCOME (2026-07-09):** The above build (`dinput8.dll`) was deployed to the game `x64\` and play-tested in the Reks/Nalbina prologue. This is the ACTUAL deployed state (uncommitted):
- **WORKS:** handle-table enumeration — the tester found the iron GATE by cycling the objects category (log shows `rescan 1→2→3 field objects` as the gate step is reached; the gate is the 3rd object). NPC classification works (NPCs land in the Person category). Compass, names, and the gate all confirmed.
- **STILL BROKEN / SURFACED:** (a) switching INTO a category vocalizes a STALE count ("Objects, 0") — `ChangeCategoryLocked` counts `g_entities` WITHOUT a fresh `RescanLocked()`; only cycle/describe rescan. (b) `/` turn-by-turn is STILL silent. The field-frame hook IS installed (log: `field-frame hook installed`, RVA 0x10A770), so the failure is downstream (hook-fire / `IsFieldNavSafe` / `PlanRoute`→NoPath) — but `path_planner.cpp` has ZERO logging, so it's currently undiagnosable.
- **NEW REQUESTS (this session, not yet built):** rename spoken categories Object→"Interactables" and Person→"NPC"; add an ENEMIES category; read enemy target-info (name/level/HP, Libra-gated); and describe route legs by CARDINAL decomposition — a 15-north/3-east leg must read "North 15, East 3", never "Northeast".
- **HANDOFF:** these 6 follow-ups (category rescan, renames, enemies, enemy-info, direction rule, route diagnostics) were handed to a SECOND, accidentally-started Claude instance (same context) to avoid double work. THIS instance wrote NO further code — only read-only RE. Enemy discriminator (confirmed, for the enemies category): a field enemy is a class-3 scene object with a CharUnit at `*(sceneObj+0xC0)`, battle id ≠ -1, an active battle stat block (`DAT_022c6f28 + id*0x740`, `+0x284` ≠ 0), and NOT party (`sceneObj+0x12` == 0xFF); the enemy name comes from the battle/kernel unit record, not the field npcdic key.
- **DEFERRED (unchanged):** find-a-gate-from-across-the-map needs pre-instantiated positions (only cached in the transform node once loaded); button-glyph 0x0f mapping; egocentric mode.

## Session 26 — 2026-07-09 — [menu-reader + PATHFINDER] Session-25 follow-ups + in-game menus SHIPPED straight-to-C++ (built+deployed, uncommitted, PENDING in-game validation)

**KEYWORDS:** This instance = the "second instance" Session 25 handed the 6 follow-ups to. Built + deployed 5 workstreams straight-to-C++ (user granted an ALL-workstreams no-probe exception this round: no save point → each Frida pass would cost a full tutorial run; revert to probe-first once a save exists). NOTHING committed; NOTHING yet validated in-game. Plan file `~/.claude/plans/we-have-a-supposed-humble-waterfall.md`.

**Root cause of the "universal menu reader" silence past title/config (evidence-backed):** `FUN_00247510` msg `0x8000` is a shared TRANSPORT, not a shared contract. Title/config send `owner=*(subwidget+0xC8)` + `val`=small index + rows painted by `FUN_002d28e0`; the in-game menus violate BOTH assumptions, so `FocusedItemText` misses → silent. They are THREE separate families.

**A — IN-GAME MENU READING (menu-track; new `src/ui/ingame_menu_reader.{h,cpp}`; `menu_reader.cpp` delegates unhandled 0x8000 owners; `CMakeLists.txt`).** Two families, one universal reader each (covers ALL their submenus — decompile-confirmed single window class per family):
- **FIELD menu = window class `FUN_002a6190` (RVA 0x186190)** (NOT `FUN_002a1fb0`, which is only the cursor sprite). All field submenus (Items/Equip/Magicks/Technicks/Gambits/License/Status/Party/Maps/Config/Save) are ONE class (factory `FUN_002a2c00` 0x182C00). On 0x8000 `owner`=the window; the game's handler resolves the focus into row-buffer DATA index at **`window+0x124`** during `s_origDispatch`, so we speak the row AFTER calling the original. Row text = a `0x03`-separated codec buffer at **`window+0xF8`** (= container[+0xD0]+0x28); replicated `FUN_002b2930` (row 0 = buffer start; row N = bytes after the N-th `0x03`; row ends at next `0x03`/`0x00`); decode w/ GameText.
- **BATTLE command menu = window class `FUN_0055cd40` (RVA 0x43CD40)** (singleton `*(DAT_0209ac30+0x320)`). All battle submenus share it. On 0x8000 `val` is a POINTER to the focused **0x38-byte cell** — name codec* at **cell+0x00**, validity s16 at **cell+0x08** (`-1`=empty); read immediately (cells built on move). Cell also: id `+0x08`, kind `+0x0B`, MP `+0x0C`, type `+0x30`. `val = *(win+0x120) + (col + colCount*row)*0x38`.
- Recognize each by `obj[0]` identity; mod-side `Log::Write("INGAME",...)` logs owner obj[0] + focused label so any silent submenu (a family the reader didn't cover) shows in the log.

**E1 — category-switch STALE COUNT — FIXED.** `ChangeCategoryLocked` (`entity_list.cpp`) now calls `RescanLocked()` before counting, like the cycle commands. Fixes "Objects, 0" on switch-in.

**E2 — category RENAMES.** `CategoryWord`: Person→**"NPC"**, Object→**"Interactables"** (dual-use: also the fallback label for unnamed objects).

**B1 — ENEMIES IN SCANNER (new `Category::Enemy`).** `ScanEnemiesLocked` walks the BtlWork combatant pool `DAT_0208e688` (32×0xf50; count `DAT_0208e6a0`): per slot def=`actor+0x698` (null=empty), active `actor+0` & 0x10, foe = **`def+5`==0** (0.85 — VERIFY, flip `ENEMY_DEF_KIND` if party shows as enemy), name codec* = **`actor+0x18`**, position via `sceneObj=actor+0x10`→node+0xB8 (reuse ReadSceneObjectPos), identity = scene obj (dedupes vs handle table). `'` diagnostic now ALSO dumps the pool (each slot's def+5/name/pos) to verify polarity. **NOTE divergence:** Session 25 proposed a handle-table discriminator (class-3 sceneObj + CharUnit@+0xC0 + `sceneObj+0x12`!=0xFF party); I used the pool per a fresh cross-verified dig — the `'` dump will settle which is right in-game.

**D1 — ROUTE INSTRUMENTATION (unblocks D2).** `path_planner.cpp` + `nav_commands.cpp` now `Log::Write("NAV-ROUTE",...)` the whole path: `/` pressed → target acquired / "No target"; `request seq/epoch`; per-drain decision (stale-epoch drop, not-nav-safe [once/req], gave-up, `PlanRoute` outcome + legs). Planner had ZERO logging before, which is why the turn-by-turn failure was undiagnosable from the log.

**Files:** new `src/ui/ingame_menu_reader.{h,cpp}`; edited `src/ui/menu_reader.cpp`, `CMakeLists.txt`, `src/navigation/{entity_list.{h,cpp}, nav_rva.h, path_planner.cpp, nav_commands.cpp}`. All built + deployed clean (2 builds, exit 0).

**PICKUP — in-game validation pass (then iterate), in priority order:**
1. **Menus (A):** open the field menu + each submenu, and the battle menu + submenus — do focused rows speak? `[INGAME]` log lists owner obj[0]+label; any silent submenu = an uncovered family to add.
2. **Enemies + polarity (B1):** walk near a foe, cycle to the **Enemy** category — names + bearing? Press `'` and check the `pool[i] ... def+5=N(enemy?/ally?)` dump: party must be "ally?", monsters "enemy?"; if inverted, flip `ENEMY_DEF_KIND`.
3. **Route (D1→D2):** press `/` on a selected target; paste the `NAV-ROUTE` lines → localizes the turn-by-turn failure for D2.
4. **Categories (E):** switch categories → live counts (no stale "0"); hears "NPC"/"Interactables".

**PENDING (not yet built):** B2 target-HP hotkey (read `*(DAT_0209be80+0x10fa4)` unitId → BattleUnit HP%; cur HP `+0x48`/max HP unlocated → pin via mod-side logging first); D2 route fix (needs the D1 log); C secondary-direction leg decomposition (deferred until D works — crow-flies bearing stays as-is). GameArchitecture.md canonicalization of the new menu/enemy RVAs held until in-game validation (Session-15 lesson: probe/test-confirm a script-opened controller ID before canonicalizing).

## Session 27 — 2026-07-09 — [PATHFINDER + menu-reader] F: scanner-0 ROOT CAUSE + combatant listing (enemy=bit24); G: Controls key-binding values (both built+deployed, uncommitted, PENDING validation)

**KEYWORDS:** F game-blocking scanner-0 fixed via BtlWork-pool combatant listing (def+5 disproven, enemy=def-attr bit24); G Controls key-binding names via FUN_001e0b00 arithmetic + existing FUN_002f9860 id-cache; A held for confirmation probe. Plan `~/.claude/plans/we-have-a-supposed-humble-waterfall.md`. Methodology reverted to confirmation-first (autosave now exists).

**F — SCANNER RETURNED ZERO (GAME-BLOCKING) — ROOT CAUSE + FIX.** The `'` NAV-DIAG dump (17:34 log) settled it deterministically: the handle table was FULL (container 0 `active=1 count=37 shown=23`, container 1 `count=27 shown=13`) — NOT empty, NOT streaming, NOT a walk regression. The loaded save is the **Reks-prologue BATTLE**: every object is a class-3 combatant character (`Reks`[leader], `Basch`, 3× `Dalmascan Soldier`, `Imperial Swordsman`, `Air Cutter Remora`) with flags `0x34881`/`0x30800`, `nameIdx=-1` — none carry the talk(`0x400`)/action(`0x4`) flag or an npcdic-band name, so `RescanLocked`'s interactive filter dropped ALL of them → 0. It "worked" in Session 25 because that was a field area with a real gate + talk-NPCs; this save has neither.
- **`def+5` DISPROVEN as faction.** Same dump: `def+5=0` for **Reks (the player)** and `def+5=1` for EVERYONE else incl. the real enemies. So `def+5` is **player-vs-AI**, NOT enemy-vs-ally (0.99 offline conclusion was wrong for the guest/prologue setup). Renamed `ENEMY_DEF_KIND`→`PLAYER_DEF_KIND` (0 = player-controlled leader, skipped).
- **FIX:** reworked `ScanEnemiesLocked`→**`ScanCombatantsLocked`** (`entity_list.cpp`): list ALL live BtlWork-pool combatants (skip empty `def=actor+0x698`==0, inactive `actor+0` & 0x10==0, and the player `def+5`==0), classify **Enemy via def-attribute bit 24 `((*(u32)(def+0x3c)|*(u32)(def+0x64)) & 0x1000000)`** else ally→NPC; name `actor+0x18`, position `sceneObj=actor+0x10`→node+0xB8 (skip `(0,0,0)`), identity = scene obj (dedupes vs the handle table). This lists Basch+soldiers (NPC) and Swordsman+Remora (Enemy) → navigation unblocked in battle. Folds in B1 done correctly.
- **bit-24 confirmation:** `LogDiagnostic`'s pool line now prints `def+5 attr=(lo|hi) bit24=N(enemy/ally)` so the `'` dump confirms the classifier (expect Swordsman/Remora bit24=1, party bit24=0). Even if a label is off, combatants list **by name** so the user can already navigate to "Air Cutter Remora".
- nav_rva.h: `DEF_ATTR_LO=0x3c`, `DEF_ATTR_HI=0x64`, `ENEMY_ATTR_BIT=0x1000000`, `PLAYER_DEF_KIND=0` (replaces `ENEMY_DEF_KIND`).

**G — CONTROLS KEY-BINDING VALUES (keyboard only).** The Controls sub-screen (off new-game/config) read row NAMES but not the bound-key VALUE. Binding rows are class **`FUN_0023c5c0` (0x11C5C0)** — they cache only DIK CODES (col 0 = primary keyboard at `row+0xd0`, `row+0xec`!=0 = mid-rebind), never the name; the game resolves the name transiently at draw via `FUN_001e0b00(code)`→id→`FUN_002f9860`. `FUN_001e0b00` is pure arithmetic (replicated `KeyCodeToStringId`): layout base `0x46e1` US / `0x4749` FR(0x40c) / `0x47b1` DE(0x407) via `GetKeyboardLayout(0)&0xfff`; `id = code>=0x1c ? code-0x1c+base : code>=1 ? code+0x46dd : 0x46dc`. Value read (`ControlsBindingValue` in `menu_reader.cpp`): `row+0xec`!=0 → speak id `0x46dd` ("press a key"); else `code=*(u32)(row+0xd0)` (0=unbound) → `StringById(KeyCodeToStringId(code))`. NO game call — `text_capture.cpp` `HookResolve` now caches the key-name id block `[0x46dc..0x4882]` (was only 1000/1001), which the game's own per-frame draw of the visible bindings populates; cache miss = value silently omitted (self-corrects next frame). Added `RVA_VALROW_C5C0` to `IsConfigValueRow`; `SafeReadU32` to the using-list. Gated by the existing `IsActiveConfig(RVA_INST_CE10)` so no closed-menu deref.

**A — HELD for the confirmation probe (methodology reverted to confirmation-first).** The Session-26 straight-to-C++ field/battle classes (`FUN_002a6190` field, `FUN_0055cd40` battle) were WRONG — the live log proved the R-menu owner obj[0] is neither, and no in-game menu vocalized. Re-RE'd deterministically to a 5-class row-chain map (`FUN_00280de0`/`002c2320`/`00565e00`/`0056f810`/`0057b890`, ROW_OFF 0xD8/0xC8/0xC8/0xE0/0xD0; battle cmd = `FUN_002c2320` row chain; `0x43CD40` = License Board grid, separate cell-pointer path). Confirmation probe `FFXII-Decompile/frida/probe_ingame_menu.js` (authored, auto-discovered by run_frida.bat) asserts the map without sweeping. **Rewrite `IngameMenuReader` only after the probe confirms** — do not repeat the wrong-class silent ship.

**Files:** `src/navigation/{entity_list.cpp, nav_rva.h}` (F); `src/ui/{menu_reader.cpp, text_capture.cpp}` (G). Two builds, exit 0, deployed. Nothing committed.

**PICKUP — validation pass (priority order):**
1. **F (game-blocking):** fully quit+relaunch the game (DLL loads at launch only), load the autosave, walk into the field; `[`/`]` cycle should list the combatants by name and pathfinding should work. Press `'` and paste the `pool[i] ... bit24=N(enemy/ally)` lines → confirms the enemy classifier.
2. **G:** open the Controls sub-screen, arrow through the binding rows → each should speak "name: key" (e.g. "Confirm: Space"); mid-rebind says the game's "press a key".
3. **A:** run `probe_ingame_menu.js` (run_frida.bat), open the R party menu + submenus (and a battle command menu) → paste the `[focus]` lines confirming the owner obj[0] RVA→ROW_OFF map; then A's C++ rewrite lands.
4. **D2:** press `/` on a target, paste the `NAV-ROUTE` lines → localizes the route failure.

**PENDING:** A C++ (post-probe); D2 (needs route log); C secondary directions (after D); B2 target-HP (future session — needs actively-targeted-enemy tracking). F follow-ups: the 3 class-1 `cat=21 flags=0x38` objects + the never-populated `Category::Exit` (map-jump API) for a "where to go next" target between battles. GameArchitecture.md canonicalization of the new offsets held until the `'` dump validates bit24.

## Session 28 — 2026-07-09 — [PATHFINDER + menu-reader] Post-test #2: field/battle menu NAMES + targeting reader + enemy classifier (scene-kind nibble), all built+deployed, PENDING validation

**KEYWORDS:** Confirmed from the user's probe + mod log: F listing WORKS (`rescan: 4 field objects`, no more 0). Fixed the 3 issues the user reported via 3 parallel decompile-RE agents, then one build. Menu names row+0x10 (not +0x18=desc), targeting hook FUN_003bfe10, enemy faction = scene-kind nibble. Plan `~/.claude/plans/we-have-a-supposed-humble-waterfall.md`.

**Ground truth pulled directly (probe output `FFXII-Decompile/notes/probe_ingame_menu_output.log` + mod log):**
- Probe: field menu owner `FUN_00280de0` ROW_OFF `0xd8` CORRECT, but `*(row+0x18)` decodes to the DESCRIPTION ("View and sort equipment and key items." etc.) — the `case 0xc/0x8000` handler literally passes `*(row+0x18)` to `FUN_00291d80` (the description-bar setter). A 2nd class `FUN_0027ad70` (0x15ad70) also fired but is a focus-forwarding wrapper, not a name holder.
- Mod log: all field-menu `[READER]` focuses = "text not ready — awaiting paint" (silent); the deployed wrong-class IngameMenuReader never matched.

**Issue 2/3 — IN-GAME MENU NAMES (A) — REWROTE `ingame_menu_reader.{h,cpp}` as a row-chain family reader.** RE (agent, 0.93): the row record (0x20 bytes) is built by the SHARED builder `FUN_002cd3c0(cmdId, descId)` → **row+0x00=command id, row+0x10=NAME codec\*, row+0x18=DESCRIPTION codec\***. Same layout for ALL row-chain classes (field command column + battle command + magicks/gambits/etc.), so ONE reader: `name = *( *(owner+ROW_OFF) + index*0x20 + 0x10 )`. Classes→ROW_OFF: `FUN_00280de0`/0xD8, `FUN_002c2320`/0xC8 (battle cmd, confirmed name@+0x10), `FUN_00565e00`/0xC8, `FUN_0056f810`/0xE0, `FUN_0057b890`/0xD0. `menu_reader` dispatch now delegates any row-chain owner to `OnRowChainFocus` (reads immediately — rows pre-built; no window+0x124 deferral). Descriptions already reach the `o` key via the existing `FUN_00291d80` capture (text_capture HookedDesc). Dropped the old FUN_002a6190/FUN_0055cd40 guesses entirely.

**Issue 3 — TARGETING MENU — NEW hook `FUN_003bfe10` (RVA 0x29FE10).** RE (agent, HIGH): the battle target reticle (`FUN_005528c0`, window `DAT_02ca8f38`+0x160) calls `FUN_003bfe10(nameId, out)` ONLY when it lands on a new target; fills `out` (5×u32), returns 1. **`out+0x08` = the target's NAME codec\*** (the exact label drawn over the reticle; no Libra needed for the name). `IngameMenuReader::HookedTargetName` speaks it, deduped on the hovered nameId (arg1). Installed via new `IngameMenuReader::Init/Shutdown`, called from `MenuReader::Init/Shutdown`. (Memory-only poll alt exists — `win=*0x2CA8F38; child=*(win+0x160); node=*(child+0x9f40); id=*(u8)(node+0x39)` — but the id→codec step needs `FUN_002f9920`, a game call, so the hook is preferred.)

**Issue 1 — ENEMY vs ALLY — REPLACED bit-24 with the scene-kind nibble.** RE (agent, 0.85, TRIPLE-confirmed in the damage path `FUN_0030ab40`, HUD builder `FUN_00329220`, target classifier `FUN_002f8e90`): the game's own faction test is `kind = *(u8)(sceneObj+0x0e) & 0x0f` (accessor `FUN_00263c20`): **kind==3 ally, kind∈{1,2,7} enemy, kind==5 dead/removed**. def+5 CANNOT do this (prologue: every non-leader is def+5==1). `ScanCombatantsLocked` now: skip player (def+5==0), skip kind==5, `Enemy` iff kind!=3 else `NPC`. `'` pool dump now prints `def+5=N kind=N charid=N -> ally/enemy/dead` for confirmation. nav_rva.h: removed DEF_ATTR_*/ENEMY_ATTR_BIT; added `SCENEOBJ_KIND_OFF=0x0e`, `KIND_MASK=0x0f`, `KIND_ALLY=3`, `KIND_DEAD=5`, `DEF_CHARID=0x04`.

**Files:** rewrote `src/ui/ingame_menu_reader.{h,cpp}`; edited `src/ui/menu_reader.cpp`, `src/navigation/{entity_list.cpp, nav_rva.h}`. Built (1 fix: `kind` var collision → `def5`), deployed. Nothing committed.

**PICKUP — validation:**
1. **Enemy classifier:** load autosave, cycle categories — enemies should be under **Enemy**, party under NPC. Press `'`, paste the `pool[i] ... kind=N ... -> ally/enemy` lines to lock kind values (agent flagged one caveat: FUN_00269fe0/002659a0 also write this nibble, so confirm live).
2. **Field/battle menu:** open R menu + submenus and a battle command menu — focused item NAMES should speak; `o` key = description.
3. **Targeting:** in battle, select attack target — the hovered enemy's name should speak (`[INGAME] target:` in log).
4. **D2 route:** press `/`, paste `NAV-ROUTE` lines.

**PENDING:** D2 (needs route log); C secondary directions (after D); B2 target-HP (future). F follow-ups: 3 class-1 `cat=21` objects + `Category::Exit` map-jump wiring. License Board (FUN_0055cd40) cell-name read still deferred. GameArchitecture.md canonicalization of row+0x10 / scene-kind nibble / FUN_003bfe10 held until in-game validation.

## Session 29 — 2026-07-10 — [menu-reader] Bundle: active-pane isolation + item/cmd descriptions + battle command names + menu-style targeting (built+deployed, PENDING validation)

**KEYWORDS:** All five menu fixes RE'd to >0.96 (memory-only / hook-and-read, no direct game calls) then bundled into ONE build. Root correction: item entries were ALREADY read (menu_reader painter `[READER] item:` — Potion/Cure/Mythril Sword/Buckler), the bug was NO pane isolation → items/magicks/equipment jumbled. Plan `~/.claude/plans/we-have-a-supposed-humble-waterfall.md`. Strict user bar: >0.98 or discard (0.90 battle-command-reimpl DISCARDED for a 0.97 read).

**1. ACTIVE-PANE ISOLATION (issue 1) — `menu_reader.cpp`.** The pause/inventory screen is multi-pane; several windows fire `FUN_00247510` 0x8000 per keypress with no focus filter. FIX = the global input-focus pointer **`DAT_0208ebc0`** (RVA 0x1F6EBC0; the input pump `FUN_00250540`→`FUN_002481c0` routes the D-pad only to it): `IsFocusedPane(owner) = owner == *(void**)DAT_0208ebc0`. Gate BOTH readers — `IngameMenuReader::OnRowChainFocus` (gated in `HookedDispatch`) AND `menu_reader::OnFocus` (gated after title/pop-up/config early-outs; pop-ups+config exempt). **Rejected the earlier `0x200000` widget-bit idea** (only 3/6 pane classes use it, inconsistent polarity). Entry item: the entered pane's first 0x8000 fires just BEFORE `DAT_0208ebc0` flips, so hook **`FUN_00244830(old,new,flag)`** (RVA 0x124830, sets DAT_0208ebc0=new) and replay the stashed focus when `stash.owner==new`. Added a deduped `[READER] pane owner/focus/focused` diag line.

**2. ITEM/ABILITY/CMD DESCRIPTIONS on `o` (issue 2) — `text_capture.cpp`.** Descriptions use a different sink than `FUN_00291d80`: the display notifier **`FUN_00293170`** (RVA 0x173170) formats via **`FUN_00292b70`** (RVA 0x172B70) which writes the codec to `outBuf+8` (ret 1). Hook FUN_00293170 to set a `thread_local s_inItemDesc` flag around its original; hook FUN_00292b70 and capture `outBuf+8`→`g_helpText` (gen-gated) ONLY while the flag is set (excludes the 3 off-screen width-measurement callers). `o` now reads item/ability/equipment/battle-command descriptions.

**3. BATTLE COMMAND NAMES (issue 3a) — `ingame_menu_reader.cpp`.** The in-battle command list `FUN_002c59d0` does NOT use FUN_00247510; it calls **`FUN_00293110(0xe, cmdId)`** on highlight (RVA 0x173110). Its cell painter **`FUN_002c5900`** (RVA **0x1A5900** — note abs 0x2c5900) resolves+STORES each command's name codec at `*( *(cellCtx+0x10)+8 )+0x18` (line 34), cmdId at `dataCtx+cellIdx*4+0xC8`. Hook the painter → cache `cmdId→codec` (game's own rendered text, memory-only); hook FUN_00293110 (kind==0xe) → look up + speak. Chain is FUN_002c2320→FUN_002c59d0→FUN_002c5900 (battle-command-specific, so cache is clean). Discarded the 0.90 `FUN_0035d330` data-file re-impl.

**4. MENU-STYLE TARGETING NAME (issue 3b) — `ingame_menu_reader.cpp`.** Removed the dead `FUN_003bfe10` hook. Each target node carries the **pre-resolved name codec at `node+0x48`** (built by FUN_00550a00/FUN_003bd040; mirrors Libra "????"). Hook the reticle **`FUN_005528c0`** (RVA 0x4328C0) as the trigger; read `node = *(reticle+0x9f40)`, dedup on the node ptr, speak `*(node+0x48)`. Gate to MENU-STYLE (not field free-roam): `*(void**)DAT_02ca8f38 != 0` (RVA 0x2B88F38) AND `mode (*(u8)(*(void**)DAT_0209be80 [0x1F7BE80] +0x10fa2)) != 3`. Both DATs are POINTERs (deref once). Pure memory read.

**Files:** `src/ui/{menu_reader.cpp, ingame_menu_reader.cpp, text_capture.cpp}`. One build, exit 0, deployed. Nothing committed.

**PICKUP — validation (relaunch; DLL loads at launch):**
1. **Inventory:** only the focused pane speaks (no items/magicks/equipment mixing); `o` reads item descriptions. Check `[READER] pane ... focused=1` fires for exactly one owner per keypress.
2. **Battle command menu:** Attack/Magicks/Technicks/Items/Gambits speak on move (`[INGAME] command:`); `o` = description.
3. **Targeting:** pick Attack, move cursor between enemies — each enemy name speaks (`[INGAME] target:`; reads "????" for un-Libra'd); does NOT fire on the field auto-target line.

**PENDING:** validation of the 4 fixes; then GameArchitecture.md canonicalization of the new RVAs. Deferred: pathfinding (D2/C), B2 target-HP, License Board cell read, battle spell/item SUBmenus (only the ROOT command menu kind==0xe is read).

### Session 29 — TEST RESULTS (2026-07-10, deployed build)

- **Inventory pane isolation: WORKS.** Items menu reads correctly now — no more items/magicks/equipment jumbling. The `[READER] pane owner=… focus=… focused=N` diag confirms the gate: exactly one `focused=1` per keypress (the active pane), background panes `focused=0`. All 6 hooks installed clean (0x1F6EBC0 read; hooks 0x124830/0x173170/0x172B70/0x1A5900/0x173110/0x4328C0), no install failures.
- **OPEN (UX, not a bug):** left/right arrows in the inventory do something unannounced (likely category-tab or character switch). Next session: identify + announce it.
- **BATTLE COMMAND MENU: STILL SILENT.** Hooks `FUN_002c5900`(0x1A5900) + `FUN_00293110`(0x173110) installed but produced **zero `[INGAME] command:` lines**. Either not reached, or fired with `kind != 0xe`, or the cell-codec read failed. **NEXT: instrument** — log every `FUN_00293110(kind,id)` call (kind+id) and every `FUN_002c5900` (cmdId + codec printable?) to see if they fire and why no speech. Verify `FUN_002c59d0`/`FUN_00293110(0xe,...)` is the ACTUAL in-battle command path in this build (the ROOT ATB command menu, Attack/Magicks/…), and that the ATB command menu was actually opened.
- **TARGETING: STILL SILENT.** Hook `FUN_005528c0`(0x4328C0) installed but **zero `[INGAME] target:` lines**. **NEXT: instrument** — log per `FUN_005528c0` call: `*DAT_02ca8f38` (win), mode `*(u8)(*DAT_0209be80+0x10fa2)`, `node=*(reticle+0x9f40)`, and `*(node+0x48)` printable? — to find which gate/read fails (win null? mode==3? node null? node+0x48 not a codec?). Confirm the reticle handler is actually `FUN_005528c0` and it's invoked during menu-style target select.
- Descriptions on `o` for items: NOT explicitly confirmed by the user this pass (verify next session).
- **Committed** the accumulated Sessions 24-29 work (menu + pathfinder) to master as a baseline before starting a fresh session.

## Session 30 — 2026-07-10 — [menu-reader] Battle command menu LOCATED + reading (built+deployed); ≥0.98 RE confidence rule added; targeting/field-menu RE corrected

**KEYWORDS:** Battle command menu = owner **FUN_0027ad70** (RVA 0x15AD70) via the SAME FUN_00247510 0x8000 dispatch the field menu uses (it was the persistent `[focus] UNKNOWN owner=+0x15ad70`); NAME = FUN_00276be0 (0x156BE0) row-draw resolves it into panel+0x1578 (FUN_0035d330(0x15,id)->FUN_002b58b0), cmdId = u16 at panel+0x510+index*8, count panel+0x500. Prior "battle command = FUN_002c2320/FUN_002c5900/FUN_00293110" was WRONG (FUN_002c2320 = field EQUIPMENT). Added CRITICAL ≥0.98 decompile-RE confidence rule to CLAUDE.md.

**Root-cause chain of mislabels (all corrected this session):** every prior battle-command/targeting theory was re-RE'd from scratch (multiple agents + direct reads) after the user kept getting `[focus] UNKNOWN` on the probe. Findings: (1) **FUN_002c2320 = FIELD Equipment screen** (opened only via the pause menu FUN_00281ed0 cmd 0x4b6; its FUN_003ff360/FUN_003ff9f0/FUN_002cd3c0/FUN_002c9bf0 framework is field-only) — NOT the battle command menu. (2) **FUN_002b7590/FUN_002b7b80/DAT_0209e5c0 chain-menu theory = WRONG** (that's the message/dialogue framework; the probe never fired on it). (3) The real battle command menu routes through **FUN_00247510 msg 0x8000** like the field menu — owner class **FUN_0027ad70** (the command PANEL, embedded in container FUN_002778c0 at container+0xf0). USER CONFIRMED via probe: `[bcmd] obj0=+0x15ad70 count=3 cmdId=0x0 "Attack" | 0x12 "Magicks & Technicks" | 0x3 "Items"` (screenshot-verified the on-screen entry IS "Magicks & Technicks").

**SHIPPED (built+deployed, uncommitted):**
- **F1 battle command reading** — `src/ui/ingame_menu_reader.cpp`: removed the wrong FUN_002c5900/FUN_00293110 hooks; hook **FUN_00276be0** (0x156BE0) to cache the DECODED name per cmdId (memory-only — the game's own localized text at panel+0x1578); `menu_reader.cpp` FUN_00247510 0x8000 handler gains an `IsBattleCommandOwner(owner)` (obj[0]==0x15AD70) branch → `OnBattleCommandFocus(owner,index)` reads cmdId at owner+0x510+index*8, looks up the cache, speaks (dedup on cmdId). NOT gated by IsFocusedPane (separate system).
- **Decoder:** `src/core/game_text.cpp` — added codec `0xa0`→'&' ("Magicks & Technicks").
- **Step 0:** CLAUDE.md "Decompile RE confidence bar — ≥0.98 or it does not ship (CRITICAL)" + `feedback_re_confidence_bar` memory. State 0-1 confidence on every RE conclusion; <0.98 discarded or Frida-probe-confirmed, never guessed.
- Probe `FFXII-Decompile/frida/probe_ingame_menu.js` reworked onto the located battle functions (FUN_00276be0 [bcmd] + FUN_005528c0 [tgt]); it CONFIRMED F1.

**BATTLE TARGETING (F2) — RE-CONFIRMED (0.99), not yet re-tested:** `FUN_00552250` window / `FUN_005528c0` reticle (DAT_02ca8f38), driven by battle engine DAT_0209be80; hovered node `reticle+0x9F40`; **name codec node+0x48**, **faction node+0x54** (ally bit 0x10000, else enemy); Libra "????" built in; free-roam auto-target reticle is a SEPARATE object (DAT_0209be80+0x8FA0). The mod's existing HookedReticle (node+0x48) is unchanged this build. Not exercised in the probe run.

**PICKUP:**
1. **Relaunch + test F1:** enter combat, press Confirm → the command menu should speak "Attack" / "Magicks & Technicks" / "Items" as you scroll (`[INGAME] command:` in log).
2. **F2 targeting:** pick Attack → sweep enemies; does the target name speak? (existing node+0x48 path). If yes, ADD the faction-branched vitals: ally = name + Lv/HP, enemy = name + HP% — needs a targeting probe run to pin the Lv/HP + enemy-HP% offsets (the `[tgt]` hook logs node+0x48/node+0x54).
3. Battle sub-lists (Magicks/Technicks/Item spell/item lists) use a DIFFERENT per-row draw callback (FUN_0027ce70 etc. by command type at panel+0x4c0) → different resolver; add after the top-level menu is validated.
4. Deferred field-menu track: inventory L/R character switch (FUN_0027f360/ed10), Status char chooser grid (FUN_00285290).
5. GameArchitecture.md updated with the battle command menu map.

## Session 31 — 2026-07-11 — [menu-reader] Magicks/Technicks two-level + targeting-name fix CONFIRMED; items reverted; Status char-select DEFERRED

**KEYWORDS:** Battle sub-lists two-level: "Magicks & Technicks" (top cmdId 0x12) → confirm returns kind 2 (FUN_0027c3d0) → FUN_0027e050 case 2 type 8 → **category chooser draw FUN_0027d240** (RVA 0x15D240, resolver `FUN_0035d330(cat,id)` cat=(panel+0x513+row*8 & 4)?0x18:0x15) → pick a category → type 0xb → **spell/technick list draw FUN_0027ce70** (RVA 0x15CE70, `FUN_0035d330(0x14,id)`). Items = **FUN_0027e530** draw (RVA 0x15E530) + **FUN_00272cb0** resolver (0x152CB0) — CONFIRMED; my prior "correction" to FUN_0027d5c0/cat2 was WRONG (never deployed) and was reverted. Battle targeting NAME fix = **removed the `*(u8)(DAT_0209be80+0x10fa2)==3` bail** (mode 3 = a real in-menu line/locked target SHAPE, NOT passive; the target window DAT_02ca8f38 only exists during command targeting, so its non-null gate alone excludes free-roam). USER CONFIRMED: "works" (magicks two-level + targeting name).

**SHIPPED + USER-CONFIRMED (built+deployed, committed this session):**
- **Magicks & Technicks reading** — `ingame_menu_reader.cpp` `BattleCommandName` now branches by the panel's per-row draw callback (`*( *(panel+0x1510)+0x120 )`): top-level (FUN_00276be0, cache) / items (FUN_0027e530→FUN_00272cb0) / M&T chooser (FUN_0027d240, cat 0x18/0x15 by per-row flag) / spell list (FUN_0027ce70, cat 0x14). Each branch is guarded by an exact callback-pointer match → wrong guess stays SILENT (never wrong speech). Root cause of the earlier magicks silence: my code only handled the deeper spell list; the tutorial opens the CHOOSER first.
- **Targeting name** — `ReadHoveredTargetName` drops the wrong mode-3 gate; keeps only the window-non-null gate. Reticle FUN_005528c0 / node+0x48 / faction node+0x54 unchanged.
- **Decode 0xa0→'&'** already in game_text.cpp (Session 30).

**DEFERRED (per user — untestable in the one-character prologue tutorial):**
- **Status field-menu character-select reader** (`ingame_menu_reader.cpp` HookedStatusCursor on FUN_00285a10 + ReadStatusSlot): SILENT and NOT a working feature. Decompile root cause: FUN_00285a10 does NOT fire for the highlight on menu ENTRY — FUN_00285290 case 1 clears via FUN_00285a10(-1) then sets ctrl+0x117=0 by DIRECT WRITE; FUN_00285a10 only gets a valid slot from nav FUN_00285190 on a d-pad MOVE, and the tutorial party is one character (nothing to move to). Needs the broader **"speak initial focus on menu entry"** work + a different hook event. Also corrected a latent bug: `RVA_PAUSE_CTX` 0xE9AC30 → **0x1F7AC30** (DAT_0209ac30; add-back verified). Values read design (FUN_00283e40 offsets: charId block+0x60, curHP +0x20, maxHP +0x24, Lv +0xba, curMP +0x2c, maxMP +0x30) and name (FUN_0035d330(2,charId)) are kept for the revisit. Column LABELS (LEVEL/HP/MAX/MP/MAX) are NOT resolvable via FUN_002f9860/FUN_002f9920/FUN_0029feb0 as probed — likely type-7 baked atlas glyphs OR a build-timing miss; unresolved. See `notes/battle_target_vitals_2026_07_10.md` and `notes/status_char_select_labels_2026_07_11.md`.
- **Battle target vitals** (ally Lv/HP, enemy HP%) — RE done, not shipped; offsets in `notes/battle_target_vitals_2026_07_10.md` (id→FUN_002367a0→BtlChr@actor+0x698; curHP bc+0x48, maxHP bc+0x24, Lv bc+0x1c2).

**PICKUP:**
1. Post-tutorial (multi-character party): revisit the Status char-select — probe FUN_00285a10 to confirm it fires on d-pad move + the corrected read chain (ctx=*(base+0x1F7AC30)) yields valid values; find the initial-focus-on-entry hook event (read ctrl+0x117 after entry).
2. General feature: **speak the initial focus when a menu screen opens** (applies beyond Status).
3. Battle targeting vitals (ally Lv/HP, enemy HP%) and field-menu ability targeting (heal/buff → pick ally) — both deferred targeting interfaces.
4. Char-select column labels: decide source once the values work (draw-path probe with fire counters, or accept baked-glyph read).

## Session 32 — 2026-07-11 — [target-reader] Battle target-selection readout SHIPPED (real path found on DAT_0209be80)

**KEYWORDS:** The Session-28..31 targeting hook (reticle `FUN_005528c0` / `node+0x48` / gate `DAT_02ca8f38`) **NEVER FIRED** — DISPROVEN by live Frida (zero `[tgt]`/`[ret]` lines). It is the free-aim/area-target mode only, NOT the normal Foes/Party/Allies **highlight menu**. Root cause of every failed attempt: target selection is a **SEPARATE object on the battle-HUD context `DAT_0209be80`** (RVA `0x1F7BE80`), **not** the command controller `DAT_0209ac30` (`ctx+0xde0` = the ACTING character, stayed on Reks even while aiming at the enemy — that shipped a wrong "Reks at battle start" readout, now removed). Confirmed by three independent decompile traces converging on the same field + a Frida run. **Current highlighted target handle = `*(int)( *(DAT_0209be80) + 0x9FD8 )`** (nameplate/target-info mgr `0x8fa0+0xac0+0x578`); target selection is active while `*(P+0x10f78) != 0` (single-target selector obj; absent during plain command navigation); the target is committed by `FUN_002be300` (RVA `0x19E300`) which plays the cursor-move beep `FUN_00249c60(1)` only on a real change. Handle decode `FUN_003588b0` (RVA `0x2388B0`, ABS 0x3588B0) = `(list=bits16-19, slot=low16, gen=bits20-30)` — UNRELIABLE to call directly from a hook (returned garbage). So the reader instead: `FUN_002bfd20` (RVA `0x19FD20`, nameplate render) resolves the handle to the real BtlChr and passes it to the vitals builder `FUN_00329220` (RVA `0x209220`); we flag "this render == current target" when `panel+0x288 == *(P+0x9FD8)` and capture `bc` in the nested `FUN_00329220` call. Name = actor pool (`*(actor+0x698)==bc` → `actor+0x18`); **real** HP = `bc+0x48`/`bc+0x24`; faction = scene-kind nibble `*(u8)( *(actor+0x10) + 0x0e ) & 0x0f` (`3`=ally). USER: enemy readout "so close" (worked); ally was mis-classed as enemy (percentage) via the wrong `panel+0x280 & 2` flag → switched to the scene-kind test.

**SHIPPED (built + deployed):**
- **New `src/ui/battle_target_reader.{h,cpp}`** — two-hook readout (`FUN_002bfd20` marks the current-target render; nested `FUN_00329220` captures the real BtlChr), gated on the selector `+0x10f78`, deduped on target-handle change (covers initial entry). Speech: **enemy** = `"<name>, HP <pct> percent"` (percentage from real cur/max; no MP, no numbers pre-Libra); **ally** = `"<name>, HP <cur> of <max>"`. Reads memory-only, SEH-guarded, game thread, no game calls. Wired into `MenuReader::Init/Shutdown`.
- **Removed the dead reticle hook** (`FUN_005528c0` / `ReadHoveredTargetName` / `HookedReticle` + its constants) from `ingame_menu_reader.cpp` — it never fired.

**DEFERRED:**
- **Context-sensitive ally HP↔MP** (MP for MP restoratives, DQ7R-style) — needs an MP restorative to test (only Cure/Thunder in the tutorial). Detect from the aimed action's effect.
- **Enemy post-Libra numbers** — the real HP-visible/Libra flag is still UNKNOWN (BtlChr status bit `0x10000` was WRONG: read 0 for the un-Libra'd enemy). Enemy stays percentage-only, correct for the whole no-Libra period.
- **Multi-target sweep** — the tutorial is solo-Reks + one enemy, so the cursor can't move; initial-entry works, sweep verifies once a battle has multiple targets (targetId `+0x9FD8` will change per move).
- **Party-HUD status bar** (lower-right, all members' HP/MP) — separate feature.

**PICKUP:**
1. Confirm the **ally** readout in-game (scene-kind fix); confirm no readout during plain command navigation (gate `+0x10f78`).
2. Multi-target battle: confirm the cursor-move re-announces (targetId `+0x9FD8` changes; beep `FUN_00249c60(1)`).
3. Stage 3 context-sensitive ally MP when an MP restorative is obtained; find the real enemy HP-visible/Libra flag when Libra is available.

## Session 33 — 2026-07-12 — [pathfinder] Turn-by-turn routing SHIPPED via the SQEX field walkmap (Bullet was a dead end); Enter-key + ally-HP fixes

**KEYWORDS:** turn-by-turn route `\` key SQEX walkmap FUN_003208c0 FUN_00230b60 mask=4 walk-class getgroundy string-pull secondary-direction decomposition DIERR_INPUTLOST WH_KEYBOARD_LL retired ally HP scene-kind def+5

**THE WIN — turn-by-turn field directions now WORK in the prologue.** Long arc this session:
- **Remap:** `\` = turn-by-turn route (was mis-wired to describe; the A* route had drifted onto `/` in S24 and was never actually invoked by the user). `/` now = describe. Diagnostics baked in (per-condition `IsFieldNavSafe` fail-mask, ray/floor stats, `'` self-test).
- **Bullet DEAD-END (do not retry):** the route's walkability was built on the Bullet raycast world (`FUN_006a1a70`, world at `*(ctx+0x60)`). Runtime-proven the **Nalbina prologue builds NO Bullet world**: `FUN_006a0310` is the *sole* Bullet-world constructor and runs only inside the master physics tick `FUN_00698c80`'s per-region loop, **skipped when region count == 0**. Four ctx-capture hooks (builder `FUN_006a0310`, step `FUN_0069f070`, raycast `FUN_006a1a70`, char-ground `FUN_006a5c00`) ALL installed but NEVER captured — the prologue moves characters on a non-Bullet system. `failMask=0x40[world]` every attempt.
- **THE ORACLE — SQEX field walkmap (shipped, `src/navigation/map_query.{h,cpp}`):** the actual floor/wall mesh the field + AI NPCs walk on, loaded with every map independent of Bullet. Ground = **`FUN_003208c0`** (RVA **0x2008C0**) `bool groundAt(float x, float z, float* outY)` (arbitrary-XZ floor probe, uses cached ctx `DAT_02ec1370`). Wall/segment = **`FUN_00230b60`** (RVA **0x110B60**) `int seg(ctx0, out16, from[4], to[4], u16 mask, u32 flags)` (>=0 BLOCKED, <0 CLEAR). ctx0 = `*DAT_0209a678` (RVA 0x1F7A678), gated by `DAT_0209a670` (RVA 0x1F7A670). Both reentrant, read-only, safe for thousands of calls/route. **Confirmed live in prologue:** `hasWorld=1, ground hit=1 y=25.00`, routes drain `plan=Route legs=N`.
- **Fix A — walk mask (0.98, RE-confirmed):** the seg "mask" is a query-CLASS enum (`==4` in `FUN_0022cc50`), NOT a bitmask. Routing now passes **mask=4, flags=0** (the WALK class) instead of the camera class 0xffff/1. Proven decisive: the PLAYER leader's own per-frame wall feelers use it (`FUN_002593a0`→`FUN_00259990(1,0,…)` slot-0=leader→…→`FUN_0032cf50`→`FUN_003d97e0(…,4)`→`FUN_00230b60(…,4,0)`). Class 4 blocks real+character-only walls, skips camera-only planes/floors/ceilings; 0xffff was *doubly* wrong (fixed the "detours around a wall that isn't there"). `'` self-test now logs walk(4) vs cam(0xffff) per cardinal as the runtime confirm.
- **Fix B — smoothing + secondary directions:** greedy line-of-sight string-pull collapses the 1m-cell A* staircase; each smoothed leg is decomposed into **primary + secondary cardinals** ("North 16, East 2"), NEVER a lone intercardinal ("Northeast 18"). Killed the "South 0" overshoot jog.
- **Fix D — Enter-key drop:** static trace proved OUR code has no key-swallow (dinput8 `GetDeviceState` hook read-only; LL hook always `CallNextHookEx`; no combat-log modal exists). Shipped: retire the redundant global `WH_KEYBOARD_LL` hook once DInput latches (log: `WH_KEYBOARD_LL hook retired`); add a `GetDeviceState` diagnostic. **The diagnostic CAUGHT the drop:** `INPUT-DIAG keyboard GetDeviceState FAILING hr=0x8007001E` = **DIERR_INPUTLOST** → the keyboard device went UNACQUIRED (game sees no keys) → the drop is **upstream (game/OS focus/acquisition), NOT our mod**. See Known Issues for the follow-up.
- **Ally HP (S32 pickup):** `battle_target_reader.cpp NameForBtlChr` — ally now = `def5==0 || kind==3` (self-target leader has scene-kind != 3), so a curative on yourself reads "HP N of M" not a percentage.

**KNOWN ISSUES — routing polish for NEXT SESSION (user-reported; see debug.md Known Issues):**
1. **Directions point away from the objective (intermittent).** Holding a route direction moves the player *further*; re-route reports a *growing* distance ("East 15"→"East 17"→"East 20"). User: not a constant flip ("works sometimes"). Lead hypotheses: (a) **world-cardinal vs. player control frame** — route speaks WORLD cardinals (north=-Z, east=+X) but stick/movement is CAMERA-relative, so "east"=="right" only at certain camera angles (explains the intermittency) → likely needs an EGOCENTRIC route mode (ahead/left/right off `ReadPlayerYaw`) or a facing-vs-route cue; (b) a geometric route bug (bad first waypoint) tied to issue 3. Next session: instrument the raw A* polyline + smoothed polyline + target + player yaw and correlate.
2. **Distance cap too small.** `kMaxRange=40m` → targets beyond ~53 steps get "Too far to route" (log: target `(49,-0.01,3)` ~51m north → TooFar). User wants to route to ANY map entity regardless of distance. Raise/remove `kMaxRange`; raise A* budgets (a 27×15 route already used 1266 rays / 157 expands vs caps `kMaxRays=2000` / `kMaxExpand=500`, so long routes will blow them — need much larger caps or a coarser long-range search). Verify the entity scan lists all map entities (handle table + actor pool, no distance filter — confirm distant objects are enumerated).
3. **Routes cut through walls/rooms.** A route said "East 15, South 17" (single straight diagonal) to a target the player can't reach by going east/south (walls between) → walkability is UNDER-detecting walls / the string-pull straightened a valid detour into an impassable line. Opposite of the old 0xffff over-block. Next session: log the seg-test result along the reported straight line (does mask=4 block the wall?); check whether the LOS smoothing is too permissive (a body-height straight line can be clear over a floor while a room wall blocks elsewhere, or the wall is a type mask=4 skips) → add the deferred smoothed-segment validation (denser sample + floor-continuity + `|ΔfloorY|` check); verify `GroundAt` doesn't return floor across gaps/room boundaries. Related to issue 1.

**ENTER-KEY follow-up:** the drop is upstream (DIERR_INPUTLOST = device unacquired on focus loss). Not our bug. If it recurs and hurts, consider a mod-side re-`Acquire()` nudge when we detect the failure — but that touches the game's device and needs care; defer unless it becomes a real blocker.

**COMMIT:** this session's working state committed (routing via SQEX walkmap + Fix A/B/D + ally-HP). Full RE archived in `GameArchitecture.md` (SQEX walkmap oracle + mask=4 taxonomy).

## Session 34 — 2026-07-12 — [pathfinder] Whole-map walkmap-grid overlay (Session-33 fixes)

**KEYWORDS:** whole-map routing overlay walkmap grid direct read FUN_0022ffe0 FUN_00233050 FUN_00231890 ctx0 header nCols nRows cellSize origin brick stagger NavGrid EnsureBuilt ReadCellFloor SegmentTraversable dense validator kMaxRange removed 32767 short cap world-absolute directions delta-scan non-regression prologue no treasure nameIdx -1

**Fixed all three Session-33 routing issues by rebuilding the planner's grid layer** on a
whole-map walkability OVERLAY read directly from the game's own SQEX walkmap grid — the
architecture the user asked for ("we effectively need a map overlay"). Built + deployed; **PENDING
one runtime confirmation pass (baked self-diagnostic), NOT yet committed.**

- **KEY RE (conf 0.92, offline; runtime-confirm via `'`):** the SQEX walkmap **is itself a uniform
  staggered ("brick") grid** over a floor-triangle + wall-segment mesh — the same structure the
  engine's own full-grid enumerator **`FUN_0022ffe0`** (RVA 0x10FFE0) walks. `ctx0 = *DAT_0209a678`
  exposes: grid header ptr `+0x00` (→ `nCols +0x08`, `nRows +0x0C`, `cellSizeX +0x10`, `cellSizeZ
  +0x14`, all int meters), vertex array `+0x08` (stride 0x10), floor-poly array `+0x10` (stride
  0x20: planeA/B/C @0/4/8, flags @0xC type=low3bits, baseVert s16 @0x10), wall array `+0x18`
  (stride 0x90, phase-2), CSR cell→list table `+0x20` (u16[nCols*nRows+1]), primitive list `+0x28`
  (u16; <0x4000 floor-poly idx, 0x4000-0x4FFF wall, ≥0x5000 empty), originX `+0x38`, originZ
  `+0x3C`. World→cell = `FUN_00233050` (0x113050): `col=(originX+wx)/cellSizeX`; `if(col&1)
  gz-=cellSizeZ/2` (**integer**); `row=(gz)/cellSizeZ`; cell=`nCols*row+col`. Height = `FUN_00231890`
  (0x111890): `vy + ((vx-x)*A + (vz-z)*C)/B`. **Map extent is free at runtime** = `nCols*cellSizeX ×
  nRows*cellSizeZ` m, min-corner `(-originX,-originZ)`; the 16-bit cell index hard-caps every map at
  **≤32767 cells** (a few hundred m/side). NO callable engine A* (AI = reactive steering +
  waypoint-follower `FUN_00335180`), so we keep our own A*.

- **Architecture (`src/navigation/`):** new `nav_grid.{h,cpp}` = persistent whole-map overlay
  (`EnsureBuilt(epoch)` bakes `walkable[]+floorY[]` in one O(cells) pass, cached per map-epoch,
  game-thread-only, `Invalidate()` on teardown). `map_query` gained `GetGridInfo`/`WorldToCell`/
  `CellCenter`/`ReadCellFloor` (direct grid parse, zero raycast) + `SegmentTraversable` (dense
  validator). `path_planner` A* now runs over the overlay end-to-end.

- **Bug 2 (40 m cap) — ELIMINATED.** Dropped `kMaxRange`/`Plan::TooFar`/per-cell `GroundAt` raycast;
  A* searches the whole ≤32k-cell grid at the walkmap's native resolution; budgets raised
  (`kMaxExpand=20000`, `kMaxRays=60000`) as safety ceilings only. Per-cell floor sampling is now a
  zero-raycast overlay read; the only rays are A* edge wall tests + string-pull validation (bounded
  by ROUTE size, not map size). Full turn-by-turn at ANY distance — no approximate heading.

- **Bug 3 (routes through walls) — FIXED.** `SegmentTraversable(a,b,step=1m,...)` samples every 1 m
  requiring floor-continuity (GroundAt) + `|ΔfloorY|≤kMaxStep` + per-substep `SegmentClear(mask=4)`
  at the local floor height (shrinks the SegmentHit Y-flatten to ~1 m). Used in the string-pull in
  place of the single long ray, so a detour is only collapsed when the straight span is densely
  traversable — every committed leg is wall-validated. Dead `kMargin` removed.

- **Bug 1 (points away) — kept WORLD-ABSOLUTE + instrumented.** Per user directive: directions stay
  north/south/east/west from the player (NO egocentric/facing mode — camera only moves the view).
  The "points away" symptom is chiefly Bug 3 (through-wall routes) bleeding in; added `NAV-ROUTE`
  logging (yaw°, target, raw+smoothed polyline sizes, first leg, spoken text) to catch any residual
  first-waypoint geometry bug in the runtime pass.

- **Confirmation = baked C++ self-diagnostic (user's choice, no Frida).** `'` now logs the grid
  header (→ exact map extent) and cross-checks the direct read (`ReadCellFloor`) vs the confirmed
  `GroundAt` oracle over a ~400-cell stride sample (`grid xcheck: walkAgree=N (X%)`). High agreement
  promotes the 0.92 direct read to ship bar; `NavGrid::SetDirectRead(false)` is the GroundAt-bake
  fallback lever if it fails.

- **Non-regression (verified before coding):** the live enemy scanner is stateless input-thread
  re-enumeration of the actor pool (`ScanCombatantsLocked`), separate mutex from the router, on the
  same frame hook but ordered EntityList-first. Preserved all 5 couplings (untouched
  `nav_hooks::HookedFieldFrame`; router never calls `EntityList::*`; teardown keeps the epoch bump +
  request clear + adds `NavGrid::Invalidate`; `map_query` changes additive-only; scan path
  untouched). Router changes cannot reach the scanner.

- **Prologue objects (user check):** confirmed no treasure on Nalbina Inner Ward — `ClassifyByNameKey`
  labels Treasure only for npcdic id 434/468 with `nameIdx≥0`; every observed prologue object has
  `nameIdx=-1` (combatants, talk NPCs, an iron gate=ACTION→Object). Deployed log has 0 "Treasure".
  No classifier change; the enhanced `'` dump (nameIdx/flags/name) verifies identities live.

**PENDING (next session, tester on a fresh field map):** press `'` → confirm `grid NxM cell=…` +
`grid xcheck walkAgree ≥~90%` on ≥2 maps; `\` to an object >40 m → `plan=Route` (not TooFar), full
turn-by-turn; reproduce the through-wall case → smoothed poly retains detour corners; spawn enemies +
cycle scanner → still appear. THEN commit + move the 3 Known Issues to Solved. Straight-to-C++
exception (no new RE crash surface — tuning confirmed primitives + a struct read confirmed in-C++).

## Session 35 — 2026-07-12 — [pathfinder] Facing-relative directions ("North" = forward), DQ7R model

**KEYWORDS:** facing bug comp+0x100 frozen 129 comp+0x15C current heading comp+0x160 target comp+0xAC brad byte camera-relative movement DQ7R relative directions North=forward RelativeCardinal frame toggle `.` key ReadFacingCandidates convention calibration pending

**Runtime (S34 log) proved absolute directions are unfollowable + facing read is broken.** With the
S34 overlay giving geometrically-correct WORLD-absolute routes, the tester held the stick toward
"North" and the character moved SOUTH (from Z 110→116→122→124, +Z=south, target north) — re-route
"North" grew 30→38→41. And `yaw=129deg` was **frozen all session**. Two agents diagnosed both:
- **Facing bug root cause (0.92):** `comp+0x100` (was read as a "forward row") is NOT a forward
  vector — it's an internal point-vector lane on the persistent char component, so `atan2` froze at
  ~129deg. **Live facing = `comp+0x15C`** (current heading, rad); alts `comp+0x160` (target/cached,
  = actor-pool +0x160) and `comp+0xAC` (byte brad, deg=×1.40625). `comp = *(sceneObj+0x30)`. 0.85.
- **Movement is camera/facing-relative: CONFIRMED (0.9)** — the leader seeks a target set upstream
  from `stick+cameraYaw` (driver `FUN_0032aec0`; camera = behind-cam `PPhysicsCharacterCamera`,
  update 0x574260). So absolute cardinals can't be followed without a facing frame.

**DQ7R correction (I mis-read it twice; user corrected):** DQ7R does NOT remap movement to north — it
speaks directions **relative to the frame, using cardinal words where "North" = forward = up on the
stick**. Facing southeast + target ahead → "North 3" = push up 3 (southeast *becomes* "north" for that
instruction). It's a **speech relabel, NOT an input/movement remap**. Works because the behind-camera
makes forward ≈ up-on-stick ≈ facing — so we only need the live FACING, no camera-yaw discovery and no
input hook. (An earlier "up=north stick remap" plan was drafted then RETRACTED by the user.)

**Shipped (built+deployed, straight-to-C++ per the overlay precedent; PENDING calibration, NOT
committed):**
- `player_state`: `ReadPlayerYaw` → `comp+0x15C`; new `ReadFacingCandidates` (logs all 3).
- `nav_common`: `RelativeCardinal(dx,dz,facingYaw)` decomposition (project leg onto facing
  forward F=(sin f,−cos f)/right R=(cos f,sin f) → North=ahead/East=right) + `RelativeCardinalBearing`
  + a shared `g_relativeDirections` frame flag (default RELATIVE).
- `path_directions::Describe(poly, relative, facingYaw)`; `path_planner` reads facing + frame, logs
  `frame=relative/absolute yaw=…`; `entity_list` `/` describe uses the same frame.
- `nav_commands`: **`.` key** toggles relative/absolute (spoken); `'` diagnostic now logs the 3 facing
  candidates + `cardinal(cur)`. `input_tracker`: `.` wired (DIK 0x34).

**PENDING = CALIBRATION (the facing angle convention is assumed = BearingDeg atan2(dx,−dz), unconfirmed
at 0.85).** Tester: walk toward an object so you FACE it, press `\` (relative mode) → expect "North N";
also press `'` + `;`. Send the log — I compare logged `yaw` vs the world bearing to the target to lock
the convention (add an offset/sign in `ReadPlayerYaw` if the frame is rotated/mirrored). Then confirm
`;` tracks turning + directions are followable, and commit. Separate follow-up still open: 8 m coarse
overlay grid (some false NoPath) + 84% direct-read agreement.

**UPDATE (same session) — frame is CAMERA, not facing.** User clarified the field camera is a FREE
look-camera on a separate stick that does NOT trail or turn the character; movement is CAMERA-relative
(decompile ~0.9 + the runtime south-while-north). So "up on the stick" = camera-forward, and facing is
the WRONG reference (diverges whenever the camera is rotated). Switched the relative frame from facing
to the **camera yaw**. Camera-yaw source = **static community globals** `DAT_020955e0` (pos, RVA
0x1F755E0) + `DAT_020955f0` (look-at, RVA 0x1F755F0), Vector3f each; `camYaw = atan2(fwdX,-fwdZ)`,
fwd = look-at − pos. **These are the same globals `feedback_validate_community_rvas` grep-flagged
"absent" — but that is a FALSE NEGATIVE** (register-written struct fields have no literal DAT_ xref;
XIIHook + ffgriever freecam read them). Now `player_state::ReadCameraYaw`/`ReadCameraVectors`; route +
`/` describe use camera yaw for the relative frame (fall back to absolute if not live); `'` diagnostic
logs `camera: pos/look/yaw` to VALIDATE the globals live (yaw must track a camera rotation) before we
depend on them. Built+deployed. PENDING: user presses `'`, rotates camera-only, re-presses `'` → does
`camYaw` change? + directions line up. If globals dead → escalate to a read-only hook on the camera
object (`PPhysicsCharacterCamera` update, getCameraPosition=this+0x48; getCameraDirection thunk
0x583950). If live → update [[feedback_validate_community_rvas]] (globals ARE usable) + commit.

## Session 36 — 2026-07-12 — [pathfinder] Absolute directions only; fine GroundAt routing grid

**KEYWORDS:** removed facing relative camera orientation dormant code world-absolute CardinalBearing crow-flies parity turn+forward tank fine grid GroundAt 1.5m lazy cache 8m walkmap coarse false NoPath wall margin SegmentTraversable lateral offset rays

**Resolved the long direction-frame saga + attacked the real pathing bug.** Runtime tests killed every
frame theory: camera-position globals `DAT_020955e0/f0` read the **dead freecam slot** (frozen
pos=(0,0,0)); `comp+0x15C` facing IS live but was the wrong reference; a decompile trace of the player
input path (`FUN_0032c910`→`FUN_0032b9c0`: stick-X=turn `char+0x70`, stick-Y=forward, no camera in the
math) said movement is turn+forward, but the user's in-game test (holding left/right STEPS the char)
contradicts pure tank — mechanism stays unresolved. **User's decisive call: directions are
WORLD-ABSOLUTE, no facing/relative/camera/orientation; remove that dormant code; the real fix is the
pathing.**

- **REMOVED** all direction-frame machinery: `RelativeCardinal`/`RelativeCardinalBearing`/
  `EgocentricBearing`/`kEgo`/`g_relativeDirections`, `ReadPlayerYaw`/`ReadFacingCandidates`/
  `ReadCameraYaw`/`ReadCameraVectors`, `COMP_HEADING_*` + `CAMERA_*` RVAs, `SpeakFacing`(`;`) +
  `ToggleDirectionFrame`(`.`) + their key bindings, `NorthSouthWord`/`EastWestWord`,
  `SetEgocentric`/`IsEgocentric`, and the facing/camera `'`-diagnostic blocks.
- **Directions = world-absolute, IDENTICAL to `/` crow-flies:** `DescribeDirection(from,to)` +
  each `\` route leg now use a single 8-point `NavCommon::CardinalBearing` (same axes north=−Z, same
  `ReadPlayerPos`), so `\` and `/` never disagree ("North 18, Northeast 5. 23 steps").
- **PATHING FIX (the priority):** the routing grid was the walkmap's NATIVE **8 m** cells (a coarse
  spatial index; log showed `cell=8x8`, causing `NoPath` to ~3 m targets + blocky/through-wall routes).
  Rebuilt `NavGrid` as a **fine ~1.5 m uniform grid lazily sampled via `MapQuery::GroundAt`** (the real
  floor mesh) + cached per map-epoch (cost = explored cells, no whole-map bake; off-map GroundAt=false
  bounds the search, no distance cap). Re-added a **0.5 m lateral wall margin**: `SegmentTraversable`
  (string-pull) + the A* edge test now fire two rays offset ±margin perpendicular, so a leg only counts
  clear with body width on both sides (stops through-walls + wall-sliding/corner-cut).

Built + deployed clean. **PENDING runtime test:** `\` to a close object that used to say `NoPath` →
real route; routes stop cutting through/along walls; `\` first-leg cardinal == `/` crow-flies cardinal;
`'` stats show `fineCell=1.5m gridSamples=N`. Then commit + correct
[[feedback_validate_community_rvas]] (camera globals = dead freecam) + record in GameArchitecture.
Superseded [[project_pathfinder_whole_map_overlay_session34]]'s 8 m direct-read overlay.

## Session 37 — 2026-07-13 — [pathfinder] Egocentric directions re-anchored to camera-forward (camera-relative CONFIRMED)

**KEYWORDS:** camera-relative movement confirmed egocentric directions North=forward camera-forward DAT_02aedf30 row2 fwd.x DAT_02aedf50 fwd.z DAT_02aedf58 atan2(-fwd.x,-fwd.z) ReadCameraForward faceNode wrong reference FUN_004742a0 FUN_00358cb0 FUN_003820c0 DAT_02aedf94 view-matrix diagnostic sign trap position-delta ground truth calibration walk W-leg D-leg right=East ; Forward points combat target-facing FUN_00307300 FUN_0037b4d0 FUN_0031adb0 boss loses focus

**Reversed Session-36's "world-absolute, no camera" call — the decompile PROVED movement is
camera-relative, so directions had to become egocentric on the RIGHT reference.** Session 36 removed all
facing/camera code on the premise the camera doesn't govern movement. A boss-fight report ("boss called
NE while audibly to my left; going left made me attack it") + a first-time faceNode-egocentric build that
came out ~90° off in the field reopened it. Read the actual code (not just agents):

- **Movement is CAMERA-RELATIVE (≥0.98, code-proven).** `FUN_004742a0` (RVA 0x3542A0) *unconditionally*
  rotates the stick by the camera matrix `DAT_02aedf30`: `worldMove = stickX·row0 − stickY·row2` (row0
  camera-right, row2 camera-forward, Y zeroed + normalized) — NO top-down branch. `FUN_00358cb0`
  (RVA 0x238CB0) writes the move vector `DAT_022c7fd0/4/8` and sets facing = `atan2(moveX,moveZ)` via
  `FUN_0026a0d0` (node+0xA4) ONLY while moving. The "top-down feeling" is the follow-cam trailing behind.
- **`faceNode` (node+0xA4) was the wrong reference.** It = "where UP takes you" ONLY while actively
  walking; when stationary (exactly when you query) it's stale, and in combat the battle action /
  target-steering subsystems (`FUN_00307300` 0x1c7 / `FUN_0037b4d0` / `FUN_0031adb0`) turn it to face the
  TARGET — that's the boss "NE" bug. (bit 2 of the flag byte, the skip-move-facing flag, is script-only, NOT a lock.)
- **`DAT_02aedf94` scalar is NOT the movement forward** — `FUN_003820c0` builds it from the view/sibling
  matrix `DAT_02aede70` with sign-flips; its delta to the move heading wanders. Diagnostic-only.

**FIX (shipped, built, deployed):** anchor "North = forward = where UP takes you" to the LIVE camera-forward
read from the MOVEMENT matrix `DAT_02aedf30` row 2 (+0x20): `fwd.x=DAT_02aedf50` (RVA 0x29CDF50),
`fwd.z=DAT_02aedf58` (RVA 0x29CDF58); up-direction = `atan2(−fwd.x,−fwd.z)`. New `PlayerState::
ReadCameraForward`; every `facingRad` source repointed `ReadPlayerFacing`→`ReadCameraForward`
(`path_planner` `\` legs, `entity_list` scanner describe + obstacle hint, `nav_commands` `;`). `;` now says
"Forward points <cardinal>" (which real-world way UP points). `nav_rva.h` +`CAMERA_FWD_X/Z`. The egocentric
transform in `nav_common.cpp` was already correct — only the reference vector changed.

**CALIBRATION (baked `'` diagnostic, position-delta ground truth — user-run, no Frida):** logs pos-delta
(actual walked dir) + move vector + faceNode + BOTH camera-forward sign candidates. Result:
- W leg (walk forward, sampled moving): `camFwdNeg=168.9°` (camera matrix ALONE) == move vector `168.9°` ==
  faceNode `168.9°` == walked `174.7°` (±6° follow-cam drift); `camFwdRaw` was the 180° opposite. ⇒ camera
  orientation alone predicts movement ⇒ **camera-relative confirmed at ground truth; sign `(−,−)` correct.**
- D leg (walk right): forward `151.1°`, moved at `61.1°` = forward−90 ⇒ transform yields `ego=+90=East` ⇒
  **right→East, no handedness flip.**
So the provisional sign was right: **no further feature-code change.** User confirmed `;` tracks camera
rotation. `'` diagnostic KEPT (lean; re-verify after game updates / boss fights).

Supersedes Session-36's world-absolute direction model and the Session-35 DQ7R relative-facing model.

### Next steps — combat targeting (NOT implemented this session)
Addresses the original "loses focus in combat" via an explicit locked-target route, bypassing the mod cursor:
1. **`p` (VK_P) = route to the game's locked/selected target.** Reuse the Session-32 battle-target reader
   (`DAT_0209be80`, sel handle +0x9fd8, gate +0x10f78 — the combat trace confirms `DAT_0209be80` is the
   target-selection/reticle module, 61 refs all `0028exxx`). Read target scene object → world pos →
   `PathPlanner::Request` (same pipe as `\`, but to the locked target). Wire `VK_P` in `input_tracker.cpp`
   (free `g_extraDown` slot) + an `OnNavKey` case. **PRE-SHIP CHECK (≥0.98):** confirm the game's Lock-On
   (`2`) drives that SAME `DAT_0209be80` object vs a camera soft-lock — a quick xref, not a guess.
2. **Target-cell walkability snap** in `PlanRoute` (`path_planner.cpp`): snap the target to its nearest
   walkable cell (nav_grid / map_query) so an off-mesh / map-edge target can't cause a latent NoPath.
3. **`\`-cursor lock-by-identity** (`entity_list.cpp`): re-anchor the `[`/`]` cursor to its object by stable
   identity across rescans so it can't silently jump to the nearest.

## Session 38 — 2026-07-14 — [pathfinder] `p` = route to locked battle target; walkable-cell snap; cursor lock-by-identity

**KEYWORDS:** p key VK_P DIK_P 0x19 g_extraDown[5] route to locked target lock-on combat loses focus
DAT_0209be80 handle +0x9FD8 gate +0x10F78 battle_target_reader GetLockedTarget TargetCache 300ms freshness
NameForBtlChr actor+0x698 sceneObj actor+0x10 ReadSceneObjectPos sceneObj+0xB8 PathPlanner::Request
RouteToLockedTarget OnNavKey 'P' PRE-SHIP-CHECK lock-on persistence nav-safe battle-state FIELD_ACTIVE2
0x1F69300 PlanRoute target-cell walkability snap nearest walkable cell ring search snapped goal CursorId
FindFocusInViewLocked CursorMatch identity re-lock pointer aliasing SetFocusLocked entity_list Controls swap

**Implemented all three of Session-37's "Next steps — combat targeting" items.** Key correction from the
user: **FFXII battles run ON the field** — the battle-state mode only changes camera/targeting; the field
frames keep ticking and the SQEX walkmap stays live, so routing works in combat exactly as on the field.
That flipped `p` from a bearing-only readout to a **full turn-by-turn route** (the Session-37 note's literal
intent). Shipped straight to C++ under the Session-34 confirmed-primitives exception (every read below was
already shipped/live-confirmed — no new RE crash surface), with baked `NAV-ROUTE` logging that answers the
PRE-SHIP CHECK empirically instead of by guess.

- **Item 1 — `p` (VK_P) = turn-by-turn route to the locked/selected battle target.** `battle_target_reader`
  now resolves the target's world position in the same actor-pool pass it already used for name/faction
  (`*(actor+0x698)==bc` → `sceneObj=*(actor+0x10)` → `PlayerState::ReadSceneObjectPos` `sceneObj+0xB8`), and
  keeps a mutex-guarded **cache** (`pos`+`label`+`handle`+`tickMs`) refreshed on EVERY flagged-target render
  (not the announce-dedup). New `BattleTargetReader::GetLockedTarget(pos,label,maxAgeMs)` returns it only when
  the capture is younger than 300 ms (⇒ selection/lock still live) — no cross-thread game-pointer deref on the
  input thread. `nav_commands::RouteToLockedTarget()` feeds that into the SAME `PathPlanner::Request` pipe as
  `\`. Key wired via `input_tracker` (`DIK_P=0x19`, free `g_extraDown[5]`, `isNav=true`) → `OnNavKey('P')`.
- **Item 2 — target-cell walkability snap in `PlanRoute`.** When the A* GOAL cell is non-walkable (target on
  a ledge / map edge / enemy-only tile / fine-grid gap), ring-search outward ≤6 cells (~9 m) for the nearest
  walkable cell (closest-to-true-target within a ring) and use it as the goal; the final poly point becomes
  that walkable cell (not the off-mesh target), so the last leg lands on ground, not into a wall. `snap:` line
  logged. Non-off-mesh targets are byte-for-byte unchanged (zero regression to `\`).
- **Item 3 — `\`-cursor lock-by-identity.** Replaced the bare `void* g_currentObj` focus with a `CursorId`
  (obj + npcdic `nameIdx` + label + category). Centralized the 3 duplicated match sites into `CursorMatch`
  (tier 2 exact = pointer+key+label, rejecting a pooled-slot pointer alias; tier 1 re-lock = key+label+category
  under a new pointer) + `FindFocusInViewLocked`. Focus now tracks its object across rescans; the
  "fall back to nearest" path fires only when the object truly departed, not on a transient rescan wobble.

Also fixed a stale doc bug: `Docs/Controls.md` had `\` and `/` swapped (code: `\`=route `VK_OEM_5`,
`/`=describe `VK_OEM_2`).

**Shipped: built clean + deployed, UNCOMMITTED.** PENDING one runtime confirmation pass:
1. **PRE-SHIP CHECK** — `p` during **Lock-On (`2`/R2) with no command menu**: routes ⇒ Lock-On drives
   `DAT_0209be80+0x9FD8` (≥0.98, record in `GameArchitecture.md`); "No target" ⇒ `p` is scoped to command
   target-selection. The `NAV-ROUTE` log reports gate/handle/cache-age + drain outcome either way.
2. **Nav-safe in battle** — the same log's drain line proves whether the field stays nav-safe in battle-state
   mode. If a persistent "not nav-safe" appears, relax the one battle-cleared gate bit in `IsFieldNavSafe`
   (contingency; not expected — `FIELD_ACTIVE2 0x1F69300` is the "field/battle-active" flag).
3. Item 2 snap around a real off-mesh target; Item 3 focus-persistence across a rescan + scanner non-regression.

## Session 39 — 2026-07-14 — [pathfinder] Fix `p` "No target" (havePos) + wire the never-populated `Category::Exit`

**KEYWORDS:** p No target havePos ReadSceneObjectPos null +0xB8 battle target actor+0xE0 fallback ACTOR_POS
GameArchitecture:728 ResolveActorPos NonZero PosDiag freshness 300->1000ms GetLockedTarget cache age log
Category::Exit never populated map-jump exit table getmapjumpposbyindex FUN_00264b90 FUN_0020e600 reloc
_DAT_01f83530 0x1E63530 TBL_EXIT_OFF 0x54 MAPJUMP_RELOC_BASE EnumerateExits map_query ScanExitsLocked fixed
entity RefreshPositionsLocked skip sanity gate 2000m EXIT_COUNT_MAX ' dump NAV-DIAG exit-table confirm

Two Session-38 runtime bugs, diagnosed from the live log + already-documented RE (NOT fresh RE):

- **Bug 1 — `p` always "No target" (FIXED).** The log proved the target reader announced targets
  (`[TARGET] "Imperial Swordsman, HP 100 percent"`) yet every `p` press hit an empty cache. Root cause:
  `battle_target_reader`'s cache write is gated on `havePos`, and `havePos` = `ReadSceneObjectPos(target
  sceneObj)` which returns false when the battle target's `sceneObj+0xB8` transform node is null during
  attack-menu selection (the same node worked for combatants in *real-time* combat — state-dependent).
  **Fix:** `NameForBtlChr` now falls back to the actor's own cached world position `actor+0xE0/E4/E8`
  (the documented field-actor pos, `GameArchitecture.md:728`) when the scene node fails or reads (0,0,0);
  `havePos` set iff a **non-zero** position came from either source. Freshness window 300→1000 ms (hedge
  vs. a non-per-frame nameplate render). Baked `[TARGET]` log records both sources + cache age at each
  press — confirms which source populates the cache in a battle. If BOTH are 0 during selection, escalate
  to gating on the live `DAT_0209be80+0x10F78` (noted, not built).
- **Bug 2 — no routable fortress objects: wired the never-populated `Category::Exit`.** This was a
  **documented, never-completed follow-up**, not new RE: the enum slot existed, the map-jump API was named
  2026-05-07 (`GameArchitecture.md:194-210`), and `plan.md` marked "exits" `[x]` prematurely — nothing
  ever populated it. FFXII inter-area exits are walk-into map-jump zones invisible to the
  `FLAG_TALK|FLAG_ACTION|gimmick` scanner filter (category-0 triggers with null `+0xB8`, or not scene
  objects); they live in the per-map exit array behind `getmapjumpposbyindex` (`FUN_00264b90`).
  **New `MapQuery::EnumerateExits`** reads that array per handle-table container
  (`exitBase = containerBase + *(u32)(containerBase+0x54) + reloc`, `reloc=*(u32)@0x1E63530`; count at
  `[0]`; `x/y/z/angle` at word `[i*8+1..4]`, 0x20 stride) behind a **strict sanity gate** (count 1..64,
  finite, non-zero, ≤2000 m). `entity_list::ScanExitsLocked` appends each as a **fixed-position**
  `Category::Exit` entity (`sceneObj=nullptr`, synthetic stable `nameIdx`, label "Exit");
  `RefreshPositionsLocked` skips the scene-node re-read for `fixed` entities so they aren't erased. Exits
  are now navigable via `[`/`]`, describable via `/`, routable via `\`. The `'` diagnostic dumps the raw
  exit table (rel-offset/reloc/base/count/records + verdict) — the ≥0.98 confirmation.

Also fixed `Docs/Controls.md` (already done S38) and reconciled `plan.md` exits checkbox.

**Shipped: built clean + deployed, UNCOMMITTED.** PENDING runtime confirmation:
1. **Bug 1** — `p` in a battle with a target selected: should route (`[TARGET]` log shows `src=1` scene or
   `src=2` actor+0xE0, `havePos=1`); if still "No target", the log's `sceneOk/actorOk` reveal why.
2. **Bug 2** — `'` in the fortress: the `[NAV-DIAG]` exit-table dump confirms the offset recipe (sane
   `count` + positions). If exits don't appear, the dump shows which term (rel-offset/reloc/stride) is off
   → adjust; if right, canonicalize the offsets in `GameArchitecture.md` and drop the ~0.85 caveat.
3. Non-regression of the scanner (NPCs/gimmicks/combatants; fixed exits survive rescans).

## Session 40 — 2026-07-14 — [pathfinder] Fix `p` works-once (live-gate), exits-0 (mapData deref), missing objects (named widening)

**KEYWORDS:** p works once No target frozen cache event-driven nameplate FUN_002bfd20 not per-frame live-gate
DAT_0209be80 OFF_GATE 0x10F78 OFF_TARGETID 0x9FD8 GetLockedTarget re-resolve bc NameForBtlChr exits 0
EnumerateExits missing dereference mapData=*(u64*)(containerBase+0) TBL_GUARD_OFF slot 0 FUN_00264b90
FUN_0020e600 reloc ~0 tag>2 tableOff +0x54 dest +0x84 stride 0x20 named-object widening ResolveObjectName
nameIdx +0xf8 fieldsign categories 1-4 OnFieldFrame per-area exit dump logRaw auto-diagnostic

Three tester regressions from Session 39, root-caused (Bug 3 from the live log; Bugs 1&2 from two agreeing
decompile re-traces):

- **`p` routed ONCE then "No target" (FIXED).** Log proof: `GetLockedTarget` succeeded only while cache
  age <1000 ms, then `tickMs` stayed FROZEN between target changes → the nameplate render that refreshes
  the cache is **event-driven (redraw on state change), not per-frame**, so any age window ages out. **Fix:**
  gate `p` on the **LIVE** `DAT_0209be80` selection state read at press time — `gate=PtrAt(P,0x10F78)!=null`,
  `liveHandle=*(u32)(P+0x9FD8)` (same reads `HookedNameplate` does; SEH-guarded on the input thread, like
  `\`/entity_list). Cache now stores the target `bc` pointer; `p` requires `gate && liveHandle==cachedHandle
  && bc`, then **re-resolves a FRESH position from `bc`** via `NameForBtlChr` (handles a moving target),
  falling back to cached pos. Removed the `maxAgeMs` param. `[TARGET] GetLockedTarget: gate=.. liveHandle=..
  match=..` logs each press. (Also directly answers the PRE-SHIP CHECK: gate set during Lock-On?)
- **Exits STILL 0 (FIXED).** `EnumerateExits` was missing one dereference. `FUN_00264b90` reads container
  **slot 0** and dereferences it first: `mapData = *(u64*)(containerBase+0)` (`TBL_GUARD_OFF`); precondition
  `*(u16)(mapData)>2`; jump table off at `mapData+0x54` (dest `+0x84`); `exitBase = mapData + tableOff +
  reloc` (reloc `_DAT_01f83530`@0x1E63530 ≈0); `count=*(u32)exitBase`; x/y/z/angle at word `[i*8+1..4]`
  (0x20 stride). My code used `containerBase` directly + iterated all 5 → garbage → 0 exits everywhere.
  Now slot-0 + deref + tag/tableOff guards, kept the sanity gate.
- **Only enemies to navigate to (FIXED).** The scanner already sees a superset of the game's interaction
  scanner (`FUN_0025b820`, two flag passes only), but dropped **named** objects (gates/doors/field-signs/
  NPCs, scene categories 1-4) whose talk/action flag isn't armed. **Fix:** `RescanLocked` also lists an
  object with a resolvable name — `ResolveObjectName` (npcdic `nameIdx>0`, or custom field-sign string at
  `+0xf8` for `nameIdx<0`), skipping the ambiguous `nameIdx==0`; still gated by `ReadSceneObjectPos`
  (excludes cat-0 null-node triggers).
- **Auto-diagnostic:** `OnFieldFrame` now fires a one-shot `EnumerateExits(logRaw=true)` on each
  active-container-mask change (area load), so the raw exit-table dump is captured passively (no `'` needed).

**Shipped: built clean + deployed, UNCOMMITTED.** PENDING confirmation: (1) `p` pressed repeatedly on a held
target keeps routing; (2) the auto `[NAV-DIAG] exit-table slot0` dump shows `mapData≠0 tag>2` + sane count/
positions and `]` cycles exits; (3) `rescan: N` rises and `]` cycles named gates/doors/NPCs, not just enemies.

## Session 41 — 2026-07-14 — [pathfinder] Fix S40 categorization regression: characters out of the Interactables bucket

**KEYWORDS:** category regression named-object widening NPCs Interactables Object defeated enemies scene
category byte sceneObj+0x03 SCENEOBJ_TYPE_BYTE isCharacter 5-7 char component ClassifyByNameKey ScanCombatants
dedup AlreadyListed per-category count log rescan breakdown

**S40's named-object widening over-reached.** Tester: targeting + exits work, BUT everything except active
enemies (NPCs, DEFEATED enemies, party) got bucketed as **Interactables**. Root cause: the widening surfaced
**character-type scene objects** (cat 5-7, which carry a char component) via the handle-table pass, where
`ClassifyByNameKey`'s old "named person outside gimmick band => NPC" / else-Object heuristic mislabeled them,
and `AlreadyListed` dedup then blocked `ScanCombatantsLocked` from classifying them as ally/Enemy/dead.

**Fix:** read the scene CATEGORY byte (`sceneObj+0x03 & 0x1f`; classes **5-7 = character/actor**, 1-4 =
gates/doors/signs/props, 0 = null-node trigger). The name-widening now applies **only to NON-character**
objects (cat 1-4) — character objects are left to their proper handlers (talk-flag → NPC, or the combatant
pool scan → ally/Enemy, or dropped when dead). `ClassifyByNameKey(flags,nameIdx,isCharacter)`: FLAG_TALK or
`isCharacter` → NPC; non-character unflagged named → Object (fixes named GATES being called NPCs too). Net:
defeated enemies drop (combatant scan, kind==dead), non-talk NPCs/party classify via the combatant scan,
gates/doors/signs stay as Object. Added a per-category count to the `rescan:` log to confirm the breakdown.

**Shipped: built clean + deployed, UNCOMMITTED.** PENDING: `]` cycles NPCs as NPC (not Interactables),
defeated enemies gone, gates/doors as Object; `rescan:` breakdown line confirms. STILL OPEN (tester-flagged):
exits don't resolve to real area NAMES (deferred: dest table `+0x84` via `getmapdestposbyindex` + planmapname
PLMN reader) and need runtime validation that they're real exits (route to one, confirm the area transition).

## Session 42 — 2026-07-14 — [pathfinder] Exit destination NAMES via the dest-id table + planmapname

**KEYWORDS:** exit destination name planmapname PLMN FUN_00377870 area name by id FUN_00264870 FUN_00264920
FUN_002648f0 dest table mapData+0x8c destIdx jump record +0x1d word[5] areaId +0x0A ResolveAreaName
CallAreaNameById GameText::Decode ExitRec destName To area label 1314 Nalbina Dungeons

Exits now speak the **destination area name** (e.g. "Nalbina Dungeons") instead of generic "Exit". Traced the
game's own "▲ To <area>" path (`FUN_003f9720` sign renderer → `FUN_00377870`):
- **Dest area id** lives in a THIRD per-map table at **`mapData+0x8c`** (`FUN_00264870`), separate from the
  jump `+0x54` and dest-pos `+0x84` tables. It's indexed NOT by the exit index but by the **jump record's
  `+0x1d` byte** (`FUN_002648f0`): `destIdx = *(u8)(exitBase+4+i*0x20+0x1d)`; `destRec = destBase+4+
  destIdx*0x10`; **`areaId = *(u16)(destRec+0x0A)` = word[5]** (raw; `0xffff` = no name) (`FUN_00264920`).
  (`getmapdestposbyindex`/+0x84 gives the dest ARRIVAL POSITION, NOT an id — the community label was wrong.)
- **Name**: reuse the game's planmapname area accessor **`FUN_00377870(areaId)`** (RVA 0x257870; the same
  getter family as `FUN_003778b0`, the current-area name the mod already calls) → codec ptr → `GameText::
  Decode`. Sidesteps the PLMN header/bias uncertainty. Blob base is `DAT_02add0f8` (0x29BD0F8) if we ever
  need a pure read. Impl: `MapQuery::ResolveAreaName` (SEH game call) fills `ExitRec.destName`;
  `ScanExitsLocked` labels the exit with it (else "Exit"). Baked log: the `'`/auto exit dump now prints
  `destIdx areaId "name"` per exit.

**Shipped: built clean + deployed, UNCOMMITTED.** Recipe ~0.85 (two decompile traces + the sign-renderer
path) → ship + confirm. PENDING: `]` on an exit speaks the real area name; the `[NAV-DIAG] exit[i]` line shows
sane `areaId`+name; VALIDATE by routing to an exit and confirming it leads to that named area. If names are
wrong/mismatched, the log's `destIdx`/`areaId` pinpoint whether the `+0x1d` linkage or the `+0x8c` table is off.

## Session 43 — 2026-07-14 — [pathfinder] REAL exits (mapData+0x70) + minimap objective markers + Event category

**KEYWORDS:** real map exits mapData+0x70 field-sign array FUN_00264ae0 FUN_00264ac0 FUN_002649b0 FUN_002648f0
story-gate usable buf[0] areaId +0x54 ARRIVAL spawn table wrong all along FUN_00353490 place party naviicon
minimap markers DAT_02b45a80 _DAT_02b45a70 objective target crystal EnumerateMarkers EnumerateSpawnTriggers
Category::Event ScanMarkersLocked ScanSpawnTriggersLocked FUN_003f9720 radar FUN_003c34e0

**The exits I'd shipped (S39-42) were the WRONG table.** Tester confirmed they were event triggers / spawn
points. Two decompile traces found why: **`mapData+0x54` (getmapjumpposbyindex) is the party ARRIVAL/SPAWN
table** — where you LAND after a jump (proven: `FUN_00353490` calls it to place the party post-jump) — which
is exactly the entrance gates + dialogue/cutscene triggers seen. Live log confirmed: all had `areaId=0xffff`,
duplicate positions.

- **REAL exits = the field-sign array at `mapData+0x70`** (the curated list the game draws as radar blips /
  3D "→ \<area\>" arrows, `FUN_003f9720`/`FUN_003c34e0`). Read via the game's own getters: `count=
  FUN_00264ac0()` (0x144AC0), `obj=FUN_002649b0(0,i)` (0x1449B0, null=not shown / leader-visibility filtered),
  `FUN_002648f0(obj,buf)` (0x1448F0) → `buf[0]=usable` (STORY GATE — a disabled forward exit reads buf0=0,
  explaining "No simple way through the fortress"), `buf+4=areaId`. Record floats off `obj`: X@+0, Z@+8,
  enable@+0xc. Name via `FUN_00377870(areaId)`. `MapQuery::EnumerateExits` rewritten to this; `ExitRec.usable`.
- **Minimap / naviicon MARKERS** (`DAT_02b45a80`, count `_DAT_02b45a70`, stride 0x20: type@+0, subtype@+1,
  label@+4, X@+8, Z@+0xc) — the icons the radar shows for party/crystals/save/**target/objective**. New
  `MapQuery::EnumerateMarkers`; surfaced as `Category::Event` "Marker \<subtype\>" (v1 — subtype meaning ~0.6,
  log-confirmed; the objective is the "where to go" the tester needs).
- **Old +0x54 read renamed `EnumerateSpawnTriggers`** → `Category::Event` (the tester said these belong in an
  events category, not exits). New `Category::Event` enum + word.
- Auto per-area dump now logs exits(+0x70) + markers + spawn-triggers; `rescan:` breakdown adds `Event=`.

**Shipped: built clean + deployed, UNCOMMITTED.** Confidences: real-exit structure 0.9 (two agents agree),
markers 0.85 struct / 0.6 subtype → ship behind sanity gates + baked logs. PENDING: (1) `]` in Exit cycles the
REAL exits with names + only-shown; route to one and confirm the transition; `[NAV-DIAG] exit[i]` shows
`enable/usable/areaId/name/XZ`. (2) `]` reaches the objective marker (route to it → the way forward); the
`[NAV-DIAG] marker` log pins which subtype = objective (then label properly + skip party next pass). (3) old
spawn/dialogue triggers now under **Event**, not Exit. Fast-follow: naviicon subtype→category + label-id→text;
optional "(locked)" hint on story-gated exits.

## Session 44 — 2026-07-14 — [pathfinder] Read-only guarantee + Controls fix + markers REMOVED + all-maps exit DB (offline)

**KEYWORDS:** read-only input controls game speed 1 2 3 not lock-on not target group mod does not modify
SendInput WriteProcessMemory const DirectInput buffer VirtualProtect vtable pure getters naviicon markers
DISPROVEN removed unit dots EnumerateMarkers MarkerRec NAVIICON MARK_ ScanMarkersLocked p-key DAT_0209be80
0x9FD8 battle_target_reader parse_mapdata.py VBF map_ctrl mpk mld +0x70 exits +0x8c dest +0x54 arrival
planmapname map_exits.csv all maps offline extraction Nalbina fortress post-boss

Four items, all from tester follow-ups after the Nalbina fortress test.

- **(A) Confirmed the mod is strictly READ-ONLY on input + game memory** (tester's speed jump made them ask).
  Audit: NO `SendInput`/`keybd_event`/`mouse_event`/`PostMessage(WM_KEY…)`, NO `WriteProcessMemory`, NO
  mem-write helper anywhere in the repo; the DirectInput hook passes the buffer to the tracker as
  `const unsigned char*` and never mutates it; the only `VirtualProtect` is the one-time vtable patch that
  installs the read-only `GetDeviceState` hook; all game calls are pure getters. Documented in `debug.md`
  (Solved) + a hard guardrail in the mod's `CLAUDE.md` (any feature that *drives* the game needs explicit
  permission — it's a category change).
- **(B) `Docs/Controls.md` corrected.** The menu-captured labels "`1`=Game Speed/Target Group, `2`=Lock On,
  `3`=Target Group" were **WRONG** — runtime shows **`1`/`2`/`3` all change Game Speed (1×/2×/4×)**, mirroring
  `F1`/`F2`/`F3`. There is no keyboard Lock-On / Target-Group binding. **The mod reserves NONE of `1`/`2`/`3`.**
  Also clarified the `p` key: it routes to the battle target the game is currently selecting
  (`DAT_0209be80 + 0x9FD8`, `battle_target_reader`) — it does NOT press or depend on a keyboard lock-on.
- **(C) Naviicon minimap "markers" DISPROVEN + REMOVED.** The Session-43 hypothesis (0.6 conf) that the
  `DAT_02b45a80` array carried objective/crystal/target icons was disproven by two decompile traces: it holds
  ONLY character/unit dots (party/ally/enemy/neutral), each a 1:1 duplicate of a live scene object the
  combatant + handle-table scans already list; no objective, no crystal, no treasure, and `marker[+0x04]` was
  the entity HANDLE not a label id. Deleted `EnumerateMarkers`/`MarkerRec` (map_query.h/.cpp),
  `ScanMarkersLocked` + its RescanLocked call + the marker auto-dump (entity_list.cpp), and `NAVIICON_*`/
  `MARK_*` (nav_rva.h). Nothing labelled "Marker" appears anymore. `Category::Event` (old +0x54 spawn/dialogue
  triggers) and real exits (+0x70) unchanged. Built clean + deployed.
- **(D) Offline all-maps exit DB — INVESTIGATED DEEPLY; the premise failed and uncovered a ROOT-CAUSE bug.**
  Wrote `..\FFXII-Decompile\tools\parse_mapdata.py` + ran it against the VBF (tester granted permission). It
  does NOT work, and 3 decompile passes + all-550-map empirical validation proved why: **the exit data is not
  in the `.mpk` files.** The map-control blob's `+0x70` (exit/field-sign) and `+0x8c` (dest-id) offsets are
  **ZERO in ALL 550 files**; only arrival `+0x54` is file-resident (396/550). Every simpler source ruled out
  (mapjumpgroup=story-flag ROM; jsondata/maps=render config; efb=sound; no global connectivity file; the
  per-event `.ebp` are cutscenes). The `.mpk` map-control blob is a cluster child (not `mpk[+0x10]`), runtime
  base = global ptr at `0x2098E10`.
- **ROOT CAUSE of "nothing detected in the fortress":** the live mod log shows `[NAV-DIAG]
  map-exits(+0x70): count=0` — **`blob+0x70` is empty at RUNTIME too**, not just on disk. Source accessor
  `FUN_00264ae0` reads `blob+0x70` (blob=`*(u64*)0x2098E10`, gate `DAT_02098e20 & 1`); getters
  `FUN_00264ac0`/`FUN_002649b0`(story-gate `record+0x1c` vs chapter `FUN_00377860`)/`FUN_002648f0`; renderer
  `FUN_003c34e0` (record floats X@+0/Z@+8/enable@+0xc, story@+0x1c, destIdx@+0x1d). **So Session 43's
  "`+0x70` = field-sign exits" (0.9 conf) is almost certainly WRONG — `blob+0x70` is empty everywhere, so the
  mod has detected NO exit on ANY map** (not story-gating). Another 0.9-conf RE that didn't hold.
- **DECISIVE NEXT STEP (authored):** `frida/probe_fieldsign_source.js` — run on a map with VISIBLE "→ Area"
  field signs (early field/town gate). It dumps `blob+0x70`/`+0x54`/`+0x8c` live + the gate + hooks the
  getters. If `blob+0x70` is nonzero there → it populates at runtime → reverse the writer/source & extract
  offline (or fix the mod hook). If still 0 → the `blob+0x70` path is dead in TZA; re-RE field signs from the
  renderer side. This is the pivotal experiment for the committed offline-DB effort.

**Shipped A/B/C: built clean + deployed, UNCOMMITTED.** Part D is an open, committed RE effort — offline exit
extraction is blocked at the source (data not in files) pending the probe result. Goal remains detection
COMPLETENESS, honestly reported.

## Session 45 — 2026-07-15 — [pathfinder/menus/battle] Exits restored, obtained-item text, party status keys

**KEYWORDS:** exits Category::Event retired Category::Exit mapData+0x54 map-jump point table
FUN_00353490 getmapjumpanglebyindex NOT party placement Session 43 demotion reverted +0x1d dead bytes
FUN_00264b90 sole reader +0x70 group table ABI bug Pfn_ExitCount no argument FUN_00264ac0 ecx
passthrough parse_mapdata wrong blob base obtained item FUN_0035e070 widget+0xC8 FUN_002b4090
codec-sprintf 0f31 slots _DAT_022ca430 timed toast e5f0 map screen FUN_003c02b0 FUN_002baf80 REMOVED
mini_face_c texture bundle FUN_0057c480 menu manager DAT_0209ac30+0x328 party status 4 5 6 btlAtel
FromPartySlot FUN_00320ab0 roster list 3 DAT_02ebf190+0x5a7e BtlChr 0x1c8 HP i32 MP i16 status OR
bc+0x64 bc+0x3c escape selector FUN_002ac5f0 0x31 3 params target clear on death KIND_DEAD

Five load-bearing conclusions in the session log / read-point spec were **wrong**, and the live mod log
already contained the disproof. Offline-only session (user directive: no Frida discovery probes) —
every finding came from the decompile plus the existing log.

- **(1) Exits restored.** Session 43 demoted `mapData+0x54` from Exit to "party ARRIVAL/SPAWN" on one
  claim: *"proven: FUN_00353490 places the party post-jump"*. **False** — `FUN_00353490` is abs
  `0x353490` = RVA `0x233490` = the mod's own `GETMAPJUMPANGLEBYINDEX`; it returns a jump's angle and
  places nothing. `+0x54` is the **map-jump point table** = the intra-map "Mapjump" exits the tester
  walks through (Inner Ward -> Upper Apartments). `Category::Event` **retired** (nothing produced it);
  `EnumerateSpawnTriggers` -> `EnumerateMapJumps`.
- **(2) The `+0x54` dest chain was fiction.** `FUN_00264b90` is the SOLE reader of that table in all
  33,105 functions and touches only x/y/z/angle — record bytes `+0x10..+0x1f` are read by **nothing**.
  The Session-42 `destIdx@+0x1d -> +0x8c` chain read dead bytes; the log proves it (`destIdx=0
  areaId=65535` on every record of every map). Deleted. `+0x1d` is real only on `+0x70` records.
- **(3) `+0x70` had NEVER been measured — an ABI bug.** `Pfn_ExitCount` was typed `int(*)()` and called
  with no argument, but `FUN_00264ac0` passes its incoming `ecx` through to `FUN_00264ae0(group)`,
  which uses it as an array index — so the count came from register junk and was 0 on **every** map.
  That is what "the `+0x70` path is dead" measured. And `parse_mapdata.py`'s corroborating "+0x8c = 0
  in all 550 .mpk" is not credible either: the live log shows `+0x8c` populated (`destCount=2`), so the
  parser reads the wrong blob base (Session 44's own note concedes the blob "is a cluster child, not
  `mpk[+0x10]`" — which is what the parser uses). Fixed the signature, enumerate all groups, and added
  a **direct-memory** group-table dump (`map-exits(+0x70) raw: ... groupCount=`) that needs no call and
  no ABI assumption — the first honest measurement. Destination names via the game's own chain,
  confirmed in its sign renderer `FUN_003f9720`: `rec+0x1d` -> `FUN_00264920(destIdx,buf)` -> `buf+4`
  s16 areaId -> `FUN_00377870` -> planmapname. Signs are matched to jump points within 3 m XZ.
- **(4) "Obtained \<item\>" SOLVED** (the priority). Proc **`FUN_0035e070`** (RVA `0x23E070`), gate
  `*(int*)param_2 == 1`, composed text at **`widget+0xC8`** (cap `0x4A0`) — `FUN_002b4090`
  (codec-sprintf) fills it from a template of three `0f 31 80 80 80` slots separated by `0x02`
  (item/item/gil, truncated in-place for 1-2 entries; verified first-hand). Widget 0x6e8 B from
  `FUN_0035df40`, handle `_DAT_022ca430`; reached from BOTH `FUN_0050faf0` (treasure) and
  `FUN_002a59c0` (field), so the read point holds either way. **Timed toast** (case 2 self-destructs)
  -> no pagination, one fire per popup.
- **(5) The e5f0 "dialogue" hooks were the WORLD MAP screen — REMOVED.** `page+0x138` is a MAP id;
  `out+8` is a MAP NAME (`DAT_02b457e0+0xe8` = map DB, `FUN_003be680(id,&w,&h)` -> zoom-to-fit,
  `ArtData/menu/localmap/`, sibling `FUN_002b7b80` tracks player pos). The spec's "decisive"
  `mini_face_c` pillar was a misread — a **texture-bundle name** for `FUN_0024a5a0`, never referenced
  by the `002baf80` family. Left wired, they'd have spoken a map name over real dialogue.
  `FUN_0057c480` demoted to menu-only (its case 1 registers into `DAT_0209ac30+0x328`; zero fires in a
  17-min log). Spec §A + §B(field) refuted — both self-tagged 0.98/0.90.
- **(6) Party status keys `4`/`5`/`6` — start of combat support** (`src/battle/` was empty). New
  `src/battle/party_status.{h,cpp}`: roster **list 3** `DAT_02ebf190+0x5a7e` -> BtlChr
  `base+8+idx*0x1c8`; HP **i32** `+0x48`/`+0x24`, MP **i16** `+0x4c`/`+0x28` (guarded by
  `+0x6c`/`+0x7c` sign bits), charId `+0x04`; `bcIdx >= 0x28` = empty -> **silent**. Derived from the
  seven `btlAtel*FromPartySlot` natives (`FUN_0050fd00`..`FUN_0050ff20`) found by fingerprint
  (consecutive fns through resolver `FUN_00320ab0`; address order == .dbg order 7/7, body==name 7/7).
  **Not called** — they are VM natives that abort on arg-stack underflow. List 3 (not the HUD-masked
  list 2) is why it works on the field and with the HUD hidden. Name from the actor pool
  (`actor+0x18`), NOT `FUN_0035d330` — that writes a shared static and these keys run on the input
  thread. `;` = **active target status** (facing readout dropped per user: orientation isn't needed).
  `nav_rva.h:247`'s `dbg_idx-5140` formula is **BROKEN** (implies 5142 vs 5140 on two known natives) —
  resolve natives by behaviour, never index arithmetic.
- **(7) `p` target no longer sticks to a dead enemy.** `GetLockedTarget` had **no liveness test** and
  **discarded** `NameForBtlChr`'s return, so a departed actor -> `havePos=false` -> `posOut` = the
  STALE cached pos -> route to the corpse. Now `ResolveLiveTarget` (shared by `p` and `;`) rejects
  empty-resolve / `KIND_DEAD(5)` / `curHP==0` and clears the cache; the cache is also cleared when the
  selection gate drops (previously only `Shutdown` ever cleared it).
- **(8) `GameText::Decode` escape fix.** Param counts are **selector-dependent** (from the game's own
  interpreter `FUN_002ac5f0`): `0x21`=0; `0x20/0x27/0x2f/0x32/0x34/0x35/0x37/0x3a/0x3c/0x3d/0x3e/0x56`=2;
  **`0x31`=3**. The old "skip every byte >= 0x80" rule ate digits (`0x85`-`0x8e`) and punctuation —
  visible in the live log as `"The  command can"`. Unknown selectors keep the old fallback.

**Shipped: built clean + deployed, UNCOMMITTED.** PENDING RUNTIME VALIDATION (all via baked logs, no
probe): (a) `]` in Exit cycles the ex-"Event" entries and routing to one transitions areas;
`rescan:` shows `Exit=N`, no `Event=`. (b) `map-exits(+0x70) raw: groupCount=` — non-zero => destination
NAMES are live; zero => `+0x70` genuinely empty on that map and the `mapjump(mapNo,jumpIndex)` script
route (`FUN_003145e0`, RVA `0x1f45e0`, writes gameState `+0x1044`/`+0x1048`) is next. (c) open a chest
-> `MSGTEXT item: "Obtained ..."`. (d) `4`/`5`/`6` HP/MP vs the on-screen gauges (MP is i16 — a wrong
width shows as absurd numbers); empty slot silent. (e) kill a target, press `p` -> "No target".
**STILL OPEN:** `meswin` field dialogue window + multi-page pagination — the telop (`FUN_002e16b0`) is a
whole-message setter, which is exactly why the Basch/Reks fortress screen reads all at once. Lead:
`FUN_003cb650` (RVA `0x2AB650`) case 1 vs case 0x20 — UNVERIFIED, do not ship. Also open:
`FUN_002aeb20` (RVA `0x18EB20`) selector->button-name mapping so button glyphs read.
**Note `MSGTEXT`/`NAV` are not flushed** (`logger.cpp` flushes only ERROR/INIT/HOOK_HEALTH) — exit the
game cleanly before reading the log.

- **(9) World MAP screen RE KEPT, not discarded** (user call). The e5f0 pair was removed as a
  *dialogue* read-point, but the RE is what a **"speak the map/area"** feature wants — recorded in
  `GameArchitecture.md` under *"World MAP screen (`page+0x138` = map id)"*: page proc `FUN_002baf80`
  (RVA `0x19AF80`), **map id = `*(s16)(page+0x138)`** (set at case 1 from `msg[2]`, consumed at case
  0xE — passed unmodified to the resolver, so page and resolver share an id space), resolver
  `FUN_003c02b0(mapId, out)` (RVA `0x2A02B0`) with `out+0` id echo / `out+2` attr / **`out+8` codec
  name ptr** (NULL when `entry+4 == -1`) / `out+0x10` flags bit15 = no-text; map DB
  `DAT_02b457e0+0xe8` (`[u32 count][8-byte entries]`, `entry+4` = s32 text id → `FUN_002f9920`).
  **Caveats recorded:** "out+8 is a map NAME" is 0.93 (inferred from the cluster, string never read
  live), and whether this id shares the **planmapname** id space is UNPROVEN — different blob
  (`DAT_02add0f8`), so don't cross-use ids without checking. For map-TRANSITION speech the simpler
  proven route is noted alongside: `FUN_003778b0` (current-area name, already used) + an observe hook
  on the map-jump executor `FUN_003145e0` (RVA `0x1f45e0`) → gameState `+0x1044`/`+0x1048`.
### Session 45 — TEST RESULTS (tester, same day)

**PASS — "obtained item" reads.** The new read point is confirmed live:
```
[MSGTEXT] diag: item popup proc FUN_0035e070 fired
[MSGTEXT] item: "You obtain a Potion"
```
`FUN_0035e070` (RVA `0x23E070`) + `widget+0xC8` is **CONFIRMED ≥0.98 at runtime**. Canonicalized.

**FAIL — multi-page dialogue still reads as one chunk.** Expected: not fixed this session, and the log
re-confirms the mechanism — the telop hands over speaker + every page in a single string:
```
[MSGTEXT] telop[slot=0]: "Basch
This is a save crystal Reks.You can save your progress by approaching a
save crystal and pressing Touching one of these crystals will also fully
restore your HP and MP.Though you will gradually recover MP as you
```
Still needs the consumer-side page state. Lead remains `FUN_003cb650` (RVA `0x2AB650`) case 1 vs
case `0x20` — UNVERIFIED.

**NEW BUG surfaced by that same line — the escape fallback still eats punctuation.** *"and pressing
Touching one of these crystals"* is missing both the **button glyph** AND the **`.`** after it. The
Session-45 decode fix is only half a fix: icon-family selectors (`0x40`-`0x6b` → `FUN_002aeb20`) have
an **unknown** param count, so they fall through to the legacy "consume every byte `>= 0x80`" run —
which then swallows the following `0xa8` (`.`). So the class of corruption I fixed for known selectors
still bites on icon inserts. **Fix = decode `FUN_002aeb20` (RVA `0x18EB20`)** for the selector→param
count AND the selector→button-name mapping; both fall out of the same function. Do NOT "fix" this by
having the fallback stop at any decodable byte — a genuine param byte in `0x85`-`0x8e` or the
punctuation range would then be emitted as literal text. Get the real count.

**FAIL — exits still have no destination name. But `+0x70` is now HONESTLY measured for the first
time, and the answer is "structurally valid, genuinely empty":**
```
[NAV-DIAG] map-exits(+0x70) raw: off=0x20650 table=2CCFFBD0 groupCount=5
[NAV-DIAG]   group[0] off=0x20600 sub=2CCFFB80 count=0
[NAV-DIAG]   group[1] off=0x20610 sub=2CCFFB90 count=0     ... groups 2-4 identical, all count=0
```
Read this carefully — it is **not** the old "count=0" (that was the ABI bug reading register junk):
- `blob+0x70` is **NON-ZERO** (`off=0x20650`) — so the Session-44 claim "`blob+0x70` is empty at
  runtime" is **FALSE**.
- `groupCount=5` is sane, and the five group offsets (`0x20600`..`0x20640`, spaced `0x10`) sit
  **exactly** in the `0x50` bytes immediately before the group table at `0x20650`. That is five
  `0x10`-byte sub-table headers packed head-to-tail — i.e. the layout derived from `FUN_00264ae0` /
  `FUN_002649b0` is **CORRECT**, and the reader now works.
- Every group's record count is **0**. The map has **no field signs**. There is no destination name
  to read here, because the game does not have one to draw either.

So `+0x70` is confirmed as the right structure and the right chain, with no data on this map (Reks
prologue / Nalbina — a railroaded corridor with no "→ area" arrows, which is consistent). **The
Session-43/44 conclusions were BOTH wrong in different ways**: 43 said `+0x70` was the exit source
(right table, but it never worked because of the count ABI); 44 said `+0x70` was dead (wrong — it is
populated and well-formed, just empty of records here).
- `rescan: 9 field objects (NPC=0 Enemy=4 Object=0 Exit=3 Save=0 Gate=0 Treasure=2)` — **Exit=3, no
  `Event=`**: the relabel shipped correctly, and Treasure=2 re-confirms the tester's report.

**DECISIVE NEXT STEP for exit destinations** (do NOT re-litigate `+0x70` — it is settled):
1. Re-run the `'` dump on a map that visibly draws "→ \<area\>" arrows (a town gate / open field, NOT
   the prologue). If a group's count > 0 there, names light up with zero code changes and `+0x70` is
   simply prologue-empty.
2. If it is empty there too, `+0x70` is authored-empty in TZA and the destination must come from the
   **script**: observe-hook the map-jump executor **`FUN_003145e0`** (RVA `0x1f45e0`), which receives
   `(mapNo, jumpIndex, flags)` and writes gameState `+0x1044`/`+0x1048`. That gives the true
   destination pair per transition; `FUN_00264f90(mapNo)` + the global map table `DAT_02099d88`
   (header `+0x04` → 8-byte records by mapNo; `rec+0x02` = area index into a `0x10`-stride table at
   `+0x08`; `rec+0x06` = group) then maps it to a name.

**FAIL — `4`/`5`/`6` DO NOT WORK (tester-confirmed).** Zero `[PARTY]` lines in the log, so
`PartyStatus::SpeakSlot` is **never reached** — the failure is in the **INPUT path**, not the BtlChr
reads (those never got a chance to run; they remain ≥0.98 offline and untested). **Removed from the
end-user `README.md`** per the tester; `Controls.md` marks them NOT WORKING with the suspect list.
Suspects in order: the `g_extraDown[6]` → `[9]` growth; the `DIK_4/5/6 = 0x05/0x06/0x07` scan codes;
the `'4'`/`'5'`/`'6'` VK tokens through `WM_NAVKEY` → `OnNavKey`. **Add a first-fire diagnostic in
`DInputEdge` before theorising** — the mod currently logs nothing on the input path, so there is no
way to tell "key never seen" from "key seen, dispatch dropped it". Possibly shares a root cause with
the already-recorded, still-undiagnosed "`[` failing in-game is a scan-code/read bug" note in
`Controls.md`.

**UNTESTED — `;` (target status).** Only the `[TARGET]` init line appears; the key was never
exercised. Given `4`/`5`/`6` fail on the input path and `;` was re-pointed at a new handler this
session, treat `;` as suspect until proven — though note `;` uses `g_extraDown[2]`, a **pre-existing**
slot that worked before, whereas `4`/`5`/`6` use the **newly added** `[6]`/`[7]`/`[8]`. That
difference is itself a clue: it points at the array growth rather than at `DInputEdge` generally.

- **(10) New CRITICAL rule in `CLAUDE.md`: CHECK `GameArchitecture.md` FIRST**, before any decompile
  research — grep it (plus `debug.md` Tried & Failed) for the function/global/offset/feature before
  chasing it. Motivated directly by this session: `FUN_00353490`'s real identity was already in the
  mod's own `nav_rva.h`, and the file itself asserted several wrong conclusions **as fact** that got
  built on repeatedly. The rule has two halves: (a) read before you dig, and if the file contradicts
  the runtime, **the runtime wins and you fix the file**; (b) when you disprove something there,
  **STRIKE it — don't just append a newer entry** — or the stale claim gets re-derived and re-shipped.

## Session 46 — 2026-07-20 — [pathfinder] Exit DESTINATIONS shipped — `__MJ_CTRL<N>` owns `+0x54[N+1]`

**KEYWORDS: exit destination name __MJ_CTRL routine table name pool hdr+0x18 hdr+0x4c hdr+0x54 mapjump
literal flags=0 field door door rule N+1 arrival slot 0 map_script.cpp ReadExitDests SafeReadBytes
getmapdestposbyindex struck nowjumpindex lastjumpindex 0x5C label not routine index gateway record**

**SHIPPED:** exits now speak their real destination — *"Exit, Nalbina Fortress: The Highhall, 15 steps
north"* — with the existing bearing/steps, on any map, resolved on the first frame from that map's own
data. User-confirmed working.

### The problem

The destination is stored on **nothing the mod can index**. `+0x54` records are x/y/z/angle then zero
bytes; `+0x70` field-signs have an empty destination slot on interior maps; `+0x8c` is keyed by a
field-sign byte, not a jump index; and **no engine getter maps door → destination** (all 33,105 functions
traced). A stale line in `GameArchitecture.md` claimed `getmapdestposbyindex` returned a destination map
id — it returns an arrival *position* — and that one wrong line cost most of the session. Struck.

### The mechanism

The map toolchain emits **one routine per map-jump door**, named **`__MJ_CTRL<NNN>`**, carrying its
destination as a `mapjump` literal (`4f<dest> 4f<ent> 4f<flags> 5d 8d 00`, `flags==0` = field door;
`0x0A` = teleport menu). Routine table at `hdr+0x18` (`[u32 count][0x30 records]`, `{nameOff@+0, codeOff@+8}`),
name pool at `hdr+0x4c`, **name = pool + nameOff**. Routine span = `codeOff` → next-highest `codeOff`.

**DOOR RULE: `__MJ_CTRL<N>` owns `+0x54` slot `N+1`.** Slot 0 is the default/cutscene arrival; a slot with
no controller is an arrival point, not a door. Walk-tested on five maps (274/275/279/280/282; routine
tables 22/25/31/24/37) and independently cross-checked by the arrival relation (*M jumps to D with entrance
E ⇒ D's door back to M is D's slot E*) — agrees on every measured pair.

Doors are matched to controllers **by position, not slot index**: `+0x54` repeats records and the
enumerator de-duplicates them, so a surviving entry's index can differ from the owning slot for the same
doorway. Position matching also fails safe — a mismatch drops the exit instead of mislabelling it.

This also fixed a real bug: Upper Apartments was announcing **three** exits when it has two. Slot 0
`(59.7, 39.5)` has no controller — an arrival point the mod was sending the player to walk to.

### Wrong turns (all struck in `debug.md`)

Claimed a "jump-index dispatcher" mid-session and had to retract it within minutes: `nowjumpindex` has
**zero** call sites, `lastjumpindex` returns a **map id**, and `0x5C`'s operand is a label — not a routine
index (values 221 vs 24 routines), which invalidated the call-graph it rested on. Also dead: the "gateway
record" (a denormalized mirror nothing reads), cross-map arrival-symmetry as a *blocker*, the routine
table's "2 entries" (word 0 is a **count**), and offline `.mpk` literal extraction (bytecode is
Phyre-encoded on disk; plain only in the loaded blob).

### Files

- **NEW** `src/navigation/map_script.{h,cpp}` — `MapScript::ReadExitDests`, generic script reader. New file
  because `map_query.cpp` is already 747 lines, past the 500 limit.
- `src/core/mem_read.h` — `SafeReadBytes` promoted here from a private static so the SEH guard lives once.
- `src/navigation/entity_list.cpp` — `ScanExitsLocked` labels `Exit, <region>: <sub-area>`, skips
  controller-less slots, logs once per map change (the detail dump was running every rescan → 28k-line log).
- `src/navigation/map_query.cpp` — diagnostic widened to `+0x0..+0x9000`; the old `+0x400` start had never
  captured the routine table or name pool, which is why this took so long to find.

### Lesson

The generality demand was the thing that cracked it. Repeatedly insisting this be **one global system**,
never per-map, is what pushed past single-map inference to the five-map test that exposed the wrong
`NNN = slot` reading and produced the correct `N+1` rule.

## Session 47 — 2026-07-20 — [input/release] Battle menu "won't open" = the game's own Left Ctrl Escape toggle

**KEYWORDS: left ctrl escape toggle battle menu won't open menus locked ping whoosh sound cue
Controls.md binding INPUT-DIAG modifiers reverted stuck ctrl STRUCK not a stuck key ReadMe exits
destination stale release V0.02-shotgun-build**

**RESOLVED — no code change. The mod was never involved.**

### The report and the wrong turn

The tester could not open the battle menu; the game behaved as though **Ctrl were held** on every
keypress. First move was a **stuck-Ctrl diagnostic** in `FeedDInputKeyboard` — log the modifier bits
from the game's own DIK buffer beside `GetAsyncKeyState(VK_CONTROL)`, on change, plus a `CTRL STUCK?`
warning. It was committed (`7d51917`) and then **reverted in this same session**, because:

**The tester identified the real cause from play:** pressing Left Ctrl emits a sound cue and locks the
menus; pressing it again emits a second cue and unlocks them. It is a **toggle**, not a stuck key.

**`Docs/Controls.md:43` already had the answer** — the game binds **Left Ctrl → "Escape"**, captured
from the in-game Controls menu on 2026-07-07. The mod reserves no modifier (every hotkey is standalone)
and its DirectInput hook takes the buffer as `const`, so a held Ctrl was never something the mod could
cause or explain. **One grep of `Controls.md` would have skipped the whole detour** — the same
read-the-docs-first failure as Session 45.

### STRIKE

- **"Stuck Ctrl / the game sees `Ctrl+<key>`"** — WRONG. It is the game's Escape binding on Left Ctrl,
  working as designed. Struck here and in `debug.md`.
- The diagnostic was also **actively bad to ship**: its change-detector includes Shift, and the game
  binds **Left Shift to Walk/Run**, so it would have written an `INPUT-DIAG modifiers:` line throughout
  ordinary movement — log spam in service of an answered question.

Confidence in the binding: **0.99** (captured from the game's own Controls menu, and it predicts the
observed lock/unlock toggle exactly). Confidence in what the sound *is* (a "zoom"?): **not claimed** —
the binding name is what is established; the audio cue is not identified and is not needed.

### Files

- `src/input/input_tracker.cpp` — diagnostic added then removed; **net zero change this session.**
- `README.md` — Left Ctrl now documented as a menu-locking toggle (it was a bare "escape (flee from
  battle)", true but useless for diagnosing this); stale exits Known Issue dropped, since Session 46
  shipped destination names.

### Lesson

A tester's plain description of what they hear beat a memory-instrumentation plan outright. When a
report smells like an input bug, **read `Controls.md` before writing a diagnostic** — the game's own
bindings are captured there precisely so this class of "bug" is a lookup, not an investigation.

## Session 48 — 2026-07-20 — [battle] Combat-system RE: messaging, committed target, damage, Neutral, log plan

**KEYWORDS: combat log battle_message.bin FUN_0028e110 game composes own sentences own battle log
committed target actor 0x710 0xBB8 FUN_00311cf0 browse P+0x9FD8 damage FUN_003283d0 FUN_003112f0
FUN_00300530 neutral faction FUN_00263be0 party_status missing deref DAT_02ebf190 pointer 0x5071901
Home End walk run shift conflict auto-speech KO 20 percent probe P-DUMP research only**

**RESEARCH ONLY — NOTHING IMPLEMENTED.** Deliverable: **`Docs/combat_system.md`** (~1100 lines).
Five parallel decompile research passes + one follow-up. Raw reports archived as
`..\FFXII-Decompile\notes\combat_re_2026_07_20_*.md` (6 files).

### The five things that changed what we believed

1. **FFXII composes its OWN localized combat sentences, and keeps its own scrollable battle log.**
   `battle_message.bin` = table `DAT_02ebf018` (RVA `0x2D9F018`), format strings at `hdr+0x14`, args
   bound by `FUN_005369c0` → codec-sprintf `FUN_00536410` → route `FUN_0035b990`. The game's log is a
   linked list at `*(u64*)(P + 0x9FF8) + 0x4098` (0x198-byte nodes, newest at TAIL, text at `node+0x00`).
   Finished string hookable at **`FUN_0028e110` (RVA `0x16E110`)**, arg0. Conf 0.93-0.97.
   ⇒ **STRIKES** `project_combat_log_design.md`'s "no textual combat strings exist in the game" and the
   whole 12-locale synthesized-template phrasebook built on it. The old grep failed because the text is
   codec-encoded master data, not ASCII. **We read the game's text; we synthesize nothing.** Combo
   aggregation dropped with it.
2. **The committed target is on the ACTOR, not the HUD** — the cause of the `;`/`p` bug. `P+0x9FD8` is
   written ONLY by two *browse* UIs (`FUN_0027b880` menu highlight, `FUN_003c9420` free cursor).
   Committed: queued `actor+0xBB8` (gate `*(u64*)actor & 0x4000`), active `actor+0x710` (gate
   `actor+0x714 != 0xFFFF`). Commit hook **`FUN_00311cf0` (RVA `0x1F1CF0`)**, player-only — gambits/AI
   bypass it. Must **post-verify** after calling the original (it no-ops on Stop/Sleep/Confuse).
   Conf 0.98-0.99.
3. **`4`/`5`/`6` silence = a missing dereference, NOT the input path.** `party_status.cpp:58` treats
   `DAT_02ebf190` (RVA `0x2D9F190`) as the struct; it is a POINTER (`= FUN_002ef640() + 0x4e68c`, magic
   `0x5071901` at `+0x00`). Three independent write sites, conf 0.99. `[PARTY]` also isn't in the
   logger's flush list, which destroyed the evidence. **`Controls.md`'s "g_extraDown array growth is the
   sharpest lead" is STRUCK** — the input path is correct end to end. Same class as the S40 `mapData`
   missing deref.
4. **The engine has a literal NEUTRAL faction** — `FUN_00263be0` (RVA `0x143BE0`): {1,2,7}→foe, 3→ally,
   **everything else→neutral** — and the target-select **Foes list already accepts neutrals**, so the
   category is faithful to the engine. Conf 0.99 on the partition, but **only 0.55 that the bucket is
   non-empty in real play — measure before shipping the label.** Also resolves an old puzzle: **for a
   player character the scene-kind nibble is 1 or 8 and is NEVER a faction**, which is why solo-Reks
   wasn't kind 3. `battle_target_reader`'s faction test is byte-for-byte correct.
5. **The community RVA CSV's address convention is not uniform.** Proven counter-example:
   `SetCurrentMpAndMistChargesMax` at **abs** `0x30F5A0`, body matches label exactly. But the seeder
   applied every value as absolute, so index labels prove nothing about a source's convention.
   DrummerIX's `ChainLevelPtr`/`ChainCountPtr`/`GilPtr`/`StealPtr*` are stale under **both** readings;
   his **AOBs are fine** (`DamageModAOB` → `FUN_00300530`, 0.97).

### Damage pipeline (all ≥0.98)

Applier chokepoint `FUN_003112f0` (`0x1F12F0`). HP/MP writers `FUN_00300530`/`FUN_00300ce0`
(`0x1E0530`/`0x1E0CE0`) are a **closed set** — the old "Frida-watch HP writes and walk callers" plan is
obsolete. Flying-number spawn **`FUN_003283d0` (`0x2083D0`)**, args `(work, signedDelta, isPositive,
isMP)` — that is all four requested categories directly; drain/absorb arrives as two events. Status+KO
`FUN_0030e360` (`0x1EE360`). **No critical-hit flag exists anywhere** (0.85 negative). **No element
survives to the apply site.** Reaction words (Miss/Block/Immune) are **pixels, not text** — use message
bus ids `0x17`/`0x1C`. Battle status names = `FUN_0035d330(0x1A, bit)`, table `DAT_02ebf118`.
Two RVA arithmetic errors in the source reports were caught and corrected (`DAT_02ebb560` →
`0x2D9B560`, `DAT_02ebf018` → `0x2D9F018`).

### User decisions

- Combat-log keys **`,` / `.` / `Home` / `End`**. `Shift` rejected: the game binds Left Shift to Toggle
  Walk/Run and the mod cannot swallow keys (`const` DirectInput buffer), so a chord would silently flip
  walk/run every press. Capacity 100, continuous FIFO, **no modal overlay and no pause** — which
  supersedes the F4/Esc/50-entry/`WH_KEYBOARD_LL` design in `CLAUDE.md` and `project_combat_log_design`.
- **Critical-event auto-speech KEPT** — party KO + crossing below 20 % HP, edge-triggered, party-side
  only. No new hooks (both are already needed by the log).

### Files

- `Docs/combat_system.md` — **NEW**, the deliverable.
- `Docs/GameArchitecture.md` — "Damage / Heal / Status Event Funnel" superseded-box + two strikes.
- `Docs/Controls.md` — the `4`/`5`/`6` diagnosis block replaced with the real root cause.
- `..\FFXII-Decompile\notes\combat_re_2026_07_20_*.md` — 6 raw reports.
- No source files touched. No build, no deploy.


### Addendum — the message table decoded offline, and the "log" claim struck

**`battle_message.bin` was already in the VBF extract** (`extracted/ps2data/image/ff12/test_battle/us/
binaryfile/`). Parsed with a new `tools/parse_battle_message.py` → `notes/battle_message_table_us.txt`:
**exactly 102 messages, ids `0x00`-`0x65`, full text**. Header self-validates (count `0x66` at `+0x04`,
records `0x20`, `0x20 + 102*8 = 0x350` = the pool offset at `+0x14`). Conf 0.99. **Probe P-DUMP is
cancelled — it was answered without touching the game.** Also confirms: **no Miss/Block/Critical string
exists anywhere in the table**, and §4.1's emitter-inferred id map was largely wrong (`0x17` = HP *and
MP* restored; `0x18`-`0x1B` = statuses *cured/faded*, not inflicted; `0x1C` = regains consciousness;
`0x4F`-`0x62` = field on/off pairs, not stat up/down).

**STRUCK: "FFXII keeps its own scrollable battle log."** `mgr+0x4098` is the **PENDING queue**, not a
log — 40 inline nodes at `mgr+0xD8` stride `0x198`, siblings `+0x40A0` displayed / `+0x40A8` free
(`40 * 0x198 + 0xD8 = 0x4098` — the arithmetic proves the layout). It is a **two-line transient ticker**
(`FUN_002bf230`, state machine `FUN_002beac0`); entries leave the moment they display and
`FUN_002be8d0` **drops messages outright** under load. **No viewer, no menu option, no Config setting**
(0.98, earned: complete 11-function xref set on the manager offsets + full decode of `menu00..04`,
`menu_command`, `menu_message`, `help_menu`, `word` — zero log/history labels; Config label set
`10064`-`10107`/`10259`-`10285` has nothing about messages). ⇒ **Hook the message, never walk the list.
The mod's `,`/`.` log is genuinely new functionality.** Hook `FUN_0028e110` (RVA `0x16E110`) promoted to
**0.98** — exactly one caller. Style byte solved: low 5 bits = dwell class (rate x {12,15,20,25,30}
frames), bit `0x20` = bypass the 24-unit distance cull.

**NEW BUG FOUND — `GameText::Decode` eats punctuation (affects ALL mod text, not just combat).**
Selector `0x29` returns -1 → the ">= 0x80 run" fallback swallows the next literal character
(`{0}'s HP...` → `{0}s HP...`; raw `0f 29 80 80 | ac | 4c`, `0xac` = apostrophe). Real rule from
`FUN_002ac5f0` case 0x29 → **`FUN_003ffbc0`** (RVA `0x2DFBC0`): `n = (byte after selector) & 7`;
`n==0` → 2 param bytes (a colour), `n>0` → `1+n`. Call convention confirmed (it loads a byte through the pointer, switches on it, and advances the
caller's cursor by the return value ⇒ the pointer is the SELECTOR, so return 3 = selector + 2 params),
which also resolves 13 more selectors as 2-param. Applying it cut unmapped bytes across all 102 strings
to **two**. Partially closes the S45 "icon family still broken" item. **Added to Phase 1.**

### Content strategy for the log — CONFIRMED with the user

**Read the game's own 102 messages as the primary content; cherry-pick which of them also interrupt in
realtime; synthesize our own entries ONLY where no game message exists.** Written up as
`combat_system.md` §9.1.1-§9.1.4.

**The decisive gap: there is NO "X deals N damage to Y" message anywhere in the 102** (verified against
the full decoded table). Per-hit damage and healing are numeric **sprites only**. So the single most
important stream in a combat log is Tier 2 (synthesized from `FUN_003283d0`, RVA `0x2083D0`), while
KO / restores / cures / steal / poach / loot / level-up / boss mechanics / party-wide fields all come
from the game verbatim. Other Tier-2 gaps: Miss/Block/Immune (**pixels** — needs probe P3, do not guess
the words), status **inflicted** (the table only has *cured*/*faded*), EXP/LP amounts, and the three
mod-only concepts (target confirmed, battle start/end, below-20% HP).

Realtime allow-list (default, tunable): action failed/cancelled `0x05`-`0x0C`, party KO `0x10`-`0x12`,
revive `0x1C`, Paling/Shield/White Wind `0x3E`-`0x43`, party-wide fields `0x4F`-`0x5C`, "Back attack!"
`0x61`, level-up `0x04`, loot/steal/poach `0x24`-`0x26`/`0x2D`-`0x34`/`0x37`-`0x38`/`0x64`-`0x65`, boss
mechanics `0x44`-`0x4E`. Log-only: action announces `0x0D`-`0x0F` and routine restores `0x13`-`0x1B`
(the spam tiers), and **the per-hit damage numbers** — that stream is exactly what made linear
narration unusable. Ship it as a `static const` id→policy table, not `if` statements.

**Phase 4 is now "log skeleton + Tier 1"** — one hook (`FUN_0028e110`), no formatter, real localized
combat text immediately. **Phase 5 is Tier 2, damage numbers first.**

### Attack lines — the game will NEVER say them (decisive for the log)

Read firsthand from **`FUN_00469af0`** (abs `0x469AF0` / RVA `0x349AF0`):

1. **No "X attacks Y" message exists**, and **no announce ever names a target**. The only three announce
   forms in all 102 are `0x0D` "{0} begins casting {1}.", `0x0E` "{0} readies {1}.", `0x0F` "{0} uses
   {1}." — arg lists `[attacker, action]`, no target slot. Conf 0.99.
2. **The announce fires only for GUESTS and FOES, never for your own party.** First statement is
   it takes the faction mask from `FUN_002f8e90()` and returns unless `0x0A` is set — `10` decimal = `0x0A` = guest `0x02` |
   foe `0x08`. A normal party character is bucket `0x01`, so it returns. Category map (conf 0.98):
   `actionRec+0x1e` 1 -> 0x0D, 2/7/9 -> 0x0E, 3 -> 0x0F, **anything else -> no message at all**.
   ⚠️ That the gate tests the ATTACKER's faction is **0.90** — Ghidra dropped the register arg
   (`FUN_002f8e90()` shows none), the classic hazard from `feedback_resolve_natives_by_behaviour`.
   **Probe P-ANN** added.

⇒ **The whole attack line is Tier 2.** We synthesize `"<attacker> <verb> <target>"`, mirroring the
game's own category→verb vocabulary so wording matches when both appear.

### Pairing the number to its action — ONE hook, no heuristics

**`FUN_003112f0` (RVA `0x1F12F0`) gives the complete line in a single call**: args
`(result, attackerBc, targetBc, actionId, flags)`, and the result struct already holds every number —
`+0x24` target HP delta, `+0x2a` target MP, `+0x20`/`+0x28` attacker side (drain), `+0x04` outcome
(9 = AI preview, discard), `+0x1c` valid. So one call = one `(action, attacker, target, number)` tuple =
one log entry:

```
Vaan attacks dire rat. 25
Vaan casts Cure on Penelo. 50
Vaan uses Ether on Vaan. 25
```

**No timing window, no correlation buffer, no dedup** — which matters, since pairing by timestamp would
have been a debounce in disguise (house rule). AoE fires once per target, so each of three Cure targets
gets its own numbered entry — **which is why the target belongs in the line** even though the user's
example omitted it; without it the three entries are identical. `FUN_003283d0` demoted to SECONDARY,
still needed for regen/poison/doom ticks (no action, no attacker, never reaches the applier).
**Phase 5's priority hook is now `FUN_003112f0`, not `FUN_003283d0`.** Probe **P-PAIR** added.

### CORRECTION (tester challenge) — message emitters gate PER FAMILY, and one is party-EXCLUSIVE

The tester pushed back on "the game never narrates party members", citing message `0x08`
("{0}'s target is too far from the party leader") — obviously about a party member. **They were right,
and the real picture is more useful than either framing.** The faction gate lives in the *announce*
emitter only; each family has its own emitter with its own gate (all three read firsthand, conf 0.98):

| emitter | abs / RVA | ids | gate |
|---|---|---|---|
| `FUN_004697c0` | `0x4697C0` / `0x3497C0` | `0x05`-`0x07` cancel/interrupt/out-of-range | `if (FUN_002f8e90(param_1) != 1) return;` = **PARTY ONLY** |
| `FUN_00469a50` | `0x469A50` / `0x349A50` | `0x08`, `0x0A` target-too-far | **no gate** — anyone |
| `FUN_00469af0` | `0x469AF0` / `0x349AF0` | `0x0D`-`0x0F` announce | `(FUN_002f8e90() & 0x0A)` = guest \| foe |

So the game DOES narrate party members — just never their action *announce*. Bonus: `FUN_004697c0`
calls `FUN_002f8e90(param_1)` with the arg **visible**, the same idiom as the argument-less call in
`FUN_00469af0`, which lifts "the gate tests the ATTACKER" from 0.90 to **0.95**.

**The attack line is still Tier 2 anyway**, for the simpler reason: no "attacks" message exists in the
102 and **no announce carries a target slot** (args are `[attacker, action]`).

**"Does the game dump finished text or templates?" — finished text, already established (0.95).**
`FUN_005369c0` binds args -> `FUN_00536410` codec-sprintfs into a 0x180 buffer -> `FUN_0035b990` routes;
names via `FUN_00536280(nameIdx, isPc)` incl. the player-entered name. `FUN_0028e110` arg0 is a fully
substituted sentence. **P-MSG verifies live and is the first probe to run.**

### Design changes from the tester

1. **Ticks are OUT of the combat log** (regen/poison/doom). They surface in the `4`/`5`/`6` vitals
   readout as concise state — `"Vaan, HP 412 of 690, poisoned"` — using the status word `ReadSlot`
   already reads but never speaks, with names from `FUN_0035d330(0x1A, bit)`.
   **Consequence: `FUN_003283d0` is no longer needed as a log source at all** — ticks were its only
   unique contribution over the applier. Drops us to ONE Tier-2 hook.
2. **Multi-target actions AGGREGATE into one entry**, not one per target:
   `"Ashe casts Cure on party. 403 average"` / `"Fran casts Firaga on all enemies. 2900 average"`.
   Key on `(attacker, actionId)` (calls may interleave), average the numbers, always say "average",
   and state a count when it is not the whole set. **Completion must be deterministic** (count down
   `actor+0x77C`), never a timer — a timer would be a debounce in disguise.
   Use **"party" / "all enemies"**, not "Group A", unless entry-groups turn out to carry a real label.
3. Phase 5 split: 5 = single-target line, **5b = AoE aggregation (blocked on the §9.1.3d research)**,
   5c = status-inflicted + EXP/LP.

### AoE aggregation mechanics — RESOLVED (and 2 more RVA slips caught)

**STRIKE — two RVAs in `combat_system.md` were wrong by `0x60000`** (inherited from the hostility pass,
never shipped): `FUN_0030ab40` relation test = RVA **`0x1EAB40`** (~~0x18AB40~~); `FUN_0030bd00` target
filter = RVA **`0x1EBD00`** (~~0x18BD00~~). Verified against `decompile_index.csv`. **Third arithmetic
slip in this document** — always add `0x120000` back and confirm the abs exists in the index.

- **Target vector** `actor+0x778` = 0x208-byte inline vector: `+0x04` i32 count (= `+0x77C`),
  `+0x08 + i*0x10` sceneObj, 32 slots (`8 + 32*0x10 = 0x208` = the memset size). Conf 0.99.
- ⚠️ **`+0x77C` is 1 until PHASE 9.** `FUN_0030f760` appends only the primary target; `FUN_00304850`
  phase 9 calls `FUN_0030d160` which clears and rebuilds with the full AoE set. Reading it earlier
  reports a false "single target".
- ⚠️ **Do NOT count down.** The count includes immune/missed (0.98, no `return` on any outcome branch),
  but a target killed by earlier splash yields N-1 calls, so a countdown hangs. **Flush on
  `FUN_003105d0` (RVA `0x1F05D0`)** — deterministic, once per action, no timer.
- ⚠️ **Attacker can be NULL** (`attacker == 0`, `actionId == 0xFFFF`) — those are **status ticks/DoT**
  from `FUN_0030e130`/`FUN_0030e360`/`FUN_00310db0`. **Filter them**, or ticks fold into the preceding
  cast. Doubles as the clean discriminator that keeps ticks out of the log entirely.
- ⚠️ **Calls DO interleave** — a synchronous burst (`FUN_00325260`, RVA `0x205260`) *and* per-target
  animation events (`FUN_002eb740` -> `FUN_00319f50` -> `FUN_003191b0`, RVA `0x1F91B0`) spread across
  frames. **Key on `(attackerBc, actionId)`, never adjacency.** Reflect/counter re-enter and can make
  `received > expected` — tolerate, do not assert.
- **"Group A" is OFF the table.** `actor+0x54` is written only by `FUN_00239170` (RVA `0x119170`,
  `setentrygroup`) and AI opcodes `0x4048`-`0x404F`; read only by `FUN_0030bd00` and `FUN_00301ad0`.
  **No name, text id, string or bestiary link exists anywhere** (0.97, grep-complete). Say **"4 enemies"
  / "party"**.
- **Target scope from the action row** (`FUN_003230e0`, RVA `0x2030E0`): `row+0x0C` bit0 self, bit1
  whole active party (no distance test), bit2 allies, bit3 foes, bit21 area-on-caster, bit24 cone,
  bit25 line; `row+0x06` = area radius, **0 = single target**; `row+0x2C` `0x40`/`0x80` = may target
  KO'd/petrified. ⚠️ **There is NO "all foes" bit** — "all enemies" is a big radius + relation filter,
  so we say **"N enemies"** and never assert totality (0.96).
- ⚠️ **Attack decodes against the wrong row** unless the id is substituted: the list is built with
  `FUN_00387090`'s id (RVA `0x267090`), which swaps in a per-character id when `actor+0x714 == 0x9F`,
  while `FUN_003112f0` gets the raw `0x714`.
- **"party" is verifiable:** `FUN_0030d160`'s party branch walks roster **list 1** (`W+0x5A5A`, charIds
  — NOT the mod's list 3 at `+0x5A7E`) and skips KO'd/petrified; guests excluded. Compare `+0x77C`
  against occupied active slots; say "on party" only on a match, else "on 2 allies" (0.98).

### Next session

**Start with probe P-MSG** — confirm `FUN_0028e110` (RVA `0x16E110`) fires once per displayed message
with the finished string in `arg0`. ~~P-DUMP~~ and ~~P-LOG~~ are **CANCELLED**: the message table was
decoded offline (102 entries) and there is no game-side log to read — see the addendum above.
Then the phased plan in `combat_system.md` §10 — **Phase 1 (the `4`/`5`/`6` deref fix + the `0x29`
codec fix) needs no probe at all.**

---

## Session 49 — 2026-07-20 — [battle] Combat implementation plan: decompile exhausted, 11 claims struck, probes authored

**KEYWORDS: combat implementation plan decompile exhausted strike FUN_00536410 message id RCX+4
codec escape table complete ffxii_codec.py 75 of 102 leader W+0x5AA4 FUN_00327150 P-E cancelled
DAT_0209a1f0 not leader row+0x34 action name not row+0x00 neutral foes group 0 only aggression
pointer deref actor+0x6A0 outcome 9 not preview result+0x00 0x20 tick actionId 0xFFFF
FUN_0035c8a0 game over FUN_00313b30 per action not battle actor+4 0x100000 in battle
probe_combat_state probe_combat_target probe_combat_messages probe_combat_damage
dump_combat_unknowns DAT_01e09ee8 guest slot W+0x5AD4 status names 32 decoded**

**PLANNING + RE ONLY — NO C++ WRITTEN.** Task: read `Docs/combat_system.md` and plan implementation,
exhausting the decompile offline before any confirmation probing. Deliverables: the plan
(`~/.claude/plans/find-the-document-labelled-hazy-summit.md`), `tools/ffxii_codec.py`, one Ghidra
script, four Frida probes, and **11 strikes applied to `combat_system.md`**.

### Method

Five parallel offline sweeps over the 33,128-function corpus **and over the shipped master data**,
which turned out to be extractable and parseable — that is what settled the name questions without
a probe. Every sub-0.98 claim in the S48 document was re-mined.

### The strikes (all applied inline in `combat_system.md`, indexed in its new header block)

1. **Tier-1 hook moves: `FUN_0028e110` → `FUN_00536410` (RVA `0x416410`).** The ticker never
   receives the message **id**, only a dwell class — so §9.1.4's id-keyed realtime policy table had
   nothing to key on. The codec-sprintf sees id (`RCX+4 & 0x7FFF`) *and* finished string (`RDX`) in
   one frame, and sits **upstream of the toast fork** where the ticker is not called at all. 0.97.
2. **"outcome `+0x04 == 9` is the AI preview" — REFUTED.** 9 is the default "run the formula" seed.
   Preview = `result+0x00 & 0x20`, emitted by `FUN_00308b90`, which never calls the applier ⇒ the
   planned filter is unnecessary. Read firsthand. 0.99.
3. **Tick filter is `actionId == 0xFFFF`, not `attacker == 0`** — `FUN_00310db0` makes two real
   calls with `attacker == 0`. 0.99.
4. **Leader: `DAT_0209a1f0[3]` REFUTED; probe P-E CANCELLED.** That array is 4 × `sceneObj*` in
   roster order. Real getter `*(u8*)(W + 0x5AA4)` → BtlChr → pool scan (`FUN_00327150`, read
   firsthand). Strictly better than the shipped `bc[5]==0` test, which matches every roster member.
5. **Ability name is `row+0x34`, not `row+0x00`** — the latter is a description id, and `Attack`
   plus every `Reserve` row share `4000` there. Verified against `action_data.bin`. 0.99.
6. **"Neutrals are legal attack targets" — BACKWARDS.** Emit gate `(g & ~2) == 0` ⇒ foes = group 0
   only; Neutrals appear in **neither** list. This was the entire justification for Phase 3.
7. **Aggression reads are wrong by a dereference** — `actor+0x6A0` is a POINTER. *Same bug class as
   the `4`/`5`/`6` bug this document was written to fix.* Does not ship.
8. **In-battle test**: `actor+0xEA4` is "who has an action aimed at me", not hostility (an
   out-of-combat Cure sets it). Use `*(u32*)(actor+4) & 0x100000` (`FUN_002fead0`).
9. **`FUN_00313b30` fires ~2× per ACTION**, not per battle.
10. **Game over: use `FUN_0035c8a0`** — no caller ever passes `0x17` to `FUN_0035c7b0`.
11. **`FUN_00312280` passes none of EXP/LP/gil/loot** — gil → `FUN_00469e80`, loot →
    `FUN_003180f0` (a ground pickup), EXP/LP → before/after diff.

### The codec fix is four times bigger than §1.7 said

Complete escape table derived; **`tools/ffxii_codec.py`** is now the reference implementation with a
regression: **zero structurally-unexplained bytes across all 102 messages**. Reimplementing the
mod's current `game_text.cpp` in Python and diffing shows it is **wrong on 75 of 102** — it eats the
character after every `0x29`, so sentence-final `.` and `!` vanish, not just apostrophes. Newly
explained: `0x2d` is the **numeric** substitution slot (the cause of `"{0} is now level !"`),
`0x10`–`0x1f` are 2-byte glyphs the mod renders as stray letters, and the icon family takes 1 byte.
`notes/battle_message_table_us.txt` regenerated (prior kept as `.pre-codec-fix`).

### Confirmed and raised

`+0x2c` all 19 bits (0.99) · no-critical-hit 0.85 → 0.95, **and FFXII's real Combo is readable at
`result+0x18 & 2`** · **all 32 status names decoded offline** (so "bit 5 = Confuse" 0.85 → 0.99) ·
guest slot reachable (`list3[3]`, `W+0x5AD4`) · roster gate is literally `slot < 9` · the `4`/`5`/`6`
fix verified as byte-offset arithmetic, firsthand.

### Artifacts (user-run — Claude never executes Ghidra or Frida)

- `..\FFXII-Decompile\ghidra\dump_combat_unknowns.java` — **G1** dumps `DAT_01e09ee8`
  (abs `0x1E09EE8`), which **decides Phase 3 offline**: side 4 + never-engage is the one authored
  Neutral generator, so if `4` is absent from that 4-entry table the category must not ship.
  **G2** dumps raw disassembly for the ~8 dropped-argument call sites. **G3** enumerates the
  `0x0f` jump table to settle selectors `0x30`/`0x5f`.
- `..\FFXII-Decompile\frida\probe_combat_{state,target,messages,damage}.js` — one per phase gate,
  attach-mode, read-only, console output O(unique).

### Next

Run `dump_combat_unknowns` first (it is offline and decides Phase 3), then `probe_combat_messages`
(one hook, unblocks the whole Phase-4 log). **No C++ until each phase's probe passes and explicit
permission to port is given.** Phase 3 is **deferred** — both its rationale (strike 6) and its
fallback (strike 7) are gone.

### Session 49 addendum — Ghidra `dump_combat_unknowns` run (results)

**KEYWORDS: DAT_01e09ee8 1 5 3 4 neutral authorable side 4 never engage arg3 P-NEUTRAL resolved
P-DWELL cancelled node+0x184 DAT_0209e610 mgr P+0x8FA0+0x1058 G3 inconclusive MSVC sparse switch
byte index table**

**G1 — PASSED.** `DAT_01e09ee8 = {1, 5, 3, 4}`, only two xrefs, both in `FUN_002396b0`. The script
native's four selectable side values are `arg0→1 foe · arg1→5 removed · arg2→3 ally · arg3→4
NEUTRAL`. **Neutral is a deliberately authorable faction**, so Phase 3's gate passes — but strike
F20 still stands, so ship it as **"will never engage"**, not "a legal attack target"
(`FUN_002396b0` sets the never-engage bit exactly when side==4, and `FUN_00238bf0` normalises side
4 → foe when that bit is clear ⇒ a neutral that can fight is impossible by construction). Still
open: whether any shipped map authors arg 3 — a frequency question `probe_combat_state.js` answers.

**G2 — PASSED, and it cancelled a probe.** Dwell passthrough traced end to end:
`FUN_0028e110` → `FUN_002bdb20(dwellClass,&A,&B)` gives `A = DAT_0209e610[dwellClass-1]`
(the `{12,15,20,25,30}` table) → passed as `R9D` to `FUN_002bdb00` → shuffled into `FUN_002be8d0`'s
5th stack arg → `MOVZX EAX, word ptr [RSP+0x70]; MOV word ptr [RBX+0x184],AX`. **`node+0x184` is
the per-style dwell, not the 150-frame cap. P-DWELL CANCELLED (0.98).** Also literally confirmed
`mgr = *(u64*)(P + 0x8FA0 + 0x1058)` (`MOV RCX,[RCX+0x1058]`) and §4.5's free-list → steal-oldest →
**drop** structure, `memset(node,0,0x198)`, `FUN_00254f30(node,text,0x180)`.

**G3 — INCONCLUSIVE; my script was at fault. Re-run DEFERRED by decision.** It printed `sel 0x20+i`
labels that were an *assumption, not measured data*, and the run disproved the assumption: 34
unique targets cannot cover selectors `0x20`–`0x70` one-to-one. It is MSVC's two-level sparse
switch (`movzx eax, byte [BYTETAB+reg]` → `jmp [JMPTAB+rax*8]`), so index `i` is the i-th *unique
target*. `dump_combat_unknowns.java` G3 has been rewritten to dump the setup instructions plus
`BYTETAB`.

**Decision (user, 2026-07-20): leave selectors `0x30`/`0x5f` documented at 0.97 and do NOT spend a
Ghidra pass on them; fold the re-run into a later session that needs Ghidra anyway.** Costs
nothing: both occur **zero times** across all 102 messages, `ffxii_codec.py` treats them as 0-param
(what a missing case implies), and the codec regression already reaches zero
structurally-unexplained bytes without them. They simply stay below the ship bar, unused.
First-run raw output preserved as `output/combat_unknowns_run1_G1G2.txt`.

### Session 49 addendum 2 — first live capture from `probe_combat_messages.js`

**KEYWORDS: FUN_00536410 confirmed live msgId RCX+4 isPc Dire RatC Rat Pelt word.bin variant
prefix 00 00 FUN_002b58b0 pool_string chunk 8 combatant names chunk 11 loot chunk 0 actions
chunk 5 status probe file handle append mode plain attack composes no message**

**F8 CONFIRMED LIVE.** `FUN_00536410` (RVA `0x416410`) fires, and one call carries both the id and
the finished sentence exactly as predicted:

```
[1] msg 0x2e isPc=1  "Vaan stole a Rat Pelt\nfrom Dire RatC!"
```

The offline table has `0x2E` as `{0} stole \nfrom {1}!` with a blank where the item goes — live,
the item is substituted in place. **The hook choice is validated; Phase 4's Tier 1 is unblocked.**

**Tier-2 name chain validated against live text.** Both substituted names resolve out of the shared
pool exactly as the S49 sweep predicted: `word.bin` **chunk 8 [409] = "Dire Rat"**, **chunk 11
[96] = "Rat Pelt"**. Chunk map confirmed by sampling: 0 actions · 1 equipment · 3 Quickenings ·
4 battle commands · 5 status (32) · 6 gambit labels · 8 combatant names (629) · 9 shop packages ·
11 key items/loot · 12 area names · 13 genus · 14 family.

**NEW, and required for the implementation: shared-pool strings carry a 2-byte `00 00` variant
prefix.** `00 00 | 22 4e 4b 3e 00` = `"Cure"`. This is `FUN_002b58b0`'s variant selector
(combat_system.md §1.4) and **without skipping it every pool string decodes to EMPTY** — which is
exactly what happened on the first scan here, making all 15 chunks look blank. Added to
`tools/ffxii_codec.py` as `variant_skip()` / `pool_string()`, both verified to reproduce the live
strings. NOTE `actor+0x18` needs **no** skip — the binder `FUN_0023a570` stores it already
variant-selected, which is why the shipped `party_status.cpp` name read works.

**"Dire RatC" is GAME text, not a decoder artifact.** The data holds `"Dire Rat"` clean; both the
article `"a "` and the instance letter `"C"` are appended at substitution time. Whether a space
belongs before the letter is unresolved and cosmetic — Tier 1 is read verbatim, so the mod speaks
whatever the game composed either way. Raw-hex logging now captures the bytes to settle it.

**Two probe defects found and fixed:**
1. *Instructions were wrong.* A plain physical attack composes **no message at all** (the announce
   is gated to guests/foes), so "fight normally" captured nothing on the first run. Steal is the
   right trigger and exercises the whole codec fix by itself.
2. *File logging destroyed its own output.* `new File(path,'w')` + Frida's auto-reload meant two
   handles both truncating to offset 0; the log ended up with two headers and a NUL gap, and the
   captured lines were lost (console had them). **Now opens in APPEND mode with a RUN START
   banner**, and writes raw hex for every message, not just the first per id.

**Still UNCONFIRMED live (one message is not enough):** the apostrophe fix needs `0x33`/`0x34`
(steal failures), and the numeric slot needs `0x31` (gil steal) or `0x04` (level-up). F9 remains
validated offline (zero unexplained bytes, 75/102 differing) but only PARTIALLY in-game.

### Session 49 addendum 3 — F9 CONFIRMED LIVE, and a new decoder bug found via the instance letters

**KEYWORDS: apostrophe confirmed live Vaan's steal failed 0x33 codec fix validated in game
control byte 0x06 is a SPACE FUN_002ac5f0 case 0x01 0x04 0x06 0x07 fall through Dire Rat B
instance letter sceneObj+0x100 +0x102 FUN_00263a10 FUN_002b58f0 targeting readout**

**F9 (the codec fix) is CONFIRMED LIVE on the exact failure case.** Captured:

```
[1] msg 0x33 isPc=1  "Vaan's steal failed!\nDire RatC has nothing to steal."
[2] msg 0x2e isPc=1  "Vaan stole a Rat Pelt\nfrom Dire RatB!"
```

`Vaan's` — **the apostrophe survives**, as do the sentence-final `.` and the `!`. That is precisely
what the shipped decoder mangles (it would give `Vaans steal failed` and eat the period). The full
chain fired 1:1 as predicted: bus → binder → sprintf → router (`routeSel=0`) → ticker, camera cull
passing. **Phase 4 Tier 1 and Phase 1's decoder change are both validated in-game.**

~~**Striking ratio worth remembering: 1275+ damage applications produced only TWO messages.**~~
⛔ **STRUCK immediately — the counter was MISLABELLED (tester correction).** It counted every call
to `FUN_003112f0`, not damage events; the tester reports only ~10-20 actual damage instances in
that session. **Never report a raw count of that function as "damage".**

What *does* stand: the game composed only **two** messages in a full play session, and a basic
attack composes none at all. So Tier 2 (§9.1.3) is still the core of the combat log rather than a
supplement — but that conclusion rests on the message count, which was measured, not on the
applier count, which was misread.

**This opened a real question — see the Tried & Failed entry.** `FUN_003112f0` is the designated
Tier-2 source, so a ~100:1 call-to-real-hit ratio decides both how load-bearing the tick filter is
and whether hooking it is acceptable under the no-per-frame-hooks rule.
`probe_combat_damage.js` now buckets every call and prints the ratio.

**NEW BUG FOUND — control byte `0x06` is a SPACE, and the mod drops it.** Chasing the user's
instance-letter request through the raw capture:

```
... 31 3a 4d | 06 | 21 | 0f 29 80 80 | 99
     R  a  t   ^^   B                   !
```

`FUN_002ac5f0:115-118` lists `case '\x01': case '\x04': case '\x06': case '\a':` as **one
fall-through group**, and `0x04` is the known space — so all four render identically. The decoder
mapped only `0x04`, producing `"Dire RatB"` where the game draws `"Dire Rat B"`. Confidence 0.99.
Fixed in `tools/ffxii_codec.py` (`SPACE_CONTROLS`); re-decoding the captured live buffer now gives
`"Vaan stole a Rat Pelt\nfrom Dire Rat B!"`, and the 102-message regression still passes at zero
structurally-unexplained bytes. **`0x05` deliberately NOT included** — it has its own case and the
`0x0a` handler scans for it as a structural delimiter, so it is not plain whitespace.

**Enemy instance letters — REQUESTED FEATURE, mechanism fully traced (user, 2026-07-20).**
To be implemented **alongside combat messaging**. The letter is *not* part of the name:
`FUN_00536280` is pure pool lookup and `word.bin` chunk 8 [409] is `"Dire Rat"` clean. It is
appended by `FUN_00536410:85-95` from the arg slot's `+1` byte, whose source is
`FUN_00263a10(sceneObj)`:

```c
if (*(i16*)(so + 0x102) < 0) return *(u16*)(so + 0x100);   return 0;
```

⇒ the mod reads it with two loads off a sceneObj it already holds. Neat cross-check:
`sceneObj+0x102` is the **same field `entity_list.cpp` already uses as the npcdic name key**, so
that field's sign bit is the "has an instance letter" flag. Still open: the value→letter mapping
(`FUN_002b58f0` indexes `value - 1` into a locale table at `reloc(cfg+0x14)`/`+0x18`,
`cfg = FUN_0039c4a0(0)`), so `probe_combat_target.js` now logs the raw `inst` value to pin it
empirically. ⚠️ `FUN_005369c0:87` calls `FUN_00263a10()` with the arg dropped by Ghidra — the guard
above it tests `actor+0x10`, so it is near-certainly the sceneObj, but confirm before shipping.

### Session 49 addendum 4 — damage probe results: Tier 2 is VIABLE, call rate settled

**KEYWORDS: 1400 applier calls 3 real hits 1397 ticks 99.8 percent actionId 0xFFFF filter
essential act=0x96 Attack chunk0[150] row+0x34 idx150 variant skip previews=0 confirmed
Dire Rat Vaan tgtHP -3 -40 Tier 2 viable**

**The call-rate question is ANSWERED.** Final counters: **1400 applier calls, 3 REAL HITS, 1397
ticks (`actionId == 0xFFFF`), 0 previews, 0 invalid.** That is **99.8% ticks** — the
`actionId == 0xFFFF` filter is not a nicety, it is the entire hook. `nullAttacker` tracked `ticks`
exactly (1397/1397) in this session, but the filter must still key on the **action id** (F2): the
loop in `FUN_00310db0` makes real calls with a null attacker, and keying on the attacker would drop
them.

**Tier 2 is VIABLE — the applier delivers the whole line in one call, both directions:**

```
[605]  act=0x96  Dire Rat -> Vaan     tgtHP=-3   rc=0 [normal hit] valid=1 f0=0x0 bits=0x40141
[1250] act=0x96  Vaan     -> Dire Rat tgtHP=-40  rc=0 [normal hit] valid=1 f0=0x0 bits=0x40141
[1371] act=0x96  Dire Rat -> Vaan     tgtHP=-3   rc=0 [normal hit] valid=1 f0=0x0 bits=0x40141
```

Attacker name, target name, action id and signed damage — all present, no correlation needed,
exactly as §9.1.3b promised. `bits=0x40141` has `0x100` set = target HP delta valid ✓.

**F19's open contradiction is RESOLVED: a plain swing reports `act=0x96`**, and
`word.bin` chunk 0 [150] = **"Attack"** (150 == 0x96, i.e. `row+0x34 == actionId` for this row).
So **`0x9F` is NOT the basic attack id** — whatever `FUN_00385f60:69` uses it for, it is not this.
Always read `row+0x34`; never assume it equals the id.

**F1 confirmed live: `previews=0` across 1400 calls** — previews never reach the applier, so the
planned outcome-9 filter really is unnecessary.

**Probe bug found and fixed:** action names printed as `"idx150"` because the probe's `poolString`
lacked the `00 00` variant skip (`FUN_002b58b0`) that had been added to `tools/ffxii_codec.py` but
never back-ported. Fixed; the next run should print `"Attack"`. `actor+0x18` needs no skip, which
is why the combatant names resolved correctly all along.

**Not observed this session:** any rc other than 0 (no parry/block/evade — the early-game Dire Rats
carry no shields), and no combo (`result+0x18 & 2` never set; the `0x8` seen is the knockback bit).
Neither blocks anything: §14.2 derives the rc→mechanic mapping from the processing code, so it does
not depend on observing one.

### Session 49 addendum 5 — target probe: commit CONFIRMED, instance letters PINNED, divergence test still pending

**KEYWORDS: COMMIT +0xBA0 +0xBB8 queuedFlag confirmed instance letter inst=1 2 3 A B C
sceneObj+0x100 FUN_002b58f0 value-1 actor+0x714 AI opcode 0x4000 band guard id < count
browse divergence not tested**

**Commit mechanism CONFIRMED (§3.2/§3.3).** One confirm produced exactly one commit, and the queue
read back correctly:

```
COMMIT#1 actor=Vaan argTgt=0x20000e argCmd=0x96 ret=0x0 -> queuedFlag=1 +0xBA0=0x96 +0xBB8=0x20000e
```

`+0xBA0` = the command, `+0xBB8` = the target handle, flag bit `0x4000` set. **Zero PHANTOM commits**
(no `cmdId == 0x113` from the idle timer / party-change path) during normal play.

**★ Instance letters PINNED.** Three simultaneous Dire Rats reported **`inst=1`, `inst=2`,
`inst=3`** from `sceneObj+0x100` (gated on the sign of `+0x102`). Combined with the earlier message
captures showing `"Dire RatB"` and `"Dire RatC"`, and with `FUN_002b58f0` indexing `value - 1`, the
mapping is **`letter = 'A' + (inst - 1)`** — 1→A, 2→B, 3→C. Confidence 0.97. The targeting readout
can therefore append the letter with two memory reads and no game call.

**⚠️ NEW — `actor+0x714` is NOT always an ability id.** Enemy activations reported ids in a
**`0x4000`+ band** (`0x4000, 0x4002, 0x4021, 0x4117, 0x4118, 0x4119, 0x4125`) with `argTgt = 0`,
alongside ordinary ability ids (`0x96` Attack). Those are AI/behaviour opcodes, not entries in the
ability table (which has 543 rows, ids `< 0x21F`). **Any ability-row lookup MUST be guarded with
`id < count`** or it indexes far out of the table. The probes already guard; the C++ must too.

**⛔ STILL NOT TESTED: the browse-vs-committed divergence, which is the whole point of Phase 2.**
The tester selected a single target without scrolling, so the cursor never moved — all three BROWSE
samples read `cursor=0x20000e`, identical to the committed target, which cannot discriminate. The
`*** CONFIRMED ***` divergence line never fired.

**To close it:** in a fight with 2+ enemies (three Dire Rats were present), open Attack and **scroll
the target cursor across at least two enemies WITHOUT confirming**. Pass = the BROWSE cursor value
changes on each scroll while `QUEUED`/`ACTIVE` stay unchanged, and the `*** CONFIRMED ***` line
appears.

### Session 49 addendum 6 — Phase 2 gate PASSED

**KEYWORDS: browse divergence confirmed cursor 0x20000f 0x200010 committed 0x20000e third enemy
P+0x9FD8 browse only proven ACTIVE 0x4021 AI opcode tgt 0 resolution order correction guard
id < count queued flag retains stale values**

**The Phase-2 gate is closed — the divergence is demonstrated live:**

```
*** CONFIRMED: cursor moved 0x20000f -> 0x200010 while the committed target did NOT change. ***
```

17 browse events alternating the cursor between two enemies; `+0xBB8` never moved off `0x20000e`.
**And `0x20000e` is a THIRD enemy**, so the shipped `;` readout does not merely lag the commitment —
it names a unit the character is not acting on, while the real target is one the cursor never
visited. `P+0x9FD8` is browse-only, 0.99. Same run: one commit per confirm, zero phantom commits.

**⛔ §3.4's resolution order is WRONG as written and is corrected in §14.3.** The leader's ACTIVE
slot held `act=0x4021, tgt=0x0` — an **AI/behaviour opcode**, not an ability id. "Active first,
else queued" would have returned a null target. Correct order: prefer ACTIVE **only** when
`act != 0xFFFF && act < abilityCount && tgt != 0`, else fall back to the queued pair, and test the
`0x4000` flag because `+0xBA0`/`+0xBB8` **retain stale values after it clears**.

**Guard every ability-row lookup with `id < count`** — `actor+0x714` is not exclusively an ability
id (a whole `0x4000`+ opcode band was observed).

### Session 49 addendum 7 — Phase 3 DEFERRED (no reachable neutral mob); all other gates passed

**KEYWORDS: phase 3 deferred neutral mob unreachable trait table opportunistic capture
probe_combat_state two jobs phase 1 gate BtlWork leader aggression aiData 0x20 never engages
FUN_00308040 gambit predicate not a trait**

**Decision (user, 2026-07-20): Phase 3 is DEFERRED — the tester cannot reach a neutral mob yet.**
It is the only phase blocked on data that cannot currently be obtained, so it does **not** gate the
rest. Phases 1, 2, 4 and 5 have all passed their gates.

**`probe_combat_state.js` has TWO jobs; only one needs a neutral:**
1. **Phase-1 gate** — `BtlWork` deref + magic `0x5071901`, the five roster lists, and the
   `W+0x5AA4` leader getter. Needs no special monster and is still wanted before Phase-1 C++.
2. **Phase-3 gate** — the per-creature trait table. Needs a neutral.

The probe now accumulates traits **keyed by creature name** across a whole session, so it will pick
up a neutral opportunistically during ordinary play; no dedicated expedition is required.

**Offline progress on the aggression axis while Phase 3 waits:**
- `aiData` is at `*(u64*)(actor + 0x6A0)` — a **POINTER**. `flags = *(u32*)aiData`,
  `radius = *(f32*)(aiData + 4)`. (F21; the doc's `*(u32*)(actor+0x6A0)` / `+0x6A4` were wrong.)
- **`aiData & 0x20` = "never engages" is CONFIRMED at 0.98** — `FUN_002396b0` sets exactly that bit
  when the authored side is 4, and `FUN_00308040` returns false on it immediately. Two independent
  sites.
- ⚠️ **`FUN_00308040` is NOT a trait getter.** It is a **gambit condition predicate**
  (`FUN_00301830:113` XORs its result into a condition) that blends the authored flag with *current*
  engagement via `FUN_0030b050` — which walks the actor's target list (`+0xBC4`/`+0xBC8`) testing
  each entry with `FUN_0030ab40` for hostility. So "attacks on sight" remains **inferred** and must
  not be spoken as such.

**Expectation to test when a neutral is reachable:** the tester describes monsters that *do not
attack unless attacked first*. Engine-NEUTRAL means **never engages at all**, so a creature that
retaliates is more likely a **foe with a passive aggression trait** than an engine-Neutral. If the
data lands that way, ship §6.5's two-axis wording (`"Enemy, Wolf, not yet aware"`) rather than a
third faction name.

### Session 49 addendum 8 — Phases 1 and 2 IMPLEMENTED in C++ (builds clean)

**KEYWORDS: phase 1 phase 2 implemented battle_state.cpp game_text.cpp escape table party_status
deref magic 0x5071901 Empty slot status names logger flush PARTY COMBAT committed target
ResolveTarget browsing queued instance letter DisplayNameForActor build clean**

User gave explicit permission to port (FRIDA-FIRST rule (b) satisfied). Both phases build with no
errors or warnings.

**New shared module `src/battle/battle_state.{h,cpp}`** — required by the centralization rule, since
`NameForBtlChr` was duplicated in `party_status.cpp` and `battle_target_reader.cpp` and both
re-declared actor-pool constants `nav_rva.h` already owned. It holds: `Work()` (the BtlWork deref +
magic), `BtlChrForSlot`, `LeaderBtlChr`/`LeaderActor` (the `W+0x5AA4` getter), `ActorForBtlChr` /
`ActorForHandle` / `BtlChrForActor`, `NameForActor`, `InstanceIndex` + `DisplayNameForActor`,
`FactionOf`, `CommittedTargetOf`, `AbilityName`, `StatusName(s)`. **Every function is pure reads** —
`FUN_0035d330` and `FUN_002f8e90` are reimplemented rather than called, because both write process
globals and the latter also calls the banned full-pool scan.

**Phase 1:**
- `game_text.cpp` — the complete escape table ported from `tools/ffxii_codec.py`; the high-bit
  fallback is **deleted**. Adds the three variable-length escapes, the 1-byte icon family, the
  `0x10`-`0x1f` two-byte glyph rule, terminators/sub-`0x10` lengths, and `0x01/0x04/0x06/0x07` as
  spaces. New `GameText::SkipVariantPrefix` for shared-pool strings.
- `party_status.cpp` — reads through `BattleState::BtlChrForSlot` (the fix), speaks **"Empty slot"**
  instead of nothing, and appends **status names** from the game's own table. `kMaxSlots` 4 -> 9.
- `logger.cpp` — `PARTY` and `COMBAT` added to the flush list.

**Phase 2:** `battle_target_reader.cpp`'s resolution is replaced by `ResolveTarget()`, which takes
the **committed** target from `BattleState::CommittedTargetOf(LeaderActor())` and falls back to the
browse cursor **only while the select UI is open, explicitly labelled "browsing"**. A queued (not
yet acting) commitment is labelled "queued". `p` and `;` now share it, so they always agree.
**The nameplate cache is gone from the resolution path** — it existed only because `FUN_003588b0`
was unreliable to call, and handle→actor is now a direct scan of `actor+0x08`. The two nameplate
hooks remain, demoted to driving the browse announcement. Names include the **instance letter**
("Dire Rat B").

**Not yet built:** Phases 4/5/6 (the combat log itself) — next increment. Phase 3 remains deferred.

### Session 49 addendum 9 — Phases 4/5/6 implemented; guest slot on `7`; empty slots now SILENT

**KEYWORDS: combat_log.cpp combat_events.cpp combat_format.cpp ring 100 seq cursor auto-follow
FUN_00536410 Tier 1 FUN_003112f0 Tier 2 monitor only actionId 0xFFFF first compare guest slot 7
silent empty KO below 20 percent latch build clean x64**

Implemented and building clean (x64, machine 0x8664).

**User decisions applied this pass:**
- **`7` reads the guest slot** (roster list 3 index 3).
- **Empty/unavailable slots are now SILENT**, reversing combat_system.md §8.2's "Empty slot"
  speech. §8.2's argument was that silence lets a broken mod masquerade as an empty slot — but that
  reasoning came from a session where the mod WAS broken, and announcing an empty guest slot on
  every press is noise. The diagnostic moved to the log, and `PARTY` is flushed so it survives a
  hard exit.

**`combat_log.{h,cpp}`** — 100-entry ring, continuous across battles, cursor keyed on a monotonic
`seq` (so an eviction that passes it is detectable rather than silently shifting the read
position), auto-follow while parked at the newest entry, and speech performed OUTSIDE the mutex
because Tolk can block and the game thread appends under it. Only the genuinely-empty log speaks an
explanation; boundaries re-speak the edge entry.

**`combat_events.{h,cpp}`** — the only file with combat RVAs. Two hooks:
- **Tier 1 `FUN_00536410`** (RVA `0x416410`): id read from `RCX+4 & 0x7FFF` BEFORE the call, the
  finished sentence decoded from `RDX` after. The buffer is raw codec bytes in the caller's frame
  and dies on return, so it is decoded and copied in the hook.
- **Tier 2 `FUN_003112f0`** (RVA `0x1F12F0`), **monitor-only per §14.1**: rejects on
  `actionId != 0xFFFF` as the FIRST test — one compare, ~99.8% of calls exit there with no
  allocation, lock, SEH read or string work — then the `+0x1c == 1` emission gate.

**`combat_format.{h,cpp}`** — owns every mod-emitted word. The realtime policy is a **data table
keyed by message id** (not `if` chains), which is only possible because the Tier-1 hook carries the
id. Outcome words come from the mechanic identified in the processing code (§14.2), never a sprite.

**Phase 6 folded into the applier hook — no extra hooks.** KO and the below-20% crossing are
computed from the pre-apply HP plus the delta, latched edge-triggered per BtlChr and re-armed on
recovery. ⚠️ **Known gap:** a KO caused purely by a status TICK is not caught, because ticks are
filtered out before this point. Accepted for now rather than adding a second hot hook; revisit if
it matters in play.

**Not built:** the 12-locale phrasebook (mod-emitted strings are English literals centralized in
`combat_format.cpp`), AoE aggregation (5b), status-inflicted / EXP / LP (5c), and mod-owned entries
(Phase 7). Phase 3 still deferred.

### Session 49 addendum 10 — KO gap CLOSED (it never existed), tick monitoring, readout order

**KEYWORDS: KO already narrated FUN_00469bb0 FUN_002fa390 party guest ally gate HP writer any cause
poison doom message 0x10 has fallen no mod KO line duplicate tick monitored never logged
result+0x24 one read status order name statuses HP MP**

**The KO gap I flagged does not exist.** `FUN_00300530`'s KO path calls `FUN_00469bb0` → message
`0x10` "{0} has fallen", gated by `FUN_002fa390` = `(FUN_002f8e90(bc) & 7) != 0` = party|guest|ally.
It hangs off the **HP writer**, so it fires for **any** cause — hit, poison tick, or doom — and the
message is cull-exempt, not dedup-eligible, and already in the Tier-1 realtime list.

⇒ **Removed the mod's own "is KO'd" line**: it would have been a duplicate announcement of text the
game supplies. `0x11`/`0x12` (void / stone) ride the same path.

**Tick handling split** in `HookedApply`: ticks never produce a log entry, but a single read of
`result+0x24` catches poison/doom draining a party member. Zero delta exits immediately.
The 20% warning remains mod-emitted because the game has no text for it.

**Readout order changed to name, STATUSES, HP, MP** — statuses must be reachable the instant the
line starts speaking. Builds clean.

---

## Session 50 — 2026-07-20 — [release] Release 0.1-shotgun-build: `;` battle-only, silence rule restored, four stale designs struck

**KEYWORDS:** release 0.1-shotgun-build ReadMe.txt four files Tolk nvdaControllerClient64 combat log
non-modal F4 struck 100 entries menus paused Empty slot SILENT guest slot 7 DiagnoseSlot semicolon
battle-only committed target browsing ResolveTarget SpeakTargetStatus GetLockedTarget phrasebook
does-not-exist release.md two-files four-files

Release prep. **No RE this session** — one small behavioural change, plus a documentation sweep that
found four separate places where the docs described designs that were never built or had been
reversed. The build was clean and deployed twice.

### `;` is now battle-only and silent otherwise (user instruction)

Reported: *"`;` should not work out of battle. It should only track what is selected as the active
combat target."*

Applied at **`SpeakTargetStatus`**, NOT in `ResolveTarget` — `ResolveTarget` is shared with `p`
(`GetLockedTarget` → routing), and routing to a browsed target is still wanted, so deleting the
browse fallback there would have silently changed `p`. The two callers now differ deliberately and
both headers say so.

- A **browsed** cursor is rejected. `ResolveTarget` only reports `browsing` when there is no
  commitment, so by construction it can never be the "active combat target". The `, browsing`
  speech suffix is gone; `, queued` stays.
- **Out of battle the key does nothing at all** — not even the old `"No target"`. Announcing
  anything *is* the "works out of battle" behaviour that was reported.
- **No new "am I in battle" flag was needed, and none was invented.** Out of combat there is no
  commitment anyway: `CommittedTargetOf` tests the queued flag (`+0xBA0`/`+0xBB8` retain stale
  values after it clears) and the active path requires a real action-table row. The silent path is
  reached naturally. This mattered — an in-battle predicate would have been new RE below the 0.98
  bar and would have needed a Frida probe, delaying the release for no gain.

### "Empty slot" — the code was right, four docs were wrong

Reported: *"`7` should **not** announce 'Empty slot'. It should be silent like the rest of the keys.
I gave you explicit instructions about this."*

**`party_status.cpp` already was silent** ("user decision, 2026-07-20"), and `battle_state.h:32`
already said *"Empty slots are SILENT by design"*. **No `"Empty slot"` string literal exists anywhere
in `src/`.** The mod has never spoken it. What existed was a stale doc claim in four places, all now
struck: `party_status.h`'s own header comment (which asserted the opposite of its `.cpp`),
`Docs/Controls.md`, `Docs/combat_system.md` §8.2, and `CLAUDE.md`'s phrasebook list.

**This is the session's real lesson: a header comment contradicted its implementation, and trusting
the comment put a false claim into the player-facing ReadMe.** Verify against the `.cpp`.

Also struck: `CLAUDE.md` cited `speech/phrasebook.cpp` as "sanctioning" the string. **That file does
not exist** — the phrasebook is planned, not built, so it cannot sanction anything. `CLAUDE.md` now
says so, and carries a new standing rule: never speak filler; diagnostics go to the log
(`BattleState::DiagnoseSlot`), never to speech. Memory: `feedback_never_speak_filler_be_silent`.

### Combat-log design: the F4 modal spec is struck everywhere

`CLAUDE.md`, `Docs/plan.md` and `Docs/RiskAudit.md` all still specified **F4 to open / Escape to
close / game pauses / 50 events / `WH_KEYBOARD_LL` hard input intercept**. That design was **never
built**. What ships is non-modal, 100 entries, `,` `.` Home End.

Confirmed and documented as a **requirement**: the log **works in menus and while the game is
paused**, because the only dispatch gate is `GameIsForeground()` (`input_tracker.cpp`, `DInputEdge`)
and the game keeps polling DirectInput while its own menus are open. A player who cannot keep up
pauses and reads back. Any change gating nav-key dispatch on menu state would break it.

Non-modal also deleted a whole risk class — `RiskAudit.md`'s "pause game during log read" and "modal
hard input intercept" entries are struck: the mod mutates no game state and swallows no input.

Memory `project_combat_log_design` rewritten. It was wrong on its **core premise** too: *"FFXII is
not narrative, no textual combat strings exist, so the mod must synthesize every line from a
12-locale template phrasebook"* — refuted in S48, the game composes its own sentences and Tier 1
reads them verbatim.

### Release procedure: two files → four

`CLAUDE.md`'s summary still said **"exactly two files"** and **"never bundle `Tolk.dll` /
`nvdaControllerClient64.dll`"** — the deploy rule leaking into the release rule, the exact error that
cost an earlier release its TTS DLLs. `Docs/release.md` itself said four in step 2 but **"the two
files"** in step 3. Both corrected, with the three concerns (deploy / build / release) kept separate.

### ReadMe

Rewritten for 0.1 and then **edited directly by the user**, who reformatted it and finalised the
wording. Known Issues dropped the two now-false claims ("no party status readout yet", "no combat log
yet"); the button-glyph entry was narrowed to what is still true (the icon has no readable name, but
the codec fix stopped it swallowing following punctuation and leaving stray letters); the exits entry
lost its stale "new in this build" (that shipped in V0.02).

**Docs updated:** `CLAUDE.md` (combat log, release, speech-filler rule), `Docs/release.md`,
`Docs/Controls.md`, `Docs/combat_system.md`, `Docs/plan.md`, `Docs/RiskAudit.md`, `README.md`.
