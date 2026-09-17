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
| about to state a conclusion, an RVA, an offset, a cause | `TAG:concluding` | L-01…L-09, L-59, L-64, L-69, L-72, L-73, L-74, L-76, L-79, L-80, L-85, L-86, L-87, L-90 |
| a tester reported something | `TAG:tester` | L-10…L-14, L-77, L-91 |
| reading a log to find out what happened | `TAG:logreading` | L-15…L-19, L-61, L-62 |
| adding/changing a hook, or reading game state | `TAG:hooking` | L-20…L-26, L-83, L-89 |
| editing code that already works | `TAG:refactor` | L-27…L-32, L-81, L-82, L-84 |
| anything that makes the mod speak | `TAG:speech` | L-33…L-37 |
| writing docs, committing, closing a session | `TAG:process` | L-38…L-43, L-67, L-68 |
| something is slow, or timing-dependent | `TAG:timing` | L-44…L-47, L-60, L-65, L-75, L-88 |
| how wide should the fix be; is this key free | `TAG:scope` | L-48…L-51, L-63, L-66, L-70, L-71, L-78 |
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
**S174 again, on the ABSENCE side, and this is the form that bites hardest.** The pad survey dedups
per distinct value. Its `L3` and `R3` lines were missing from the first real pad log, and that was
read as "the thumb-click bits never reach the mod" -- a hardware diagnosis, stated with confidence,
carried into a plan. The player had simply not pressed them. **The refutation was two lines above
the code being read**: *"an absent line means that value never occurred, NOT that the control was
never pressed. Presence is evidence; absence is not."* Reading the comment on the instrument is not
the same as applying it to the output.

### L-05 CHECK WHAT AN INSTRUMENT EMITS, NOT WHAT IT MEASURES
**Why:** S152. An entire measurement plan ("one play log at 1x and 4x settles it, no code") was
built on `FrameTick`, which measures exactly the right quantity and then throws it away below a
threshold. The discard is invisible in the function name and in its header comment.

### L-06 ⟲ A NEGATIVE RESULT IS ONLY AS GOOD AS THE SHAPE YOU SEARCHED FOR
**Why:** S147. "Not found anywhere" meant "not found in the shape I assumed". **S166 again, and this
one cost a whole session:** S165 searched both Westersand map files for the cactus coordinates
"aligned *and* unaligned, against known-good runtime coordinates as controls" and concluded "the
coordinates are in none of them". They were in the file the whole time, as ordinary floats. The
search looked for an **adjacent (x,y,z) triple**, and a script coordinate is not stored as one: the
float pool is allocated in source order across the whole map, so the cactus's x and z sat eleven and
twelve entries apart from anything else of its own, and its y was not in the pool at all (it is an
immediate operand). Controls prove the search RAN; they cannot prove the shape was right.

