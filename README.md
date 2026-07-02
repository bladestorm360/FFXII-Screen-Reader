# FFXII-Screen-Reader

Accessibility / screen-reader mod for **Final Fantasy XII: The Zodiac Age** (PC, Steam).

## Status

**Pre-alpha — Phase 0 scaffolding.** No installable build yet. Reverse-engineering
of `FFXII_TZA.exe` is in progress; production features arrive in subsequent phases.

## Goal

Allow blind and low-vision players to play FFXII TZA without sighted assistance.
Speech-only output via [Tolk](https://github.com/dkager/tolk) (NVDA / JAWS / Narrator
/ SAPI5). Pause-and-narrate combat log replaces real-time battle vocalization. Full
support for all 12 shipped game locales.

## Installation (when releases exist)

1. Extract the release zip into the game's `x64\` folder
   (`D:\…\FINAL FANTASY XII THE ZODIAC AGE\x64\`). The zip contains a single
   `dinput8.dll` (our mod). Windows' DLL search order picks our local copy
   before searching System32; we forward the real dinput8 calls along, so input
   keeps working.
2. **Required: install Tolk yourself.** Copy your own `Tolk.dll` and
   `nvdaControllerClient64.dll` (from your Tolk install) into the same `x64\`
   folder. The mod does NOT redistribute Tolk.
3. Launch the game. The mod announces "FFXII screen reader loaded" within a few
   seconds.

### Compatibility caveat

The mod uses the `dinput8.dll` proxy slot, which is the same slot used by
ffgriever's [FF12 External File Loader](https://www.nexusmods.com/finalfantasy12/mods/170)
(needed by Insurgent's Toolkit, Lua Loader, etc.). If you have External File
Loader installed, only one `dinput8.dll` can win — keeping ours means you
temporarily lose the file-loader features. Co-loading is not yet supported in
v1; if you depend on ELF, hold off on this mod for now.

## How it works

- **Injection:** standalone `dinput8.dll` proxy. Our DLL ships as `dinput8.dll`,
  forwards all calls to `C:\Windows\System32\dinput8.dll`, and runs our
  deferred init from a background thread.
- **Speech:** [Tolk](https://github.com/dkager/tolk) bridges to whichever screen
  reader is running (NVDA, JAWS, Narrator, SAPI5).
- **Game state reads:** RVA-first, sourced primarily from DrummerIX's Cheat Engine
  table. Self-healing AOB byte-validation handles minor game updates.

## Combat log

FFXII's combat is real-time and dense — too dense for linear narration. Instead
we keep a 50-event ring buffer (continuous across battles) and provide a
pause-and-read UI. Default keys:

- **F4** — open the combat log (game pauses).
- **Up / Down** — move one entry.
- **Page Up / Page Down** — move ten entries.
- **Home / End** — jump to oldest / newest.
- **Escape** (or whatever cancel/back is bound to) — close and resume.

Critical events (party-member KO, party member crosses below 20% HP) auto-speak
in real time even when the log is closed. Configurable.

## Locales supported

US, UK, JP, FR, DE, IT, ES, CN, KR, IN, ASIA, CH — auto-detected at startup.

## Building

See `CLAUDE.md` for the full build environment. Quick version: install Visual
Studio 2026 with the C++ toolchain, install Ghidra 12 (for RE work), then run
`build_and_deploy.bat` from this directory. The script configures CMake, builds,
and copies `dinput8.dll` into your game install.

## License

The mod itself is — license TBD before public release.

This project is not affiliated with Square Enix.

## Acknowledgements

- **DrummerIX** — Cheat Engine table for FFXII (struct map seed).
- **ffgriever** — FF12 External File Loader (BSD-2, source consulted as a
  reference for the game's dinput8 init path; not used in this mod's build).
- **Davy Kager** — Tolk.
- **DQ7R-Screen-Reader** — sister project whose plumbing patterns this mod ports.
- **BlindGuyNW / bladestorm360** — FF Pixel Remaster screen readers (FF4/5/6) — design
  references.
