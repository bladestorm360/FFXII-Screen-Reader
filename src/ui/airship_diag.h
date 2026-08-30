#pragma once

// AIRSHIP DESTINATION PANE -- LOG-ONLY DIAGNOSTIC. IT SPEAKS NOTHING, AND IT IS MEANT TO BE DELETED.
//
// WHY IT EXISTS. The private airship (Strahl) destination screen is silent, and the V0.7 log
// (2026-08-30, `Build: V0.7 (76f0e66)`) shows why it is silent in a way no reader could have fixed:
// across the 17.4 s the menu was up the mod emitted ONE line --
//
//     [READER] unclaimed pane: obj0 RVA=0x4328C0 win=... -- no reader spoke for it
//
// -- and nothing else. No focus, no rows, no speech. `FUN_005528c0`, the class behind that RVA,
// NEVER CALLS `FUN_00247510`, so the universal 0x8000 focus dispatch the whole menu-reading
// architecture hangs on is not merely missed here, it is never sent. The player confirmed the cursor
// moved and the game made its own cursor sound, so the events exist; they do not travel that path.
//
// WHAT THE DECOMPILE ALREADY SETTLED (offline, `..\FFXII-Decompile`), so this probe confirms rather
// than searches -- a probe with a fallback branch is a search, and this one has none:
//
//   * the destinations are a NODE GRAPH, not an indexed list. That is the root of it: markers with
//     screen coordinates, walked by direction, so there is no row index to send in the first place.
//   * pane+0x118 = list head, node+0x140 = next, node+0x39 = id byte, node+0x3C/+0x3E = screen x/y,
//     node+0x54 = gating flags, node+0x130 = render flags.
//   * pane+0x9F40 = the node the cursor is hovering; case 0xA/0xB copies it to pane+0x120 on input.
//   * the pane is 0xA020 bytes (`FUN_00244f50(0xa020, FUN_005528c0, ...)`), which is what puts
//     +0x9F40 inside it, and it is stored at manager+0x160.
//
// THE ONE THING STILL UNKNOWN, and the only reason this file exists: WHERE A DESTINATION'S NAME
// COMES FROM. The pane resolves exactly two text ids (0x4B45, 0x4FF) and both feed a static
// sub-panel, not a node. So this dumps the whole node record once -- `Docs\GameArchitecture.md`'s own
// rule, dump the WHOLE record before inventing a model -- and fires TextCapture's ring dump, which
// has been built and callable since Session 112 with no caller at all.
//
// ONE BOARDING ANSWERS IT. It needs no positioning and no aiming: the census fires when the pane is
// constructed, wherever the player happens to be standing.
namespace AirshipDiag {

bool Init();
void Shutdown();

}  // namespace AirshipDiag
