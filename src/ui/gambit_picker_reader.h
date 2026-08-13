#pragma once

// The GAMBIT PICKER -- the condition list and the action list the gambit setup screen opens on
// Confirm. `FUN_0056b4d0` (RVA 0x44B4D0); `picker+0x588` says which list it currently is (1 =
// conditions, 2 = actions), and one object serves both.
//
// WHY THIS EXISTS (Session 158). The picker was read by the generic painted-row path, which resolves
// a row's text out of TextCapture's paint cache. That cache is refreshed per row per PAINT, and the
// focus message for a category switch arrives BEFORE the new rows are drawn -- so the player heard
// the previous category's first row on every switch, and the previous LIST's first row when the
// action list opened over the condition list ("Foe: party leader's target" where "Attack" was
// highlighted). Reading the picker's own row array removes the timing question entirely: both
// rebuilds (FUN_0056a890 actions / FUN_0056bc70 conditions) fill that array BEFORE they move the
// list cursor, so by the time the focus reaches us the rows are already the new ones.
//
// NO NEW HOOK. The focus arrives as msg 0x8000 through FUN_00247510, which menu_reader.cpp already
// owns, exactly as the panel's does.
//
// MEASURED (decompile, Session 158, conf 0.98) -- this is what S94 was missing when it declined to
// claim the surface. 17 row slots at `picker + 0x0E0 + i*0x20`:
//     row+0x00  codec*  name
//     row+0x08  u16     id, 0xFFFF = slot unused
//     row+0x0C  u16     cost (MP / owned count) -- read but not spoken
//     row+0x10  u8      availability: 0 selectable, 0x10 not acquired (measured; the decompile
//                       suggested 0x0F). Spoken as ", unavailable"
//     row+0x18  codec*  help text
// and the category state: `+0x595` current index, `+0x596` count (11 action tabs, 15 condition tabs),
// `+0x599` row cursor. **THE CATEGORIES HAVE NO NAMES — see the block in the .cpp; that question is
// answered and closed, and the mod deliberately says nothing for a tab.**
//
// CONSERVATIVE BY CONSTRUCTION: OnFocus claims the focus ONLY when it actually spoke, so any shape
// this reader does not recognise still falls through to the generic path that covers it today. That
// is the same contract ChoiceReader carries, and it is what stops a wrong class constant from
// silencing a working surface.
namespace GambitPickerReader {

// Is this dispatch owner the picker? By CLASS (obj[0]), never by address -- menu objects are pooled.
bool IsPicker(void* owner);

// Speak the focused row. `index` is the dispatch's own `val`. True only when something was said.
bool OnFocus(void* owner, int index);

// Is the picker the surface the player is on right now? Set when a picker focus is spoken, cleared
// when the gambit PANEL takes a focus again (the panel always does when the picker closes -- both
// live logs show it). A transition detector, not a speech filter: ability_summary_reader's page
// controller is driven by this picker as well as by the license board, so it stands down here rather
// than speaking a second line over ours.
bool IsLive();
void NotePanelFocus();

} // namespace GambitPickerReader
