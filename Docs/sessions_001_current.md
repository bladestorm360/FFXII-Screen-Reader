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
  TARGET — that's the boss "NE" bug. (`bVar5 & 4`, the skip-move-facing flag, is script-only, NOT a lock.)
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
