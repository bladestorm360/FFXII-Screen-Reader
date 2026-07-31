#pragma once

// ALWAYS-PRINTING census of the engine's FIVE script containers — log-only, no behaviour.
//
// WHY THIS EXISTS (Session 103's unpaid debt, and Session 104's finding).
//
// `MapScript::ReadExitDests` reads exactly ONE blob: the pointer at `DAT_02098e10 + 0x00`. That is
// script container 0. The engine has **five**: `FUN_00266d10` clears `5` structures of `0x288` bytes
// from that same base and stamps each with its own index at `+0x28`, `FUN_00263ff0(i)` returns the
// i-th, `FUN_0026c8c0(i, blob, entry)` INSTALLS a blob into container i and makes it current
// (`DAT_02099d70`), and `FUN_00264b90(idx, container)` reads `container[c]->blob + 0x54` for ANY c —
// i.e. every container's blob carries the same header layout the exit reader already parses.
//
// So "no routine on map 313 arms map-jump group 1" has always been a statement about ONE container,
// and the staircase into the dungeon is bound by a script the reader has never looked at. Map 313's
// own routine list names the neighbourhood it lives in: `SAKIYOMI_grm_a0380` — "pre-read event
// grm_a0380" — and `plan_master\us\event\grm_a\grm_a0380\grm_a0380.ebp` is a real, separate script
// file. 14 of the 346 extracted event scripts contain the exact `4f/4f/4f 5d 8d 00` `mapjump` call
// this reader already scans for, so an event-bound transfer in that encoding is a demonstrated shape
// in this game's data, not a hypothesis.
//
// THE RULE THIS PAYS OFF (Session 103): **a detector that only logs successes cannot report a
// failure.** S102 shipped a scan whose only output was a positive finding, so "nothing arms group 1"
// and "the scan never read those routines" printed the identical nothing. This census prints what it
// SCANNED as well as what it FOUND — routines walked, spans read, spans skipped with the reason,
// every `setmapjumpgroup(K)` with the routine that armed it, and every `mapjump` with its literals —
// per container, on every map, whether or not anything was found. One load of a map settles it.
//
// STRICTLY LOG-ONLY. It returns nothing, no caller consumes it, and it adds no exit to any list. The
// exit list is produced by `ReadExitDests` exactly as before; this file cannot change a route, a
// name or an announcement on any map. That is deliberate — the working reader is not refactored to
// share code with a diagnostic (Session 101: make the new path unreachable from the working one
// rather than testing your way to confidence). The blob-format constants it parses with are the
// shared ones in `map_script_internal.h`, which is what stops the two from drifting.
namespace MapScript {

// Walk containers 0..4 and dump the census to NAV-DIAG. Cheap enough to call once per map; the
// caller latches it. Safe to call on any frame — every read is SEH-guarded and a torn or absent blob
// prints as such instead of faulting.
void LogContainerCensus();

} // namespace MapScript
