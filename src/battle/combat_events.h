#pragma once

// Installs the combat hooks and feeds CombatLog. This is the ONLY file that holds combat RVAs, so
// a future RVA correction touches one place.
//
// THREE hooks: the codec-sprintf (Tier 1, the game's own battle sentences), the result applier
// (Tier 2, synthesized damage lines) and the reward/death batch. They are installed LAST of
// everything the mod hooks, which is why a MinHook trampoline shortage takes out combat reading
// specifically and nothing else — see Hooks::LogInstallCensus and the [LOCAL] note in
// include/MinHook/buffer.c.
namespace CombatEvents {

// False means at least one hook did not install and combat reading is DEAD for the session. The
// caller must not discard this — it did, once, and the only trace was a single log line.
bool Init();
void Shutdown();

} // namespace CombatEvents
