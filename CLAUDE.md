# FFXII-Screen-Reader — Accessibility Mod for Final Fantasy XII: The Zodiac Age

## CRITICAL RULES — MANDATORY, NO EXCEPTIONS

Every rule below is **non-negotiable**. Violating any of them is a blocking failure.

### Game install & build directory

- **NEVER** search, read, or modify the game installation directory
  (`D:\Games\steamlibrary\steamapps\common\FINAL FANTASY XII THE ZODIAC AGE\`) without
  **explicit user permission in the current conversation**. Only `build_and_deploy.bat`
  may copy files there.
- **Injection model**: standalone `dinput8.dll` proxy. Our DLL ships AS the game's
  `dinput8.dll`, forwarding all exports to `C:\Windows\System32\dinput8.dll` while
  doing our own deferred init for Tolk / hooks / hotkeys. There is **no Module
  Loader plugin host** — that approach was abandoned in Phase 0 after reading
  ffgriever's source and finding the External File Loader is a VBF redirector,
  not a plugin host.
- **Conflict with External File Loader**: both want the `dinput8.dll` slot. If a
  user has ELF installed, our install is incompatible without further work.
  Document in `README.md`. (If we revisit later: chain-load ELF from our DllMain.)
- **Exception — THE LOGS.** Reading `FFXII-Screen-Reader-Latest.log` **and the whole archive
  beside it** is always permitted (needed for debugging):
  `…\FINAL FANTASY XII THE ZODIAC AGE\x64\FFXII-Screen-Reader-Latest.log`
  `…\FINAL FANTASY XII THE ZODIAC AGE\x64\logs\FFXII-Screen-Reader-YYYY-MM-DD_HH-MM-SS.log`
  See **THE LOG CORPUS** under Auditing rules — that archive is the default evidence for every
  question, and it is NOT the same corpus as `Tester Logs\`.
- **NEVER MANUALLY EDIT `mod_config.ini`** — neither the repo copy
  (`config\mod_config.ini.template`) nor the deployed copy (`x64\modules\mod_config.ini`).
  The mod's RVA byte-validator + AOB self-healer is the **only** legitimate writer of
  RVA values. Manually patching a config value to "fix" a mismatch **masks a validator
  failure** and creates a silent ticking time bomb. **This is a CRITICAL PROJECT BLOCKER.**
  When you discover a config value is wrong, that means the byte-validator's expected
  signature for that RVA is wrong — fix the signature, rebuild, deploy, and let the mod
  write the correct RVA back.

### WHAT NEVER ENTERS THE REPO — game code, and anything that carries it (CRITICAL)

**This repo is meant to be publishable so other people can help with the mod.** That is the whole
reason the rule exists: everything in it must be OURS. Three categories never go in, in any file,
including documentation:

1. **RE source files** — decompiler output, disassembly listings, extracted game source. These live
   in the sibling `..\FFXII-Decompile\`, which is **not** this repo and never becomes part of it.
2. **Ghidra and Frida probes** — `.java`, `.js`, any script that attaches to or walks the game.
   Not because the script is secret, but because a probe **quotes the code it attaches to** in its
   patterns, offsets and comments. They live in `..\FFXII-Decompile\ghidra\` and `\frida\`.
3. **Snippets of actual game code, anywhere — including inside `Docs\`.** A pasted block of
   decompiler pseudocode is game code no matter which file it is pasted into. `Docs\` is committed,
   so a paste there is a publish.

**What IS ours and belongs here:** the RVAs, offsets, struct layouts, field meanings, function
identities and behavioural descriptions in `GameArchitecture.md`. Those are *findings about* the
binary that we derived and wrote — they are the point of the project, and they are not code.

**The line, in practice.** When you need to record what a function does, write what it DOES and cite
where, instead of pasting what it says:

```
NO   DAT_02064ac0 = DAT_02064ac0 + DAT_02064ac8 * _DAT_02064ac4;
YES  FUN_0022a770 (RVA 0x10A770) adds ac8*ac4 to the sim accumulator once per call, then drains
     it one whole tick at a time -- so game speed runs the loop body N times per rendered frame.
     Accumulator RVA 0x1F44AC0, multiplier 0x1F44AC4, base delta 0x1F44AC8.
```

Both carry the same fact. Only one of them is a transcript. **If a decompiler variable name
(`uVar7`, `iVar4`, `lVar6`, `undefined8`, `longlong` casts, `param_1`) survives into a `Docs\` file,
it is a paste and it should be rewritten.**

**One narrow carve-out, and only this one:** a lesson *about how Ghidra renders things* may name the
artifact, because the generated name IS the subject. "Ghidra shows outgoing stack arguments as
write-once caller locals, so a call site with four visible arguments may be passing six" is a lesson
about the tool — and it is the lesson that stopped a repeat of the S129 arity crash. Naming
`local_f8` there is not reproducing game logic. **Describe the artifact; do not paste the statement.**
`param_N` used in prose to mean "the Nth argument" is likewise a description, not a paste.

Where the line fell in the 2026-08-12 sweep: 15 pasted blocks and ~20 inline fragments were rewritten
into prose, tables and offset lists; 9 references survived, every one of them a tool-behaviour lesson
of the kind above.

This complements CLEAN-ROOM RE below: that rule keeps encumbered *source* out of our reasoning; this
one keeps the *game's* code out of our published artifact.

### Engine model & RE rules

- **CLEAN-ROOM RE.** Do **not** open, read, or reference the leaked PhyreEngine 8 source
  (publicly available on GitHub but legally encumbered). Work from the FFXII binary alone.
  If a struct shape is obvious from Ghidra decompile, name it ourselves from observed
  behavior. This rule exists because using leaked source for label inference would
  prevent the mod from being safely distributable.
- **RVA-FIRST. NO FISHING.** Hooks resolve via known RVAs sourced from DrummerIX's CE
  table seed (translated to RVAs once during dev). AOB pattern-scanning is permitted
  ONLY as: (a) **byte-validation self-healer** when an RVA's expected-bytes signature
  fails after a game update, and (b) **last-resort dev-time discovery** for an RVA we
  cannot otherwise obtain. Blind production-time pattern scans to "find" things are
  out of scope. The user has seen this approach fail on every prior attempt — do not
  attempt it.
- **NO UE4 ASSUMPTIONS.** PhyreEngine has no GUObjectArray, no FName, no ProcessEvent,
  no UFunction, no Blueprint widgets. Do not write or suggest code that assumes
  UE4-specific reflection. If you find yourself wishing for `findObject` or
  `GUObjectArray`, the answer is: hook the manager class via known RVA, walk its lists.
- **MSVC RTTI** is used as a labeling tool in Ghidra (Phase 0). Runtime use only after
  it's confirmed present and stable.
- **FRIDA IS FOR CONFIRMATION, NOT DISCOVERY.** All discovery of RVAs, offsets, struct
  layouts, handlers, objects, and text ids is done **offline in the decompile first**
  (`..\FFXII-Decompile\output\decompile\*.c` — 33,105 functions — plus Ghidra xref scripts
  the user runs). Mine the decompile until it has nothing left to answer, THEN write a Frida
  probe **only to confirm** the pre-derived values against the live process and to test the
  design end-to-end. Probes must NOT fish / pattern-scan / byte-diff to find unknowns. **Two
  exceptions permit Frida-side discovery:** (a) the fact genuinely cannot be obtained from the
  decompile — runtime-only state, e.g. the section→file binding that `FUN_0032ea90` zeroes at
  boot; or (b) a code/data path is **100%-CONFIRMED DEAD** — proven by showing that NO live
  loader/caller reaches it (zero live callers; no live loader references the resource). This bar
  must be **EARNED, never assumed**: a directory name (**`PS2Data\` is LIVE** — the exe hashes
  those paths to load real assets, e.g. it loads `PS2Data\...\battle_message.bin` by literal
  path), a **missing string literal** (files load by VBF filename-hash, so an absent literal
  proves nothing), and a **PS2-era symbol name** are NOT evidence of deadness. The DEFAULT is to
  find the LIVE build's code/data; "legacy/dead" is a conclusion that must be proven, never a
  reason to stop looking. (We wrongly tagged the live `PS2Data\` dialogue tree "legacy" on a
  directory name — do not repeat that.) This complements the RVA-FIRST / NO-FISHING rule above.
- **FRIDA-FIRST / PROTOTYPE-BEFORE-C++.** All new behavioral features MUST be prototyped and
  confirmed as Frida scripts first. The C++ mod is NOT modified until (a) the Frida
  prototype/confirmation works correctly and (b) the user gives **explicit permission** to
  port. This prevents compile-test-crash cycles. **Pick one: Frida OR C++, never both.**

### THE PRE-FLIGHT — three files, every task, before you plan or dig (CRITICAL)

**In this order, every time:**

1. **`MEMORY.md`** — loaded automatically. It is the index of *state*: what is open, what is
   play-confirmed, branch and session bookkeeping. Skim it; open the topic file it points at.
2. **`Docs/Lessons.md`** — the reasoning failures this project has already paid for. **GREP IT, do
   not read it end to end.** Read its ROUTING TABLE, pick the one or two `TAG:` values that match
   the task, and grep those. Lessons marked `⟲` have recurred and are checked first.
3. **`Docs/GameArchitecture.md`** and **`Docs/debug.md`** — the *facts*: RVAs, offsets, layouts, and
   what has been STRUCK. Grep for the function/global/offset/feature before deriving it.

**Why the split exists (2026-08-12):** `MEMORY.md` is loaded into every session, so anything living
there is paid for on every task whether or not it is relevant, and it had grown past 20 KB. Lessons
are *retrieved on demand*; state is *carried*. Keeping lessons in the index crowds out the state and
eventually truncates both. **So: when you learn something transferable, it goes in `Docs/Lessons.md`
with a new `L-NN` id — NOT as a new paragraph in `MEMORY.md`.** `MEMORY.md` gets at most a pointer.

Routing for new knowledge, so it lands once and is findable:
- transferable reasoning ("never trust X as evidence") → **`Docs/Lessons.md`**
- an FFXII fact (RVA, offset, struct, identity) → **`Docs/GameArchitecture.md`**
- a specific defect and its diagnosis → **`Docs/debug.md`**
- what happened this session → **`Docs/sessions_*_current.md`**
- durable *state* (open items, what is unconfirmed) → memory topic file + one index line

### CHECK `GameArchitecture.md` FIRST — before any decompile research (CRITICAL)

**Before starting ANY decompile / RE investigation, read `Docs/GameArchitecture.md`.** It is the
canonical registry of every RVA, offset, struct layout, and read-point this project has established
— and, just as importantly, of the ones that have been **STRUCK**. Grep it for the function, global,
offset, or feature you are about to chase.

This is not a suggestion. It is the first step of the task, every time. Also grep
`Docs/debug.md` (**Tried & Failed**) and the session logs.

**Why (hard-won, Session 45):** a session burned hours re-deriving `mapData+0x54`, the `+0x70` exit
array, and the message read-points — all of which were already in `GameArchitecture.md`. Worse, the
file *asserted several wrong conclusions as fact*, and they were built on repeatedly:
- "`FUN_00353490` places the party post-jump" — it is `getmapjumpanglebyindex`, and the mod's OWN
  `nav_rva.h` already had that RVA. One grep of the file would have caught it.
- "the `+0x70` array is empty on every map" — the mod was calling the count getter with no argument.
- "field-chest text reuses the `FUN_0057c480` surface" — self-tagged **0.90**, below the bar, wrong.

Two rules follow:
1. **Read before you dig.** If the answer is already recorded, use it. If it contradicts what you
   observe at runtime, **the runtime wins** — and you must fix the file.
2. **When you disprove something in there, STRIKE it, do not merely add a newer entry.** A stale
   claim left standing as fact will be re-derived and re-shipped. Every correction states what was
   wrong, what the evidence was, and what replaced it — and the same correction goes to
   `debug.md` (Tried & Failed) so it is greppable from the other direction.

### Decompile RE confidence bar — ≥0.98 or it does not ship (CRITICAL)

Every conclusion drawn from the decompile — a function's identity/role, an RVA, a struct
offset, a field's meaning, a class layout, or a "this code is dead" claim — MUST carry an
explicit confidence on a **0.00–1.00** scale. **Only conclusions at ≥0.98 may be acted on:**
built into the mod, ported to C++, canonicalized in `GameArchitecture.md`, or presented to the
user as fact.

Anything **below 0.98 is discarded** — it may NOT be shipped or built upon. The one way a
sub-0.98 hypothesis is promoted is a **Frida confirmation probe** against the live process that
raises it to ≥0.98; the probe is how uncertainty is resolved, never a guess. If it cannot be
confirmed, drop it and keep digging — silence beats a wrong hook.

**Why (hard-won):** across this project, nothing below ~0.9 confidence has ever worked, and
several **0.9-confidence** function / RVA / offset identifications turned out wrong — the
battle-command and targeting hooks were re-derived and re-shipped wrong three sessions running.
0.9 is not enough. The bar is 0.98, and confidence is always stated so it can be challenged.

This complements FRIDA-IS-FOR-CONFIRMATION and RVA-FIRST / NO-FISHING above: those say *how* to
reduce uncertainty; this says *how certain you must be before it counts*.

### TTS, dependencies, and bundling

**Tolk: three SEPARATE concerns. Do not collapse them into one rule** — the original
wording said "user-supplied, never bundled", which correctly described *deploy* but wrongly
leaked into the *release*, and cost a release its TTS DLLs. Corrected 2026-07-15:

1. **DEPLOY — `build_and_deploy.bat` deploys ONLY `dinput8.dll`. NEVER Tolk.**
   `Tolk.dll` and `nvdaControllerClient64.dll` in the **game directory** are the user's
   own; they manage that install themselves and the script must not touch, copy, or
   overwrite them. **This is what the rule was always about.** If a future contributor adds
   a Tolk copy step to `build_and_deploy.bat`, remove it. (The script is correct today: it
   copies `dinput8.dll` and prints a reminder.)
2. **BUILD — Tolk is runtime-only.** No `#include "Tolk.h"`, no vendored headers, no
   `find_package(Tolk)`, no CMake link, no build dependency. `speech.cpp` uses
   `LoadLibrary("Tolk.dll")` + `GetProcAddress` with hand-rolled typedefs. `Speech::Init()`
   returns false (never crashes) when Tolk is absent, and all speech calls no-op
   **silently**. Speech uses `Tolk_Output` (speech + braille), never `Tolk_Speak`.
3. **RELEASE — the zip DOES include them.** `dinput8.dll`, `Tolk.dll`,
   `nvdaControllerClient64.dll`, `ReadMe.txt` — see `Docs\release_procedure.md`. Players need them to
   play, and the sibling FFPR projects ship the same pair. **x64 builds only** (PE machine
   `8664`); the x86 Tolk loads and then silently never speaks.

Not deploying a DLL, not linking against it, and not shipping it are three different
things. Rules 1 and 2 are absolute; only rule 3 changed.
- **Speech uses `Tolk_Output`** — speech + braille combined. NEVER use `Tolk_Speak`
  (speech only). All new speech in `speech.cpp` must call `g_Tolk_Output`.
- **ALWAYS ASSUME MOD IS ACTIVE**: the user is blind and cannot play the game without
  the mod. When running diagnostics or testing, assume the mod DLL is installed and
  active unless the user explicitly says they removed it.

#### NEVER ASK THE USER TO FIND, REACH, OR AIM AT SOMETHING IN THE WORLD (CRITICAL)

**The user is blind. Locating an object in the game world is the thing this mod EXISTS to
do. Asking them to do it by hand, in order to produce a diagnostic about why the mod
cannot do it, is circular** -- it demands the capability whose absence is the bug.

Never write, in any form:

- *"stand next to the cactus and press `'`"*
- *"walk up to it and interact"*
- *"face the object, then..."*
- *"go to where the chest is and tell me what you hear"*
- *"count the chests on the map"*
- *"find X and check whether..."*

**These are not requests for a measurement. They are requests for the deliverable.** If the
mod could get the player to the object, there would be no ticket.

**What IS fair to ask for**, because the mod already makes it reliable:

- press a key (`'`, `` ` ``, `F5`, a category cycle) **wherever they already are**
- load a save, or enter a map by name -- the exit list gets them there
- re-run with a new build, and report what they HEARD
- confirm a count the mod itself spoke

**What to do instead, in order of preference:**

1. **Answer it offline.** The decompile archive, the map data, `npcdic_names.csv`, the VBF
   extraction and the walkmap are all readable without the game running. Most "where is
   this object" questions are map-data questions (L-08).
2. **Ship a PASSIVE instrument** -- one that captures the data from wherever the player
   happens to be standing, on a rescan or a map load, with no positioning required. The
   treasure census and the handle-table dump are the shape to copy.
3. **Only then** ask for a keypress, and say plainly that it works from anywhere.

**Why (2026-08-25):** a session investigating an interactable the nav list could not see
ended by proposing *"stand next to it and press `'` -- ground truth, no guessing"*. The
user's reply: *"I can not just magically walk up to the cactus and interact with it to get
your log."* Correct, and the same session had already spent two builds on objects that
turned out to be degenerate placeholders. **The instinct to reach for a play measurement is
what needs checking**: ask whether the question is decompile-answerable FIRST (L-08), and if
a play measurement really is required, make the instrument come to the player.

This is the same rule as **L-14 NEVER ASK THE USER TO READ THE SCREEN**, one level up: not
just "do not ask them to see", but "do not ask them to do anything that requires sight",
and above all not the very thing the feature under repair is supposed to give them.

### Code quality, layout, and centralization

- **NO DEDUPLICATION OF SPEECH.** Do not add a "same as last time, stay quiet" check,
  a debounce, or a suppression window. There are exactly **two** exceptions:
  1. The **user explicitly asked for it in the current conversation**. Not "a past
     session added one", not "it seems chatty" — asked for, this conversation.
  2. The announcement hangs off a **per-frame / per-draw** game function, where
     without a change-check the mod would speak on every frame. Every surviving
     instance MUST carry a comment **naming the per-frame function it guards**
     (e.g. `battle_target_reader.cpp`'s `g_lastHandle`, guarding the `FUN_002bfd20`
     nameplate render). No name in the comment = not justified = delete it.

  Repeated speech from an **event-driven** hook is a redundant-call-path bug. Find the
  second call path and remove it; never paper over it with a filter.

  **Why this is absolute:** dedup makes the mod go SILENT when the player re-enters a
  surface — leaving a menu pane and coming back said nothing, because the focus matched
  the cached one. For a blind player, re-entering a surface must always announce where
  they landed. A repeat is an annoyance; silence is a lost position.

  **NOT covered by this rule — do NOT strip these:**
  - **State machines that detect a transition** rather than suppress a repeat:
    `combat_events.cpp`'s low-HP and death latches fire once per *crossing* and re-arm
    on recovery. They suppress no event; they detect one.
  - **Collection dedup** that builds a list of unique things: `entity_list.cpp`'s
    `AlreadyListed`, `map_query.cpp`'s identical-bytes exit-record drop.
  - **Log-only volume control** (the CONSOLE OUTPUT BUDGET rule below). This rule is
    about speech; the log may stay deduped where it guards a per-frame hook.
- **NO** polling, timers, or per-frame checks — event-driven hooks only. **No
  exceptions** outside narrow polled-monitor cases that have been documented and
  explicitly approved.
- **NEVER ADD A FRAME STALL TO THE GAME THREAD WITHOUT THE USER'S EXPRESS PERMISSION FOR THAT SPECIFIC
  MECHANISM (CRITICAL).** No background route search, flood, scan or engine-call loop that runs on the game
  thread on the mod's own initiative — however it is rate-limited, spaced or paused. Work the PLAYER asked
  for with a keypress (a route request) is the only game-thread cost that needs no separate approval. And
  never move such work to another thread instead: the engine's collision and walkmap functions run against
  state the game thread mutates, which trades a stall for a crash. If a feature cannot be built without it,
  say so and ask; the user's standing answer is to revoke the feature. **Why (2026-09-17, S182):** a filter
  build ran one real route search in the background every 250 ms+ (2-43 ms stall each) and was revoked
  before deployment: *"game freeze is 100%, completely unacceptable and you should never have built a
  system that could potentially do that without express permission."* Lessons.md `L-88`.
- **NEVER COUNT FRAMES.** A counter incremented once per call and compared against a
  constant silently means "N/60 seconds", and it is wrong at every other frame rate —
  at 144 fps a 90-frame budget is 0.63 s, not the 1.5 s its comment claims. Use a
  `GetTickCount64()` deadline, which is what such comments always meant. Hooking a
  per-frame function is fine; deriving behaviour from the frame COUNT is not. Related:
  before re-arming on a game flag, establish whether it is an **edge or a level** — a
  field the game clears itself will fire forever (S149's dialogue repeat).
  **Full inventory + the three live offenders: `Docs\PerFrameAudit.md`.**
- **CONSOLE OUTPUT BUDGET**: the user is blind and uses a screen reader. Dumping
  hundreds of lines at once **crashes the screen reader**. ALL scripts (Frida, batch,
  etc.) split output into **console** (brief summaries, key findings, ~500 lines max)
  and **file-only** logging (verbose diagnostics, hex dumps). For ProcessEvent-style
  high-frequency hooks, console output must be O(unique_events), not O(N).
- **NO** files >500 lines per `.cpp`. When approaching 400, plan a split. Headers
  under 150 lines.
- **ALWAYS** centralize code — shared helpers, never duplicate logic across files.
- **CENTRALIZE: REUSE THE EXISTING UTILITY, DO NOT ADD A SECOND PATH.** Before writing a new
  function, hook, resolver, decoder or speech call, find the one that already does the job and
  use it. If it *almost* fits, EXTEND it — add a parameter, widen its contract — rather than
  standing up a parallel one beside it. Ask "what already does this?" before "how do I do this?".
  - **Speech especially: one choke point per surface.** A reader may have several DETECTORS (a
    focus message, a per-frame tick), but they must all funnel into ONE emit function that owns
    the wording, the logging, and the interrupt-vs-queue policy. Two speakers with two policies
    WILL race: the notice board had exactly that, and the plainer line won by 125 ms — the
    Status column vanished and the prompt got cut off, with nothing in the code looking wrong.
  - Existing choke points to reach for, not reinvent: `Speech::Output` (all speech),
    `Log::WriteW` (all wide-text logging), `GameText::Decode` (all codec text),
    `MemRead::*` (all guarded reads), `Hooks::InstallTyped` (all hooks),
    `Phrase::Get` (all mod-authored words), `BattleState::DefName` (all master-data names).
  - **Do NOT break working behaviour in the name of simplification.** Collapsing two detectors
    into one because it looked tidier silenced the board's navigation outright — the surviving
    detector's cursor field did not track that surface. If two paths genuinely cover different
    cases, KEEP BOTH and arbitrate (one stands down while the other is driving); only delete a
    path once you have shown the remaining one covers everything it did.
- **NEVER hardcode user-facing speech text** unless there is no game-supplied text
  available for that event. The game has display strings for almost everything;
  read those. Hardcoded strings are a **last resort** and require explicit user
  permission. The phrasebook (12-locale dictionary in `speech/phrasebook.cpp`)
  contains ONLY mod-emitted strings the game does not provide (the `HP`/`MP`
  gauge labels the game draws as art, the outcome words in `combat_format.cpp` —
  parried/blocked/evaded, which are sprites with no text in the binary — and the
  gambit `on`/`off`, an icon frame in battle and an alpha dim on the field).
  **BUILT in Session 87** (was "planned, not built"): every mod-authored
  user-facing string now lives there, keyed by `Phrase::Id` and reached through
  `Phrase::Get(id)`. Only the English column is populated; the other 11 are
  `nullptr` and fall back. **Adding a string to it still needs permission** —
  the file existing is not itself a licence to invent words. Do NOT put anything
  the game supplies in it, and do NOT put punctuation/joining glue (`", "`,
  `": "`), format specifiers, file paths, or log-only text in it. Never
  machine-translate a column: an invented translation is the same fabricated-label
  failure the rule exists to prevent.
- **NEVER speak filler when there is nothing to report — be SILENT.** An empty
  party slot, an absent guest (`7`), a missing target: say **nothing**. Do not
  announce "Empty slot" or equivalents — this is a **standing user instruction**,
  reversed once already after `combat_system.md` §8.2 proposed it. If the worry is
  "silence could mean the mod is broken", the answer is a **log** diagnostic
  (`BattleState::DiagnoseSlot`), never speech. Silence is the mod's normal, correct
  response to nothing-to-say.
- **REMOVE dead fallbacks.** Silence is always better than wrong speech. A fallback
  is only justified if it provides correct, relevant information.
- **THE MOD IS STRICTLY READ-ONLY on input and game memory — with ONE recorded exception
  (Auto-walk, user-authorized 2026-07-31, Session 100).** It observes — it never injects
  keystrokes/mouse (`SendInput`/`keybd_event`/`mouse_event`/`PostMessage(WM_KEY…)`), never
  presses game buttons, never writes game memory (`WriteProcessMemory`/any mem-write helper),
  and never swallows a key. The ONLY `VirtualProtect` is the one-time vtable patch installing
  the `GetDeviceState` hook; all game calls are pure getters.

  **The exception:** the user explicitly authorized, in the conversation of 2026-07-31, the
  **Auto-walk** feature to OR the four movement-key bits (**DIK W/A/S/D only**) into the
  keyboard state buffer inside `HookedGetDeviceState` (`src\proxy\dinput8_proxy.cpp`) — the
  mod's one existing input hook — through exactly **one** function, `AutoWalk::OnDevicePoll`,
  and nowhere else. Boundaries, all non-negotiable:
  1. **default OFF**, gated on the ModMenu Auto-walk toggle;
  2. with the toggle off the injection function returns on its first line — the input path is
     byte-identical to the read-only mod (the write is unreachable, not merely skipped);
  3. injection is movement keys ONLY — extending it to any other key (Confirm, Cancel, menu
     keys, anything) is a NEW category change requiring new explicit permission;
  4. `InputTracker::FeedDInputKeyboard` is always fed the PRE-injection buffer, so every
     mod-side observation sees only the player's real keys;
  5. a real movement key in the same poll suppresses injection in that poll and cancels the
     feature — the player always wins, instantly;
  6. auto-walk disengages on combat engagement (hard requirement, within one frame), route
     loss, map change, focus loss, menu open, field-tick stall, and a 15 s no-progress cap —
     it never keeps walking a character the player has lost control of.

  **THE SECOND EXCEPTION — the GAMEPAD INTERCEPT, user-authorized 2026-08-20.** The mod may
  **CONSUME** pad input: `PadRouter::OnPoll` (`src\input\pad_router.cpp`), reached only from the
  `XInputGetState` IAT hook in `src\input\pad_hook.cpp`, may clear a button bit or zero a stick
  axis in the `XINPUT_STATE` the game is about to read. Bounds, all non-negotiable:
  1. **Consumption only, never injection.** It may clear a bit or zero an axis; it may NEVER set
     one. This is the category line between it and Auto-walk, and it is what keeps "the mod cannot
     press a button for you" true.
  2. **One function.** `PadRouter::OnPoll` is the only code in the mod that writes an
     `XINPUT_STATE`; nothing else may.
  3. **The router is fed the PRE-consumption state**, always — every mod-side observation sees the
     player's real input, exactly as `InputTracker::FeedDInputKeyboard` is fed the pre-injection
     keyboard buffer.
  4. **Off means byte-identical, not skipped.** With the `Controller` mod-menu row off, or the pad
     absent, `OnPoll` WRITES NOTHING, so the `XINPUT_STATE` the game reads is what it was before this
     file existed. A fault inside it latches the intercept OFF for the session.
     **Amended S174:** it used to return on its *first* line; it now returns on the fourth, after the
     foreground check, edge bookkeeping and the `L3` kill-switch test. The mod therefore still READS
     one bit while off — that is what lets `L3` switch it back on, and a switch that can only be
     thrown once is not an escape hatch. The byte-identical bound is on the WRITE and is unchanged.
  5. **Game-foreground gated**, like every other dispatch in the mod.
  6. **What may be consumed is decided on the GAME thread** and published stamped; the verdict
     expires ~250 ms after the field tick stops, so consumption ends by itself on a map change,
     pause or stall. Widening consumption to a new control is a normal design change, not a new
     category — but widening it to *injection* is a new category and needs new permission.

  **GAME-MEMORY WRITES, each user-authorized and each chartered in its own header:** sneak assist
  (`sneak_assist.h`, S107), the shout minigame's Instant success (`shout_fill.h`, 2026-08-05), and
  Sochen Cave Palace's Solve door puzzles (`sochen_doors.h`, 2026-09-15, S180: ORs two puzzle bits
  into one save-block byte, context-gated row, default Off). Each writes only what its charter names;
  a new target is a new permission.

  Everything else in this rule stands unchanged: no lock-on presses, no speed changes, and any
  OTHER feature that would *drive* the game still requires **explicit user permission** and a
  design discussion first. (Original rule confirmed Session 44 after a tester speed-jump turned
  out to be their own `1` keypress -- keyboard `1` is the pad's L1, which cycles game speed (S183 correction; the
  S44 wording said `1`/`2`/`3` were all Game Speed); exception recorded Session 100 so the
  audit trail stays truthful — `debug.md`'s read-only-input entries carry the same note.)

### Combat log specifics

**Shipped and confirmed working in play (Session 49, release 0.1).** This section describes what
EXISTS; `combat_log.h` is the code-side source of truth. It **supersedes the original modal design**
— there is **no F4, no open/close, no Escape, and no `WH_KEYBOARD_LL` intercept**. That design was
never built; do not reintroduce it, and do not "restore" an open key.

- **NOT modal.** No overlay, no input intercept, and the game is **never paused** by the log. Every
  value it reads is an on-demand read. This is what makes it safe to use anywhere.
- **Usable everywhere, including menus and while the game is paused.** The only dispatch gate is
  `GameIsForeground()` (`input_tracker.cpp`, `DInputEdge`) — there is deliberately **no menu-state
  gate**. The game keeps polling DirectInput while its own menus are open (that poll is what drives
  them), so the log keys keep working there. **This is a requirement, not an accident:** a player who
  cannot keep up mid-fight pauses or opens a menu and reads back. Any change that gates nav-key
  dispatch on menu state would break it.
- **Navigation:** `,` back/older · `.` forward/newer · `Home` oldest · `End` newest. It is a
  **timeline, not a chat scrollback**, so `,` goes back in time.
- **Capacity: 100 events**, continuous FIFO **across battles**. Never cleared on battle entry/exit,
  area change, or save/load — only on `Shutdown()`. The cursor is keyed on a monotonic seq so an
  eviction that passes it is detectable.
- **Threading:** producers are game-thread hooks; the consumer is the input thread. Text is rendered
  **at append time on the game thread**, because BtlChr / actor-pool / name-codec / message-buffer
  pointers are only reliably readable there (the message buffer dies when its frame returns). The
  input thread only ever touches the finished `wstring`.
- **Realtime vs log-only** is a **data table keyed by message id** in `combat_format.cpp`
  (`ShouldSpeakNow`), not a chain of `if`s. Principle: interrupt for what you must ACT on or would
  otherwise never learn (failed command, KO or revive, level up, loot/gil/steal/poach, nullified
  damage type, disabled command category, back attack); log the rest. It is **NOT** headed for
  `mod_config.ini` — that file belongs to the RVA byte-validator and hand-editing it masks validator
  failures; the mod keeps its own store (`ModMenu`, `%LOCALAPPDATA%\FFXII-Screen-Reader\
  mod_settings.txt`).
- **The charge announces `0x0D`/`0x0E` are PLAYER-CONTROLLED** (Session 90) via the mod menu's
  **Combat verbosity** setting — `F8` for the menu, `F4` for the toggle. **Normal (default) does not
  speak them; Verbose does.** `0x0F` "uses" is the routine-item tier and stays log-only in both.
  Nothing else in the table is affected, and **damage lines are log-only in both modes** — they are
  appended with `speakNow=false` and never consult `ShouldSpeakNow` at all.
- **Verbose is "announce when the game announces", not "announce every cast."** Three suppressors on
  the GAME's side, all recorded in `GameArchitecture.md`: the **repeat gate** in `FUN_00304850` (an
  actor repeating one action on one target announces ONCE — the largest of the three), the ~24-unit
  distance cull in `FUN_00469570` (style `0x01` is not cull-exempt), and the 10-slot dedup ring in
  `FUN_0046ab10`. Do **not** "fix" any of them by hooking the emitter `FUN_00469af0`: it would cost
  the game's own verbatim wording in all 12 locales, and the emitter is exactly what the repeat gate
  already declined to call.
- **Two combat vocabularies, kept apart.** The game's CHARGE announce ("begins casting" / "readies" /
  "uses") is read verbatim; the mod's EXECUTION line (`DamageLine`, on the applier) is its own —
  `attacks` / `casts` / `uses`. Session 90 fixed a bug where the announce map was mirrored into the
  execution line, so a landed enemy ability said "Urstrix A **readies** Slap on Vaan. 14".
  **`Readies` is not an execution verb.**
