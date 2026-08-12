# Lessons — the reasoning failures this project has already paid for

**This file exists so the memory index does not have to carry them.** `MEMORY.md` is loaded into
every session and must stay a thin index of *state*; the durable *lessons* live here, in a file
built to be grepped on demand.

## HOW TO USE THIS FILE — the loop

```
memory loaded  ->  task given  ->  check MEMORY.md for state
                                ->  ROUTE below, then grep this file
                                ->  then GameArchitecture.md / debug.md for facts
                                ->  proceed
```

**Grep, do not read end to end.** Every lesson has a stable id (`L-NN`); every section carries a
`TAG:` line. Two ways in, both work:

```
grep -n "TAG:hooking" Docs/Lessons.md      # by section, from the routing table
grep -in "edge or a level" Docs/Lessons.md  # by natural phrase, straight at the lesson
```

**Lesson headings are kept free of inline markup on purpose**, so a plain-English grep hits them.
Do not add `**bold**` inside a `### L-NN` line — it silently breaks the second form above.

**If you only do one thing: read the ROUTING TABLE and grep the two or three tags that match your
task.** Nine times out of ten the relevant lesson is one of six.

## ROUTING TABLE — find your task, grep that tag

| your task looks like… | grep tag | lessons |
|---|---|---|
| about to state a conclusion, an RVA, an offset, a cause | `TAG:concluding` | L-01…L-09 |
| a tester reported something | `TAG:tester` | L-10…L-14 |
| reading a log to find out what happened | `TAG:logreading` | L-15…L-19 |
| adding/changing a hook, or reading game state | `TAG:hooking` | L-20…L-26 |
| editing code that already works | `TAG:refactor` | L-27…L-32 |
| anything that makes the mod speak | `TAG:speech` | L-33…L-37 |
| writing docs, committing, closing a session | `TAG:process` | L-38…L-43 |
| something is slow, or timing-dependent | `TAG:timing` | L-44…L-47 |
| how wide should the fix be; is this key free | `TAG:scope` | L-48…L-51 |
| build, release, Ghidra, Frida, menus, input | `TAG:tooling` | L-52…L-58 |

**Format of an entry:** the imperative as the `### L-NN` heading, then **Why** (the evidence that
bought it), then where the detail lives. `⟲` marks a lesson this project has learned **more than
once** — check those first, because they are the ones that recur.

---

## Concluding — before you assert anything
`TAG:concluding`

### L-01 ⟲ A SAMPLE IS NOT A POPULATION
**Never falsify a claim about all cases with the cases you already understand.**
**Why:** learned at least four times. S80 (odd-slot NPC names), S89 (empty category), S93 ("there is
no party line where the name is absent" — a claim about two logs on hand, not about the code; the
defect was broken *by construction* for every non-leader slot), S94 (roster names). Each time a
handful of working examples was read as proof, and each time the population disagreed.
**Corollary:** sweep the corpus before theorising about one member (S104).

### L-02 THE RE CONFIDENCE BAR IS 0.98, AND IT IS NOT NEGOTIABLE
**Below 0.98 is discarded, not "used carefully".** Only a Frida confirmation probe promotes a
hypothesis.
**Why:** several 0.90-confidence identifications shipped wrong; the battle-command and targeting
hooks were re-derived and re-shipped wrong three sessions running. Detail:
`feedback_re_confidence_bar.md`, and the CLAUDE.md section of the same name.

### L-03 A COMMENT IS NOT A MEASUREMENT
**Why:** S126. A constant's comment claimed what the code did not do. Also S152: three comments
saying "~1.5 s" over a frame counter that meant that only at 60 fps.

### L-04 ⟲ A THROTTLED, DEDUPED OR CAPPED LOG LINE IS NOT A MEASUREMENT
**Check whether the line you are counting is rate-limited before you count it.**
**Why:** S150 read three throttled log lines as measurements and built five wrong inferences on one
defect. S152 nearly repeated it: `StallProbe::FrameTick` *computes* the frame gap and then discards
it below 100 ms, so "twenty logs contain five gap lines" means nothing about the frame rate.

