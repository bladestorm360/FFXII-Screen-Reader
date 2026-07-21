#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "navigation/nav_types.h"

// Map EXITS: the map-jump point table, the field-sign exit array, and the in-game map's connection
// database. Split out of map_query.h, which is now walkmap geometry only.
//
// The long correction notes on each function are kept verbatim -- they record which readings were
// STRUCK and why, and re-deriving them has cost this project multiple sessions.
namespace MapExits {

// ---- Map EXITS (map-jump points + field-sign destinations) -------------------
// A map exit: fixed world position, plus a destination area when one can be resolved.
struct ExitRec {
    FVec3        pos;
    float        angle = 0.0f;
    int          index = 0;       // exit slot index
    uint16_t     areaId = 0xFFFF; // destination planmapname area id (0xffff = none)
    std::wstring destName;        // resolved destination area name (empty if unresolved)
    bool         usable = false;  // story gate satisfied (FUN_002648f0 buf[0])
};

// Enumerate the current map's MAP-JUMP POINTS — the mapData+0x54 table behind getmapjumpposbyindex.
// These are the intra-map "Mapjump" transitions (e.g. Inner Ward -> Upper Apartments): walking into one
// moves the party to another area. Tester-confirmed.
//
// CORRECTION (this session): Session 43 demoted this table to "party arrival/spawn, not exits" on the
// claim that FUN_00353490 places the party from it. That is false — FUN_00353490 is abs 0x353490 = RVA
// 0x233490 = NavRva::GETMAPJUMPANGLEBYINDEX, the script native `getmapjumpanglebyindex`; it returns a
// jump's angle and places nothing. The demotion had no basis and is reverted here.
//
// The four floats (x/y/z/angle) are the only bytes FUN_00264b90 reads; the 16-byte trailer at record
// +0x10..+0x1f is read by NO game code. The OLD destIdx@+0x1d -> mapData+0x8c chain (struck) mis-read
// that trailer through the +0x70 machinery and always yielded areaId 0xffff. This function now probes
// the trailer DIRECTLY through the game's own resolver (mapId -> FUN_00264f90 -> FUN_00377870): each
// trailer word (and its u16 halves) is fed to the resolver and the first that returns an in-range
// printable area name becomes ExitRec.destName — the DQ7R idiom, resolved on the current map with no
// traversal. Empty destName -> caller labels the exit "Exit".
// Memory-only + SEH-guarded; `logRaw` writes the raw trailer + the winning candidate to NAV-DIAG so
// the exact destination field is pinned across a run or two. Clears `out` first.
void EnumerateMapJumps(const FVec3* playerPos, float maxDist, std::vector<ExitRec>& out, bool logRaw);

// Enumerate the current map's EXITS from the field-sign array at mapData+0x70, through the game's own
// getters (FUN_00264ac0(group) / FUN_002649b0(group,i) / FUN_002648f0(rec,buf)). THE exit source: the only
// records carrying a WORLD position AND a destination together, so `pos` is a real world point (bearing and
// steps are honest) and `destName` is the game's own "<region>: <sub-area>" for the map you'd arrive in.
// `usable` = story gate satisfied; records the game says aren't shown right now are skipped, as are exits
// whose destination doesn't resolve (silence beats a bare "Exit") and any leading back into this same area.
//
// NOTE: the long-standing "+0x70 is empty on every map" verdict was a CALLING-CONVENTION bug, not data —
// the group getters take their index in ECX and were being called with no argument, so the bounds check
// failed and every count read 0. See the ABI note in nav_rva.h. Clears `out`; `logRaw` dumps each record.
void EnumerateFieldSignExits(std::vector<ExitRec>& out, bool logRaw);

// DIAGNOSTIC (log-only): scan the loaded map's field-script bytecode for `mapjump(dest, entrance, flags)`
// literals (the game's own per-map exit destinations) and log each with its resolved name + the preceding
// dispatch bytecode. Used to design the source-door <-> destination pairing; no user-facing output.
void DiagScanScriptMapjumps();

// Enumerate the region's SUB-AREA LIST from the in-game map's connection database — the source the
// map screen uses to draw the region's floors. NOT AN EXIT LIST — do not feed Category::Exit from it.
//
// CORRECTED (this session): these records are the sub-areas OF THE CURRENT REGION, not the doorways out of
// the current room. PROVEN by FUN_003c0380, which searches a group's type-0 records for `rec+0x0c == the
// CURRENT map id` and returns the group — so rec+0x0c is a map id and the current map is itself one of the
// records. FUN_003c0340 returns byte[+1]+byte[+0] = (type-0 sub-areas in region) + (type-1 inter-region
// doors). Nalbina's five records are its five floors: their +0x04..+0x07 bytes form a prev/next chain
// (i=0 next=1 prev=ff … i=4 next=ff prev=3) and their coords are ATLAS paste offsets for the map image
// (FUN_003bed70 bboxes them as rec.x + destTexture.width) — hence `pos` is MAP-SPACE, never world.
// Listing these as exits announced five floors of the fortress, most unreachable from the room you're in.
//
// Retained (unused) as the basis for a possible "areas in this region" overview readout. Fills `out` per
// record: `destName` = "<region>: <sub-area>", `areaId` = that map id, `pos` = MAP-SPACE (x,0,y).
// Clears `out` first; `logRaw` dumps each record to NAV-DIAG.
void EnumerateMapConnections(std::vector<ExitRec>& out, bool logRaw);

// (EnumerateExitDestinations was REMOVED: the +0x8c dest table is indexed by a field-sign record's +0x1d
//  byte, not the jump index — nothing pairs +0x8c[i] with +0x54[i]. Exit destination names will come from
//  the in-game map's own resolver, DAT_02b457e0 → FUN_003c0340/FUN_003bf430/FUN_003c2320, pending the probe.)

// (EnumerateExits — the mapData+0x70 field-sign walker — was REMOVED this session: the +0x70 array is
//  confirmed EMPTY on every map by two testers, so it never yielded a destination name.)

// (The naviicon minimap "markers" enumerator was REMOVED in Session 44 — two decompile traces proved that
// array holds only character/unit dots that duplicate the combatant scan; no objective/crystal source.)

} // namespace MapExits