### L-07 ⟲ DON'T MODEL THE VERDICT — READ THE WORD THE VERDICT IS READ FROM
**Why:** S149. Sessions were spent reconstructing a license-board reachability rule; the game simply
reads `cell+0x18 & 0x1000`, and `FUN_00323600` has no adjacency test at all.
**S182, and this time the verdict being modelled was OUR OWN.** The Unreachable filter was specified as
"hide what the route key cannot reach", and S179 answered it with a separate flood of the mesh plus a 3 m
ring test. The router was sitting one call away. The two disagreed on the first door the user tried —
Pilgrim's Door 1 "reachable" to the flood, "No path" to `\` from the same spot — and in two whole play
logs the flood never judged one entity unreachable, so the filter hid nothing. A cheaper model of a
subsystem's answer is a second subsystem that will disagree with the first; when the spec names the
answer ("no valid path"), use the answer the thing itself gives -- and if getting it costs the game thread
work nobody asked for, that is a question for the user first (L-88). S182 settled on recording the answer
`\` already speaks. Its silence also
went unnoticed because "reachable" was the unlogged default — L-83's shape.
**Same session, the aggregate form:** S181 read `corridor pays terrain=4000` as "crosses two waterfalls".
A price total names no poly. Every march breach in that log named material-0 ground, not the waterfall
materials. Before attributing a total to a culprit, log the items (`terrain paid:` now does).

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
**THE HASH LIES THE SAME WAY, and worse (S181).** `Build: V0.7 (8db23ed)` is the git hash captured at
CMake CONFIGURE time, so on an uncommitted tree it names the last COMMIT -- a build carrying two whole
new features reported the previous session's hash and the previous day's compile time. Identify a log's
build by a line only the new code could have written (`[SOCHEN]` settled that one in a single grep), or
by `cmp` against the build output. Never by the banner, in either field.

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

### L-85 A LOOKALIKE MATCHES THE SHARED FEATURES. IDENTITY IS DECIDED BY THE ONE THAT DIFFERS
**Before you call a known function "the surface in the report", list the report's features, find the
one the candidate would NOT have if it were the wrong thing, and check that one first. For a
script-driven screen, grep the `.dbg` native names for the screen's own words before reading any
function body.**
**Why:** S178. S72 described the hunt reward panel as *"a title line, a rule under it, then one row
per reward, with a separate quantity column"*, and said it was not the obtain toast. S147 read the
toast's body, found a row loop, a quantity field and a gil kind, and struck S72 as "wrong on both
counts". That claim then stood across three documents and a code comment for thirty sessions. The
feature S72 listed FIRST, the title, is exactly the one the toast lacks. Our own log showed that
function composing `"You obtain a Wind Globe!"`, with no title anywhere. The real panel was one
grep away, on the words the panel itself shows (`gil`, `win`, `clan` in `dbg_symbols_evctrl.csv`):
`questresultwindow`. (`reward` and `hunt` matched nothing, because the engine calls a hunt a quest.) That name then
checked out against `action_binding_tables.txt` and a two-hop call graph.
**The tell:** the evidence for the identity is a list of things the candidate HAS. Nothing on the
list is a thing the alternative would lack. Related: L-64 (a strike carries the same bar as the
claim it strikes), L-06 (a search is only as good as its shape).

### L-86 A MECHANISM RECORDED FOR A CLASS OF OBJECT IS A HYPOTHESIS ABOUT EACH MEMBER. READ THE MEMBER'S SCRIPT
**Before declaring "the mod cannot see X", open the script of the specific X in front of you and list
what it CALLS. A doc row that says "doors are dynamic prims" describes one mechanism some doors use,
not the mechanism this door uses.**
**Why:** S179. Two research passes concluded, correctly from the docs, that nothing the mod reads sees a
closed door: the prim table files doors under `>= 0x5000`, and no query for those is at the bar. One
offline disassembly of the door routine the log had already named (`gim_door01`) showed
`setmapidfloor` ×7. That native writes the walkmap material override bank, which the mod has applied
through `EffectiveFlags` since S96. The door had been in the mod's own flags the whole time. The log
even printed them: `goal … eff=0x0FA07000`, material 3, on a route to door 3.
**The tell:** the reasoning runs from a CATEGORY NAME ("door") to a mechanism, and never touches the
one member whose routine name is on the screen. Related: L-06 (the shape you searched for), L-85
(identity is decided by the feature that differs), L-01 (a sample is not a population — the census of
824 callers is what stopped this becoming "every setmapidfloor caller is a door").

### L-87 A CANNOT-BE-ANSWERED-OFFLINE CLAIM EXPIRES WHEN THE PARSER UNDER IT IS FIXED
**When a doc says "the file cannot tell us X", check what base or parser that was concluded with. If
either has been corrected since, the claim is void until re-tested. Test it against a value already
measured live.**
**Why:** S154 wrote "the variable descriptor table is not reachable offline" after reading map scripts at
the FILE base. S137 had already moved the base to `file+0x80`, and S166 rewrote the disassembler on it,
but the claim stood. It sent the Stilshrine statues to runtime measurement (S156-S157) for cells that
were in the extracted file. S180 re-tested it in one step: `mrm_b03` var `0x0D` decodes offline to
`+0x9B1`, exactly S156's live measurement. The whole Sochen puzzle was then solved from scripts alone.
**The tell:** a negative capability claim ("not reachable", "cannot answer") older than a format fix it
depends on. Related: L-38 (strike, do not append), L-08 (map-data questions are decompile questions).

### L-90 A LATER FAILURE TO REPRODUCE AN ANSWER IS NOT A RETRACTION OF IT
**When the mod has already given the player an answer they are acting on -- a route they are walking --
a re-derivation that fails from a worse vantage point must not delete it. Only a failure that is a fact
about the GOAL (proven disconnected, a barrier the game has since declared) may end it. "I could not work
it out again from here" and "it is no longer true" are different results, and only the second one is news
for the player.**
**Why:** S183. The beacon re-planned silently whenever the player was slow or off the line, and the drain
seeded the beacon with whatever came back -- so a re-plan from beside a door frame, where the first leg's
straight line clipped the frame and every repair failed, STOPPED a route the player was walking. The log
said three times in five minutes, in plain text, that the mesh still connected them to the goal and the
search had given up. The comment on the line that did it called the empty list "also the right answer for
a failed silent re-plan". The user: *"pathways should never become suddenly invalid mid walk, that is a
bug."*
**The tell:** a refresh path that REPLACES a live result with its own output unconditionally, including
when its output is a failure. Ask what the failure actually proves before letting it overwrite anything.

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

### L-72 A RECOVERED TABLE'S MACHINE-READABLE COLUMNS OUTRANK ITS HUMAN-READABLE ONES
**When a dumped table has both a name column and a typed column, and they disagree, the typed column
is the measurement and the name is a label. Line them up before trusting either.**
**Why:** S166. `DAT_01efea60` gives every opcode a mnemonic pointer at `+8` AND an operand-KIND field
at `+2`. The names are indexed one off; the kinds are not. Six names contradict their own kind at
their own index (`0x50` is called JMP and carries kind 5 = "float constant"; `0x01` is called TAG and
carries kind 8 = "label"). The kind column also happens to be exactly what a disassembler needs, so
resolving operands by kind instead of by mnemonic made the tool both correct and shorter.
**Corollary:** the archive had recorded the same off-by-one a session earlier from a completely
different argument. Two independent derivations agreeing is what 0.98 looks like.

### L-73 AN ERRATUM RECORDED IS NOT AN ERRATUM APPLIED
**Writing down that a tool is wrong does not fix the tool. Either fix it in the same pass or the note
becomes a thing future sessions read AFTER being misled by the code it describes.**
**Why:** S166. `GameArchitecture.md` already carried "**`notes\athena_opcodes.md` (and
`tools\ebp_disasm.py`'s `OP` dict) map name-index to opcode DIRECTLY; the real opcode is name-index
+ 1**" — correct, specific, naming both files. Neither file was touched. The next session to open the
disassembler read the wrong table out of it, and the plan written to fix that disassembler
(`ebp2_disasm_fix.md`) never cited the erratum at all. **Grep the docs for the FILE you are about to
trust, not only for the subject you are working on.**

### L-74 INSTRUMENT THE POINT OF USE, NOT THE POINT OF COMPUTATION
**A diagnostic that prints a value where you WORK IT OUT certifies your arithmetic and nothing else.
If anything downstream can overwrite the field, only a log line at the point the value is SPOKEN or
ACTED ON can tell you it survived. Before declaring a resolved value shipped, grep for every writer
of the field you put it in.**
**Why:** S166/S167. S166 taught the scan to recover a script-placed object's position from `setrect`
and proved it with a diagnostic — `nearest "Dynast-Cactoid" 39.20m`, arithmetically perfect, computed
inside the scan. `EntityList::RefreshPositionsLocked` then re-read the object's transform before every
command and put `e.pos` back to (0,0,0), because for these objects that read SUCCEEDS and returns the
origin. **Both lines are in the same log:** the correct 39.20m at scan time, and "South, 574 steps
(below)" — the distance to the world origin — nine seconds later. The feature was written up as
working-but-unplayed when it was already measurably half-undone by its own process. Cousin of
[L-71]: one writer of the field was corrected and its sibling was not.

### L-75 A PERMISSIVE COST MODEL TURNS AN INVALID GOAL INTO A PLAUSIBLE ROUTE, NOT AN ERROR
**Where a search PRICES an obstacle instead of cutting it, handing it a goal it should have rejected
produces no failure anywhere -- it produces a confident, speakable, wrong answer. Validate the goal
before the search, because the search is built not to.**
**Why:** S168. The router prices terrain the party may not stand on rather than cutting it, and that
is correct and hard-won (S96 proved cutting over-refuses; it was reverted twice). A script `setrect`
gave a cactus a Y 10 m under its own ground, `FindPolyAt` has no terrain test and no vertical
tolerance, so the goal anchored on a bit-23 poly beneath the terrain. Nothing errored. A* simply
breached its way to a goal it should never have accepted, the corridor paid `terrain=6000`, and the
mod spoke a five-leg route into a dune face.
**The tell was in the numbers, not the verdict: SEARCH COST THAT DOES NOT FALL AS YOU APPROACH.**
Routes to ordinary targets on that map expanded 1-12 polys. Every route to the cactus expanded
~2680 -- from 240 m away and from 19 m away alike. A goal 19 m off that costs a full-mesh flood is
not 19 m away in the graph, whatever the distance readout says. Compare cost against distance
before believing a route.
**And fix it at the goal, not in the search.** The pricing model was right; the input was wrong. The
repair went where the script coordinate becomes a world position -- one population, no router edit.
**S181 ADDS THE OTHER HALF, by the user's ruling: PRICE WHAT WE INFER, CUT WHAT THE GAME DECLARES.**
Static terrain type is our inference and stays priced (S96 proved cutting it over-refuses). A
SCRIPT-CLOSED floor -- raw class bit clear, effective bit set, i.e. the engine's own override bank
refusing the party after a `setmapidfloor` -- is the game's own declaration, and is CUT: a target
behind one must answer "No path" rather than be handed a route through the obstacle, even when the
list still shows it. Measured twice before the ruling (S179 doors, S181 waterfalls). See debug.md,
"routes buy their way through the waterfalls".

### L-76 WHEN A FIX RESTS ON A PREMISE YOU CANNOT CHECK OFFLINE, SHIP THE LINE THAT NAMES THE BRANCH
**Do not ship the fix and hope. Ship the fix AND the one log line that says which case you are in,
with the branches written down in advance. One play then either confirms it or hands you the next
step already decided -- instead of costing a round trip to find out which.**
**Why:** S168/S169. The S168 repair assumed a script placement's Y was wrong and a standable floor
existed at its own (x,z) -- unverifiable offline, because the walkmap blob is relocated at load and
no extractor reaches it. It shipped with a `placement ground:` line naming both outcomes. The premise
was FALSE on all four cacti across both maps, the log said so in one line each, and the branch it
named -- the object occupies its own coordinates, so project into the interaction circle -- was the
actual fix. **The wrong hypothesis cost one line of log, not a session.**
The distinction from an instrument-instead-of-a-fix (S163): this SHIPPED THE FIX. The line is what
makes a wrong premise cheap, not a substitute for repairing anything.

### L-79 AN OBJECT THE GAME HAS NOT PLACED IS NOT A MISSING FEATURE
**Before building anything to surface an interactable the mod cannot see, establish that the GAME is
offering it. Script-placed objects are created at a story/quest step, and until then the shell is
byte-identical to a live one -- same enable bit, same (0,0,0), same event ids. "The mod cannot see it"
and "it is not there" look exactly alike from the object.**
**Why:** S166-S171. Six sessions and five builds went into forcing the Westersand cacti to list and
route, and the tester was simply not on the quest step that places them; the mod was already pulling
interact prompts correctly. **The first question -- "does the examine prompt appear when you stand
there?" -- was never asked.** The work then regressed shipped behaviour and was reverted in full.
**Corollary, and it is the part that made the chase feel justified: AN OFFLINE CORPUS COUNT IS NOT AN
OBSERVED BENEFIT.** "134 examinable field signs game-wide" came from parsing 769 scripts and was
quoted in four documents as the payoff. **Not one was ever confirmed listed correctly in play**, while
what play actually produced was phantom NPCs and mislabelled exits. A number you derived is a
hypothesis about value; only play makes it a benefit.
See [L-77] for the one-sentence version of step 1, and `debug.md`'s CHAIN OF EVIDENCE for the order.

### L-80 A CONTROL THAT INVALIDATES AN INSTRUMENT IS A CLAIM, AND NEEDS CHECKING FIRST
**A pre-registered test of the form "if X reads negative, the instrument is broken" is only as good
as your belief about X. Verify X before you spend the instrument on it -- otherwise a wrong premise
does not produce a wrong reading, it produces a confident verdict about the TOOL, which is far more
expensive because everything measured with it afterwards is discarded too.**
**Why:** S174. S162 had written the right test in advance: *"If Start and A come back `no-reaction`,
the instrument is early -- not the game silent."* The first real pad log had `survey Start ctx=field
... no-reaction`, so the survey was declared broken and its entire output set aside -- in the same
message that told the user no button could be cleared from it. **Start does not open a menu in FFXII;
it pauses. Triangle opens the menu.** The control was never verified, and it was checkable in one
web search or one question. **Then the fallback evidence went the same way**: "one button logged two
verdicts in one context" turned out to be `pad=0x0080`, which is LEFT in the game's word space, not
R1 -- a direction being held, read as a reaction to a shoulder. Nothing at all survived, and whether
the instrument was early is STILL unmeasured.
Note the asymmetry: the same unchecked premise could as easily have CERTIFIED a broken instrument.
**And note what the two errors have in common -- both decoded a number by assuming what it referred
to.** Start's meaning was assumed; `0x0080`'s bit layout was assumed. The survey now prints the raw
XInput word beside the game's word so the correspondence is read, not inferred.
See [L-01] for the one-sample version and [L-06] for the negative-result version.

### L-78 A RISK YOU CAN STATE PRECISELY ENOUGH TO WRITE DOWN IS NOT COVERED BY WRITING IT DOWN
**An Open-section caveat is not a mitigation. If you can name the failure mode, the object it will
hit and the filter it will pass through, you know enough to guard it or to not ship -- and shipping
it with a note means the note is what future sessions read AFTER the tester hits it.**
**Why:** S166 shipped the script-placement reader with this in its own Open section: *"a
script-placed sign standing near a doorway record could take a tag a real door would have had."*
Six sessions later the tester reported labelled exits reading "Sign 1" / "Sign 2" -- that exact
failure, in those words. A second caveat in the same section ("a gated-off sign will still be listed
and walking there finds nothing") also came true verbatim and cost four more builds. **Both were
precise enough to be tests.** The whole line was reverted in S172.
Sibling of [L-73]: an erratum recorded is not an erratum applied. Same shape, one level up -- there
the note was about a TOOL, here about your own unshipped bug.

### L-91 A NOTE ABOUT A USER'S REPORT CARRIES THE REPORT'S CONTEXT, NOT A GREP OF ITS KEYWORDS
**When recording something the user reported for a later session, write down what they said and the context
they were in. Do not staple on "leads" found by grepping the key names: a key means different things on
different screens, and a grep hit carries whatever the old doc claimed -- right or wrong -- into the new note
with fresh authority.**
**Why:** S183 close. The user reported two silent screens opened with L1/R1 (`1`/`3`). The note added "R1
selects Reserve in the battle target list" and "`1`/`2`/`3` are game speed" from Controls.md. The screens are in
the item targeting menu, so neither applied -- and the second had been false since Session 44, which read one
tester speed jump as proof that all three keys change speed (L-01). Keyboard `1` is pad L1, `3` is R1, `2` does
nothing observable. Repeating it gave a stale inference a new reason to survive.
**The tell:** a "leads" or "see also" list assembled from a keyword search rather than from the surface the
report is about.

### L-77 IF THE PERSON PLAYING THE SAVE CAN ANSWER IT IN A SENTENCE, ASK THEM
**Some unknowns are not measurements at all -- they are facts about the player's own game state:
which quest step they are on, what they have already done, what they are carrying. Building a reader
for one of those is the expensive way to learn something a question answers instantly, and it puts a
map-specific gate into the mod that does not belong there.**
**Why:** S171. A script-placed field sign is gated on a quest flag, and the symptom -- routed there,
no prompt -- is exactly what a gated-off sign looks like. I shipped a build to read the object's live
prompt bits. The user: *"the mod doesn't need to be trying to read quest flags. what you should have
done instead was just asked me if we are on the correct step of the quest."* Reverted the same
session.
**The test that separates this from L-76:** L-76 is for a premise about the GAME'S DATA that only a
running build can reach. This is a premise about the PLAYER'S SAVE, and the player is right there.
**Before instrumenting, ask whether a human already knows the answer.** Note this is NOT the "never
ask the user to find something in the world" rule inverted -- asking someone to read a quest step
from their own journal costs them one sentence; asking them to locate an object they cannot see is
the thing the mod exists to do for them.


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

### L-13 ⛔ NEVER LOOK IN `Tester Logs\` — ASK FOR IT, DO NOT GO FINDING IT
`<game>\x64\logs\` is the corpus: ours, always readable, where every sweep and every count starts.
`Tester Logs\<name>\` and `Saves\<name>\` are **off limits** — not listed, not grepped, not
`find`-ed through, not even "to see what is there". The single key is **the user saying, in this
conversation, to use a specific tester log**; a tester having reported something is NOT that key.
Once handed one it is the authority for that defect, and its numbers still never mix into a
corpus-wide count.
**Why:** S147 reached for the wrong corpus, found nothing, and concluded a defect was "not
measurable from any archived log". It was sitting in nineteen of our own twenty. **And 2026-08-25:
a tester report about a missing interactable opened with `ls "Tester Logs/Dylan"`, a project-wide
`find`, and a sweep of `Saves\Dylan\`** — *"those are not from our machine and for some reason
you keep getting confused there."* The old wording ("a report **and** a pointer") kept being read
as "the report satisfies half of it, so I may go and look." It does not. Detail:
`feedback_read_the_testers_own_log.md`; rule text in `CLAUDE.md`.

### L-14 ⛔ NEVER ASK THE USER TO DO ANYTHING THAT NEEDS SIGHT — READING A SCREEN, OR FINDING AN OBJECT
The user is blind. **Locating an object in the game world is what this mod EXISTS to do**, so
asking them to walk to it, stand next to it, face it or count it — in order to produce a
diagnostic about why the mod cannot find it — demands the very capability whose absence is the
bug. Fair to ask: a keypress **from wherever they already are**, loading a save, entering a map by
name, or what they HEARD. Not fair: *"stand next to the cactus and press `'`"*.
**Instead:** answer it offline first (L-08), or ship an instrument that captures the data
passively on a rescan or map load, with no positioning required.
**Why:** the original screen-reading case, and 2026-08-25 — a session chasing an interactable the
nav list could not see signed off with *"stand next to it and press `'` — ground truth, no
guessing"*. Reply: *"I can not just magically walk up to the cactus and interact with it to get your
log."* Detail: `feedback_never_ask_user_to_read_screen.md`; rule text in `CLAUDE.md`.

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
⚠ **S177: this subtraction was done wrong in shipped code and nobody caught it for thirty sessions**
-- `DAT_01f811f8` was written as RVA `0x1EE11F8` instead of `0x1E611F8`, a transposed 6/E resolving
to an unrelated global. **Knowing the rule is not applying it. Re-do the arithmetic on every new
constant, and sanity-check it against a NEIGHBOURING constant already in the file** (`_DAT_01f83530`
-> `0x1E63530` sits on the same page and would have flagged this instantly).

