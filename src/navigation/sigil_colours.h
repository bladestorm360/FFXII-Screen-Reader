#pragma once

#include <vector>
#include "navigation/entity_scan.h"

// THE PHAROS SIGILS OF SACRIFICE, BY COLOUR (S183).
//
// The Third Ascent's Way Stones warp the party, and the game names only some of them by colour: "Way
// Stone - Black Sigil", "- Green Sigil", "- Red Sigil". The fourteen "Way Stone - Sigil of Sacrifice" all
// share ONE name and are told apart by their glow alone -- white, yellow, pink or purple -- and in the
// sacrifice rooms the right one is the colour of the altar chosen on the Second Ascent. A blind player
// heard "Sigil of Sacrifice 1..4" and had nothing to choose by. This appends the colour to the label:
// "Way Stone - Sigil of Sacrifice, Pink".
//
// ONE RULE FOR ALL FOURTEEN: the colour is read from the sigil's OWN GLOW EFFECT, the literal id its routine
// passes to `bgeffectplay` first thing in entry 0 (MapScript::RoutineFacts::bgEffect, read live from the
// loaded script). The Spire Ravel scripts number their Way Stone glows in blocks by appearance -- every
// Black sigil in both scripts is 0x20-0x24, every Green 0x25-0x2A, every Red 0x2B-0x2E, the plain Way
// Stones 0x3D-0x3F -- and the fourteen Sigils of Sacrifice are 0x2F-0x3C. Inside that block the colours are
// PINNED by the eight sigils that test the saved altar choice (save byte class0+0x93d, one bit each):
//
//     bit 1  Altar of Steel      0x30 0x31   -> White      (and 0x2F, 0x32)
//     bit 8  Altar of Wealth     0x33 0x34   -> Yellow
//     bit 4  Altar of Knowledge  0x35 0x36   -> Pink       (and 0x37, 0x38)
//     bit 2  Altar of Magicks    0x39 0x3A   -> Purple     (and 0x3B, 0x3C)
//
// Altar names are the game's own (`rbl_j02`'s name table joined to each altar routine's fieldsign id); the
// colour of each altar's sigil is from two walkthroughs that agree, and the four sigils on Spire Ravel's
// sacrifice dais sit exactly at the compass corners those walkthroughs give for those colours. The six
// sigils that test no bit are the single Sigil of Sacrifice beside a Black, Green and Red sigil in the
// order-puzzle rooms (always a wrong warp); their ids fall inside the same blocks by the same ordering.
// Full evidence: GameArchitecture.md "Pharos Way Stones and the Sigils of Sacrifice".
//
// Gated on the routine NAME `s_warp_` -- a census of the shipped scripts finds that prefix only in rbl_n01
// and rbl_n02 -- AND on the effect id falling in the sacrifice block, so a Black/Green/Red sigil, a plain
// Way Stone or any other script is never touched.
namespace SigilColours {

// Caller holds the entity-list lock (part of EntityScan::Build, after DoorBinding::PromoteDoors and before
// the fallback labels). Fresh handle-table objects only; idempotence is ApplyPlayerLabels' job downstream.
void Apply(std::vector<EntityScan::Entity>& out);

} // namespace SigilColours