### L-05 CHECK WHAT AN INSTRUMENT EMITS, NOT WHAT IT MEASURES
**Why:** S152. An entire measurement plan ("one play log at 1x and 4x settles it, no code") was
built on `FrameTick`, which measures exactly the right quantity and then throws it away below a
threshold. The discard is invisible in the function name and in its header comment.

### L-06 A NEGATIVE RESULT IS ONLY AS GOOD AS THE SHAPE YOU SEARCHED FOR
**Why:** S147. "Not found anywhere" meant "not found in the shape I assumed".

### L-07 DON'T MODEL THE VERDICT — READ THE WORD THE VERDICT IS READ FROM
**Why:** S149. Sessions were spent reconstructing a license-board reachability rule; the game simply
reads `cell+0x18 & 0x1000`, and `FUN_00323600` has no adjacency test at all.

### L-08 ASK WHETHER A BLOCKING QUESTION IS DECOMPILE-ANSWERABLE BEFORE CALLING IT PLAY-BLOCKED
**Why:** S152. The per-frame audit declared the game-speed mechanism "NOT established, measure
first" and blocked four fixes for three months. The answer was in `FUN_0022a770` the whole time.
**Inverse also true:** some facts are runtime-only (who calls a dispatch-table slot; what rate a
loop actually runs at). Say which kind you have.

### L-09 STATE THE SCOPE YOU MEASURED, NOT THE SCOPE YOU WERE THINKING ABOUT
**Why:** S152, within a single session. "Game speed cannot move a frame counter" was verified for
the *field-tick callees* and written as if it covered everything. It does not cover hooks reached
from inside the sim loop — which is exactly where the open defect turned out to point.

---

## Tester reports
`TAG:tester`

### L-10 READ THE TESTER'S OWN LOG FIRST — AND THEIR BUILD BEFORE THAT
**Why:** S150. A tester was three commits behind; two of their three reports needed a *build*, not a
fix. A tester-only defect is evidence about *their environment*; never carry a count from your own
log into reasoning about theirs. Detail: `feedback_read_the_testers_own_log.md`.

### L-11 A TESTER-ONLY, CONDITION-SPECIFIC DEFECT IS EVIDENCE ABOUT A CALL PATH — NOT THEIR MACHINE
**The condition they name is the reproduction instruction. Use it.**
**Why:** S152. "Only at higher game speeds" filed as unreproducible for months. It is a structural
clue: the field tick fires once per rendered frame at every speed, so a speed-only symptom means
the defect sits on a hook driven from *inside* the sim loop, where the call rate does multiply.

### L-12 A TESTER'S PLAY MEASUREMENT IS EVIDENCE
**Put the uncertainty in the rounding, not in refusing the number.** Detail:
`feedback_tester_measurement_is_evidence.md`.

### L-13 THE LOG CORPUS IS `<game>\x64\logs\` — `Tester Logs\` IS NOT
Ours is the default evidence and is always readable. A tester log is opened only on a reported issue
**and** an explicit pointer from the user, and never mixed into a corpus-wide count.
**Why:** S147 reached for the wrong corpus, found nothing, and concluded a defect was "not
measurable from any archived log". It was sitting in nineteen of our own twenty.

### L-14 NEVER ASK THE USER TO READ THE SCREEN
The user is blind. Detail: `feedback_never_ask_user_to_read_screen.md`.

---

## Reading a log
`TAG:logreading`

### L-15 GREP THE LOG'S OWN ANOMALY LINES FIRST
**Why:** S126 — four defects were already printed in the log nobody had read that way.

### L-16 AN ABSENT LOG ENTRY MEANS NOBODY WROTE ONE
It does not mean the event did not happen. **Fix the missing log line before theorising about the
silence** (S127).

### L-17 SPEAK-OUT LOGS WHAT WAS SENT, NOT WHAT WAS HEARD
**Why:** S127. A line in the log proves the string reached Tolk, not that it was spoken.

