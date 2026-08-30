# Release Procedure

Triggered when the user says **"prepare release X.X"** (e.g. `prepare release 0.01`,
`prepare release 1.2`). Substitute `<version>` for the user's number throughout.

**The user ships the release.** This procedure produces the zip and stops. It does **not** tag, does
not push, and does not create a GitHub Release — releases are distributed by the user by hand, and
`/Releases/` is gitignored so no artifact ever reaches the repo.

## Inputs

- `<version>` — bare version string, no leading `V` (e.g. `0.01`, `1.2`).
- Directory name is always `V<version>` (capital V), matching the sibling FFPR projects.

## Preconditions

1. **Working tree is clean:** `git status --porcelain` returns empty. If dirty, **stop and report** —
   the user must commit (or stash) first. Release artifacts must come from a committed state, or the
   shipped DLL cannot be traced back to a commit.
2. **`Releases\V<version>\` does not already exist.** If it does, **stop and report** — never
   silently overwrite a prior release directory.
3. The mod builds. Step 1 confirms this; if it fails, stop.

## Steps

### 1. Build a fresh DLL

```
cmd.exe /c "D:\Games\Dev\Custom\FFXII\FFXII-Screen-Reader\build_and_deploy.bat"
```

Must produce `build\bin\Release\dinput8.dll` and report zero errors. This also deploys to the game
directory, which is fine and expected — that is how the tester picks it up.

### 2. Assemble `Releases\V<version>\`

Create the directory and copy in **exactly five** files:

| File | Source |
|---|---|
| `dinput8.dll` | `build\bin\Release\dinput8.dll` (the fresh build from step 1) |
| `SDL3.dll` | `build\SDL3-build\Release\SDL3.dll` (produced by the same build) |
| `Tolk.dll` | most recent prior `Releases\V*\`; for the first release, `D:\Games\Dev\Unity\FFPR\ff1\ff1-screen-reader\Releases\V1.4\Tolk.dll` |
| `nvdaControllerClient64.dll` | most recent prior `Releases\V*\`; for the first release, the same FFPR `V1.4` directory |
| `ReadMe.txt` | `README.md` converted to plain text (see below) |

**Preserve casing exactly:** capital `T` in `Tolk.dll`, lowercase `n` in `nvdaControllerClient64.dll`,
capitals in `SDL3.dll`.

**`SDL3.dll` IS NOT OPTIONAL (added Session 92).** The mod links against it, so unlike the TTS pair a
missing `SDL3.dll` does not degrade the mod — `dinput8.dll` fails to load outright and the game will
not start, with nothing useful on screen for a blind player to act on. Omitting it from a zip breaks
the release completely rather than partially. It is **built from source by our own build**, so it is
always in step with the DLL beside it; never source it from anywhere else.

**All shipped DLLs must be x64** — `SDL3.dll` included. `FFXII_TZA.exe` is 64-bit; a 32-bit `Tolk.dll`
loads and then simply never speaks, which is a miserable bug to diagnose from a user report. Verify
before copying — the PE machine field must read `8664`, not `014c`:

```
od -An -tx2 -j$(( $(od -An -tu4 -j0x3c -N4 "<file>" | tr -d ' ') + 4 )) -N2 "<file>"
```

**Do NOT source Tolk from `D:\Games\Dev\tolk\tolk\dist_x86\Tolk.dll`** — that build is **x86** and
will not work. The `libs\x64\` folder there holds `nvdaControllerClient64.dll` only, not Tolk itself.

**ReadMe conversion:** strip Markdown so the file reads cleanly under a screen reader — drop `#`
heading markers, `**`/`*` emphasis, code-fence ` ``` ` lines, and leading `-` and `*` bullet markers;
convert `[text](url)` to `text (url)`; convert tables to plain columns; unescape `\[` `\-` `\\` `\_`
and drop the `&#x20;` entity outright. Strip inline code backticks only where they are **paired** — a
lone backtick is the literal `` ` `` key name and must survive. Then, on every line, **collapse runs
of spaces to one and strip trailing whitespace**; write CRLF throughout and no BOM. No leftover `#` or
backticks beyond that single one. Save as `ReadMe.txt` (capital R, capital M) in the version
directory.

> **The last two rules were undocumented until V0.7, and their absence broke a rebuild.** The
> space-collapse and the trailing-whitespace strip appeared only in the *audit* sentences of earlier
> records ("zero doubled spaces"), never here, so a converter written from this paragraph alone comes
> out 3 bytes wrong on the V0.6.5 artifact — `README.md` carries two lines with a trailing space and
> one with a doubled space. V0.6.4's record claims this paragraph is sufficient to regenerate the
> artifact with no undocumented step; it was not. **The validation step below is what caught it, which
> is the argument for never skipping it.**

**These five files are the entire release.** Do not add:

- **`mod_config.ini`** — the mod's RVA byte-validator writes it on first launch. Shipping one would
  mask a validator failure.
- **FF12 Module Loader / External File Loader** — incompatible; both want the `dinput8.dll` slot.
- **The beacon sounds** — they are embedded in `dinput8.dll` as RCDATA (`src\audio\beacon_assets.rc`),
  not shipped loose. There is deliberately no asset folder in the zip.

> **Note on the TTS DLLs (corrected 2026-07-15).** These used to be excluded from the release on the
> strength of a "user-supplied, never bundled" rule. That rule was about **deploy**, not release —
> `build_and_deploy.bat` must never copy Tolk into the *game directory*, because the user manages
> their own Tolk install there. It should never have said anything about the release zip, and the
> over-reach cost the release its TTS DLLs. **The release ships them; deploy still must not.**
>
> Three separate concerns (see `CLAUDE.md`), only the third of which is this file's business:
> 1. **Deploy** — `build_and_deploy.bat` copies `dinput8.dll` ONLY. Never Tolk. Unchanged, absolute.
> 2. **Build** — no `Tolk.h`, no CMake link, no vendored headers; `LoadLibrary` + hand-rolled
>    typedefs only, silent when absent. Unchanged, absolute.
> 3. **Release** — the zip DOES include `Tolk.dll` + `nvdaControllerClient64.dll`. This is the one
>    that changed.

> **Note on SDL3 (Session 92) — it is a THIRD case and follows neither rule above.** Do not reason
> about it by analogy with Tolk; the three answers are different:
> 1. **Deploy** — `build_and_deploy.bat` still copies `dinput8.dll` ONLY. It does **not** copy
>    `SDL3.dll`, even though our own build produces one, because the tester manages that file in the
>    game folder themselves. (DQ7R's deploy script *does* copy it — do not port that across.)
> 2. **Build** — unlike Tolk, the build **does** link SDL3: real headers, `add_subdirectory` of
>    `D:/Games/Dev/SDL3-source`, `target_link_libraries(... SDL3::SDL3)`. There is no `LoadLibrary`
>    path and no silent-when-absent behaviour to fall back on.
> 3. **Release** — the zip **must** ship it, for the reason in step 2 above: without it the mod does
>    not load at all.

### 3. Zip with 7-Zip

```
& "C:\Program Files\7-Zip\7z.exe" a -tzip "Releases\FFXII-Screen-ReaderV<version>.zip" ".\Releases\V<version>\*"
```

- Zip name: `FFXII-Screen-ReaderV<version>.zip`, placed in `Releases\` — a **sibling** of the version
  directory, not inside it.
- The zip's root contains the five files **directly**, with no nested `V<version>\` folder and no
  asset subfolder, so the user can extract straight into the game's `x64\` folder as the ReadMe
  instructs.

### 4. Record the release — in THIS file, not the session log

Append an entry to **Release Records** at the bottom of this file. A release is not a session's work
and must **not** get a `## Session N` entry in `Docs\sessions_*_current.md`: it produces no code
change, it would consume a number in the global session counter for a build, and it buries the
release history inside a file that is about RE findings. Releases 0.1 (Session 50) and 0.1.1
(Session 67) were logged that way before this rule existed — do not copy them.

Each record states: version, date, the commit the DLL was built from, whether `ReadMe.txt` changed,
and anything a future reader would need to reproduce or debug that build.

**If a KEY is missing from the readme — a new hotkey, or an existing one whose meaning changed on
some screen — say so in the record.** Do **not** list features that read on their own. The readme
documents keys and screen contexts, not the feature set, and a feature with no key has nothing for a
player to look up. Treating every unlisted feature as a gap is what produced the long lists in the
V0.2.1–V0.5 records; corrected 2026-08-03 and struck in the V0.6 record.

**Auditing a key means grepping the readme for it and reading EVERY hit (added 2026-08-14, L-67).**
A key is commonly described twice — a loose sentence in prose and the enumerated entry in the key
list — and **the two rot at different rates**. The prose one usually says what the key is *for* and
survives anything; the enumerated one lists what it actually says and goes stale the moment a
readout changes. V0.6.4 checked `o`'s prose sentence, found it still true, and shipped a record
claiming no gaps while the key list two hundred lines down still promised MP that S160 had removed.
**One reassuring hit is not coverage.**

**And if a flag survives two records, fix it or say why it cannot be fixed (L-68).** "Readme edits
are a separate commit made *before* the trigger" bounds a release's **scope** — it does not forbid
correcting a line already known to violate `CLAUDE.md`. Three records flagged the same changelog
sentence and shipped it three times.

### 5. Report

Confirm the zip was created and list its contents. Do not push, tag, or publish anything.

## What this procedure does NOT do

- **Does not tag or push.** The user ships releases by hand.
- **Does not create a GitHub Release.** The repo is private and holds source only.
- **Does not modify `README.md`.** If the readme needs changes, that is a separate commit made
  *before* the release-prep trigger.
- **Does not bump a version string in code.** ~~There is no version constant in the build; the
  release is identified by its directory and zip name.~~ **CORRECTED 2026-08-14 — the second half is
  false and has been since the build stamp landed.** `CMakeLists.txt:185` sets
  `FFXII_SR_VERSION` (a `CACHE STRING` whose own comment reads *"Mod version, as it appears in the
  release zip name"*) and `logger.cpp:165` writes `Build: V<version> (<git hash>)` into every log's
  INIT line. **It still reads `0.6`**, so a log from the V0.6.4 zip says `Build: V0.6`. The *hash*
  half is generated per configure and is correct, so a log still traces to a commit — which is why
  this has gone four releases unnoticed. **The procedure still does not bump it**: that is a build
  change, and this file forbids one at release time. Bumping it belongs in a commit made *before* a
  release trigger, like a readme edit.
- **Does not overwrite an existing release.** If `Releases\V<version>\` exists, stop and report.

---

# Release Records

Newest first. One entry per release, written at step 4. `Releases\` is gitignored, so this table is
the only record in the repo that a given zip ever existed.

## V0.7-Test-Build — 2026-08-30

**Built from:** `76f0e66`, the version bump. `HEAD` at zip time was `d2855e6`, a readme commit made at
the trigger — it changes no code, so nothing in this binary post-dates `6a9476a`. Covers **Sessions
165–175** since `V0.6.5-Sponsor-Build`'s `9e9a00f` — three commits: `6a9476a` (the S165–S174 catch-up,
which is where the pad scheme lives), `c61237c` and `9e95399` (S175, dialogue choices).

**What is actually IN this build is smaller than "eleven sessions" suggests.** S165–S171 were
**reverted in full** by S172 — the cactus line was never a defect, the tester was on the wrong quest
step — so their only lasting contribution is documentation. The code that ships here is **S173+S174's
gamepad scheme** and **S175's dialogue-choice rewrite**, on top of the S172 revert that returned
`src\` to V0.6.5's `9e9a00f`. That is why `dinput8.dll` grew only **512 bytes** across a range this
wide: S175 deleted about as much as it added — one detector, one cached page, one arbitration flag and
our copy of the game's slot resolver all went — and the reverted sessions contribute nothing.

> ⚠ **THE README NOW CARRIES THE PAD SCHEME, AND THAT REVERSES A DELIBERATE S174 DECISION. READ THIS
> BEFORE ASSUMING IT WAS AN OVERSIGHT.** S174's Open section ends: *"README still does not carry the
> pad scheme, deliberately — one play pass stands between this and it being true for players."* It was
> written into this build anyway, because **this is a test build and the pad scheme is the thing being
> tested**: a binding nobody has been told about cannot be exercised, and S174's own blocker was that
> twenty sessions of the intercept shipped with no pad ever connected. The judgement is that a tester
> who reads "R1 gives directions", presses it and hears nothing files the report that closes the
> phase, which is the point of the build. **If this zip is forwarded to a non-tester audience that
> reasoning does not carry** — see the play-confirmation section below for exactly which of these
> bindings have never been pressed by anyone.

**The version number was checked against these records rather than taken at face value, and for once
it was already right.** The trigger said *"prepare a test build according to the latest version
number. so if 0.6.5, do 0.7"* — a conditional, not an assertion, and the condition held:
`V0.6.5-Sponsor-Build` (2026-08-25) is the newest record and `0.7` is untaken. That makes four
releases running where the number was verified before use; the previous three were all wrong (V0.6.3
"0.5.3", too low; V0.6.4 "0.6.3", taken the day before; V0.6.5 "0.6.1", four behind). **The check is
cheap and has paid for itself three times in four — keep making it even when the trigger looks
self-evidently correct.**

**THE BUILD STAMP WAS RIGHT GOING IN, for the first time since it landed.** V0.6.5 fixed the string
and recorded the trap; this is the first release to benefit from it. `FFXII_SR_VERSION` 0.6.5 → 0.7 in
`76f0e66`, **with `build\CMakeCache.txt` updated in the same change**, because it is a `CACHE STRING`
and editing `CMakeLists.txt` alone is a silent no-op. **Verified in the shipped binary, never in the
source:** `dinput8.dll` holds the null-terminated strings `0.7` (once) and `76f0e66` (once) and **zero
occurrences of `0.6.5` or `d5fc11d`**, so a V0.7 log opens `Build: V0.7 (76f0e66)`. The comment above
the line was rewritten in the same commit — it had said "bumped by hand at release time", which
contradicts this file's own rule that the bump belongs in a commit *before* the trigger, and it never
mentioned the cache at all. Both now sit where the next person to bump it will read them.

**ReadMe: CHANGED** — 22,903 bytes / 255 lines (sha256 `7495b0ab…73904172`), against V0.6.5's 20,759 /
233. One commit touched `README.md` in this range, `d2855e6`, made at the trigger in response to the
key audit below.

**The converter was rebuilt from this file's rules again and validated the strong way — but the rules
as written were NOT sufficient, and that is the finding worth keeping.** Run against
`git show 2777d61:README.md` it reproduced the shipped `V0.6.5-Sponsor-Build\ReadMe.txt`
**byte-identically** — 20,759 bytes both, sha256 `fd596d59…e475ca8` — which is the gate this file asks
for. It did **not** pass first time: the output was 3 bytes long, across three lines. Two
transformations the conversion rules never state were needed — **strip trailing whitespace from every
line**, and **collapse runs of spaces to one**. Both were recoverable only from the *audit* sentences
of earlier records ("zero doubled spaces"), never from the rules paragraph that is supposed to be
sufficient on its own. **V0.6.4's record claims "the rules written down here are sufficient to
regenerate the artifact, with no undocumented step"; that is now falsified, narrowly.** Rather than
flag it for a future release, step 2's conversion paragraph has been amended to state both rules —
L-68 applied at first occurrence instead of third.

**Output audit on the shipped file:** zero `#`, zero `*`, zero `](`, zero `&#x20;`, zero doubled
spaces, no BOM, CRLF on all 255 lines with no bare LF, and **exactly one backtick** — the literal
`` ` `` key name, the same single survivor as every release since V0.2.1. The **17 backslashes** were
each read and are all content: the two Windows install paths, and prose references to the `\` route
key. The count is unchanged from V0.6.5 because the new section names its keys in words wherever it
can, so it introduced none.

**Readme key audit — ONE GAP, THE LARGEST THIS FILE HAS RECORDED, AND IT WAS FIXED RATHER THAN
FLAGGED.** S173 and S174 gave the mod an entire second input surface and the readme documented **none
of it**. Grepping for `Controller` returned the mod-menu row added at V0.6.5 and nothing else;
grepping for `stick` returned only camera prose and the auto-walk line below it. **Every binding — the
right stick's four directions, the D-pad's party slots, R1, Back, L3, and the whole of mod mode — had
nowhere a player could look it up.** Fixed in `d2855e6` with one new `Controller` section under
Keys → Mod, placed before the mod menu so that menu's `Controller` row can say "described above".

> **The section was written twice, and the first draft is the lesson.** It ran to 30 lines and spent
> most of them on rationale — why A, B, X and Y are never taken, why a pad button is too scarce to
> spend on a duplicate toggle, why passthrough is the default. Every one of those sentences is true
> and every one of them is banned: `CLAUDE.md`'s README rule is keys only, no design justification, no
> section per feature, and it exists because a tester once called an over-written update *"far, far
> too many edits"*. The shipped section is 21 lines and carries the bindings alone. **The rationale
> was already written, in `pad_router.h`, which is where it belongs.**

**The line V0.6.5 flagged and left has been fixed — L-68 working as intended at the second record
rather than the fourth.** Auto-walk's entry said *"the mod cannot see the stick, so moving the stick
does not cancel it"*, sitting one line above a setting whose whole subject is the mod reading the pad.
The **limitation is real and was kept**, verified in the code rather than assumed:
`AutoWalk::OnDevicePoll` tests the DirectInput keyboard buffer alone (W/A/S/D and the arrows), and
`grep -rn AutoWalk src/input/` returns only comments — the pad router never calls it — so the stick
genuinely does not cancel auto-walk. Only the **reason** was false. It now reads that auto-walk
watches the keyboard alone and that the left stick is passed straight to the game and never read.
**A true limitation with a false explanation is still a false claim, and the explanation was the whole
job.**

**Zip:** `FFXII-Screen-ReaderV0.7-Test-Build.zip`, **1,250,666 bytes**, five files, root flat.
- `dinput8.dll` 895,488 bytes (sha256 `ba44ff8c…61c8bd64`) — up 512 from V0.6.5's 894,976.
- `SDL3.dll` 1,748,992 bytes (sha256 `64e52809…b4ac7531`) — **byte-identical to V0.6.5**, confirmed by
  a real `cmp` rather than by matching sizes. The link timestamp that moved at V0.6.5 has not moved
  again: no source file was added to `CMakeLists.txt` in this range, so it did not relink.
- TTS pair carried over from `V0.6.5-Sponsor-Build`, verified with `cmp`: `Tolk.dll` 122,368 (sha256
  `c4fb11d3…48197225`), `nvdaControllerClient64.dll` 153,600 (sha256 `41c1f5df…b23a0b09`).
- All four DLLs verified PE machine `8664`.

**Play-confirmation status — READ THIS BEFORE ANSWERING A PAD REPORT.**
- **S175 is play-confirmed** (2026-08-30, user): the dialogue choice reads. But the *surfaces*
  exercised went unrecorded, so the child-list flavour (`child=1`) is still unwitnessed and the tick's
  call rate unmeasured. The first V0.7 log should be checked for a `child=1` line and for the `[PERF]`
  count, which now sits above the change-check and so counts calls rather than emissions.
- **The pad scheme is mostly UNPLAYED, in S174's own words:** *"Everything above is BUILT, DEPLOYED,
  UNPLAYED except the right stick and the D-pad."* Confirmed working in the 2026-08-29 log: the right
  stick driving the pathfinder, the field camera being swallowed as designed, and the D-pad speaking
  party slots on the field while still moving the game's own cursor in menus. **Never pressed by
  anyone: R1 in either context, Back and the whole of mod mode, and L3.** That last one matters most —
  L3 is the escape hatch the readme now points players at, and it has never once been thrown.
- **Two unexplained pad artefacts are still open.** `L1` and `R1` edges appear in the 2026-08-29 log
  for buttons the user is certain were never pressed; the survey now prints `idx=` and `raw=`, which
  will say whether a second pad index or a second held button explains them. **Do not build on either
  line until that is answered.** The PS2 libpad mask stays at 0.95 and stays unusable.
- **S152's six per-frame changes are play-confirmed** (2026-08-27) and are no longer the standing
  first suspect for a new nav, beacon, menu or dialogue fault. On a fault in this build, suspect
  S173/S174 (input) or S175 (choices) first — they are what changed.

## V0.6.5-Sponsor-Build — 2026-08-25

**Built from:** `d5fc11d`. **The DLL's code traces to `8704b7f`** (S162) — the commit after it is the
build-stamp bump, which changes no behaviour, and `HEAD` at zip time was `2777d61`, a readme commit
made at the trigger. Nothing in this binary post-dates S162's code. Covers **Sessions 162–164** since
`V0.6.4-Test-Build`'s `3c2c7fc` — five commits, of which **three carry code**: `e70c76a` (S163, the
Draklor lift as a numeric field), `b715d99` (S164, class-1 reach) and `8704b7f` (S162, the gamepad
intercept).

> ⚠ **THIS SPONSOR BUILD SHIPS A FEATURE WITH ZERO PLAY DATA, AND THAT WAS A DELIBERATE USER
> DECISION.** S162's gamepad intercept has never been played — its own log entry opens "MEASUREMENT
> BUILD SHIPPED — zero play data yet" and lists three questions it is waiting on, the first of which
> ends the phase if it fails. It **consumes the field right stick** and the `Controller` row defaults
> to **On**, so every pad-using sponsor gets that behaviour change. The alternative — cutting from
> `b715d99` and parking S162 in a stash — was offered at the trigger and declined; the user chose to
> include it. **If a sponsor reports the field camera ignoring the right stick, that is this feature
> working as designed, not a regression.** Point them at `F8` → `Controller` → Off, which returns the
> input path to byte-identical.

**The tree was DIRTY at the trigger, and the dirt was a whole session.** S162 was logged but
uncommitted and shared the tree — 14 modified files plus `pad_hook.{h,cpp}` and `pad_router.{h,cpp}`.
Precondition 1 stops the release there. Unlike V0.6.4, where the dirt was this file and committing it
was obviously right, **this dirt was unplayed code and the decision was the user's**, because a
sponsor build is the non-tester audience (see the V0.5-Sponsor-build record). Asked, answered
"include it", committed as `8704b7f`, tree clean before and after the build.

**No shared-doc surgery was needed this time, and it was checked rather than assumed.** S163 and S164
were already committed, so the working tree's `GameArchitecture.md` / `debug.md` /
`sessions_151_current.md` changes were S162's alone. Verified by grepping the **staged** diff for the
other tracks' keywords: four hits, **all four on context lines**, zero on added or removed lines.
Grepping the diff without separating context from changes would have raised a false alarm.

**THE BUILD STAMP IS FIXED — `FFXII_SR_VERSION` 0.6 → 0.6.5 (`d5fc11d`).** Five releases shipped logs
opening `Build: V0.6 (<hash>)`; V0.6.4's record flagged it and shipped anyway, which is exactly what
**L-68** names. This is the fix, and it was made in a commit *before* the build, which is where this
file says a version bump belongs. `CMakeLists.txt`'s own comment had said "bumped by hand at release
time" since the stamp landed and had never once been followed. Verified in the shipped binary rather
than in the source: the DLL contains the strings `0.6.5` and `d5fc11d`, so a sponsor log now opens
`Build: V0.6.5 (d5fc11d)`.

> **The gotcha that let this rot for five releases, recorded so the next bump does not repeat it:**
> `FFXII_SR_VERSION` is a **`CACHE STRING`**. Editing `CMakeLists.txt` does **not** change an
> already-configured `build\CMakeCache.txt`, so the source can say 0.6.5 while every build keeps
> stamping the old number and nothing looks wrong. The cache entry has to be updated too (or the
> cache deleted, or `-DFFXII_SR_VERSION=` passed). **Editing the source alone is a silent no-op.**

**The version number was wrong for the THIRD release running, and this time the user pre-authorised
the correction.** The trigger asked for "0.6.1-Sponsor build (if the version number is wrong, correct
it sequentially)". `V0.6.1-Shotgun-Build` shipped 2026-08-05, and 0.6.2, 0.6.3 and 0.6.4 are all
taken, so 0.6.1 would have been the *fifth* release in that range and the fourth collision. Corrected
to **0.6.5** under the trigger's own instruction — no question needed, unlike V0.6.3 (user said
"0.5.3", too low) and V0.6.4 (user said "0.6.3", taken the day before). Three for three. **Check the
last record before taking a version at face value** — and note that the standing instruction in the
trigger is what made this the cheap case.

**Zip:** `FFXII-Screen-ReaderV0.6.5-Sponsor-Build.zip`, **1,249,698 bytes**, five files, root flat.
- `dinput8.dll` 894,976 bytes (sha256 `41d4c429…22efb53c`) — up 13,824 from V0.6.4's 881,152; three
  sessions covering the lift's numeric field, class-1 reach, and the gamepad intercept.
- `SDL3.dll` 1,748,992 bytes from `build\SDL3-build\Release\` (sha256 `64e52809…b4ac7531`).
- TTS pair carried over unchanged from `V0.6.4-Test-Build`, verified with a real `cmp`: `Tolk.dll`
  122,368 (sha256 `c4fb11d3…48197225`), `nvdaControllerClient64.dll` 153,600 (sha256
  `41c1f5df…b23a0b09`).
- All four DLLs verified PE machine `8664`.

> **`SDL3.dll` IS NO LONGER BYTE-IDENTICAL, AND THE STREAK ENDING IS NOT A SOURCE CHANGE.** The last
> five records tracked it as byte-identical (sha256 `056db4a9…fa3a1d19`); this one is not, and the
> difference was measured rather than waved through. **Exactly 9 bytes differ out of 1,748,992**, and
> they are three copies of one 4-byte link timestamp: the COFF `TimeDateStamp` at file offset 288,
> and its echoes in the two debug-directory entries at 1,492,164 (type 13, the `/Brepro`
> reproducibility marker) and 1,492,192 (type 20). **No section data differs at all** — same size,
> same source tree, same build. It relinked when S162 added two source files to `CMakeLists.txt` on
> 2026-08-20, which is the value the stamp now carries. **Do not record this as "SDL3 changed."**

**ReadMe: CHANGED** — 20,759 bytes / 233 lines (sha256 `fd596d59…e475ca8`), against V0.6.4's 20,500 /
232. One commit touched `README.md` in this range, `2777d61`, and it was made **at the release
trigger** in response to the key audit below.

**The converter was validated the strong way again.** It was run against `git show 3c2c7fc:README.md`
— the readme as it stood at the previous release — and its output compared to the shipped
`Releases\V0.6.4-Test-Build\ReadMe.txt`: **byte-identical, 20,500 bytes both.** That reproduces a
known-good artifact from its own source before the current one is converted with the same code. Note
this gate is only as good as its source: because `README.md` had **not** changed between the two
releases, the same run also proved the pre-edit V0.6.5 ReadMe would have been byte-identical to
V0.6.4's, which is how the *readme* was cleared while the *key list* still had a hole in it. Output
audit on the shipped file: zero `#`, zero `*`, zero `](`, zero `&#x20;`, no BOM, CRLF, and **exactly
one backtick** — line 91's literal `` ` `` key name, the same single survivor as the last five
releases. The 17 remaining backslashes were each checked and are all content: Windows install paths
and the `\` route key.

**Readme key audit — ONE GAP FOUND, AND IT WAS FIXED RATHER THAN FLAGGED.** The mod menu settings
list documented **nine** rows; this build has **ten**. The missing row was **`Controller` (Off/On,
default On)** — S162's kill switch, and the only documented way a pad player gets back to a stock
controller. It was fixed in `2777d61` before the zip was cut, using the menu's own wording
(`ControllerDesc` / `ControllerDescOff`) so the readme and the `O` key say the same thing, and naming
the right-stick swallow so a sponsor can get from the symptom to the setting. **Fixing rather than
flagging is L-68 applied at first occurrence instead of third** — and it mattered more than a normal
omission, because the undocumented switch guards an unplayed feature that is on by default.

> **This gap is a textbook L-67, in the direction the lesson warns about.** Grepping the readme for
> `Controller` returns **three hits, and all three are `nvdaControllerClient64.dll`**. Grepping for
> `pad` returns three more, and they are `Notepad` and `Numpad`. A keyword search that stopped at the
> count — or at the first reassuring hit — would have reported full coverage of a word that appears
> six times and never once as the setting. **Read every hit, and check what each one actually is.**

**The `o` key was re-audited and is correct.** V0.6.4 shipped a record claiming no gaps while the
enumerated entry still promised MP that S160 had removed; that entry (line 229) now reads HP as
numbers, level, statuses and the affinity list, with no MP. The other `MP` hits in the file were
checked individually — line 138 is the party-status `4` key, which legitimately reads MP. **Both `o`
descriptions were read, not just the prose one.**

**Flagged, NOT fixed — one line, and the reason it was left.** Auto-walk's entry still says "the mod
cannot see the stick, so moving the stick does not cancel it". That remains **true of auto-walk** —
its cancel path reads the keyboard and Phase 1 changed nothing there — but it now sits one line above
"Whether the mod reads your controller", so the file says both that the mod cannot see the stick and
that it reads the pad. Left alone deliberately: rewriting an approved paragraph about a feature this
release does not change is the overcorrection the readme rule was written for. **Revisit when Phase 2
gives auto-walk the pad.** If it survives the next record, fix it — that is L-68.

**Play-confirmation status.** S163 is play-confirmed on **one** lift only (Draklor 66F North Lift
Terminal); other lifts, the range flavour and digit-column editing are unverified. S164 is
play-confirmed 2026-08-25. **S162 is not play-confirmed at all** — see the warning at the top. S152's
six per-frame changes remain untested and are in every build since `592e142`: on any new nav, beacon,
menu or dialogue fault, suspect that first rather than this release's three sessions.

## V0.6.4-Test-Build — 2026-08-14

**Built from:** `3c2c7fc`. **The DLL's code traces to `6bfa74a`** — the two commits after it are
V0.6.3's release record and this release's readme commit, both documentation only, so nothing in
this binary post-dates S161. Covers **Sessions 159–161** since `V0.6.3-Shotgun-Build`'s `5f13705` —
six commits, of which **three carry code**: `7b8af05` (S159), `5c38af7` (S160) and `6bfa74a` (S161).

> **THE ZIP WAS CUT TWICE, and the second cut is the one to ship.** The first assembly was completed
> at `c488495` and is described below as originally written; the user then struck a readme line, the
> readme fix turned up a **false claim about a key**, and the zip was re-cut on the corrected readme.
> **`dinput8.dll` is byte-for-byte the same binary in both** — it was not rebuilt, and its build
> stamp still names `c488495`, which is why "built from" and "the stamp" disagree by one docs commit
> here. Only `ReadMe.txt` and the zip around it changed. **This is the one case where overwriting an
> assembled release directory is right rather than forbidden:** the precondition exists to stop a
> *shipped* release being clobbered, and nothing had left the machine.

**The tree was DIRTY at the trigger, and the fix is recorded because the dirt was this file.** The
V0.6.3 record above had been written at that release and deliberately left uncommitted; 117 lines of
it were still unstaged. Precondition 1 stops the release there, so it was committed as `c488495`
before anything was built. Tree clean before and after the build. **A release record that never
reaches git is invisible to the next release** — commit it at the release that writes it.

**The version number was changed before the build, for the second release running.** The user asked
for "0.6.3-Test Build", but **`V0.6.3-Shotgun-Build` had been cut the previous day**, so a second
0.6.3 would have sat beside it in `Releases\` with only the label to tell them apart. Asked rather
than assumed; the user confirmed **0.6.4**. Directory and zip are `V0.6.4-Test-Build`, hyphenated to
keep a space out of the zip name, as every release since V0.6 has been. Note the shape of this: the
V0.6.3 record says the same thing happened at that release, from the opposite direction (the user
said "0.5.3", which was too low). **Check the last record before taking a version at face value.**

**Zip:** `FFXII-Screen-ReaderV0.6.4-Test-Build.zip`, **1,243,806 bytes** (the first cut was 1,244,003
— the difference is the deleted readme paragraph), five files, root flat.
- `dinput8.dll` 881,152 bytes (sha256 `da38ebb4…d1ba9c38`) — up from V0.6.3's 878,080; three
  sessions covering the Libra key's un-shadowing, the Libra readout's contents, and the floor-trap
  navigation category.
- `SDL3.dll` 1,748,992 bytes from `build\SDL3-build\Release\` — **byte-identical to V0.6.3's,
  V0.6.2's, V0.6.1's and V0.6's** (sha256 `056db4a9…fa3a1d19`), same source, unchanged build. **Five
  releases running.**
- TTS pair carried over unchanged from `V0.6.3-Shotgun-Build` (`Tolk.dll` 122,368 sha256
  `c4fb11d3…48197225`, `nvdaControllerClient64.dll` 153,600 sha256 `41c1f5df…b23a0b09`).
- All four DLLs verified PE machine `8664`.

**ReadMe: CHANGED** — 20,500 bytes / 232 lines (sha256 `a1c57381…97407eaf`), against V0.6.3's 21,052
/ 234. One commit touched `README.md` in this range, `3c2c7fc`, and it was made **in response to the
user at the release trigger**, after the first zip had already been assembled.

> ~~**ReadMe: UNCHANGED — and this is the first record in this file that can say so truthfully.**
> 21,052 bytes / 234 lines, byte-identical to V0.6.3's, verified with a real `cmp`. Zero commits
> touched `README.md` in this range.~~ **STRUCK the same day.** It was true of the *first* cut and is
> false of the shipped one. Kept visible rather than deleted because the paragraph it was proudest of
> — reproducing the previous artifact byte-for-byte — is still the gate that ran, and because a
> record that quietly rewrote itself would hide the reason the readme changed.

**Why it changed, in the user's own framing:** *"readMe should not have a 'New in this build,' ever.
the ReadMe is not a changelog as is stated in claude.md. the changelog is mine to post on discord
when I actually ship the release."* **The changelog is the USER's artifact and it lives on Discord,
not in the zip.** `CLAUDE.md` already said "Never a changelog"; what this adds is *why* — there is a
real changelog with a real author, and the readme duplicating it badly is worse than useless.

Three passages went, and a **fourth problem surfaced only because the first three were being fixed**:
- **"New in this build: item and equipment descriptions now read the elements as words"** — the line
  V0.6.2, V0.6.3 and this record's own first draft all *flagged and shipped anyway*. **Removed
  outright rather than reworded**: element reading has no key and the player makes no decision about
  it, so under the keys-only rule there is nothing to look up and no entry to keep.
- The **Known Issues** button-prompt paragraph carried "still skipped", "this build fixes" and
  "silently dropped before". Rewritten to describe current behaviour, keeping the issue itself and
  the request to report a lost sentence ending.
- **`O`'s entry** said the Equipment screen "used to read" the standing help.
- **`O`'S ENTRY WAS ALSO FALSE, AND THAT IS THE PART THAT MATTERED.** It promised the Libra readout
  gives *"MP where the enemy has any"* — and **S160 took MP out deliberately** three commits before
  this build (`battle_target_reader.cpp:242`: a number the enemy does not spend). It had also never
  gained the **Absorb / Half / Immune** clauses S160 added. Now: HP as numbers, level, statuses, and
  what it is weak to, absorbs, halves or is immune to.

**The converter was re-validated a second time, immediately before the second conversion**, and
passed again: `git show 5f13705:README.md` still reproduces the shipped
`V0.6.3-Shotgun-Build\ReadMe.txt` byte-identically. Output audit of the new file: zero `#`, zero `*`,
zero `](`, zero leftover escapes, zero `&#x20;`, zero doubled spaces, no BOM, CRLF on all 232 lines
with no bare LF, and **exactly one backtick** — the literal `` ` `` key name, the same single
survivor as the last eight releases. The shipped artifact was then grepped directly for the offending
string: **zero occurrences of "new in this build", zero of the MP claim.**

**The converter had to be REBUILT, and that is the finding worth keeping from this release.** The
script V0.6.3 used lived in a session scratchpad and no longer exists — nothing in the repo carries
it. It was reconstructed **from the conversion rules recorded in this file and nothing else**, and
it reproduced the shipped `V0.6.3-Shotgun-Build\ReadMe.txt` from `git show 5f13705:README.md`
**byte-identically on the first attempt** — 21,052 bytes both, sha256 `4cf81a2d…9c10cbe3`. **So the
rules written down here are sufficient to regenerate the artifact, with no undocumented step.** That
is the strongest evidence this file has ever carried that its own procedure is complete, and it is
also the reason not to bother checking a converter into the repo. It keeps V0.6.3's fix: it writes
its output as **bytes** to a destination path given as an argument, so no shell can re-encode it.
Only `git show … > f` is still a redirect and still needs git-bash. (It earned its keep an hour
later, when the readme changed and the whole conversion had to be run a second time — the audit of
that run is above.)

**Readme key coverage: ONE GAP, FOUND LATE AND FIXED — and the way it was missed is the lesson.**
The *registration* half was settled by measurement and holds: `src\input\` was **not touched at all**
in this range (`git diff --stat 5f13705 HEAD -- src/input/` returns empty), and grepping the entire
`src/` diff for `DInputEdge`, `DInputMenuNavEdge`, `VK_` and `DIK_` returns **zero added or removed
lines**. No key was added, removed or rebound. But "no key was added" is only half the audit this
file asks for; the other half is **an existing key whose meaning changed**, and that half was
answered wrong:

> ~~**`o`, the Libra readout.** The readme's sentence — *"With Libra up the enemy gives real numbers
> too, and O reads the rest of what Libra reveals"* — is generic by construction and stayed true
> across a wholesale content change. **A readme line written about what a key is FOR survives
> changes to what it says.**~~ **STRUCK the same day. `o` HAD A SECOND ENTRY AND IT WAS FALSE.**

**THE README DESCRIBED `o` IN TWO PLACES AND ONLY ONE OF THEM WAS CHECKED.** Line 17, in Known
Issues, is the generic sentence quoted above, and it did survive. **Line 231, in the Reading key
list, is the real entry** — it enumerated the readout clause by clause and promised *"MP where the
enemy has any"*, which S160 had removed three commits earlier. The audit found the first, reasoned
about it correctly, and stopped. **A key can have more than one entry, and the one that goes stale
is the SPECIFIC one, never the generic one** — precisely because the generic one commits to nothing.
Grep the readme for the key and read **every** hit before declaring coverage; a single hit that reads
reassuringly is the failure mode. Fixed in `3c2c7fc`, with the Absorb/Half/Immune clauses added.

- **`-` / `=`, the navigation category cycle**, gained **Trap**. No new key; traps ride the existing
  cycle, and the readme names no category at all — the gap the V0.5 record flagged, carried forward
  unchanged rather than created here.

**One behavioural change to a documented key, flagged not fixed.** `=` now **skips** the Trap
category when the game's own visibility latch is clear (`entity_commands.cpp:135`), so the cycle has
a different number of stops with Libra up than with it down. Trap is the **only** category that can
vanish — the obvious generalisation "skip any empty category" would silence `"Shop, 0"` and every
other zero the cycle deliberately announces, which is a surface the tester navigates by (L-48). This
is not a readme gap under the keys-only rule, but it is the first time a documented key's stop list
is state-dependent. **If a tester reports "the category key skipped one", this is the answer.**

**Flagged, not fixed — one new, three inherited:**
- **NEW: the build stamp's version half has been stale for four releases.** Every log this zip writes
  opens with **`Build: V0.6 (c488495)`** while the zip says 0.6.4. `FFXII_SR_VERSION` in
  `CMakeLists.txt:185` has not moved since V0.6. The hash half is right — `c488495` was verified
  present in the shipped `dinput8.dll`'s own bytes — so a log still traces to a commit, which is why
  nobody has noticed. **This also falsified a claim in this file's own "What this procedure does NOT
  do", now struck above.** Bumping it is a build change and belongs in a commit *before* the next
  release trigger.
- ~~`ReadMe.txt` line 173 still carries **"New in this build…"** … Unavoidable here: with no readme
  commit ahead of this trigger, the shipped text cannot change.~~ **FIXED, and the excuse was wrong.**
  It was removed in `3c2c7fc` and this zip re-cut. **"A readme edit is a separate commit made before
  the trigger" is a rule about SCOPE, not a prohibition** — it stops a release quietly rewriting the
  readme, and it has been misread three releases running as a reason to ship a line already known to
  violate `CLAUDE.md`. **Flagging the same defect in three consecutive records is not respecting the
  rule, it is documenting a failure to apply it.** If a flag survives two records, fix it or say
  plainly why it cannot be fixed.
- **The Puzzle-guide paragraph still sits under the Stilshrine section**, so it still reads as though
  Puzzle guide and Instant success belong to the statues. Introduced by V0.6.3's own readme edit,
  still a two-line reorder.
- **`Docs\Controls.md` still does not list `Home`/`End` for the mod menu** — third release running.

**Purpose:** test build of Sessions 159–161. The user's framing at the trigger was *"this is just a
libra bug fix"*, and that is right about the headline — S159 and S160 are both the Libra readout —
**but the zip also carries S161's floor-trap category, which is navigation, is unverified, and ships
a diagnostic.** Recorded here so a later reader does not take "libra bug fix" as the whole contents.

**Play-confirmation status — MIXED, and this is the part to read before diagnosing anything on this
build.**
- **S159 — CONFIRMED, transitively and deliberately noted as such.** Its own session entry left the
  play-confirm gate OPEN. The **S160 play session on 2026-08-14 exercised `o` and it read**, which
  is exactly the gate S159 needed: a key that answers at all is a key no longer shadowed.
- **S160 — PARTIAL, and the unexercised half is named.** Confirmed 2026-08-14 on the immediate
  pre-release build: MP is gone (**zero** `o:` lines carry an MP clause), level, statuses and the
  **Weak** clause all read, and the baked falsifier printed libra bit30 = "Libra". **Absorb, Half and
  Immune did NOT fire** — the test enemies were a Giza Rabbit and a Hyena, and neither has any of the
  three. **Every clause S160 added is still unexercised**; "the readout works" means nothing
  regressed, not that the feature landed. Do not upgrade this without a log against an enemy that
  actually has one of the three.
- **S161 — UNVERIFIED, and it could not be verified before the build.** There is **no save near a
  trap dungeon**, which is why it went straight to C++ with the instrument attached. The trap list
  has never run against real data. What *is* checkable on any map is the negative: the category is
  skipped with Libra down, every other category announces exactly as before, and the scan runs on
  rescan rather than per frame.
- **A DIAGNOSTIC SHIPS IN THIS ZIP AND IS MEANT TO BE DELETED.** `entity_scan.cpp:337` emits one
  `[NAV] traps: map=… latch=… mask=0x… tableCount=… listed=… [i]=(x,z) r=… flag=…` line per distinct
  `(map, mask, latch, count)` state — not throttled, not capped, silent while standing still.
  **One log from Barheim Passage, Lhusu Mines, Zertinan Caverns or Garamsythe Waterway settles the
  whole chain in a single pass**: a plausible count with in-map coordinates confirms it, an absurd
  `tableCount` condemns `TRAP_TABLE`, and a latch that never reads 1 under Libra condemns the gate.
  **Delete the block once that log exists.**
- **The S160 build-stamp caveat does NOT apply to this zip.** That session's log read one commit
  behind because its DLL was built before the session was committed. This build was made after
  `c488495` and the hash baked into the shipped DLL is HEAD — confirmed by finding `c488495` in the
  binary itself, not by assuming the configure step re-ran.

**⚠ S152's four converted frame budgets STILL have no targeted measurement**, and they are in this
binary exactly as they were in V0.6.3's. **A nav, beacon, menu or dialogue timing fault on this build
should suspect `592e142` before the session that owns the surface.** The `textWalk …/frame` probe
ships again and is still how the "aaaaaaaaa" runaway gets settled — roughly `1.00` per frame refutes
the sim-loop lead, roughly `4.00` at 4× speed confirms it.

**Known-unfinished items still shipping, unchanged and still open in `Docs\debug.md`:** the **Clan
Primer wrap-around settle is SILENT** (S127, tester-accepted, with the do-not-fix-by-reverting
warning recorded there), and the `__MJ_CTRL` exit-builder widening means maps **318, 319, 321, 322
and 568** may list exits they previously dropped.

## V0.6.3-Shotgun-Build — 2026-08-13

**Built from:** `5f13705`, which is also HEAD, so nothing in this binary post-dates S158. Tree clean
before and after. Covers **Sessions 152–158** since `V0.6.2-Shotgun-Build`'s `75af007` — six commits,
of which only **two carry code**: `592e142` (S152) and `5f13705` (S153–S158, one commit for six
sessions). The other four are documentation, one of them V0.6.2's own release record.

**The version number was changed before the build, and the reason belongs here.** The user asked for
"0.5.3-Shotgun Build". Releases number upward and the previous one is **V0.6.2**, so `0.5.3` would
have sorted this build *below* the release it supersedes — a zip a player could reasonably read as
older. Asked rather than assumed; the user confirmed **0.6.3**. Directory and zip are
`V0.6.3-Shotgun-Build`, hyphenated to keep a space out of the zip name, as V0.6, V0.6.1 and V0.6.2
all were.

**Zip:** `FFXII-Screen-ReaderV0.6.3-Shotgun-Build.zip`, 1,242,229 bytes, five files, root flat.
- `dinput8.dll` 878,080 bytes (sha256 `643e2c36…375f5578`) — up from V0.6.2's 846,848; seven sessions
  covering the frame-budget conversion, the presence pruner's scope fix, the Stilshrine statue
  readout, the bare-key F-key guard, the beacon's driving gate, and the gambit editor's row decode
  and picker.
- `SDL3.dll` 1,748,992 bytes from `build\SDL3-build\Release\` — **byte-identical to V0.6.2's,
  V0.6.1's and V0.6's** (sha256 `056db4a9…fa3a1d19`), same source, unchanged build. **Four releases
  running.**
- TTS pair carried over unchanged from `V0.6.2-Shotgun-Build` (`Tolk.dll` 122,368 sha256
  `c4fb11d3…48197225`, `nvdaControllerClient64.dll` 153,600 sha256 `41c1f5df…b23a0b09`).
- All four DLLs verified PE machine `8664`.

**ReadMe: CHANGED** — 21,052 bytes / 234 lines (sha256 `4cf81a2d…9c10cbe3`), against V0.6.2's 20,879
/ 228. **One commit touched `README.md` in this range** (`5f13705`) and it made exactly three edits:
the new **Stilshrine of Miriam: the three statues** section on `B`, and two "field menu" → "party
menu" renames — S158's reversal of the Session 93 vocabulary, at the tester's request.

**Converter re-validated the documented way, and it passed.** Run against `git show 75af007:README.md`
it reproduced the shipped `Releases\V0.6.2-Shotgun-Build\ReadMe.txt` **byte-identically** — 20,879
bytes both, sha256 `1d9f2db7…c1ba4e4a`. The current file was then converted from `git show
HEAD:README.md` with the same code. Rules unchanged from V0.6.2. Output audit: zero `#`, zero `*`,
zero `](`, zero leftover escapes, zero doubled spaces, no BOM, CRLF on all 234 lines with no bare LF,
and **exactly one backtick** — line 89's literal `` ` `` key name, the same single survivor as the
last seven releases.

> **V0.6.2's byte-exactness trap was avoided by construction rather than worked around.** That record
> records PowerShell's `>` re-encoding redirected output and producing a false MISMATCH. The fix used
> then was to run the whole validation through git-bash. This time the converter **writes its output
> as bytes itself** and takes the destination path as an argument, so the conversion step cannot be
> spoiled by whichever shell it runs under. `git show … > f` is still a shell redirect and still needs
> git-bash; only that one step does.

**Readme key coverage: NO GAPS — and no key was added or removed in this range.** Every `DInputEdge` /
`DInputMenuNavEdge` registration in `input_tracker.cpp` was diffed against `75af007`, not sampled.
Two existing keys changed, and both are already covered:
- **`B` gained a second context** — the Stilshrine of Miriam statue readout, alongside Bhujerba's
  infamy meter. The two can never both be live, and `nav_commands.cpp` raises both requests rather
  than deciding which applies on the input thread. The readme gives it its own section, and
  `Docs\Controls.md` carries it too.
- **The bare-press rule now actually holds for all six F-keys.** S155 moved `F4`, `F5`, `F6`, `F7`
  and `F8` onto the same `bareF()` guard `F11` already had. The readme **needed no edit**: it already
  asserted the rule in general terms ("Every mod key is pressed on its own — no Shift, Ctrl or Alt")
  and spelled it out for `F11`. Before S155 that claim was true only of `F11` — `Alt+F4` flipped
  Combat verbosity as the OS closed the game. **The code caught up to what the readme already said**,
  which is the one shape of key change that produces no readme work.

**Flagged, not fixed — one stale line, one new misdirection, one doc gap:**
- `ReadMe.txt` line 173 still carries **"New in this build: item and equipment descriptions now read
  the elements as words"**. That was new in **V0.6** (S125), three releases ago, and it is the
  changelog framing the README rule in `CLAUDE.md` forbids. V0.6.2 flagged it and it is still
  shipping. It belongs in the next readme commit.
- **NEW, and introduced by this release's own readme edit.** The Stilshrine section was inserted
  between the Bhujerba paragraph and the sentence that follows it, so **"Two settings appear in the
  mod menu while you are shouting, and only then. Puzzle guide covers…" now sits directly under the
  Stilshrine section** and reads as though Puzzle guide and Instant success belong to the statues.
  They do not: `mod_menu.cpp:97` gates both rows on `ShoutMeter::PuzzleActive`, and the statue
  readout consults neither. A player reading in order will look for a Puzzle guide row in the
  Stilshrine and never find one. **Move that paragraph above the Stilshrine section** — a two-line
  reorder in the next readme commit, not a rewrite.
- **`Docs\Controls.md` still does not list `Home`/`End` for the mod menu** — the same gap V0.6.2
  flagged, and the one the README closed for itself in `75af007`. Controls.md's mod-menu row (line
  176) and its section (line 312) still stop at Up/Down, Left/Right and `o`.

**Purpose:** shotgun build of Sessions 152–158. What the tester is exercising: the four frame budgets
now on wall-clock deadlines (S152), the presence pruner that no longer deletes gate crystals and
named NPCs (S153), the Stilshrine of Miriam statue readout on `B` (S154/S156/S157), the bare-key
F-key guard (S155), the audio beacon's "is the player driving" gate (S157), and the gambit editor's
incomplete-row decode and its picker (S158).

**Play-confirmation status — MIXED, and this is the part to read before diagnosing anything on this
build.** Unlike V0.5's blanket attestation, this range has per-session evidence:
- **Confirmed 2026-08-13**, on the build that became this one: the gambit editor's incomplete-row
  decode and the gambit picker (S158, both halves — the picker from that session's own log: eleven
  category switches, zero stale items, zero duplicate `[LICENSE] summary` lines), and the
  **Stilshrine statue readout** (S157 — map 599 reads from anywhere immediately; map 600 needs one
  entry into Walk of Reason before it reads from elsewhere).
- **Partially confirmed:** the beacon's driving gate (S157) is **confirmed in MENUS**, the case its
  report was filed against. The **cutscene and dialogue cases are not verified** — the tester
  deliberately left them for a later pass — so `IsBoxLive()` carrying a captioned scene remains an
  inference. A silent camera scene with no message box has no measured signal at all. **Do not
  upgrade either without a log.**
- **NOT exercised:** S153's presence-pruner fix, and S155/S156. S153's play-confirm gate is written
  down in the session log — stand at the Rabanastre gate crystal and expect `Gate=1` in the rescan
  line, the crystal reachable with `=`/`\`, a `spared:` line naming it, and the `ABSENT` count still
  rising when a Hyena dies.
- **⚠ S152's four converted budgets have NO targeted measurement.** They were deployed 2026-08-12 and
  they are in the build played on 2026-08-13, so they have been *in play* — but nothing measured
  them. **S152 touched nav, beacon, menu and dialogue timing in one commit, so a fault in any of
  those four on this build should suspect it before the session that owns the surface.**

**Known-unfinished items still shipping, unchanged and still open in `Docs\debug.md`:** the **Clan
Primer wrap-around settle is SILENT** (S127, tester-accepted, with the do-not-fix-by-reverting
warning recorded there), and the `__MJ_CTRL` exit-builder widening means maps **318, 319, 321, 322
and 568** may list exits they previously dropped.

**The three frame-counting offenders V0.6.2 named are CLOSED in this build** — `path_planner` and
`nav_probe` (`kWaitFrames=90` → `kWaitMs=1500`) and `audio_beacon` (`kStrayFrames=45` → `kStrayMs=750`),
plus a fourth in `menu_reader`. **But the biggest item in `Docs\PerFrameAudit.md` is OPEN, not
closed:** the tester's "aaaaaaaaa" runaway reproduces at **raised game speed**, not at high frame
rate, which points at a hook inside the sim loop rather than at any frame budget. The `textWalk
…/frame` probe **ships in this build** and is how that gets settled — roughly `1.00` per frame refutes
the sim-loop lead, roughly `4.00` at 4× speed confirms it.

## V0.6.2-Shotgun-Build — 2026-08-11

**Built from:** `75af007`. Tree clean before and after. **The DLL's code traces to `f017e24`** — the
one commit after it is the readme change below, so nothing in this binary post-dates S151. Covers
**Sessions 147–151** (8 commits) since `V0.6.1-Shotgun-Build`'s `42014e0`, one of which (`140d868`)
is V0.6.1's own release record.

**Directory name normalised**, as V0.6 and V0.6.1 were: the user asked for "0.6.2-Shotgun Build" and
the directory and zip are `V0.6.2-Shotgun-Build`, hyphenated to keep a space out of the zip name.

**Zip:** `FFXII-Screen-ReaderV0.6.2-Shotgun-Build.zip`, 1,227,715 bytes, five files, root flat.
- `dinput8.dll` 846,848 bytes (sha256 `986ed4d0…4740238b`) — up from V0.6.1's 821,760; five sessions
  covering the stale-entity pruner, Esper vitals, the HP display clamp, the license board's own
  availability bit, collected treasure, and the off-hand cursor host.
- `SDL3.dll` 1,748,992 bytes from `build\SDL3-build\Release\` — **byte-identical to V0.6.1's and
  V0.6's** (sha256 `056db4a9…fa3a1d19`), same source, unchanged build. Three releases running.
- TTS pair carried over unchanged from `V0.6.1-Shotgun-Build` (`Tolk.dll` 122,368 sha256
  `c4fb11d3…48197225`, `nvdaControllerClient64.dll` 153,600 sha256 `41c1f5df…cb23a0b09`).
- All four DLLs verified PE machine `8664`.

**ReadMe: CHANGED** — 20,879 bytes / 228 lines (sha256 `1d9f2db7…c1ba4e4a`), against V0.6.1's 19,968
/ 222. Two commits touched `README.md` in this range: `367b10f` (S147/S148 — the Libra sentence, `F7`,
key `8` for the summoned Esper, and Auto detail replacing the removed Text glyphs row) and `75af007`,
the readme commit made immediately before this release trigger.

**Converter re-validated the documented way, and it passed.** Run against `git show 42014e0:README.md`
it reproduced the shipped `Releases\V0.6.1-Shotgun-Build\ReadMe.txt` **byte-identically** — 19,968
bytes both, sha256 `443e6ec9…a0e9680c`. The current file was then converted from `git show
HEAD:README.md` with the same code. Rules unchanged from V0.6.1. Output audit: zero `#`, zero `*`,
zero `](`, zero leftover escapes, no BOM, CRLF on all 228 lines, and **exactly one backtick** — the
literal `` ` `` key name, the same single survivor as the last six releases.

> **A trap worth recording, because it produced a false MISMATCH on the first attempt.** The
> validation must be run through a **byte-exact** shell. PowerShell's `>` re-encodes redirected
> output (it added a BOM and five bytes here), so `git show … > f` and `python md2txt.py … > f` both
> produce a file that differs from the artifact for reasons that have nothing to do with the
> converter. Use git-bash for the redirect and `cmp`; the same run then matched exactly.

**Readme key coverage: TWO GAPS FOUND AND CLOSED IN THIS RELEASE'S README COMMIT.** Every key
`input_tracker.cpp` registers was audited against `README.md`, not sampled. Two contexts had no
entry at all, both of them keys that already worked:
- **`Home` / `End` in the `F8` mod menu** — first and last setting (`mod_menu.cpp:261-268`). The
  readme documented `Up`/`Down` and `Left`/`Right` there and stopped.
- **`Up` / `Down` on an open Clan Primer entry** — step the page a line at a time
  (`primer_reader.cpp:496-499`). `Home`/`End` on that surface *were* documented, but only inside the
  combat log's exception paragraph; `Up`/`Down` appeared nowhere, and the Primer had no section of
  its own for a reader to look them up in. It has a two-sentence one now.

**One key changed meaning on a screen, and the readme still described the old behaviour.** `4` on
the party menu's **Equipment** screen: S150 put that per-highlight stat preview behind **AutoDetail**
(`equip_compare.cpp:405`), so the readme's "is the exception, and deliberately so" — i.e. it reads
automatically — had been false since. Corrected. `4` still reads it in both modes; AutoDetail only
decides whether it *also* speaks on its own.

**Flagged, not fixed — one stale line and one doc gap:**
- `README.md` still carries **"New in this build: item and equipment descriptions now read the
  elements as words"**. That was new in **V0.6** (S125), two releases ago, and it is changelog
  framing the README rule in `CLAUDE.md` forbids. Left alone because this release's readme commit
  was deliberately scoped to the keys the user asked for; it belongs in the next readme commit.
- **`Docs\Controls.md` does not list `Home`/`End` for the mod menu either** — the same gap the readme
  had, in the canonical dev reference. The Primer `Up`/`Down` entry is present there.
- The V0.6.1 record's mod-menu **row-ordering** flag (readme lists Text glyphs before Auto-walk while
  `kSettings` walks Auto-walk first) is **closed by attrition** — Text glyphs was removed in S147, so
  the readme's list and `kSettings` now agree.

**Repo housekeeping done at this release, and it is worth recording because it was a first.** All
three worktrees were verified clean and **every branch was pushed to `origin` for the first time** —
`combat-system` (the trunk), `nav/surface-goal` and `nav/event-transfer` had existed only locally, so
until now the sole branch on the remote was the long-stale `master`. One uncommitted change was found
in the `nav/surface-goal` worktree and committed there rather than discarded (`8d29827`): it derives
the SDL3 source path from `CMAKE_CURRENT_SOURCE_DIR` instead of a literal `D:/Games/Dev`. It resolves
to the same directory on this checkout, so the build is unaffected, and it was **deliberately not
merged to `combat-system`**, which was about to cut this build.

**Purpose:** shotgun build of Sessions 147–151. What the tester is exercising: the stale-entity
pruner and the Esper vitals on `8` (S147/S148), the HP display clamp, the license board's spoken
availability (S149), collected-treasure pruning (S150), and the off-hand/shield candidate list, which
was silent on every cursor move until S151 found its cursor lives on a host object at
`container+0xC0`.

**Play-confirmation status.** S149, S150's two pruners and S151 are all play-confirmed on the current
build (2026-08-11). **The S148 low-HP threshold is the one thing in this range still unconfirmed** —
the 20% latch now divides by the clamped `BC_MAXHP` rather than the raw field, and no session records
a play check of it. Treat a wrong-sounding low-HP warning in this build as new, not inherited.

**Known-unfinished items still shipping, unchanged and still open in `Docs\debug.md`:** the **Clan
Primer wrap-around settle is SILENT** (S127, tester-accepted, with the do-not-fix-by-reverting
warning recorded there — note this build gives that surface its own readme section for the first
time, so a tester is now more likely to walk into it), and the `__MJ_CTRL` exit-builder widening
means maps **318, 319, 321, 322 and 568** may list exits they previously dropped. **Also open and
NOT a session's work yet: the three frame-counting offenders** in `Docs\PerFrameAudit.md` —
`path_planner` and `nav_probe` (`kWaitFrames=90`) and `audio_beacon` (`kStrayFrames=45`). At 144 fps
the route budget is 0.63 s rather than the 1.5 s its comment claims, so a high-refresh tester can see
spurious "No path". This is the first release record to name them.

## V0.6.1-Shotgun-Build — 2026-08-05

**Built from:** `42014e0`. Tree clean before and after. **The DLL's code traces to `c907b7a`** — the
one commit after it is the readme change below, so nothing in this binary post-dates S146. Covers
**Sessions 128–146** (25 commits) since `V0.6-Test-Build`'s `a5a7900`.

**Directory name normalised**, the same way V0.6's was: the user asked for "0.6.1-Shotgun Build" and
the directory and zip are `V0.6.1-Shotgun-Build`, hyphenated to keep a space out of the zip name.
Capitalisation follows the user's own, as V0.6 did.

**Zip:** `FFXII-Screen-ReaderV0.6.1-Shotgun-Build.zip`, 1,214,891 bytes, five files, root flat.
- `dinput8.dll` 821,760 bytes (sha256 `ff8a09ea…bc4a46bf`) — up from V0.6's 783,360; nineteen
  sessions covering the Bhujerba shout minigame, the Polish glyph overrides, the MinHook
  trampoline-exhaustion fix, the shop detour fix, and the battle-menu second column.
- `SDL3.dll` 1,748,992 bytes from `build\SDL3-build\Release\` — **byte-identical to V0.6's**
  (sha256 `056db4a9…fa3a1d19`), same source, unchanged build.
- TTS pair carried over unchanged from `V0.6-Test-Build` (`Tolk.dll` 122,368,
  `nvdaControllerClient64.dll` 153,600).
- All four DLLs verified PE machine `8664`.

**ReadMe: CHANGED** — 19,968 bytes / 222 lines (sha256 `443e6ec9…a0e9680c`), against V0.6's 18,527 /
210. Four commits touched `README.md` in this range: `a51af2e` and `7911c7c` (the Bhujerba section —
`B`, `N`, the self-speaking meter, and the two context-gated settings), `e886a01` (S139), and
`42014e0`, the readme commit made immediately before this release trigger.

**Converter re-validated the documented way, and it passed.** Run against `git show a5a7900:README.md`
it reproduced the shipped `Releases\V0.6-Test-Build\ReadMe.txt` **byte-identically** — 18,527 bytes
both, sha256 `49f4c92a…`. The current file was then converted from `git show HEAD:README.md` with the
same code. Rules unchanged from V0.6, including the paired-code-span unwrap and the lone-backtick
exemption. Output audit: zero `#`, zero `*`, zero `](`, zero leftover escapes, no BOM, CRLF on all 222
lines, and **exactly one backtick** — the literal `` ` `` key name, the same single survivor as the
last five releases.

**Readme key coverage: no gaps.** `B` and `N` are the only keys added since V0.6 and both are
documented, as is `O`'s behaviour in the mod menu. No existing key changed meaning on any screen.

**One flag, not fixed here:** the mod-menu settings list in `README.md` orders Text glyphs before
Auto-walk, while the menu's Up/Down walks Auto-walk first (`kSettings` order in `mod_menu.cpp`). It
misleads only a player counting rows, so it is recorded rather than fixed — readme edits are a
separate commit made *before* the release trigger, and this release's readme commit was deliberately
scoped to the puzzle rows the user asked for.

**What the readme commit added.** The Bhujerba section already documented `B`, `N` and the spoken
meter from S131/S132, but the mod menu's own "settings it holds" list stopped at Auto-walk. A player
looking settings up therefore never saw **Puzzle guide** or **Instant success**, and had no way to
learn the two rows are hidden until a shout sequence is live — which is the one thing needed to find
them. Both are now in that list with their defaults (guide On, instant success Off) and the
visibility condition.

**Purpose:** shotgun build of Sessions 128–146. What the tester is exercising: the Bhujerba shout
minigame end to end (spoken infamy meter, `B`, `N`, the two mod-menu rows, earshot at 3.0 m from
S141), the Polish fan-translation glyph overrides (S130), the combat log after the MinHook
trampoline-slot fix (S130), the shop crash fix (S129), and the battle menu's second column (S146).

**Both of V0.6's known-unfinished items still ship in this zip**, unchanged and still open in
`Docs\debug.md`: the **Clan Primer wrap-around settle is SILENT** (Session 127, tester-accepted as the
better of the two behaviours, with the four candidate causes and the do-not-fix-by-reverting warning
recorded there), and the `__MJ_CTRL` exit-builder widening means maps **318, 319, 321, 322 and 568**
may list exits they previously dropped — still the maps logging unclaimed surfaces.

## V0.6-Test-Build — 2026-08-03

**Built from:** `a5a7900`. Tree clean before and after. **The DLL's code traces to `7f90dff`** — the
three commits after it (`304e5df`, `73c61aa`, `a5a7900`) are documentation only, so nothing in this
binary post-dates S126+S127. Covers **Sessions 124–127** since `V0.5-Sponsor-build`'s `023e58f`.

**Directory name normalised.** The user asked for "0.6-Test Build"; the directory and zip are
`V0.6-Test-Build`, hyphenated to match `V0.5-Sponsor-build` / `V0.2-test-build` and to keep a space
out of the zip name.

**Zip:** `FFXII-Screen-ReaderV0.6-Test-Build.zip`, 1,196,553 bytes, five files, root flat.
- `dinput8.dll` 783,360 bytes (sha256 `351806bc…f575d3f6`) — up from V0.5's 720,384; four sessions of
  menu readers (equip comparison, save slots, Clan Primer, element decoding).
- `SDL3.dll` 1,748,992 bytes from `build\SDL3-build\Release\` — byte-identical to V0.5's, same source.
- TTS pair carried over unchanged from `V0.5-Sponsor-build` (`Tolk.dll` 122,368,
  `nvdaControllerClient64.dll` 153,600).
- All four DLLs verified PE machine `8664`.

**ReadMe: CHANGED** — 18,527 bytes / 210 lines, against V0.5's 16,394 / 191. One commit touched it
(`73c61aa`): keys `8`/`9`, the `4`-`9` context switch, the `Home`/`End` page exception, a Gambits
entry, and the mod-menu setting list. **The final wording is the tester's own edit** — they cut the
version I wrote back by roughly half and the house rule that came out of it is in `CLAUDE.md`
("README edits — CONCISE. Keys only. Never a changelog.").

**Converter re-validated the V0.5 way, and it passed.** Run against `git show 023e58f:README.md`, it
reproduced the shipped `Releases\V0.5-Sponsor-build\ReadMe.txt` **byte-identically** — 16,394 bytes
both, sha256 `c37e06fc…790aeb`. The current file was then converted from `git show HEAD:README.md`
with the same code. Rules unchanged from V0.5, including the paired-code-span unwrap. Output audit:
zero `#`, zero `*`, zero `](`, zero leftover escapes, no BOM, and **exactly one backtick** — line
89's literal `` ` `` key name, the same single survivor as the last four releases.

**Readme coverage — and a correction to how every record below measured it.** The readme documents
**keys**: what a key does, and the screen contexts where its meaning changes. Every mod key is in it,
including this build's `8`/`9`, the `4`-`9` context switch, the `Home`/`End` page exception and the
Gambit screen.

**The "gaps shipped" lists in the V0.2.1–V0.5 records were measuring the wrong thing.** They counted
*features* absent from the readme — Game Over, the battle damage-line element, the leader prompt, the
notice board, the shop item list, inventory quantity, `LP` on the defeat line — and **not one of those
has a key.** They read on their own, with no decision for the player, so there is nothing for a reader
to look up and no entry to be missing. Tester, 2026-08-03: *"none of that matters for end users… they
only need to know the keys and roughly what does what, with screen contexts. The readme is not a
changelog… by your logic we should break down the gambits screen for people, and at that point we
might as well write a walkthrough."* **Stop generating that list.** The only thing worth flagging in a
future record is a KEY that is missing or wrong.

Sneak assist came out of the readme in the tester's edit and stays out: it is automatic, has no key
and needs no player action. V0.5's standing caution — automatic, no key, `F10` free again and never to
be documented as a sneak key — belongs in `CLAUDE.md` and `Docs\Controls.md`, which both carry it.

**Purpose:** test build of Sessions 124–127. What the tester is exercising: the funnel polarity fix
that cleared map 315's replan "No path" (S124); element names, the battle damage type, Game Over, the
shop/equip comparison on `4`-`9` and the expanded status lists (S125, all play-confirmed on 2026-08-03
before this build); the four log-visible defects — Bhujerba's section announcements, Lhusu Mines'
missing onward exits, shops speaking on open, and the clerk re-speaking on re-entry (S126); and save
slots plus the Clan Primer (S127).

**Two known-unfinished items ship in this zip.** The **Clan Primer wrap-around settle is SILENT** —
scrolling a list past its end announces nothing on the row it lands on. Tester-accepted as the better
of the two behaviours (it replaced a *stale-text* read) and documented in Known Issues;
`Docs\debug.md` holds the four candidate causes and the instruction to add the missing log line
before attempting a fix, and the warning not to fix it by reverting the deferral. Separately, the
`__MJ_CTRL` exit-builder widening from S126 means maps **318, 319, 321, 322 and 568** may now list
exits they previously dropped — those are the maps still logging unclaimed surfaces, and this is the
first build a player walks them on. **Map 569 is ruled out** of that set.

## V0.5-Sponsor-build — 2026-08-01

**Built from:** `023e58f` (Sessions 84–123). Tree clean before and after; no code change for the
release. **63 commits** since `V0.2.1-shotgun-build`'s `877ed2a` — the largest gap between two
releases so far, spanning the audio beacon, the mod menu, the navmesh route rebuild, auto-walk, and
the palace sneak assist.

**FIRST FIVE-FILE RELEASE.** `SDL3.dll` ships for the first time. It was added to the procedure in
Session 92; every release before this one predates it and shipped four files. This is the file that
turns a partial failure into a total one — without it `dinput8.dll` does not load and the game does
not start, so a zip that omits it is not a degraded release, it is a broken one.

**Zip:** `FFXII-Screen-ReaderV0.5-Sponsor-build.zip`, 1,169,183 bytes, five files, root flat.
- `dinput8.dll` 720,384 bytes (sha256 `65408386…e9ab64`) — 1.65× V0.2.1's 435,712. The growth is
  forty sessions of navigation/audio/menu code plus the beacon WAVs embedded as RCDATA
  (`src\audio\beacon_assets.rc`), **not** a debug build; the deliberate absence of a loose asset
  folder in the zip is what puts those bytes inside the DLL.
- `SDL3.dll` 1,748,992 bytes, from `build\SDL3-build\Release\` — our own build, in step with the
  DLL beside it, never sourced from elsewhere.
- TTS pair carried over unchanged from `V0.2.1-shotgun-build` (`Tolk.dll` 122,368,
  `nvdaControllerClient64.dll` 153,600).
- All four DLLs verified PE machine `8664`.

**ReadMe: CHANGED** — 16,394 bytes / 191 lines, against V0.2.1's 9,526 / 136. Ten commits touched
`README.md` in this range (the beacon, the F8 menu, auto-walk, the F9→F11 beacon move, the sneak
section, and the withdrawal of the false "NVDA keys do not work" note).

**The converter was validated by a real `cmp` this time, and it is worth recording how.** Because
`README.md` changed heavily, the V0.2.1 method — diff the new conversion against the previous
`ReadMe.txt` and require the shared lines to match — would have compared two mostly-different files
and proved little. Instead the converter was run against **`git show 877ed2a:README.md`**, the
readme as it stood at the *previous* release, and its output compared to the shipped
`Releases\V0.2.1-shotgun-build\ReadMe.txt`: **byte-identical, 9,526 bytes both, sha256
`5c845c43…dd340b`.** That reproduces a known-good artifact from its own source and is a stronger
gate than any diff of the current file. **This is the answer to the V0.2 failure recorded below**
(a `cmp` written down as passing when it could not have been run) — regenerate the *previous*
release and match it, then convert the current one with the same code. The current `ReadMe.txt` was
likewise converted from **`git show HEAD:README.md`**, not the working tree, so the V0.2
uncommitted-source mistake cannot recur even though the tree was clean.

**Conversion rules — one addition since V0.2.1.** Unchanged: strip heading and bullet markers,
strip code-fence lines, strip `**`, flatten `[text](url)` → `text (url)`, unescape `\[` `\]` `\-`
`\\` `\_` `\*`, drop `&#x20;`, collapse doubled spaces, right-trim, drop trailing blank lines, CRLF,
UTF-8 no BOM. **New:** paired code spans are unwrapped — `` `F11` `` → `F11` — because the readme
now uses them on the beacon-troubleshooting line, while a **lone** backtick is left alone, since an
unpaired backtick in this readme is the ` key's own name and therefore content. Output audit: zero
`#`, zero `*`, zero `](`, zero leftover backslash escapes, and **exactly one backtick** — line 91's
literal `` ` `` key name, the same single survivor as the last three releases.

**Readme gaps still shipped in this build (flagged, not fixed).** The V0.2.1 record listed four;
one closed on its own and three did not, and this build adds four more. Verified by keyword search
of the shipped `ReadMe.txt`, not from memory:
- **Shop reader** — Buy, Sell and Bazaar (Session 69). Still no entry, now three releases running.
  The only `buy` in the file is "Buy and install … on Steam".
- **Inventory quantity + category switching** (Session 70). Still no entry; `quantity` appears zero
  times.
- **Ground loot in the navigation Items category** (Session 72). The `-`/`=` category keys are
  documented but no category is ever named, so a tester has no way to learn ground loot is one.
- **EXP/LP defeat line** (Session 72) — **partly closed.** The Combat verbosity description now says
  Normal "speaks enemy defeat and EXP", but `LP` appears nowhere, and the line the mod actually
  emits is `"Dire Rat defeated. 34 EXP, 2 LP."`
- **Notice board reader** (Session 87) — new gap. `notice` occurs once and it is the palace guards.
- **In-dialogue choices** (Session 87) — new gap. Both `choice` hits are "your choices are
  remembered between sessions" in the beacon/menu sections.
- **Party membership on the party menu** (Session 93) — new gap. `R` is listed only as a game key.
- **Equipment reading** — `equip` occurs zero times.

Not fixed here because readme edits are a separate commit made *before* the release trigger (see
"What this procedure does NOT do"). **These belong in a readme commit ahead of the next release**,
and the shop reader has now survived three of them.

**Purpose:** sponsor build — the first zip a non-tester audience receives, and the first that is
playable end-to-end without the tester's own SDL3 in place.

**Play-confirmation status — CLEARED BY THE TESTER, 2026-08-01.** This record first said that
everything from Session 93 onward was "built and logging but not play-confirmed", because only four
features (map 315's routes and auto-walk, map 313's dungeon staircase, the sneak assist on map 568,
map 572's event exit) had a session entry recording a play check. **On 2026-08-01, at this release,
the tester stated that all of it had in fact been confirmed in play and that the confirmations
simply were not reported back at the time.** The tester is the only person who plays this game, so
their word is the primary evidence and it supersedes the silence of the logs: **the whole Session
84–123 feature set is play-confirmed**, including the notice board, in-dialogue choices, the
personal-name NPC lookup, the party-menu party list, the mod menu, the beacons, and the navmesh
route rebuild.

**Note the shape of this evidence, because it changes how a future defect should be read.** It is a
single blanket attestation covering forty sessions, not forty per-feature checks each recorded
against the build it was made on. It establishes that these features *worked when played*; it does
not establish *which build* each was last exercised on, so it cannot by itself localise a
regression. If something in this range misbehaves in a later build, the correct response is a fresh
measurement on the current build — not "this was confirmed, therefore the fault is elsewhere."
That is the same trap as the V0.2 `cmp` recorded above: a status written down once and then leaned
on as though it were a live check.

**The standing lesson stands unchanged: report confirmations as they happen.** The gap this entry
repairs cost nothing here only because the tester caught it. Four sessions' worth of "not
play-confirmed" warnings were carried forward into planning and into this record while the features
had in fact been working the whole time.

**Two standing cautions carried into this zip.** Auto-walk is the mod's one authorized write to game
input and is **default OFF** — with the toggle off the injection function returns on its first line,
so the input path is byte-identical to the read-only mod. And the sneak assist is **automatic with
no key**, on the palace maps only; `F10`, which an earlier design reserved for it, is free again and
must not be documented as a sneak key.

## V0.2.1-shotgun-build — 2026-07-27

**Built from:** `877ed2a` (Sessions 74–83 — the navigation rebuild). Tree clean before and after;
no code change for the release. Three commits since `V0.2-test-build`: `44efdcc` (navmesh routing
rebuild + an NPC class that was invisible on every map), `676e86c` (readme + doc rename), `877ed2a`
(NPC personal names from the game's own npcdic slot; phantom entries dropped by placement +
reachability).

**Zip:** `FFXII-Screen-ReaderV0.2.1-shotgun-build.zip`, 339,420 bytes, four files, root flat.
`dinput8.dll` 435,712 bytes (sha256 `530607ea…c3f01f`) — *smaller* than V0.2's 446,464, which is
the `nav_grid` deletion of Session 75, not a truncated build. All three DLLs verified PE machine
`8664`. TTS pair carried over unchanged from `V0.2-test-build`.

**ReadMe: CHANGED** — first release since `V0.1.1` where it did. `README.md` was edited in
`676e86c`, so the converted `ReadMe.txt` is 9,526 bytes / 136 lines against V0.2's 9,164 / 132.
Validated by diffing the new conversion against `V0.2-test-build\ReadMe.txt`: **the only delta is
the four-line Status-screen block**, and all 132 shared lines come out byte-identical — that is what
confirms the converter, since a byte-identical whole-file `cmp` is no longer available once the
source changes. Conversion is reproducible and unchanged: strip heading/bullet markers, unescape
`\[` `\-` `\\` `\_`, drop `&#x20;`, flatten `[text](url)`, collapse doubled spaces, right-trim,
drop trailing blank lines, CRLF, UTF-8 no BOM. Output re-checked for leftover markup: zero `#`,
zero `**`, zero `](`, and exactly one backtick — line 83's literal `` ` `` key name, which is
content.

**Correction to the V0.2-test-build record below — two of its claims are false.** That record says
its `ReadMe.txt` was "byte-identical to `V0.1.1-shotgun-build\ReadMe.txt` (verified with `cmp`, not
by eye)" and lists `g` and `;` among the readme gaps it shipped. Neither holds: the two files are
8,979 vs 9,164 bytes and differ, and `V0.2-test-build\ReadMe.txt` already carries the `g:`, `;:`
and `':` entries. What actually happened is that V0.2's `ReadMe.txt` was
converted from an **uncommitted working-tree `README.md`** — those edits did not reach git until
`676e86c`, three days later. So the shipped V0.2 zip documented `g` and `;` while its own record
says it did not, and the `cmp` gate was recorded as passing when it cannot have been run. **The
lesson for future releases: convert from the committed `README.md`** (`git show HEAD:README.md`)
if there is any doubt, and never write down a `cmp` result that was not actually produced.

**Readme gaps still shipped in this build (flagged, not fixed).** `676e86c` closed three of the
seven gaps V0.2 flagged — `g`, the reworded `;`, and the Status screen all now have entries. Still
undocumented for testers of this zip:
- **Shop reader** — Buy, Sell and Bazaar (Session 69). No entry at all.
- **Inventory quantity + category switching** (Session 70).
- **Ground loot appearing in the navigation Items category** (Session 72). The combat-log paragraph
  mentions "loot, gil" as spoken battle events, which is a different surface and does not cover it.
- **The EXP/LP defeat line** (Session 72) — `"Dire Rat defeated. 34 EXP, 2 LP."` is mod-emitted and
  has no readme entry.

Not fixed here because readme edits are a separate commit made *before* the release trigger (see
"What this procedure does NOT do"). **These belong in a readme commit ahead of the next release.**

**Purpose:** play test of the Sessions 74–83 navigation rebuild, **none of which is
play-confirmed.** The walkmap is now understood as a navmesh (per-edge neighbours at `+0x16/+0x18/
+0x1A`) and `nav_grid` is gone, so this build routes on a fundamentally different graph than V0.2
did — a routing regression here is expected to look like a wrong turn, not a crash. Also new and
unconfirmed: NPCs speak their personal name from the npcdic slot rather than a generic one, an
NPC class that was invisible on every map is now listed, and phantom entries are filtered by
placement + reachability. **Do not tighten the reachability filter blind** — the grace window bug
it replaced silently re-admitted everything the old filters deleted, so a filter that appears to
do nothing has precedent for being genuinely inert rather than correctly quiet.

## V0.2-test-build — 2026-07-24

**Built from:** `fd459de` (Session 73 — elevation-aware navigation: `AllFloorsAt`, approach-cell
routing, `;` interact target). Tree clean before and after; no code change for the release.

**Zip:** `FFXII-Screen-ReaderV0.2-test-build.zip`, 344,619 bytes, four files, root flat.
`dinput8.dll` 446,464 bytes (sha256 `55d8fbde…31d6ed`). All three DLLs verified PE machine `8664`.
TTS pair carried over from `V0.1.1-shotgun-build`.

**ReadMe:** ~~unchanged — `README.md` has not been touched since Session 67, so the converted
`ReadMe.txt` is **byte-identical** to `V0.1.1-shotgun-build\ReadMe.txt` (verified with `cmp`, not by
eye).~~ **STRUCK 2026-07-27 — both halves are false.** The two files are 8,979 vs 9,164 bytes and
differ; V0.2's copy was converted from an uncommitted working-tree `README.md` whose edits only
reached git in `676e86c`. See the V0.2.1 record above. Conversion is reproducible: strip
heading/bullet markers, unescape `\[` `\-` `\\` `\_`, drop `&#x20;`, collapse doubled spaces, CRLF,
no BOM. The one surviving backtick is line 83's literal `` ` `` key name — content, not markup.

**Readme gaps shipped in this build (flagged, not fixed).** Sessions 69–73 added player-facing
surfaces that never reached `README.md`, so testers of this zip have no documentation for
(**note:** the `g` and `;` items below are ~~struck~~ — the shipped `ReadMe.txt` did carry them,
see the correction above; the rest stand):
- ~~**`g`** — party gil total (Session 69). No entry at all.~~ **STRUCK** — present in the shipped
  `ReadMe.txt`.
- **Shop reader** (Buy/Sell/Bazaar), **inventory quantity + category switching** (Session 70),
  **Status screen** (Session 71).
- **Ground loot in the Items category** and the **EXP/LP defeat line** (Session 72).
- ~~**`;`** — the readme says "status of the active target"; since Session 73 it also falls through
  to the interact-target readout when there is no battle target.~~ **STRUCK** — the shipped
  `ReadMe.txt` already carried the reworded `;` entry covering the interact-target fallthrough.

Not fixed here because readme edits are a separate commit made *before* the release trigger (see
"What this procedure does NOT do"). **These belong in a readme commit ahead of the next release.**

**Purpose:** first play test of Session 73's approach-cell routing, which is built but **not
play-confirmed**. `kApproachRadius = 4.0` is still an invented constant — real reach is somewhere
between 0.51 and 1.70 — and must not be tightened blind.

## V0.1.1-shotgun-build — 2026-07-23

Zip 318,778 bytes. Built from `661432d`. ReadMe rewritten: F6 clipboard naming got its own section,
the wall-camera flip became its own Known Issue, and two false claims were removed (License Board
"not implemented", NVDA keys "do not work"). `F5` and `U` gaps filled. Logged as **Session 67**
(before the step-4 rule existed).

## V0.1-shotgun-build — 2026-07-20

Zip 253,699 bytes. Logged as **Session 50** — the release that restored the silence rule, made `;`
battle-only, and struck four stale designs. This is the release the four-file/TTS-bundling
correction was made against.

## V0.02-shotgun-build — 2026-07-20

Zip 244,864 bytes. No dedicated session entry.

## V0.01-shotgun-build — 2026-07-15

Zip 231,435 bytes. First release. No dedicated session entry. The deploy-vs-release TTS confusion was
corrected on this same date (see the note in step 2) — `Releases\V0.01-shotgun-build\` as it stands
holds all four files, so which state the zip was actually distributed in is not recorded.
