# FFXII-Screen-Reader — Debugging Log

This file is structured for keyword searching. **Always grep before proposing solutions.**

## Tried & Failed

Approaches that were attempted and did NOT work. Each entry tagged with `KEYWORDS:` for
grep. Check this FIRST to avoid repeating failed approaches.

*(none yet — Phase 0)*

## Solved Problems

Problems that were resolved. Each entry has `KEYWORDS:` + `SOLUTION:`. Check this to
reuse known-good solutions.

*(none yet — Phase 0)*

## Mod Architecture

Current module interaction diagram + logging format. Keep up to date as modules land.

```
                  ┌────────────────────────┐
                  │ FF12 Module Loader     │
                  │ (dinput8.dll proxy,    │
                  │  ffgriever, BSD-2)     │
                  └─────────┬──────────────┘
                            │ loads our DLL from modules/
                            ▼
┌──────────────────────────────────────────────────────────┐
│ FFXII-Screen-Reader.dll                                  │
│                                                          │
│  ┌─────────────┐                                         │
│  │ proxy/      │ → DllMain → deferred init thread        │
│  │ module_entry│                                         │
│  └─────┬───────┘                                         │
│        │                                                 │
│        ▼                                                 │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────┐     │
│  │ core/       │  │ speech/     │  │ input/       │     │
│  │ logger      │  │ speech      │  │ keyboard_    │     │
│  │ memory      │  │ locale      │  │ hook         │     │
│  │ config      │  │ phrasebook  │  │ hotkeys      │     │
│  │ hooks       │  └──────┬──────┘  └──────┬───────┘     │
│  │ events      │         │                │              │
│  │ phyre_types │         │ LoadLibrary    │              │
│  └─────────────┘         ▼                │              │
│                     Tolk.dll              │              │
│                  (user-supplied)          │              │
│                                                          │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────┐     │
│  │ ui/         │  │ navigation/ │  │ battle/      │     │
│  │ game_handler│  │ entity_list │  │ combat_log   │     │
│  │ menu_state  │  │ pathfind    │  │ event_capture│     │
│  │ dialogue    │  │ compass     │  │ battle_state │     │
│  │ menus/*     │  └─────────────┘  └──────────────┘     │
│  └─────────────┘                                         │
└──────────────────────────────────────────────────────────┘
```

## Logging Format

`FFXII-Screen-Reader-Latest.log` (next to `FFXII_TZA.exe` in `x64\`):

```
[+0000ms] [INIT     ] Mod entry; deferred init started
[+0123ms] [SPEECH   ] Tolk.dll loaded
[+0124ms] [SPEECH   ] Screen reader detected: NVDA
[+0125ms] [HOOKS    ] MinHook initialized
[+0140ms] [CONFIG   ] mod_config.ini loaded; 12 RVAs validated, 0 self-healed
[+0145ms] [SPEAK    ] FFXII screen reader loaded
```

## Known Issues

*(none yet — Phase 0)*

## Session Log

Index of session log files (split into `sessions_*.md` every 50 sessions).

- `sessions_001_current.md` — sessions 1–N (current)
