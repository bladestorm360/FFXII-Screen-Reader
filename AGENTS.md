# Custom/FFXII/FFXII-Screen-Reader — source entry

Read the shared [development agreements](../../../AGENTS.md) and the [project guidance](../AGENTS.md) once if they are not loaded, then [CODEX_MEMORY.md](CODEX_MEMORY.md). The project guide contains the converted Claude rules, current ownership, build command and deployment boundaries.


Preserve existing source changes and upstream attribution. Keep technical reference lookups scoped to the task. Update project memory and the established status/architecture document when their facts change; this entry file does not need a parallel journal.

## Engineering and model delegation

Default to solving each task using Sol alone. Sol performs the overwhelming majority of work independently and owns architecture, reverse engineering, planning, debugging, implementation decisions, integration, review, testing and verification. Reverse engineering, pathfinding, algorithms, geometry and architecture remain Sol responsibilities; subject matter or perceived complexity alone never justifies Astra escalation.

Astra is an emergency specialist, never a general reviewer or standing coworker. Do not use it for code review, validation, testing, routine debugging, implementation or maintenance/repair of existing working or understood mod systems, including systems Sol built. Escalate only for a specific blocker in genuinely unfamiliar physics, geometry, unusually complex functions/formulas or extremely complex RE, after Sol records at least two distinct unsuccessful evidence-producing approaches and the exact unresolved question. Retries or parameter changes are not distinct approaches; two failures alone do not justify escalation. Sol/Astra cooperation is allowed only for extremely complex reverse engineering, such as a specific Unreal subsystem, whose code Sol genuinely cannot understand after those attempts; an Unreal target alone does not qualify. Other eligible emergencies receive bounded specialist analysis. Astra returns documented findings/guidance; Sol implements, integrates and verifies. Escalate on demonstrated need, never difficulty alone.

Use Luna for routine, bounded, mechanical, low-risk implementation, repetitive edits, straightforward refactors, boilerplate and documentation cleanup only when handoff plus Sol review/testing is likely to save effort. Sol supplies architecture context, source locations, explicit requirements, boundaries and acceptance checks so Luna need not rediscover or decide the architecture. Sol reviews for bugs/implementation errors and appropriately tests changes before accepting them; otherwise implement independently with Sol. Never use Astra to review or repair Luna work.

Keep handoffs concise but complete, and record important findings/evidence in project documentation for later agents. Keep tightly coupled work with the primary when splitting increases duplication or risk; do not delegate merely to delegate. Follow the [shared model policy](../../../codex/MODEL_POLICY.md) for identifiers and handoff requirements. Delegation does not expand runtime/publication permissions.

Summarize each delegation to another model when it finishes. Give the user a concise tracking summary naming the model, assigned task, outcome, review/validation and remaining issues. Include failed, interrupted or inconclusive outcomes honestly; retain durable findings in project documentation linked from memory.