---

### L-89 MEMBERSHIP IN THE GAME'S REGISTRY IS NOT THE STATE YOU NAMED IT FOR
**A registry lists everything its builder builds. Before a predicate "X is on screen" rests on "a slot holds
an X", find every caller of the builder and check that each one builds the thing you mean. Then test the
property you need (here: the window carries text), not the membership.**
**Why:** S183. `DialogueReader::IsBoxLive` asked the message-window registry "does any slot hold a window"
and three features trusted it as "a dialogue box is on screen": the audio beacon's suspend, the gamepad
router's `FieldBusy`, and the `t` key. `shapewin` -- a script native that draws a full-screen IMAGE -- builds
through the same setter with no text, and every Pharos map keeps two open for the whole map. The beacon went
silent for entire dungeons. The builder's caller list (five natives) would have shown it in one grep; S130
had even written down "open, unmeasured: whether the game nulls its slots" and moved on.
**The tell:** a boolean named for what the player SEES, implemented as "the collection is non-empty".

### L-83 A DETECTOR WHOSE FAILURE MODE IS ITS DEFAULT ANSWER CANNOT BE SEEN TO FAIL
**If "could not tell" and "it is the ordinary case" produce the same behaviour, the feature is
untestable from the outside: a total failure is indistinguishable from a correct negative, and it
will be reported as a regression by a user, not by you. Give the two outcomes different observable
consequences -- a positive log line for the identification AND one for the give-up -- and never let
an early, retryable failure latch the default in permanently.**
**Why:** S177. Font-atlas detection answered "standard" for every install, Polish included, because
of two wrong constants. Standard is also what ~everyone legitimately runs, so nothing looked wrong
anywhere: no crash, no silence, no wrong-looking log -- just a Polish tester whose diacritics were
gone. **Worse, S147 had removed the manual `TextGlyphs` override in the same change that added the
detection**, so the one path that could have exposed the fault, or worked around it, was gone.
Detection had never once fired in the field and the mod had no way to say so.
**Three things this asks for, all cheap:** (1) log the POSITIVE identification, not just the
failure, so its absence in a log is itself evidence; (2) distinguish "not ready yet" from "loaded and
unrecognised" and only let the second one be final -- a bounded retry, because asking too early is a
different fact from asking and being told no; (3) **think twice before deleting a manual override in
the same change that automates it.** Ship the automation, keep the escape hatch one release longer,
and let a log confirm the automation actually fires before the fallback goes.
**What S177 did about it:** the row is back, and S147's objection (*"a wrong answer could be
persisted and then trusted forever"*) was answered not by removing the row but by making the override
**asymmetric**. The row wins only in the direction that is unambiguously a decision: `Polish
translation` forces and disables detection, while `Standard` -- the value every untouched install
carries -- means "no override" and re-arms the detector. **A default value is not a decision, and
reading it as one is what turns an override into a silent override of everything.** Note this needs
no third "Automatic" value; the two-valued row already carries the distinction, because one of its
values is also the detector's own fallback.

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

