#pragma once

// The SAVE / LOAD slot list (FUN_0057fe80, RVA 0x45FE80).
//
// WHY THE GENERIC PATH IS NOT ENOUGH. The slot row paints exactly TWO codec strings -- the region
// `FUN_003778b0(mapId)` and the sub-area `FUN_00377b60(mapId)` -- so TextCapture's painted-cell
// capture joins them and the row reads "Garamsythe Waterway: East Spur Stairs" and stops. Everything
// else on that row (playtime, levels, gil, slot number) is drawn by FUN_00393160 -> FUN_0029f6e0, a
// NUMERIC SPRITE-CELL setter that never touches the codec path. TextCapture cannot see a sprite, so
// the tester could hear WHERE a save was and nothing about WHICH save it was -- with several saves in
// the same area that is not enough to choose one.
//
// WHERE THE REST LIVES. The save container allocates 200 x 0xA0 preview records (container+0x590) and
// the list window caches that base at +0x1C0; the row->record map is `u8[win + 0xEF + row]`. Fields
// used here, all confirmed against a live probe over six real saves spanning 22 minutes to 92 hours:
//     +0x18  u16  playtime HOURS      +0x1A u8 MINUTES   +0x1B u8 SECONDS
//     +0x4C  u32  map id              (357 -> Lhusu Mines, 806 -> Bhujerba, 306 -> Rabanastre)
//     +0x20 + n*4  party slot n: charId / level / HP|MP gauge nibbles / flags, BIT 0 = PARTY LEADER
// Playtime is stored PRE-SPLIT, not as a counter: cross-checked against the frame counter at +0x10,
// which is exactly h:m:s x 60 on every record (26h34m07s -> 5,738,828 frames).
//
//     +0x08  u32  GIL -- settled by a screenshot: the 92-hour save reads 2,782,150 here and the
//                  panel reads "GIL 2782150G". `+0x0C` (961,190 on the same save) is NOT gil; it was
//                  the rival candidate and it is a different counter. Do not re-open this.
//
// THE SLOT NUMBER IS THE ARRAY INDEX, not `+0x54`. Same screenshot: the rows read 004, 005, 006,
// 007, <icon>, 008 in exactly the order the row map gives (...04 05 06 07 00 08), and the record at
// index 8 is the one whose `+0x54` holds 3. Index 0 is drawn with an ICON instead of a number, so it
// is spoken without one -- what that icon means is not established and naming it would be inventing
// a label.
//
// DELIBERATELY NOT SPOKEN:
//   * A SAVE DATE. There is none. FFXII stores playtime and a counter, not a timestamp -- two saves
//     77 seconds apart in the same playthrough differ only in playtime, that counter, the map and the
//     party gauges. Do not go looking for one again.
//   * The other five party members ON THE ROW LINE. The panel lists all six with levels, but they are
//     usually the same number repeated and the row line exists to TELL SAVES APART, not to inventory
//     them. Keys 4-9 read them on demand instead -- see PartyMemberKey.
//
// CLAN RANK IS SPOKEN, and it needed no rank->string-id table. `FUN_003153f0(rank)` returns the NAME
// itself as a codec string: the probe caught `FUN_002f9860(1254) -> "Knight of the Round"` resolving
// INSIDE that call and the pointer coming back out of it. So the mod calls the game's own resolver
// rather than the two-point "id = 1243 + rank" formula that was nearly built -- and if the return
// were ever not the name, the decode fails IsMostlyPrintable and this says NOTHING. There is no path
// from a wrong pointer to a wrong LABEL, only to silence.
//
// CONTRACT: read-only, SEH-guarded reads, no game calls -- the leader's name comes from
// BattleState::CharacterName, which is pure memory reads. An unreadable or empty slot returns false
// and lets the generic path speak the game's OWN "empty file" string; nothing here is fabricated.
namespace SaveReader {

// True if `w` is the save/load slot list. MenuReader asks before letting the generic painted-cell
// path speak, because that path would otherwise announce the location half a beat before this one
// announces the whole row.
bool OwnsSurface(void* w);

// A slot row was focused (FUN_00247510 msg 0x8000). Returns true when it spoke, false to fall
// through -- which is the right answer for an empty or unreadable slot.
bool TryFocus(void* owner, int index);

// Keys 4-9 while the slot list is up: read party member 1-6 of the HIGHLIGHTED save. The detail
// panel shows all six with their levels and a sighted player can see them at a glance; this is the
// same information, on demand.
//
// Joins the existing context-gated 4-9 chain in nav_commands.cpp -- an equipment comparison claims
// them first, then this, then the live party. The gate is STRUCTURAL, exactly as the equipment one
// is: the cached (owner, row) is re-validated against the live window class and the record is
// re-derived from it on every press, so nothing here can answer for a screen that has closed.
//
// Returns true when the key belonged to this screen -- INCLUDING when the addressed slot is empty,
// which is silent rather than spoken (never-speak-filler). Returning false there would hand the key
// to the live-party reader and answer a question about a different party.
bool PartyMemberKey(int n);

} // namespace SaveReader
