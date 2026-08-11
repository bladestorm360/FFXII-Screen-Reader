#pragma once
#include <cstdint>
#include <string>

// The game's per-character equipment COMPARISON panel — the row of columns that appears whenever a
// piece of equipment is highlighted in a shop, showing each character an arrow and a number for the
// one or two stats that item would change.
//
// FFXII draws that entirely as art: the number is a sprite and the direction is an arrow glyph, so
// a blind player buying equipment has no way to know whether it is an upgrade for anyone. The stat
// LABELS are real text and are read back off the game's own widgets, so the only mod-authored words
// here are the direction and the two states (see Phrase::StatUp / StatDown / CannotEquip /
// AlreadyEquipped).
//
// For the SHOP surfaces this module speaks nothing: it snapshots the panel and owns the wording,
// and ShopReader / EquipTargetReader each emit through their own single point.
//
// It DOES speak for one surface — the pause menu's Equipment screen, which has no other reader
// (status_reader deliberately covers only Status). That screen uses a SECOND and different
// mechanism: one character, `current > preview` absolute values rather than a per-character signed
// delta, on the attribute panel at menuCtx+0x138. It is also reachable as column 1, so `4` re-reads it.
//
// CORRECTED (Session 150): this used to claim the preview "is announced automatically on each
// highlight", which reads as covering the item you are choosing between. It does not.
// FUN_003fe720 has exactly TWO call sites — FUN_002c2320:240 (the screen's 4-row action list) and
// FUN_002c2cd0:20, reached from FUN_003ff360:91 (the 5-slot Weapon/Offhand/Helm/Armor/Accessory
// list). NEITHER is the candidate-item list (FUN_003fdfe0, parked at menuCtx+0x150). So the preview
// announces on each SLOT highlight, and says nothing while the player cursors the actual candidates.
// Whether that gap should be closed is open; the claim is corrected here so it is not built on.
//
// PANEL: class FUN_002cbf80 (RVA 0x1ABF80) parked at menuCtx+0x2E0. Refreshed by FUN_002cc4f0
// (0x1AC4F0), which the shop calls on EVERY highlight (FUN_0056e5d0:44-47), on confirm
// (FUN_0056d370:118) and from the equip-target screen (FUN_0057b1a0:95).
namespace EquipCompare {

// Installs the two hooks. Must run BEFORE ShopReader::Init so a snapshot exists by the time the
// shop's highlight handler asks for one.
bool Init();
void Shutdown();

// Is a compare panel live and structurally valid right now? Cheap pure reads; safe from any thread.
bool IsLive();

// How many character columns the last refresh resolved (0 when none). Columns are numbered 1..N in
// the order the panel lays them out, which is the order the keys address.
int ColumnCount();

// The spoken line for column `n` (1-based), e.g.
//     "Vaan: Attack Power up 12"
//     "Fran: Defense down 3, Magick Resist up 1"
//     "Balthier: cannot equip"
//     "Basch: already equipped"
// Empty when `n` is out of range or the column resolved no name — the caller then says NOTHING
// rather than filler.
std::wstring LineFor(int n);

// Same, addressed by the panel's own member index (0..8) rather than column order. Used by the
// equip-target screen, which tracks menuCtx+0xDE0 directly.
std::wstring LineForMember(int memberIdx);

} // namespace EquipCompare