### L-81 TWO DETECTORS THAT EACH COVER HALF A SURFACE ARE NOT REDUNDANCY, THEY ARE A MISSING READ-POINT
**When one reader needs an arbitration flag to stop it speaking over another, stop tuning the
arbitration and go find the field the GAME writes in both branches.**
**Why:** S175. `choice_reader` carried a per-frame tick keyed on the option cursor `widget+0x58` and
a second detector on the `0x8000` focus dispatch, with a stand-down flag deciding which drove. It
looked like belt-and-braces. It was not: `FUN_002a5590` lays an option block out in two ways, and
when it builds a child list window it sets bit 22 of the state word, which `FUN_002a9980` tests and
returns on -- so `+0x58` NEVER moves for that flavour, and the dispatch never fires for the other.
Each detector was blind to exactly the half the other saw, and the flag was hiding it. Both flavours
resolve the highlight through the same helper into the same address (`widget+0x54` IS
`window+0x124`), so ONE detector on that field covers everything and the second one, its cached
page, the flag, and a re-implementation of the game's own hidden-slot walk all delete together.
**The tell:** you are writing code to decide which of your own readers is allowed to speak.
**Corollary, and the reason this is not L-30:** deleting a path is only safe once you have shown
where the survivor gets the same fact. Here that was two decompiled functions writing the same
offset, not a hunch that one reader looked sufficient.

