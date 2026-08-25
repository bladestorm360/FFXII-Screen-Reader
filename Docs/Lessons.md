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
| about to state a conclusion, an RVA, an offset, a cause | `TAG:concluding` | L-01…L-09, L-59, L-64, L-69 |
| a tester reported something | `TAG:tester` | L-10…L-14 |
| reading a log to find out what happened | `TAG:logreading` | L-15…L-19, L-61, L-62 |
| adding/changing a hook, or reading game state | `TAG:hooking` | L-20…L-26 |
| editing code that already works | `TAG:refactor` | L-27…L-32 |
| anything that makes the mod speak | `TAG:speech` | L-33…L-37 |
| writing docs, committing, closing a session | `TAG:process` | L-38…L-43, L-67, L-68 |
| something is slow, or timing-dependent | `TAG:timing` | L-44…L-47, L-60, L-65 |
| how wide should the fix be; is this key free | `TAG:scope` | L-48…L-51, L-63, L-66, L-70, L-71 |
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

### L-59 ⟲ SCOPE A RULE BY THE MEASURED DATA, NOT BY WHAT YOU BELIEVE THE POPULATION IS
**"Only ask this of X" is worthless if your test for X is an assumption.**
**Why:** S153, the third failure on one bit. S148 pruned the entity list on `sceneObj+0x14 & 0x40`;
S150 caught it deleting a Save Crystal and scoped it to `isCharacter`, reasoning *"a crystal, a gate,
a door and a treasure are not characters"*. A gate crystal **is** scene category 5-7, so the gate
excluded nothing — the shipped filter went on deleting a gate crystal and four named NPCs (153 wrong
drops against 44 right ones in one session). The fix scopes by the **byte shape the state was
measured in** (`0x80` set, `0x40` clear) instead of by a belief about object kinds — and the
discriminating bit had been written down in `nav_rva.h` since S150 with nothing acting on it.
**Corollaries:** narrow in the direction where being wrong is cheap (a stale list entry beats a
deleted landmark); and when a filter has never been observed to hit its target, that is the finding.

### L-60 ⟲ A SHARED BUDGET MAKES ONE NET'S NOISE INTO ANOTHER NET'S SILENCE
**Give every instrument its own budget. A noisy channel must only ever starve itself.**
**Why:** S156. One 400-line budget served a variable diff and an object diff; wandering enemies burned
362 lines on yaw jitter and the cap closed **22 seconds before** the event the whole visit existed to
capture. The log then looked like a clean negative result rather than a blinded run.
**Corollaries:** compare floats with an epsilon, never `!=` (a denormal reads as a change every
frame); retire a channel that moves on consecutive samples instead of letting it spend the budget;
and never truncate silently -- log what was dropped.

### L-61 THE LOG FILE TRAILS THE RUNNING GAME
**Check the file's mtime against the wall clock before concluding something did not happen.**
**Why:** S156, twice in one session. Both times I read the log, saw nothing past a point, and told the
user the event was missing; the writes had simply not landed yet. This is L-16's other half -- an
absent entry may mean nobody wrote one *yet*.

### L-62 A BUILD STAMP COMPILED INTO ONE TRANSLATION UNIT IS STALE ON AN INCREMENTAL BUILD
**Prove which binary is deployed by comparing it with the build output, not by reading its banner.**
**Why:** S156. `Build: … compiled <time>` is `__DATE__`/`__TIME__` in `logger.cpp`, which does not
recompile unless it changes. I used it as evidence that the user had not run a fix. They had, and it
had genuinely failed -- so the stamp cost a real defect a round of denial. `cmp` on the two files
settles it in one line. Related: L-10, read the build first -- but read it from the bytes.

