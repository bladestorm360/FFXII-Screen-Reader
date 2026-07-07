#pragma once

// Navigation hotkey dispatch. Registered with InputTracker::SetNavKeyCallback;
// runs on the input thread. Purely event-driven (no polling). All keys are
// STANDALONE — no Shift (the game binds Left Shift to Walk/Run). Key map:
//   \          describe current selection: name + bearing + distance + obstacle hint
//   [ / ]      previous / next object (nearest-first)
//   - / =      previous / next category
//   `          rescan the field-object list + area name
//   ;          facing readout (which way the player is pointing)
//   '          diagnostic dump to the log (raw coords + def bytes)
namespace NavCommands {

void OnNavKey(int vk, bool shift);

} // namespace NavCommands