---

### L-82 WHEN YOU RECOVER THE SAME THING TWICE BY DIFFERENT ROUTES, PUT THE TWO LISTS SIDE BY SIDE
**Two independent derivations of one underlying object are a free correctness check, and the check
only happens if you actually perform it. Write down the relation that ought to connect them and
evaluate it. If you never do, the second derivation is not corroboration -- it is a second chance to
be wrong that nobody spends.**
**Why:** S177. The Polish fan patch repaints ten font slots, and this project had recovered those ten
slots TWICE: once as a letter mapping (`kGlyphPolish`, derived in S130 from the patch's translated
text) and once as an advance-width fingerprint (`kFpSlot`, derived in S147 from the font file). The
relation connecting them is one line -- `byte = slot + 0x20`, and S147 wrote that relation down in
its own comment. Evaluating it turns `kFpSlot`'s 60/61/62/84/85/86/98 into bytes 0x5C 0x5D 0x5E 0x74
0x75 0x76 0x82, and only three of those appear in `kGlyphPolish`; the corrected ordinals give 0x5B
0x5C 0x5D 0x73 0x74 0x75 0x81, and eight of ten land exactly on overridden bytes. **Every ordinal was
one too high -- counted from one against a zero-based file -- and detection therefore never fired on
any build from V0.6.3 to V0.7.** The disagreement sat in two adjacent files, in the same namespace,
for thirty sessions.
**The tell:** you are about to hand-transcribe a list of indices out of a file, and a table derived
some other way already describes the same set. Cross-multiply before you ship, not after a report.
**Corollary:** re-deriving from the RAW ARTIFACT is cheap when the artifact is on disk. Both
`font00.dat` files were sitting in the repo tree the whole time; the diff that settled this took one
command and needed no game running.

