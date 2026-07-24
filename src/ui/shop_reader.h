#pragma once

// Reader for the SHOP item lists (Buy / Sell / Bazaar). The three tabs share ONE window class pair --
// a container (FUN_0056e140) holding an item panel (FUN_0056d370). Buy is a single column; Sell is the
// "sideways" grid; both refresh the highlighted row through the container's handler FUN_0056e5d0, which
// is what this reader hooks. (Buy also routes cursor moves through the universal FUN_00247510 0x8000
// dispatch, but hooking FUN_0056e5d0 covers Buy AND Sell in one place, so the dispatch path is unused.)
//
// On each highlight it speaks "<name>, <price> gil, <owned> in inventory" -- name@row+0x00,
// price@row+0x10 (&0x7FFFFFFF), owned count@row+0x0E. The top Buy/Sell/Bazaar menu and the per-item
// description (`o`) are already handled elsewhere, so this only fills the item-name-on-highlight gap.
//
// It also reads the QUANTITY selector that opens once you pick an item (a SECOND hook, on the panel proc
// FUN_0056d370): quantity (panel+0xDC) and running total on each change, and the Left-arrow +1/+10 step
// (panel+0xE4 bit 0x400000) as "1x" / "10x". Quantity mode = panel+0xE4 bit1.
//
// CONTRACT (same as the other readers): read-only, SEH-guarded memory reads, no game calls. Text is the
// game's own, decoded via GameText. Empty/invalid rows -> silent. The ONE change-check (speak only when
// the highlighted item changes) is the sanctioned no-dedup exception: FUN_0056e5d0 is a redraw handler
// that fires ~2x per focus, so without it every move would speak twice. It resets when the surface
// (container) changes, so re-entering the shop always re-announces. Runs on the game thread.
namespace ShopReader {

bool Init();
void Shutdown();

} // namespace ShopReader
