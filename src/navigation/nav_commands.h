#pragma once

// Navigation hotkey dispatch. Registered with InputTracker::SetNavKeyCallback;
// runs on the input thread. Purely event-driven (no polling). All keys are
// STANDALONE — no Shift (the game binds Left Shift to Walk/Run). Key map:
//   \          turn-by-turn route to the current selection (announce-only)
//   /          describe current selection: name + bearing + distance + obstacle hint
//   [ / ]      previous / next object (nearest-first)
//   - / =      previous / next category
//   `          rescan the field-object list + area name
//   '          diagnostic dump to the log (walkmap grid + object identities)
namespace NavCommands {

void OnNavKey(int vk, bool shift);

} // namespace NavCommands