- **Enemy defeated + rewards are ONE line**, emitted from the death event `FUN_00312280`
  (`0x1F2280`), not from the damage applier: `"Dire Rat defeated. 34 EXP, 2 LP."` FFXII has no
  end-of-battle results screen and no text for EXP/LP (sprite digits only), so the sentence is
  mod-emitted while the numbers are diffed from the game's own `BtlChr+0x18C`/`+0x190` writes. Both
  deltas zero degrades to the bare defeat line — never "0 EXP, 0 LP".

### Documentation

- **MANDATORY DOCUMENTATION** after every task. Update the appropriate Docs/ file as
  a final step before reporting done:
  - `Docs/sessions_*_current.md` — what happened this session (KEYWORDS line for grep)
  - `Docs/plan.md` — feature checklist; update on user "mark feature as complete"
  - `Docs/debug.md` — Tried & Failed and Solved Problems
  - `Docs/GameArchitecture.md` — discovered RVAs, offsets, struct layouts (the persistent
    lookup, NOT in session logs)
  - `Docs/PerformanceIssues.md` — file-size audit, centralization debt
- **MANDATORY DECOMPILE ARCHIVE.** Every Ghidra/Frida finding gets archived in
  `..\FFXII-Decompile\` (sibling directory). Raw output to `output\` (use `cp -n`,
  never overwrite). Key findings extracted into `notes\`, RVAs into
  `Docs\GameArchitecture.md`.
- **ALWAYS** check `debug.md` Tried & Failed before proposing approaches. If a prior
  session tried it and it failed, do not retry without new evidence.

### RE script execution

- **NEVER execute Ghidra or Frida scripts.** Claude authors them; the user runs them in
  their own terminal. Failure to follow this is a **critical project rule violation**.
- Register new scripts in their launcher batch file (`run_ghidra.bat` / `run_frida.bat`).
- **Ghidra 12 API rules** (port from DQ7R, all FFXII-relevant):
  - Use `getSourceFile()` returning `ResourceFile` — wrap with `new File(...)`.
  - `getReferencesTo()` returns `ReferenceIterator`, NOT `Reference[]` — iterate with
    `while (refs.hasNext())`.
  - `Listing.getFunctionBefore()` removed — use `getFunctions(addr, false)`.
  - All search/decompile scripts call `currentProgram.setTemporary(true)` to prevent
    accidental saves.
  - `toAddr(long)` for vtable address conversion (never `imageBase.getNewAddress()`).
  - All loops include `monitor.isCancelled()` checks.
  - **NEVER `-import -overwrite`** after the initial import (DQ7R lost 12 hours that way).
- **Frida API gotchas** (port from DQ7R):
  - `Module.findBaseAddress()` does not exist. Use `Process.enumerateModules()[0].base`.
  - `File.read()` does not exist. The File class is write-only. Hardcode reference data.
  - `setInterval` / `setTimeout` not in injected scripts.
  - `NativePointer.toNumber()` does not exist. Use `.toInt32()` / `.toUInt32()`.
  - Keyboard input requires a Python host (Frida CLI cannot do `GetAsyncKeyState` from
    injected scripts).

### Type validation

- **MANDATORY TYPE VALIDATION**: never act on, hook, or label any memory address as a
  PhyreEngine class until it's structurally validated. "Has an in-module vtable" proves
  almost nothing — every C++ object does. For any object we plan to dereference fields
  on, validate via vtable + at least one back-reference (e.g., the manager pointer
  reaches it via a known offset chain). Ghidra-derived identities are INFERRED, not
  CONFIRMED.

### Auditing rules

#### THE LOG CORPUS — two directories, and only one of them is evidence (CRITICAL)

**`<game>\x64\logs\` is THE log corpus.** It holds OUR OWN archived dev logs, alongside the live
`x64\FFXII-Screen-Reader-Latest.log`. **The user is the DEVELOPER, not a tester** — these are their
own play sessions. Every routine analysis starts here, sweeps here, and counts here. Reading it is
always permitted (see the Exception above).

**`Tester Logs\<name>\` IS OFF LIMITS. DO NOT OPEN IT, DO NOT LIST IT, DO NOT GREP IT, DO NOT
`find` THROUGH IT — NOT EVEN TO SEE WHAT IS IN THERE.** There is exactly one condition under which
a file in that folder may be read: **the user has explicitly told you, in the current conversation,
to use a specific tester log.** Nothing else unlocks it — not a tester report, not a stuck
investigation, not "the answer might be in there", not a sweep that happens to include it. **The
user will tell you when a tester log is in play. Until those words exist, that directory does not
exist.**

The same prohibition covers every other tester-supplied artifact: `Saves\<name>\`, tester crash
dumps, tester screenshots, anything a tester sent. **THOSE FILES ARE NOT FROM THIS MACHINE.** Their
build, their settings, their install, their save state and their story progress are all unknown, so
anything read out of them is a fact about someone else's box being smuggled into reasoning about
ours. A number taken from a tester artifact is **never** mixed into a corpus-wide count.

**When a tester reports something and you think you need their log: ASK. Do not go looking.** The
correct move is one sentence — *"is there a tester log for this, and where?"* — and then wait. Once
the user DOES point you at one, that log becomes the authority for that defect
(`feedback_read_the_testers_own_log.md`); this rule is only about what you reach for unprompted.

**Why (Session 147):** a session investigating a reachability regression reached for `Tester Logs\`
first, found nothing relevant, and concluded the defect was "not measurable from any archived log".
It was measurable — the contradiction was sitting in nineteen of our own twenty dev logs the whole
time. Reaching for the wrong corpus did not just waste the search; it produced a confident wrong
answer and nearly cost the session its actual finding.

**Why the rule got HARDER (2026-08-25):** a session opened on a tester report, and the very first
moves were `ls "Tester Logs/Dylan"`, a recursive `find` across the whole project, and a sweep of
`Saves\<name>\` — hunting for a dump nobody had said was there. **This keeps happening.** The
two-condition wording above was read as "a tester reported something, so condition 1 is met, so I
may go and look", which is exactly backwards: the report is what makes the folder tempting, not what
makes it permitted. **Hence the flat ban. The only key is the user's explicit say-so.**

- **ALWAYS** check logs first when debugging — read the mod log before theorizing.
- **ALWAYS** log all diagnostic data to external file. Every significant runtime decision
  must be logged. Previous sessions archived as
  `x64\logs\FFXII-Screen-Reader-YYYY-MM-DD_HH-MM-SS.log`.
- **ALWAYS** update `Docs\GameArchitecture.md` when log analysis, Frida tracing, or
  Ghidra RE confirms new RVAs, offsets, or class structures.
- **ALWAYS** keep `MEMORY.md` (in `~/.claude/projects/D--Games-Dev-Custom-FFXII/memory/`)
  status current when documenting new features or solutions.

### Write punctuation AS punctuation — never spell a symbol out as a word (CRITICAL)

In `README.md`, `ReadMe.txt`, `Docs\Controls.md` and every other user-facing document, **a key that
is a punctuation mark is named by that character** — `[`, `]`, `;`, `'`, `` ` ``, `-`, `=`, `/`,
`\`, `,`, `.` — and **never** by an English word for it: no "Left bracket", "Semicolon",
"Apostrophe", "Backtick", "Minus", "Slash". The screen reader announces punctuation perfectly well
on its own; spelling it out is redundant noise the reader then says twice.

- **Real key NAMES stay words** — `Left Ctrl`, `Numpad Plus`, `Left Shift`, `Home`, `End`, `F5`.
  The rule is about characters typed as themselves, not about keys that have a name.
- **`README.md` MUST REMAIN VALID MARKDOWN. Markdown escaping is NOT the same thing as spelling a
  symbol out, and must NOT be stripped in the name of this rule.** Keep `\[`, `\-`, `\\`, `\_`, and
  the leading `&#x20;` that stops a line-initial backtick opening a code span. Those escapes are how
  the character renders *as itself*; removing them is a Markdown bug, not an accessibility fix.
- **`ReadMe.txt` is where escapes come off.** The plain-text conversion unescapes `\[` `\-` `\\`
  `\_` and drops `&#x20;` — see `Docs\release_procedure.md`. The source stays proper Markdown; the
  shipped artifact is the flat one.

**Why:** the tester is blind and these files are how a key gets from the code to their fingers. A
key written "Semicolon" is a key they cannot find, because the reader says "semicolon" for `;`
anyway. Corrected 2026-07-24 — and note the overcorrection made the same day, when this rule was
first written to ban `\\` and `&#x20;` as well and broke the Markdown. The ask was words → symbols,
nothing more.

### README edits — CONCISE. Keys only. Never a changelog. (CRITICAL)

When asked to update `README.md`, add **only what was asked for** — normally the new hotkeys, plus
the one line of context an existing key needs when its meaning changed — and nothing else.

- **Do NOT justify a feature, a position, or a design decision.** No "it is on a keypress rather than
  automatic because…", no "deliberately", no explaining why a surface behaves the way it does. The
  reader wants to know which key does what. The reasoning belongs in the session log.
- **Do NOT write it as a changelog.** No "new in this build", no "this used to read X", no
  before/after framing. The readme describes the mod as it is now; `Docs\sessions_*.md` holds history.
- **Do NOT create a section per feature.** A key that fits an existing list goes in that list. A new
  section is for a surface with genuinely no home, and one short paragraph is usually the whole entry.
- **Do not rewrite prose that is already correct.** Touching a working paragraph is not an update.
- **A feature with no key needs NO entry.** If it reads on its own and the player makes no decision
  about it, there is nothing to look up: Game Over, the damage-line element, the notice board, the
  shop item list all just happen. Their absence is not a gap, and `Docs\release_procedure.md` must
  not be made to list them (corrected 2026-08-03).

**Why (2026-08-03):** asked to document keys `8` and `9`, I added four new sections, a
design-rationale paragraph per surface, and changelog framing throughout. The tester's verdict:
*"far, far too many edits… you only needed to add a few new keys and context to others, not create a
thousand new readme sections."* Every surplus sentence is one a blind player listens through to reach
the key they were looking for — length in this file has a direct cost.

**This does NOT license leaving a false claim standing.** If a line the edit touches is wrong, fix
that line against the code rather than carrying it forward: "The Gambit editor is not implemented
yet" survived several readme updates, and that screen had read since Session 94.

## Project Structure

```
D:\Games\Dev\Custom\FFXII\
├── FFXII-Screen-Reader\              Mod root (git repo — initialized 2026-07-01)
│   ├── CMakeLists.txt
│   ├── build_and_deploy.bat          (gitignored — machine-specific)
│   ├── CLAUDE.md                     (TRACKED — house rules; see WHAT NEVER ENTERS THE REPO)
│   ├── README.md
│   ├── src\
│   │   ├── proxy\                    dinput8.dll proxy + DllMain deferred init
│   │   ├── core\                     events, hooks, memory, config, logger, phyre_types
│   │   ├── speech\                   Tolk wrapper, locale, phrasebook
│   │   ├── input\                    WH_KEYBOARD_LL hook, hotkey state machine
│   │   ├── ui\                       game_handler, menu_state, dialogue
│   │   ├── ui\menus\                 per-menu readers (Items, Equip, Map, Save, …)
│   │   ├── navigation\               entity_list, pathfind, compass
│   │   └── battle\                   combat_log, event_capture, battle_state
│   ├── include\                      MinHook + other vendored headers (NO Tolk — runtime-only)
│   ├── config\                       mod_config.ini.template (dev reference; not deployed)
│   ├── third_party\
│   │   └── ff12-module-loader\       Cloned ELF source (BSD-2, REFERENCE ONLY — not used in build)
│   ├── Docs\                         (see Docs/ section below)
│   ├── Releases\                     versioned zip outputs
│   └── build\                        cmake out (gitignored)
└── FFXII-Decompile\                  RE archive (NOT in mod git repo)
    ├── ghidra\                       .java scripts + run_ghidra.bat
    ├── frida\                        .js probes + run_frida.bat
    ├── output\                       script output (decompile, strings, etc.)
    ├── notes\                        dated session notes
    └── projects\                     Ghidra .rep folders + backups\
```

## Key Paths

- **Game install (READ-ONLY without permission):**
  `D:\Games\steamlibrary\steamapps\common\FINAL FANTASY XII THE ZODIAC AGE\`
- **Game exe:**
  `D:\Games\steamlibrary\steamapps\common\FINAL FANTASY XII THE ZODIAC AGE\x64\FFXII_TZA.exe`
- **Mod log (live):**
  `D:\Games\steamlibrary\steamapps\common\FINAL FANTASY XII THE ZODIAC AGE\x64\FFXII-Screen-Reader-Latest.log`
- **Mod log ARCHIVE — our own dev sessions, and THE default evidence corpus:**
  `D:\Games\steamlibrary\steamapps\common\FINAL FANTASY XII THE ZODIAC AGE\x64\logs\`
- **Tester-submitted logs — RESTRICTED, see THE LOG CORPUS under Auditing rules:**
  `D:\Games\Dev\Custom\FFXII\Tester Logs\<name>\` — opened only on a reported issue **and** an
  explicit pointer from the user. Never swept, never counted alongside ours.
- **Tolk: runtime-only, NEVER vendored.** No `#include "Tolk.h"` anywhere; no
  build dependency. `speech.cpp` uses `LoadLibrary("Tolk.dll")` + `GetProcAddress`
  with hand-rolled typedefs. The user deploys `Tolk.dll` and
  `nvdaControllerClient64.dll` manually to the game's `x64\` folder
  (already deployed as of 2026-05-04).
- **Ghidra:** `D:\Games\Dev\ghidra_12.0.3_PUBLIC\` (or whichever version is current)
- **DrummerIX CE table seed:** see `..\FFXII-Decompile\notes\drummer_ix_seed.csv`
- **DQ7R-Screen-Reader reference (READ-ONLY for porting idioms):**
  `D:\Games\Dev\unreal\dq7-r\DQ7R-Screen-Reader\`

## Build System

- **Visual Studio version:** 18 2026 (Community)
- **CMake:** not on PATH; use full path
  `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
- **Generator:** `"Visual Studio 18 2026"` with `-A x64`
- **Build command:** run `build_and_deploy.bat` from the `FFXII-Screen-Reader\` directory.
- **Running .bat from Claude Code bash:** ALWAYS use `cmd.exe //c` with the full Windows
  path. Example:
  `cmd.exe //c "D:\Games\Dev\Custom\FFXII\FFXII-Screen-Reader\build_and_deploy.bat"`
- **Output:** `build\bin\Release\dinput8.dll`. Deploy lands it as
  `<game>\x64\dinput8.dll` (replacing whatever was there — be aware if the user
  had External File Loader installed).

## RE Tools

- **Ghidra 12** (`D:\Games\Dev\ghidra_12.0.3_PUBLIC\`) — static analysis, decompile,
  pseudocode export. Scripts in `..\FFXII-Decompile\ghidra\`. Run via `run_ghidra.bat`
  (headless). Set `GHIDRA_HEADLESS_MAXMEM=8G` before running.
- **Frida 17+** — runtime instrumentation. Scripts in `..\FFXII-Decompile\frida\`.
- **DrummerIX Cheat Engine table** — primary RVA seed. Translate his absolute addresses
  to RVAs once and check into `notes\drummer_ix_seed.csv`. **Claude does NOT install
  Cheat Engine.** The user provides translated RVAs.

## Engine Notes (PhyreEngine, FFXII PC port)

- **Native 64-bit C++** PE. Compiled with MSVC. May or may not retain RTTI
  (Phase 0 confirms).
- **No reflection layer.** Discovery via known RVAs from DrummerIX seed; structures
  named ourselves from observed behavior in Ghidra/Frida.
- **Game logic uses a custom VM bytecode** (`.ebp` files inside `FFXII_TZA.vbf`). The
  community has a decompiler (Nexus mod 124). The accessibility mod likely never needs
  to touch the VM — runtime state reads are enough. If we do, do it via the existing
  decompiler tool.
- **Locales:** US, UK, JP, FR, DE, IT, ES, CN, KR, IN, ASIA, CH (per
  `FileSizeTables\FileSizeTable_*.fst`). The mod supports all 12.

## Release Process

Triggered when the user says **"prepare release X.X"** (e.g. `prepare release 0.01`).

**The full procedure lives in `Docs\release_procedure.md` — read it and follow it.** Do not improvise
a release from memory; the file is the source of truth and is versioned with the code. (Renamed from
`Docs\release.md` on 2026-07-24; older session entries cite the old name.)

Summary (details, preconditions, and the exact commands are in that file): build a fresh
`dinput8.dll` → assemble `Releases\V<version>\` with **exactly four files** → zip with 7-Zip as
`Releases\FFXII-Screen-ReaderV<version>.zip` → **record it in that file's Release Records** → report.

**Releases are recorded in `Docs\release_procedure.md`, NEVER as a `## Session N` entry.** A release
produces no code change; giving it a session number spends one from the global counter on a build and
buries the release history in a file about RE findings. Releases 0.1 (Session 50) and 0.1.1 (Session
67) predate this rule — do not copy them. Version naming follows `V<num>-<label>-build`
(`V0.1.1-shotgun-build`, `V0.2-test-build`).

The four files are `dinput8.dll`, **`Tolk.dll`**, **`nvdaControllerClient64.dll`**, and `ReadMe.txt`
(converted from `README.md` to plain text). Preserve casing exactly, and **the TTS pair must be x64**
(PE machine `8664`) — the x86 Tolk loads and then silently never speaks.

**The user ships releases by hand.** The procedure stops at the zip: no tag, no push, no GitHub
Release. `/Releases/` is gitignored — release artifacts never enter the repo.

**Do bundle the TTS pair; corrected 2026-07-15.** This summary previously said "exactly two files"
and "never bundle `Tolk.dll` / `nvdaControllerClient64.dll`". That was the **deploy** rule leaking
into the **release** rule, and it cost a release its TTS DLLs — players could not hear the mod. See
the three-concerns TTS rule above: deploy never copies Tolk, the build never links it, **the release
zip ships it**.

Still never bundle: **`mod_config.ini`** (self-healing; shipping one masks a validator failure), or
the Module Loader / External File Loader (both want the `dinput8.dll` slot).

## References

- `Docs\release_procedure.md` — **release procedure + Release Records** (triggered by "prepare
  release X.X"). Was `Docs\release.md` until 2026-07-24.
- `Docs\plan.md` — feature checklist (start of project; populated as Phase 0 lands)
- `Docs\debug.md` — Tried & Failed + Solved Problems
- `Docs\GameArchitecture.md` — RVAs, offsets, struct layouts (the canonical lookup)
- `Docs\GhidraReference.md` — Ghidra 12 API gotchas (port from DQ7R)
- `Docs\FridaScripts.md` — canonical Frida patterns
- `..\FFXII-Decompile\` — RE archive
- `~/.claude/plans/deep-research-time-i-atomic-sonnet.md` — original session plan

## Session Log Management

Session logs live in `Docs\sessions_*.md`, split into **50-session chunks** so no single
file grows unbounded. The running file is always `sessions_001_current.md`. **The number is
what bounds the file to 50 and drives the split — a session entry without a number is a BUG**
(date-only headers can't be counted, so the file can never be split; this happened once and
had to be repaired).

- **Every entry header MUST be `## Session N — YYYY-MM-DD — <title>`.** `N` is MANDATORY and
  is a single, global, monotonically increasing integer. **Never** a date-only header.
- **Before appending:** grep the current file for the highest `## Session N` and use `N+1`.
  Never skip, reuse, or drop a number.
- **ALSO check `git log` for an UNLOGGED session before taking that number.** The grep above
  assumes every session left an entry, and one did not: `6f619e3` (phrasebook, notice board,
  in-dialogue choices, battle menu) shipped with no entry at all, so a later session grepped the
  log, saw 86 as the highest, and took 87 — a number that already belonged to `6f619e3`, as
  `CLAUDE.md` itself said. **An unlogged session is invisible to a grep of the log.** Compare the
  last logged entry against the commits after it; if a code commit sits between them with no
  entry, it owns the next number and gets a reconstructed entry from its commit message (which is
  the record). Repaired 2026-07-29: `6f619e3` = 87, shop-category = 88, empty-category = 89.
- **No letter sub-sessions.** If work continues later the same day it still gets the next full
  integer (a repeated date is fine). Letter suffixes (e.g. `47b`) break the 50-count — do not
  use them.
- **Parallel tracks share ONE global counter.** When more than one track is active (e.g.
  menu-reader vs pathfinder), tag the title — `## Session N — DATE — [track] …` — so tracks
  stay greppable, but `N` is the single shared sequence. **A session's commit + log entry cover
  ONLY that track's work:** never commit another track's entry or files. Stage files explicitly
  (`git add <paths>`), never `git add -A`; if the shared session log holds another track's
  uncommitted entry, commit your changes without it (back up, truncate to your content, commit,
  restore).
- **Mandatory split** after writing Session 50 (100, 150, …): rename `sessions_001_current.md`
  → `sessions_001_050.md` and create `sessions_051_current.md`; repeat so the running file
  always spans `<start>–current`.
- **KEYWORDS line** on every entry for grep discoverability.