### L-18 AN ORPHANED DIAGNOSTIC MAKES ITS OWN BUG INVISIBLE
**Why:** S77. The instrument that would have caught it had been disconnected.

### L-19 A DIAGNOSTIC WITH NO BOUND ON ITS OWN CONFIDENCE WILL EVENTUALLY INDICT CORRECT CODE
**Why:** S93. The crossing oracle attributed departures to the nearest seam with no distance
ceiling, and confidently blamed correct code for a gate-crystal teleport that crossed no seam.

---

## Hooking and reading game state
`TAG:hooking`

### L-20 A DETOUR'S ARITY MUST MATCH THE GAME FUNCTION'S
**Count the parameters from the CALLEE's decompile.** `InstallTyped` cannot catch a mismatch.
**Why:** the shop crash (S129), root-caused from a minidump — the log ended clean. Detail:
`feedback_hook_arity_must_match.md`.

### L-21 HOOK THE WRITER OF THE STATE, NOT A READER OF IT
The function that writes the value *is* the event. **The writer of the state names the state**
(S148).
**Why:** the house fix for "the game must fire an event somewhere". Detail:
`feedback_games_fire_events_check_trace.md`.

### L-22 ⟲ ASK WHETHER A GAME FLAG IS AN EDGE OR A LEVEL BEFORE RE-ARMING ON IT
**Why:** S149. `widget+0xC0` is set by one call and cleared by the next — re-arming on it produced
speech at FPS/2 forever. A field the game clears itself will fire again immediately.

### L-23 "ONCE PER FRAME" MUST SAY WHICH FRAME
Rendered frame and sim tick are the same thing only at 1x.
**Why:** S152. `FUN_0022a770` *contains* the sim loop, so a hook on the outer function fires once
per rendered frame at any speed, while anything reached from inside fires 2x/4x as often. Every
"per frame" comment in the mod predates this distinction.

### L-24 A HOOK ON A PER-FRAME FUNCTION IS FINE; DERIVING BEHAVIOUR FROM THE FRAME COUNT IS NOT
And **not every counter is a clock** — bounded *work* and *log-volume* budgets are correctly counts.
Full inventory: `Docs/PerFrameAudit.md`.

### L-25 VALIDATE A TYPE BEFORE DEREFERENCING IT
"Has an in-module vtable" proves almost nothing. Require vtable **plus** a back-reference.

### L-26 RVA-FIRST, NO FISHING; RESOLVE NATIVES BY BEHAVIOUR, NEVER CALL ONE
Ghidra decompile uses ABS: **abs = RVA + 0x120000**. Detail: `feedback_rva_first.md`,
`feedback_resolve_natives_by_behaviour.md`, `feedback_verify_rva_arithmetic.md`.

---

## Editing code that already works
`TAG:refactor`

### L-27 MOVING ONLY THE ARM AND THE COMPARE CHANGES THE PREDICATE
**When converting a counter to a clock, port the DISARM too.**
**Why:** S152. The beacon's stray counter is zeroed the instant the player is back on route, so it
measures one *continuous* run. A deadline without the matching clear silently becomes "was stray at
some point in the last 750 ms" — looser than the code it replaced.

### L-28 AN EQUALITY TEST ON A COUNTER THAT CAN NOW OVERSHOOT LOGS NOTHING AT ALL
**Why:** S152. A one-shot notice keyed on `count == cap` stops firing the moment the cap can be
passed. The diagnostic disappears while the diff looks like a no-op.

### L-29 WHEN YOU DELETE A GATE, FIND WHAT IT WAS LOAD-BEARING FOR
**Why:** S126.

### L-30 DO NOT BREAK WORKING BEHAVIOUR IN THE NAME OF SIMPLIFICATION
If two paths genuinely cover different cases, **keep both and arbitrate**. Only delete a path once
you have shown the survivor covers everything it did.
**Why:** collapsing two detectors "because it looked tidier" silenced the notice board's navigation
outright — the surviving detector's cursor field did not track that surface.

### L-31 A REVERT JUSTIFIED BY ABSENCE NEEDS A LOG WHERE THE CHANGE COULD HAVE FIRED
**Why:** S111.