### L-64 ⟲ BEFORE STRIKING A CLAIM, CHECK WHETHER IT AND ITS REFUTATION ARE ACTUALLY EXCLUSIVE
**A correction is a conclusion and carries the same bar as the thing it corrects. Do not strike a
standing claim on one remark, and do not generalise a strike into a rule in the same breath.**
**Why:** S157, a full round trip inside one session. S154 inferred from `mrm_c01`'s routine-name pool
(`BOSS_…`, `EventDirector`, `PlayerJack*`) that it was *"the boss/event room"*. I repeated that to the
user as if measured. They pushed back — *the boss room is what this puzzle unlocks, so no guardian
stands in it* — and I struck the claim across three documents and wrote it up as a general lesson
about name pools. **Then they played it: it is the boss room AND the third guardian's room.** The
refutation was an argument about PROGRESSION and never excluded the two being one room; nothing in
either account required "boss room" and "guardian room" to be different places.
**The tells, both present:** an "A, therefore not B" where A and B were never disjoint; and a brand-new
`L-` entry whose entire evidence is one unverified exchange. A lesson wants a measurement behind it,
not a conversation.
**What is genuinely left:** the room identity was never MEASURED in either direction — only the map id
and the two save-block cells matter, and both come from standing in the room. Related: L-01 (a sample
is not a population), L-09 (state the scope you measured).

### L-69 A FIELD THAT MEANS DIFFERENT THINGS IN DIFFERENT STATES WILL HAND YOU A PLAUSIBLE WRONG NUMBER
**Read the state byte BEFORE you read anything the state byte governs. A reused field does not fail
loudly — it answers, and the answer looks reasonable.**
**Why:** S163, the Draklor lift. The message widget's `+0xA2` is the option count in mode 2 and the
digit width of the maximum in mode 4; the lift read **2**, which is exactly what a two-option Yes/No
reads. `+0x58` is a row cursor in mode 2 and a packed spinner state in mode 4; it moved eleven times,
so the cursor "worked". `+0x54` is a park reason in mode 0 and the selected value in mode 4; it read
**66**, and the log printed it under the label `wait=`. Three fields, three plausible numbers, no
error anywhere — and a whole session spent hunting a `0x0E` block that was never on the page. The
mode byte was in the same log line the entire time, reading 4 on exactly the two pages that were
broken and 0 or 1 on every page that worked.
**Corollaries:** a log LABEL is a claim about meaning, so a field whose meaning varies must be
printed with the state that fixes it (`mode=4 value=66`, never `wait=66`); and when a documented
field description has no "in mode N" on it, treat that as unscoped rather than universal (L-09).

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

### L-67 GREP EVERY HIT FOR A KEY BEFORE DECLARING README COVERAGE — THE GENERIC ENTRY SURVIVES, THE SPECIFIC ONE ROTS
A key is often described in **two** places: a loose sentence in prose, and the enumerated entry in
the key list. They rot at different rates, and **the reassuring one is the one that never goes
stale**, because it commits to nothing.
**Why:** at V0.6.4 the `o` audit found `README.md:17` — *"O reads the rest of what Libra reveals"* —
reasoned correctly that a line about what a key is FOR survives changes to what it says, and stopped.
`README.md:231` enumerated the readout clause by clause and still promised *"MP where the enemy has
any"*, which S160 had deleted three commits earlier. The record shipped claiming "no gaps". One hit
that reads well is the failure mode; count the hits before trusting any of them.

### L-68 ⟲ A FLAG THAT SURVIVES TWO RECORDS IS A FIX YOU DECLINED TO MAKE
Recording a known rule violation is not respecting the rule. **If the same defect appears in two
consecutive release records or session logs, fix it or state plainly why it cannot be fixed** — a
third flag is not diligence.
**Why:** *"New in this build: …"* violated `CLAUDE.md`'s no-changelog rule, was flagged in the
V0.6.2, V0.6.3 **and** V0.6.4 records, and shipped in all three. The stated reason — "readme edits
are a separate commit made *before* the trigger" — is a rule about **scope**, meant to stop a release
quietly rewriting the readme; it was misread as a prohibition on fixing anything. The user removed it
in one sentence at the V0.6.4 trigger. **Related:** the changelog is the USER's artifact and it lives
on Discord — the readme describes the mod as it is now.

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

