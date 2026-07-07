#pragma once

// Installs the navigation module's game hooks. Currently just the physics-world
// builder (FUN_006a0310), hooked to capture the per-map physics context for
// bullet_query. Event-driven (fires once per map load), NOT a poll. Installs NO
// interpreter/action/gambit hooks — the mod is announce-only by construction.
namespace NavHooks {

bool Init();      // install hooks (requires Hooks::Init() to have succeeded)
void Shutdown();  // uninstall

} // namespace NavHooks