---

### L-84 RESTORING A FEATURE MEANS READING THE COMMIT, NOT REIMPLEMENTING THE DESCRIPTION
**When the task is "put back the thing we removed", the deleted code is the specification. Go get it
-- `git log -S`, `git show <commit>^:<path>` -- before writing a line. A reimplementation from the
description is a NEW feature wearing the old name, and it silently discards every decision the
original had already made and every hour of play that confirmed them.**
**Why:** S177. Asked to restore the glyph-variant toggle S147 deleted, I designed a fresh three-value
row with a new settings key and a new setter API -- while the working, play-confirmed two-value row
sat in git one commit before detection landed. The user's correction: *"you already had a toggle for
this before we removed it and it absolutely was confirmed working. so if you built something new you
did it wrong."* The rebuild was not merely wasted: the invented `diacritics` settings key would have
orphaned every tester's existing `text_glyphs=1`, silently resetting the choice of the one player the
work was for. What actually shipped is the original row byte-for-byte, with the spoken label renamed.
**The tell:** you are writing a phrasebook entry, an enum, or a settings key for something that
demonstrably existed before. Approved wording, tested defaults and a persisted key are all
recoverable; recreating them from memory changes them.
**What legitimately stays new:** only what the intervening code forces. Here that was the arbitration
with the detector S147 added, which the original never had to contend with -- see L-83. Say which
part is restored and which is new, and why.

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

