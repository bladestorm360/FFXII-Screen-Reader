#pragma once
#include <cstdint>

// The "equip it to whom?" screen the shop raises after you buy a piece of equipment several
// characters could wear. You cycle characters with Left/Right; the game shows each one's portrait,
// what they currently have in that slot, and greys out anyone who cannot use it.
//
// None of that was spoken, so a blind player buying equipment landed on an unlabelled screen and
// could only guess who they were about to equip.
//
// SCREEN: class FUN_0057ac10 (RVA 0x45AC10). Refresh FUN_0057b1a0 (0x45B1A0) fires on entry
// (case 0xE), after EVERY Left/Right, and after every equip -- probe-confirmed 2026-08-03.
namespace EquipTargetReader {

bool Init();
void Shutdown();

// Is that screen up right now? Used to context-gate the per-character keys, which serve this screen
// and the shop list alike.
bool IsActive();

} // namespace EquipTargetReader
