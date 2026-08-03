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
heading markers, `**`/`*` emphasis, code-fence ` ``` ` lines, and leading `-` bullet markers;
convert `[text](url)` to `text (url)`; convert tables to plain columns. No leftover `#` or backticks.
Save as `ReadMe.txt` (capital R, capital M) in the version directory.

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
and anything a future reader would need to reproduce or debug that build. **If a shipped feature is
missing from the readme, say so in the record** — that is how the gap survives to the next release.

### 5. Report

Confirm the zip was created and list its contents. Do not push, tag, or publish anything.

## What this procedure does NOT do

- **Does not tag or push.** The user ships releases by hand.
- **Does not create a GitHub Release.** The repo is private and holds source only.
- **Does not modify `README.md`.** If the readme needs changes, that is a separate commit made
  *before* the release-prep trigger.
- **Does not bump a version string in code.** There is no version constant in the build; the release
  is identified by its directory and zip name.
- **Does not overwrite an existing release.** If `Releases\V<version>\` exists, stop and report.

---

# Release Records

Newest first. One entry per release, written at step 4. `Releases\` is gitignored, so this table is
the only record in the repo that a given zip ever existed.

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
- **Party membership on the field menu** (Session 93) — new gap. `R` is listed only as a game key.
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
personal-name NPC lookup, the field-menu party list, the mod menu, the beacons, and the navmesh
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
