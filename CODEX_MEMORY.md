# Custom/FFXII/FFXII-Screen-Reader — memory entry

Shared project context and imported topic index: [Custom/FFXII/CODEX_MEMORY.md](../CODEX_MEMORY.md).

Ownership: User custom implementation.

This is the source entry for that workspace. Read the linked memory once, then only task-relevant topics. Add durable source-specific facts below with a date and evidence; the imported Claude notes are preserved, not revalidated runtime results.

## 2026-10-04 core review and description plan

- Core text/speech/logging refactor passed three native regression tests and the full DLL build. No deployment or game launch. See `Docs/debug.md` under Core review for defects, validation and limitations.
- `tests/run_tests.bat` runs isolated decoder/logger/speech tests with private fake Tolk DLLs; no actual screen reader or game is used. The local ignored `build_and_deploy.bat` accepts `build` for build-only; no arguments retain normal deployment.
- Saved continuation plan: `../../../codex/FFXII-accessibility-draft-2026-10-04.txt`. Cutscene narration, textless tutorial/image descriptions and spoken glyph/key prompts remain planned. Viewer identity, playback clock and active prompt mappings require evidence; review actual visuals before authoring descriptions.
- Workspace catalog already lists this source. The app returned no saved projects and no registration tool was available; app registration remains pending.

## 2026-10-15 documentation-only maintenance request

- **User-set order:** [current open issues](Docs/plan.md#current-maintenance-requests--2026-10-15): 1) replace current pathfinding with baked maps; 2) mounted autowalk and voiced-dialogue suppression, assessing native pathing/movement only if beneficial versus input simulation; 3) broader threading/centralization review, preserving reported stability and changing only what is justified. This supersedes earlier sequencing/adoption-evaluation recommendations. Documentation only in this turn.
- **Correction / new report:** mounted autowalk failed for a tester; the user's own untested status does not negate it. [NavigationAudit.md](Docs/NavigationAudit.md) preserves the supplied `tidus10` Exodus/chocobo/floatweed report and requires deep physics/blocker/objective auditing plus baked-map refactor evaluation. Current autowalk injects DirectInput W/A/S/D; custom A* reads the live native mesh, with no mod-baked navigation package. No mod-code/runtime changes in this documentation pass.
- Follow-up offline investigation: [VoiceAndMounts.md](Docs/VoiceAndMounts.md). Shared native mounted movement is not gameplay confirmation. Unchanged production fixture verifies 250 ms class-cache retention and conditional slot-0 rejection, not the reported cause. Voice/captions use separate routines; registry shortcut rejected by 1,115-file US census; page/audible-voice ownership remains open. Archive linked there. Existing core work pushed as `5207c1f`; three regression tests pass.
- [Current maintenance plan](Docs/plan.md#current-maintenance-requests--2026-10-15): pending refactoring audit, voiced-dialogue suppression toggle and chocobo-first mounted autowalk coverage; accumulated nonvisual acceptance checks. The follow-up investigation above supersedes the initial difficulty assessment. Audit findings belong in [PerformanceIssues.md](Docs/PerformanceIssues.md).

## Engineering and model policy

Use [project engineering/delegation instructions](AGENTS.md#engineering-and-model-delegation) and the [shared model policy](../../../codex/MODEL_POLICY.md) for Sol-first ownership, bounded low-risk Luna implementation with a clear Sol handoff/review, and emergency-only Astra analysis after at least two distinct unsuccessful Sol approaches to a specific genuinely unfamiliar blocker. Subject matter/complexity alone never qualifies; Sol/Astra cooperation is reserved for extreme RE whose code Sol cannot understand after those attempts. Sol performs the overwhelming majority of work and owns implementation/validation. Record important discoveries and their reasoning in this memory or linked project documentation so later agents can reuse the investigation.

Summarize each delegation to another model when it finishes. Give the user a concise tracking summary naming the model, assigned task, outcome, review/validation and remaining issues. Include failed, interrupted or inconclusive outcomes honestly; retain durable findings in project documentation linked from memory.
