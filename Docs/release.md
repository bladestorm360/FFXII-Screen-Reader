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

### 4. Report

Confirm the zip was created and list its contents. Do not push, tag, or publish anything.

## What this procedure does NOT do

- **Does not tag or push.** The user ships releases by hand.
- **Does not create a GitHub Release.** The repo is private and holds source only.
- **Does not modify `README.md`.** If the readme needs changes, that is a separate commit made
  *before* the release-prep trigger.
- **Does not bump a version string in code.** There is no version constant in the build; the release
  is identified by its directory and zip name.
- **Does not overwrite an existing release.** If `Releases\V<version>\` exists, stop and report.
