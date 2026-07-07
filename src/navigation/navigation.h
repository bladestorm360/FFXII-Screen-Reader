#pragma once

// Field-navigation module (Phase 4) — announce-only, on-keypress, non-interfering.
// Entry point wired from dllmain after the other readers. Owns the nav hotkeys
// (via InputTracker) and the physics-world capture hook (via NavHooks).
//
// Milestone M0 (current): read-only self-diagnostic. Pressing `\` logs the full
// leader -> component -> physics-controller / world chain to the mod log so the
// unconfirmed position/yaw offsets and the runtime-only world pointer are pinned
// in-game before any compass / routing behavior is built on them.
namespace Navigation {

bool Init();
void Shutdown();

} // namespace Navigation
