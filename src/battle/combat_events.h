#pragma once

// Installs the combat hooks and feeds CombatLog. This is the ONLY file that holds combat RVAs, so
// a future RVA correction touches one place.
namespace CombatEvents {

bool Init();
void Shutdown();

} // namespace CombatEvents