### L-32 CENTRALIZE: EXTEND THE EXISTING UTILITY, DO NOT ADD A SECOND PATH
Ask "what already does this?" before "how do I do this?". Choke points:
`Speech::Output`, `Log::WriteW`, `GameText::Decode`, `MemRead::*`, `Hooks::InstallTyped`,
`Phrase::Get`, `BattleState::DefName`.
**Why:** two speakers with two interrupt policies WILL race — the notice board's plainer line won by
125 ms and truncated the real one, with nothing in the code looking wrong.

---

## Anything that makes the mod speak
`TAG:speech`

### L-33 NO DEDUPLICATION OF SPEECH
Two exceptions only: the user asked **in the current conversation**, or it guards a per-frame
function **named in the comment**.
**Why:** dedup makes the mod go silent when the player re-enters a surface. A repeat is an
annoyance; silence is a lost position. **A repeat from an event-driven hook is a second call path —
find it, do not filter it.** Detail: `feedback_no_dedup_rule.md`.

### L-34 NEVER SPEAK FILLER — BE SILENT
Empty slot, absent guest, missing target: say nothing. Diagnostics go to the **log**.

### L-35 NEVER FABRICATE A USER-FACING LABEL
Read the game's own string. "The game data can't answer it" means **ship nothing**, not infer.
The phrasebook existing is not a licence to invent words; adding to it needs permission.

### L-36 DECLINING TO SPEAK IS NOT THE SAME AS STAYING SILENT
**Why:** S89 — an empty category spoke the *previous* row's text.

### L-37 REMOVE DEAD FALLBACKS — SILENCE BEATS WRONG SPEECH

---

## Process, docs, and closing out
`TAG:process`

### L-38 WHEN YOU DISPROVE SOMETHING, STRIKE IT — DO NOT JUST ADD A NEWER ENTRY
A stale claim left standing as fact will be re-derived and re-shipped. State what was wrong, the
evidence, and what replaced it — in `GameArchitecture.md` **and** `debug.md`.

### L-39 SHIP THE INSTRUMENT WITH THE FIX
**Why:** S148, and applied again in S152 — the fix and the measurement that will confirm it should
land together, or the confirmation never happens.

### L-40 `git status` BEFORE STAGING — AN UNCOMMITTED FILE IS INVISIBLE TO A GREP OF THE SESSION LOG
**Why:** `5a388c8` staged a header but not its `.cpp`, leaving a shipped gate unversioned and
unlogged until S151 found it.

### L-41 SESSION NUMBERING: GREP THE LOG AND CHECK `git log` FOR AN UNLOGGED SESSION
`N` is global and monotonic. Session 110 has no entry — do not reuse it. Never squash the paired
commits listed in `MEMORY.md`.

### L-42 A RELEASE IS RECORDED IN `release_procedure.md`, NEVER AS A `## Session N` ENTRY

### L-43 BYTE-EXACT COMPARES MUST RUN IN BASH
PowerShell's `>` re-encodes and adds a BOM; it faked a converter mismatch at V0.6.2.

---

## Timing and performance
`TAG:timing`

### L-44 SLOWER THAN VANILLA = OUR CODE
First suspect: our lock in their hot path. Detail: `feedback_stall_is_always_mod_side.md`.

### L-45 USE A `GetTickCount64()` DEADLINE, NOT A FRAME COUNTDOWN
House idiom: `constexpr uint64_t kThingMs`, a `uint64_t` stamp where **0 means not armed**, compare
`(now - stamp) >= kMs`. One `now` per frame function. `StallProbe::Now()` is **QPC profiling ticks**
and is deliberately not the wall clock — do not repurpose it.

### L-46 THE CONSOLE OUTPUT BUDGET IS AN ACCESSIBILITY REQUIREMENT
Hundreds of lines at once crashes the screen reader. Console output must be O(unique events).

### L-47 MEASURE BEFORE TUNING — BUT SEE L-08
A constant "tuned" against an unverified number is a guess wearing a measurement's clothes. Every
cell of the audit's `at 144 fps` column was arithmetic on an assumption nobody had checked.