### L-65 ⟲ TEXT READ FROM THE PAINT CACHE IS ALWAYS ONE EVENT BEHIND — READ THE GAME'S OWN ARRAY
**`TextCapture`'s item map is filled per row per PAINT, and a focus message arrives before the paint
it belongs to. Any surface whose CONTENT changes without the cursor index changing will therefore
report the previous content. Fix it by reading the list's own row array, not by deferring the
announcement until the paint you hope is the right one.**
**Why:** three times now. S89 — an empty category spoke the previous category's row. S150/S151 — the
off-hand list, where the shape of the container was the question. S158 — the gambit action picker
spoke the previous category's first row on every switch, and the condition list's first row when the
action list opened over it; the picker's own row array was filled *before* the cursor moved, so
reading it removed the timing question instead of narrowing it. **A deferral is a bet that the next
paint is the one you want; the array is the answer.** Corollary: a reader that has no measured layout
is not thereby excused — S94 declined this surface as unmeasured and left it on the cache for 64
sessions, which is 64 sessions of a list reporting the wrong row.

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

### L-63 A MEASUREMENT ONLY THE PLAYER CAN TAKE IS A QUESTION, NOT A DESIGN CONSTRAINT
**When a feature needs a number that can only come from the user going somewhere in the game, ASK
FOR THE TRIP. Never route around it by shipping the part that works and reporting the rest as a
scope limitation.**
**Why:** S157. The statue readout needed `mrm_c01`'s two save-block offsets, obtainable only by
standing in that room. The user had *already asked outright* whether a visit to the third statue was
needed; the answer given was no, on the reasoning that the flag makes a solution table unnecessary —
true, and irrelevant, because not needing to know the correct *facing* says nothing about knowing the
*address*. What shipped was a 2-of-3 solver whose third line says "state unknown", against a feature
whose entire premise is *press one key, hear all three*. The user's verdict: *"needing a new variable
address is a reason. you should have just said yes and I would have obtained it."*
**The tell:** you are writing the words "self-measures on first entry", "learned automatically", or
"fills in later" about the deliverable the user actually asked for. That is a trip you decided not to
ask for. A capture the user can run in five minutes is cheaper than a partial feature plus the
session that finishes it.
**Not in conflict with L-49:** learning a value the *game* can be read for is banned; asking for a
one-off measurement of an address that exists nowhere else is just doing the RE.
**Corollary, same session, same failure family — WITHHOLDING A NUMBER IS NOT AUTOMATICALLY THE
CONSERVATIVE CHOICE.** Map 603's target facing was withheld as "ambiguous" (its flag set at facing 3,
then the facing moved 3→4 with no clear). But the rule was uniform across all three statues, and the
3→4 move is only anomalous if you forget that the statues LOCK on completion — the game says so in
words. Confirming it would have meant unsolving a finished puzzle. **Turn-by-turn directions were the
entire point of the feature; a statue reporting only solved / not-solved is the feature not working.**
Weigh the cost of the doubt against the cost of the gap: the 0.98 bar is there to stop wrong
*assertions*, not to license shipping something that does not do the job.

### L-71 ⟲ A CORRECTION APPLIED TO ONE MEMBER OF A DIVERGENT PAIR IS NOT APPLIED TO ITS SIBLING
**When you learn that two things have different layouts, fix EVERY reader of both, not the one that
raised the alarm. Grep for the discriminator and check each site.**
**Why:** S164. S76 established that FFXII has two interactable classes with different field layouts and
warned in those exact words that "reading one class's offsets on the other returns plausible-looking
floats that are simply wrong". It then made `ReadBandFor` class-aware and left `ReadReachFor` — the next
function in the same file, reading the same node — running class-3 ellipse arithmetic on every target.
Eighty-eight sessions later that returned `reach=0.50` for a class-1 terminal, the router looked for
somewhere to stand within 0.50 m of a point that is 0.69 m off the navmesh, found nothing, and said
**"No path" about a target the game was offering an ACTION on** — identically from 0.7 m away and from
48 m away. The header above the struct even documented the model as class-3 (`FUN_0025bad0`) while the
function below it applied that model unconditionally.
**The tell:** a file that branches on a discriminator in one place and not in another. `ObjectClass`
had three call sites; two consulted it and the third, the one that fed the router, did not.
**And the second half — the answer may be "this class has no such quantity".** The class-1 scorer has
no radius at all: band, mode bit, cone, then nearest-wins on a bare squared distance. So the fix was not
to compute the reach differently, it was to stop claiming there is one. A replica whose subject does not
exist reads as a number, never as an error. Related: L-69 (a field that means different things per
state), L-02 (the 0.98 bar).

