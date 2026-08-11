#pragma once

// Reader for the pause-menu ITEM LISTS -- Items, Loot, Key Items, Magicks/Technicks and the
// weapon/armor/accessory views. Adds the two things the generic painted-cell path cannot give:
// the owned QUANTITY on each row, and the name of the ACTIVE CATEGORY.
//
// These screens, the Equipment list and the Shop are ONE tabbed-container family (probe-confirmed,
// Session 70). Four window classes were observed -- 0x4436F0 weapons/armor/accessories, 0x443930
// ITEMS *and* LOOT, 0x443B10 magicks/technicks, 0x443D20 KEY ITEMS -- and one class serves several
// screens, so nothing here keys off a class list. A window is claimed purely by its SHAPE:
// a row array at +0xE0, a scroll widget at +0xD8, a tab table at +0xE8 and a sane row count.
//
// ROW (stride 0x20, the record shop_reader.cpp already decodes -- the shop just holds it at
// panel+0xC8, and FUN_0056e410:54 literally copies container+0xE0 across):
//   +0x00 name codec   +0x08 id (0xFFFF = empty; 0x0 is a VALID id -- Potion)   +0x0E owned count
//   +0x0C is deliberately NOT read: it was labelled "equipped count" offline and REFUTED in play
//   (a Dagger equipped to Vaan read 0). Its meaning is unknown; it is never spoken.
//
// CATEGORY: hooked on FUN_005655f0 (0x4455F0), the refresh shared by all three families, which runs
// on screen OPEN and on every category change. The hook is on ENTRY to that function for two
// reasons: FUN_00564010:56-70 populates the tab table and the +0x180 count/index bitfield *before*
// calling it, so the name is readable there; and the game's own FUN_002d47c0 re-fires the 0x8000
// focus during the call, so announcing first yields "category, then item" instead of the reverse.
// Name = FUN_002f9860(tabTable[srcIdx].textId), preferring TextCapture's existing id cache and
// falling back to the game's getter the first time an id is seen.
//
// The FUN_005655f0 hook is also what makes a ONE-ITEM list speak at all: entering it moves no
// cursor, so a 0x8000-only reader stays silent.
//
// SPEECH: "<name>" or "<name> <count>" -- the count is spoken only when it is greater than 1, since
// a row exists only if you own at least one, so a bare name means exactly one. No comma. On a
// category change the category is spoken first and the item is QUEUED behind it (not a dedup -- it
// suppresses nothing; without it the item's interrupt would cut the category off mid-word).
//
// EMPTY CATEGORY (e.g. SHIELDS with no shield owned): the name is spoken and then NOTHING. Such a
// category IS tabbed and IS reachable -- the older "empty categories do not occur" claim is STRUCK
// (S89). The game frees the row array and stores null at +0xE0 (FUN_0057cf20:253-257), then CLAMPS
// the row count it gives the scroll widget from 0 to 1 (FUN_005655f0:49-52). The widget therefore
// still reports a row at index 0, so this reader must CLAIM that focus and stay silent; returning
// false would hand the cell to the generic painted-cell path, which reads whatever the painter last
// drew there -- the PREVIOUS category's row ("SHIELDS" then "Leather Cap", measured in play).
// A populated category is untouched by this and still announces its first row on the switch.
//
// Left/Right on a single-category screen is SILENT, matching the game: FUN_00564e10:22-23 returns
// early when the tab count is below 2, so no refresh fires and there is nothing to announce.
//
// CONTRACT: read-only, SEH-guarded memory reads. The one game call is FUN_002f9860 (a pure message
// -book lookup, the same call the game makes microseconds later), used only when the id cache has
// not seen that id yet. Empty/unreadable rows -> silent, never fabricated. Runs on the game thread.
namespace InventoryReader {

bool Init();
void Shutdown();

// Speak the focused row of an item list. Returns true only when this reader OWNED the window and
// actually spoke, so the caller can fall through to the generic painted-cell path otherwise.
// Called from MenuReader's 0x8000 dispatch and from its pane-entry replay.
bool TryFocus(void* owner, int index);

// True exactly once, and ONLY for the surface whose category was just announced -- then false again.
// The row the game delivers microseconds later QUEUES behind that name instead of cutting it off.
// FUN_005655f0 is shared: it serves these lists, the equipment list AND the shop, so a reader that
// speaks a row of the family without consulting this silences the category. shop_reader did exactly
// that -- the name was resolved, logged and spoken, then cancelled 0.2 ms later by the item line.
// NOT a dedup: nothing is suppressed, both utterances are spoken, in order. The owner argument is
// what keeps one surface from consuming another's announcement.
bool ConsumeCategoryAnnounce(void* owner);

// The equipment CANDIDATE-ITEM list ("which weapon / shield / helm?"), by obj[0] class. Used to
// recognise the one pane whose cursor is hosted by another object (below) and, in MenuReader, to
// announce its first row when it opens without a focus event.
bool IsCandidateList(void* w);

// True when `host` is `cursorPane`'s own cursor host -- i.e. a 0x8000 addressed to `host` is really
// a cursor move within `cursorPane`, and TryFocus(cursorPane, index) is the row to speak.
//
// The OFF-HAND slot is the only list in the family whose cursor widget lives under an intermediate
// object rather than under the list itself, so its focus messages arrive on that object and the
// caller's active-pane gate drops them -- the list announced its first row on entry and then went
// silent for every move. The full decompile chain (and the two hypotheses it retires) is on the
// definition in inventory_reader.cpp.
bool IsCursorHost(void* cursorPane, void* host);

} // namespace InventoryReader