### L-88 ⛔ A FEATURE THAT NEEDS THE GAME THREAD TO DO WORK NOBODY ASKED FOR IS A QUESTION, NOT A DESIGN
**Before building anything that runs on the game thread on the mod's own initiative -- a background search,
a flood, a periodic scan -- stop and ask. Spacing it, rate-limiting it and pausing it in fights do not make
it acceptable; they make it a smaller freeze. The user's standing answer is to revoke the feature.**
**Why:** S182. The Unreachable filter's spec ("hide what has no valid path") was met exactly by running
the real router in the background: one search per 250 ms or more, 2-43 ms each. The cost was written up
honestly in the report -- and that was the problem: it was decided and built first, disclosed after.
*"Game freeze is 100%, completely unacceptable and you should never have built a system that could
potentially do that without express permission."* It also broke CLAUDE.md's existing rule against polled
per-frame work without approval, which was on the page the whole time. What shipped instead records the
answer the route key already gives when the player presses it: weaker, and free.
**The tell:** your design contains a scheduler, a "next allowed" deadline, or a cost multiplier on the game
thread. Also: "we could move it to another thread" -- the engine functions it calls are not yours to call
there (a stall traded for a crash). Rule: CLAUDE.md "NEVER ADD A FRAME STALL TO THE GAME THREAD".

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
