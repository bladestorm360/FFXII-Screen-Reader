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

- [Current maintenance plan](Docs/plan.md#current-maintenance-requests--2026-10-15): pending template/persistent-guidance refactoring audit, persistent voiced-dialogue suppression toggle and chocobo-first mounted autowalk repair. Limited source inspection only; no implementation/build/deploy/runtime tests. Voice-to-line binding, mounted failure cause and transport exclusivity remain unverified. Preserve existing core edits. Audit findings belong in [PerformanceIssues.md](Docs/PerformanceIssues.md); the plan records economical next steps and eventual nonvisual acceptance checks.

## Engineering and model policy

Use [project engineering/delegation instructions](AGENTS.md#engineering-and-model-delegation) and the [shared model policy](../../../codex/MODEL_POLICY.md) for Sol-first ownership, bounded low-risk Luna implementation with a clear Sol handoff/review, and emergency-only Astra analysis after at least two distinct unsuccessful Sol approaches to a specific genuinely unfamiliar blocker. Subject matter/complexity alone never qualifies; Sol/Astra cooperation is reserved for extreme RE whose code Sol cannot understand after those attempts. Sol performs the overwhelming majority of work and owns implementation/validation. Record important discoveries and their reasoning in this memory or linked project documentation so later agents can reuse the investigation.

Summarize each delegation to another model when it finishes. Give the user a concise tracking summary naming the model, assigned task, outcome, review/validation and remaining issues. Include failed, interrupted or inconclusive outcomes honestly; retain durable findings in project documentation linked from memory.
