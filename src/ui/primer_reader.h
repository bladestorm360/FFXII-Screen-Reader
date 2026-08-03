#pragma once

// The CLAN PRIMER (the game's "Handbook"): Bestiary, Hunts, Traveller's Tips, Sky Pirate's Den.
//
// THE DOC SAID THIS WAS UNREADABLE. IT IS NOT. `GameArchitecture.md` recorded primer/handbook content
// as "baked assets -- OCR only", which reads as a red light for the whole feature. That measurement
// was taken on TUTORIAL PANEL PICTURES. The PROSE is ordinary codec text: FUN_00573be0 takes each
// page string straight off a pointer array on the entry record, and the token measurer FUN_002b1fb0
// calls FUN_002ac5f0 -- the same escape parser game_text.cpp derives EscapeParamCount from. Confirmed
// live: a bestiary entry decoded to "Plant\nCactus", "Observations | Being a mischievous, mean-
// spirited beastie..." and "The Adventurer's Handbook | Ye adventurers only beginning...".
//
// ONE RECORD SERVES EVERY SUB-SCREEN, which is why this file is small and covers all of them:
//     record+0x03      u8      page count
//     record+0x08+i*8  char*   page i, as "HEADER 0x03 BODY" -- the same page-break byte
//                              GameText::DecodePages already splits on
// Sub-screen ids from FUN_0056f2a0, confirmed live by the strings each one resolved:
//     4 = Traveller's Tips ("Tricks of the Trade", "Restoring MP")
//     5 = Bestiary         (area names, then "Marks"/"Espers"/"Rare Game")
//     6 = Hunts            ("Wolf in the Waste", "Marauder in the Mines")
//     7 = seen but empty on the probe save -- almost certainly Sky Pirate's Den
//
// WHAT SPEAKS, AND WHY IT IS NOT KEY INTERCEPTION. Inside an entry the GAME owns the navigation --
// left/right turns the page in the Bestiary, up/down scrolls a Tip -- so this reader does not read
// keys to find out. It hooks FUN_00573be0, the game's own set-page call, and announces the page that
// resulted. Whatever key (or pad button) the player used, the announcement follows, and the mod never
// has to guess which screen binds which key.
//
// THE LIST ROWS ALREADY SPOKE before any of this: they paint through the universal painter, so
// TextCapture had them. The BODY had no cursor, so nothing ever announced it. That is the gap.
//
// A SECOND STRING RESOLVER LIVES HERE. Primer titles come through FUN_002f9920 (keyed id/10000 with
// a linear key match), NOT the FUN_002f9860 the mod hooks -- confirmed live across every sub-screen
// ("Wolf in the Waste", "The Lhusu Mines", "CLAN RANK"). If a title ever needs resolving by id,
// EXTEND TextCapture::ResolveStringById; do not stand up a second resolver beside it.
namespace PrimerReader {

bool Init();
void Shutdown();

// Arrows + Home/End walk the open page line by line. Returns true when consumed.
//
// LEFT/RIGHT ARE DELIBERATELY DECLINED: they are the game's page turn, and the hook above already
// announces the result. Taking them would put two speakers on one keypress -- the mistake
// choice_reader.cpp records as "they raced, and the plainer line won".
bool OnMenuNavKey(int vk);

// A Hunts row was focused. Speaks the WHOLE row -- mark name, status, petitioner -- and returns true
// to claim it, so the generic painted-cell path does not also announce the bare name.
//
// The row -> name/petitioner/status map is built as the list PAINTS (FUN_00577c30 pairs a row index
// with the FUN_0037e5b0 struct built inside it), because neither call alone carries both halves.
bool OnHuntFocus(void* owner, int index);

// True for every Clan Primer surface: both entry lists, the page viewer, the Hunts list and detail,
// and the Sky Pirate's Den plus its tooltip.
//
// InventoryReader MUST ask before claiming a window. Its IsEmptyCategory shape test -- null row
// array, live scroll, live table -- matches the primer's entry list exactly, so it was claiming
// Traveller's Tips and going deliberately SILENT on it. That is why the tips list read nothing while
// its buffer read fine. It already stands down for the shop the same way.
bool OwnsSurface(void* w);

} // namespace PrimerReader