---

## Scope, tooling and environment
`TAG:scope` `TAG:tooling`

### L-48 NEVER WIDEN ONTO A WORKING MAP
A fix for one map must not loosen behaviour where things already work.
Detail: `feedback_never_widen_onto_working_map.md`.

### L-49 SOLVE GLOBALLY, NEVER PER-MAP
No per-map special cases, no learned caches. **Do not reintroduce a learning cache for anything the
game itself can be read for.** Detail: `feedback_global_not_per_map.md`, `feedback_no_learned_labels.md`.

### L-50 A HIDDEN MOD-MENU ROW READS AS OFF
The test lives in the visibility predicate, not the value.
Detail: `feedback_hidden_menu_row_reads_off.md`.

### L-51 THE CONFIG SCREEN IS NOT PROOF A KEY IS FREE
**The game owns `F9`.** Check the on-screen keyboard overlay. The beacon is `F11`, bare press only.
Left Ctrl is an Escape TOGGLE — check `Docs/Controls.md` before instrumenting input.
Detail: `feedback_config_screen_is_not_key_freedom.md`, `feedback_check_controls_md_before_input_diag.md`.

### L-52 TOLK IS THREE SEPARATE RULES — DO NOT COLLAPSE THEM
Deploy never copies it · the build never links it · **the release zip DOES ship it** (x64).
Conflating the first with the third once cost a release its TTS DLLs.
Detail: `feedback_no_tolk_bundle.md`.

### L-53 MENU READING: PROBE-CONFIRM THE CONTROLLER, NEVER READ AN INACTIVE MENU
Use the universal cursor hook rather than a bespoke one; pull master data rather than inventing it.
Detail: `feedback_universal_menu_hook.md`, `feedback_no_inactive_menu_reads.md`,
`feedback_probe_confirm_menu_controller.md`, `feedback_extract_master_data.md`.

### L-54 FRIDA IS ATTACH-ONLY, AND IS FOR CONFIRMATION, NOT DISCOVERY
Discovery happens in the decompile first. Two exceptions permit Frida-side discovery: genuinely
runtime-only state, or a path proven 100% dead. **`PS2Data\` is LIVE** — a directory name, a missing
string literal and a PS2-era symbol are each *not* evidence of deadness.
Detail: `feedback_frida_attach_mode_only.md`.

### L-55 THE TITLE SCREEN IS NEVER IDLE
Detail: `feedback_title_screen_not_idle.md`.

### L-56 GHIDRA: DISABLE PE RTTI AND NON-RETURNING-DISCOVERED
Both analyzers misbehave on this binary. Headless only; never a timeout flag; never
`-import -overwrite` after the first import. Detail: `project_ghidra_buggy_analyzers.md`.

### L-57 `.bat` FILES NEED CRLF; VALIDATE BEFORE BUILDING
Detail: `feedback_bat_files_crlf.md`, `feedback_validate_before_build.md`.

### L-58 STRING-PULL ONLY ON A REGULAR STAIRCASE
Detail: `feedback_stringpull_only_regular_staircase.md`.

---

## Maintaining this file

- **One lesson, one id.** Ids are stable and may be cited from commits, `debug.md` and memory files.
  Never renumber; retire an entry in place if it is superseded.
- **Add a lesson when it is GENERAL.** A fact about FFXII goes to `GameArchitecture.md`; a specific
  defect goes to `debug.md`; only the transferable reasoning belongs here.
- **Mark it `⟲` the second time it happens.** That mark is the signal to check it first.
- **Keep the routing table current** — it is the only part read in full.
- **This file replaces lesson TEXT in `MEMORY.md`.** The index keeps state and pointers; when you
  would have added a lesson paragraph to `MEMORY.md`, add it here instead.

### Not yet migrated
Session-specific lessons still living only in the memory topic files (navigation-heavy: S64–S124
route/seam/funnel findings). Move them here as they come up rather than in one pass — a lesson is
worth migrating at the moment it is needed again, which is also when its wording gets tested.
