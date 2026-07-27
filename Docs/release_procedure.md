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

Create the directory and copy in **exactly four** files:

| File | Source |
|---|---|
| `dinput8.dll` | `build\bin\Release\dinput8.dll` (the fresh build from step 1) |
| `Tolk.dll` | most recent prior `Releases\V*\`; for the first release, `D:\Games\Dev\Unity\FFPR\ff1\ff1-screen-reader\Releases\V1.4\Tolk.dll` |
| `nvdaControllerClient64.dll` | most recent prior `Releases\V*\`; for the first release, the same FFPR `V1.4` directory |
| `ReadMe.txt` | `README.md` converted to plain text (see below) |

**Preserve casing exactly:** capital `T` in `Tolk.dll`, lowercase `n` in `nvdaControllerClient64.dll`.

**The TTS DLLs must be x64.** `FFXII_TZA.exe` is 64-bit; a 32-bit `Tolk.dll` loads and then simply
never speaks, which is a miserable bug to diagnose from a user report. Verify before copying — the PE
machine field must read `8664`, not `014c`:

```
od -An -tx2 -j$(( $(od -An -tu4 -j0x3c -N4 "<file>" | tr -d ' ') + 4 )) -N2 "<file>"
```

**Do NOT source Tolk from `D:\Games\Dev\tolk\tolk\dist_x86\Tolk.dll`** — that build is **x86** and
will not work. The `libs\x64\` folder there holds `nvdaControllerClient64.dll` only, not Tolk itself.

**ReadMe conversion:** strip Markdown so the file reads cleanly under a screen reader — drop `#`
heading markers, `**`/`*` emphasis, code-fence ` ``` ` lines, and leading `-` bullet markers;
convert `[text](url)` to `text (url)`; convert tables to plain columns. No leftover `#` or backticks.
Save as `ReadMe.txt` (capital R, capital M) in the version directory.

**These four files are the entire release.** Do not add:

- **`mod_config.ini`** — the mod's RVA byte-validator writes it on first launch. Shipping one would
  mask a validator failure.
- **FF12 Module Loader / External File Loader** — incompatible; both want the `dinput8.dll` slot.

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

### 3. Zip with 7-Zip

```
& "C:\Program Files\7-Zip\7z.exe" a -tzip "Releases\FFXII-Screen-ReaderV<version>.zip" ".\Releases\V<version>\*"
```

- Zip name: `FFXII-Screen-ReaderV<version>.zip`, placed in `Releases\` — a **sibling** of the version
  directory, not inside it.
- The zip's root contains the four files **directly**, with no nested `V<version>\` folder, so the
  user can extract straight into the game's `x64\` folder as the ReadMe instructs.

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

## V0.2-test-build — 2026-07-24

**Built from:** `fd459de` (Session 73 — elevation-aware navigation: `AllFloorsAt`, approach-cell
routing, `;` interact target). Tree clean before and after; no code change for the release.

**Zip:** `FFXII-Screen-ReaderV0.2-test-build.zip`, 344,619 bytes, four files, root flat.
`dinput8.dll` 446,464 bytes (sha256 `55d8fbde…31d6ed`). All three DLLs verified PE machine `8664`.
TTS pair carried over from `V0.1.1-shotgun-build`.

**ReadMe:** unchanged — `README.md` has not been touched since Session 67, so the converted
`ReadMe.txt` is **byte-identical** to `V0.1.1-shotgun-build\ReadMe.txt` (verified with `cmp`, not by
eye). Conversion is reproducible: strip heading/bullet markers, unescape `\[` `\-` `\\` `\_`, drop
`&#x20;`, collapse doubled spaces, CRLF, no BOM. The one surviving backtick is line 83's literal
`` ` `` key name — content, not markup.

**Readme gaps shipped in this build (flagged, not fixed).** Sessions 69–73 added player-facing
surfaces that never reached `README.md`, so testers of this zip have no documentation for:
- **`g`** — party gil total (Session 69). No entry at all.
- **Shop reader** (Buy/Sell/Bazaar), **inventory quantity + category switching** (Session 70),
  **Status screen** (Session 71).
- **Ground loot in the Items category** and the **EXP/LP defeat line** (Session 72).
- **`;`** — the readme says "status of the active target"; since Session 73 it also falls through to
  the interact-target readout when there is no battle target.

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
