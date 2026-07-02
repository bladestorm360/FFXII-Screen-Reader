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

## Session — 2026-05-07 — Data feasibility survey (go/no-go before menu work)

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

## Session — 2026-05-07 — find_ebp_interpreter.java authored

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

## Session — 2026-05-11 — Title-screen text-detection: Phase A audit + Phase B/C authoring

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

## Session — 2026-05-11 — Menu architecture lockdown (in-game menu pipeline + cursor renderer + menu registry)

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

## Session — 2026-05-20 — Title-menu vocalization + universal menu reader (Frida probes + C++ scaffolding)

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

## Session — 2026-05-20b — Input correlation rewrite (probes + menu_reader gate)

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

## Session — 2026-05-20c — Frida directory cleanup + input-correlator try/catch fix

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

