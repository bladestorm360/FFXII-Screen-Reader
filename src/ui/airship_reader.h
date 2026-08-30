#pragma once

// THE PRIVATE AIRSHIP (STRAHL) DESTINATION MAP.
//
// WHY THIS IS ITS OWN READER AND NOT A GATE ADDED TO AN EXISTING ONE. The destination screen was
// silent, and the V0.7 log showed the mod emitting ONE line across the 17.4 s it was up:
//
//     [READER] unclaimed pane: obj0 RVA=0x4328C0 win=... -- no reader spoke for it
//
// `FUN_005528c0`, the class behind that RVA, NEVER CALLS `FUN_00247510`. The 0x8000 focus dispatch
// that every list reader in this mod hangs on is not missed here -- it is never sent, and no guard
// added anywhere in `menu_reader.cpp` could have made it arrive.
//
// THE REASON IT IS NEVER SENT IS THE WHOLE SHAPE OF THE SURFACE: the destinations are a NODE GRAPH,
// not a list. Each marker carries screen coordinates and four neighbour links, and the cursor is
// walked between them by direction. There is no row index, so there is nothing for a focus message
// to carry. Every list idiom in this codebase is the wrong shape for it, which is why this reads the
// pane's own fields instead of joining the dispatch chain.
//
// THE LAYOUT (derived offline in `..\FFXII-Decompile`, then confirmed by a live census):
//
//   pane+0x118  list head          node+0x140  next            node+0x39  id byte
//   pane+0x9F40 the hovered node   node+0x3C/+0x3E screen x/y   node+0x54  gating flags
//   pane+0x120  hover, committed on input (case 0xA/0xB)       node+0x130 render flags (bit 3 hidden)
//   node+0x10/+0x18/+0x20/+0x28/+0x30  the neighbour NODES -- they match the ids at +0x40..+0x43
//   node+0x48   the destination NAME, packed codec text; +0x88 mirrors it
//
// The pane is 0xA020 bytes (`FUN_00244f50(0xa020, FUN_005528c0, ...)`), which is what puts +0x9F40
// inside it, and it hangs off the manager at +0x160.
//
// WHY IT VERIFIES THE NAME FIELD AT RUNTIME BEFORE IT WILL SPEAK. +0x48 was identified from a single
// dumped record, and one record cannot tell a per-node name from a shared placeholder -- they look
// identical when you have sampled one. Speaking one wrong name on all 34 markers would be worse than
// the silence this fixes, so the census requires the decoded names to be PRINTABLE and DISTINCT
// before `g_namesTrusted` is set. If they are not, this reader stays silent and says why in the log.
// Silence beats wrong speech, and an unproven field earns no speech.
//
// A node whose name does not decode is passed over in silence rather than announced. That is also
// what filters the graph's intermediate waypoints from its real destinations: the cursor stops on
// nodes that carry no name, and a marker with nothing to say should say nothing.
namespace AirshipReader {

bool Init();
void Shutdown();

}  // namespace AirshipReader
