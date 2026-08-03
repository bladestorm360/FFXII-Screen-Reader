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
// CATEGORY TABS are NOT read here. The shop is one of the three families served by FUN_005655f0, so
// InventoryReader::OnCategoryRefresh already resolves and speaks the tab name (WEAPONS, AMMUNITION,
// LOOT, ...) for this container too. This reader only has to stand down for it: it asks
// InventoryReader::ConsumeCategoryAnnounce(container) and, when a category was just announced, speaks
// its row QUEUED behind that name rather than interrupting it. Reading the tabs a second time here
// would duplicate that chain and re-create the very race it fixes.
//
// CONTRACT (same as the other readers): read-only, SEH-guarded memory reads, no game calls. Text is the
// game's own, decoded via GameText. Empty/invalid rows -> silent. The ONE change-check (speak only when
// the highlighted item changes) is the sanctioned no-dedup exception: FUN_0056e5d0 is a redraw handler
// that fires ~2x per focus, so without it every move would speak twice. It is cleared by the list-refresh
// event below, which is what makes re-entering the shop re-announce -- NOT, as this said until Session
// 126, by "the surface changing", a test that compares an address the engine is free to hand back.
// Runs on the game thread.
namespace ShopReader {

bool Init();
void Shutdown();

// True if `w` is the shop list container or its item panel -- the two surfaces this reader speaks.
// The shop is the same tabbed-container shape as the pause-menu item lists, so InventoryReader asks
// before claiming a window; without it both readers would announce the same shop row.
bool OwnsSurface(void* w);

// THE SHOP HAS NO OPEN EVENT OF ITS OWN, which is why entering a shop used to say nothing until the
// player pressed a direction key. FUN_0056e5d0 -- the only hook this reader had for the list -- is a
// highlight/REFRESH handler reached from the cursor mover and the L/R tab handler, never from
// construction; `debug.md` recorded the gap outright ("the shop-open hook point is still
// unestablished"). Meanwhile the generic reader did reach the container, found no captured rows,
// logged "(text not ready - awaiting paint)", retried once and gave up.
//
// So the open event is BORROWED from the list family the shop already belongs to: InventoryReader
// hooks FUN_005655f0, the unified refresh the game runs on screen OPEN as well as on every category
// change, and calls this once the original has rebuilt the rows. That is the same mechanism the
// party-menu lists were given for the identical defect ("entering a one-item list moves no cursor"),
// so this reader is joining an existing path rather than standing up a second one.
//
// Called on the game thread with the original's work already done. Stands down unless `container` is
// the shop list container, so InventoryReader can call it unconditionally.
void OnListRefreshed(void* container);

} // namespace ShopReader