### L-70 ⟲ AMBIENT STATE SET AROUND A CALL IS VISIBLE TO EVERY CALL THAT CALL MAKES
**A thread-local opened for one operation belongs to that operation, not to the tree beneath it.
Before adding one, ask what the callee calls.**
**Why:** S163. `GameText::NumericFieldScope` hands the decoder a widget's live value so a `0F 2D`
escape can render it. The scope wrapped `DecodePages` -- and `DecodePages` ends by running
`ResolveSprites`, which calls `BattleState::ElementName`, which decodes a pool string of its own.
A nested decode, inside the scope, able to pick up a value that was never its. Fixed by consuming
the scope at the first escape that uses it and disarming it before the sprite pass -- the field
belongs to the byte loop, and saying so in code is what makes it true.
**The `⟲` is earned:** `ElementName` ALREADY carries a `s_resolving` thread-local re-entrancy
guard, written for exactly this shape when the sprite resolver went in. The project had solved
this once and I reintroduced it one layer up.
**How to apply:** an RAII scope over a call is only safe when the call is a leaf. When it is not,
either consume the state at its single intended use, or bound it to the frame that owns it -- do
not leave it armed for whatever the callee decides to do. Caught in a self-audit before shipping,
which is the only reason it is a near miss and not an entry in `debug.md`.

### L-66 BELT AND BRACES IS ONLY INSURANCE IF THE BACKUP IS MEASURED TO BE MORE RELIABLE
**A second gate on a second, unmeasured flag is a second failure mode, not redundancy.** Before
adding one, state what each flag reads in EACH state you are separating. If you cannot, you are not
hardening the decision — you are widening it.
**Why:** S156 gated `o`'s Libra branch on `IngameMenuReader::BattleCommandActive()` as belt and
braces over `SpeakTargetDetail`'s own target-cursor gate, reasoning that the first reads an
*inferred* HUD flag while this one reads "the mod's own knowledge of which surface the player is
on". It does not read that. It means "the command panel is alive and was the last thing to take a
menu focus" — and the target cursor is not a menu focus surface, so nothing clears it, while the
panel stays alive behind the cursor so its liveness re-validation passes too. **The flag was true
for the whole aiming phase, which is precisely when Libra is the question.** S159 measured it: nine
`o` presses, all declined, interleaved with `ResolveTarget … BROWSING enemy`. Across the entire
corpus the refusal line appears in one log and its fall-through in none — **the gate never once
fired in the case it was written for, and cost the feature outright.**
**The tell:** the new gate is justified by distrust of the old one, and the sentence "if X turns out
to mean something other than Y, we are still protected" appears without a measurement of either.
Liveness catches a DEAD object, never a live one the player has navigated away from.
**Corollary — CHECK WHETHER THE FIX YOU ALREADY SHIPPED MADE THE SECOND ONE UNNECESSARY.** S156
fixed this defect twice in one session: reordering to description-first (which worked — 18
`describe:` lines, zero wrong Libra) *and* this gate. The second was never needed, and only it could
regress. When two fixes land together, say which one you expect to do the work.
**Related:** L-03 (the flag's own comment asserted the meaning it did not have), L-64 (S156's strike
of `P+0x10F78` was recorded against no log that could have tested it — see S159).

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
