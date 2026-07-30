#pragma once

#include <cstddef>
#include <vector>
#include "navigation/nav_types.h"

// Leg validation for a finished polyline: can the party actually walk each leg of this route?
//
// Split out of path_search.cpp (Session 93). What was there before was `firstBreach`, and it had three
// problems that between them let 9 of 53 routes in the tester's log ship a path the mod had itself just
// proved unwalkable:
//
//   1. IT USED THE WEAKEST INSTRUMENT. One `MapQuery::SegmentClear` per leg -- a hairline ray with no
//      body radius, no depenetration, and both endpoints flattened to the start's Y. It cannot see an
//      obstacle the character's 0.27 m body hits but a zero-width line misses, and the flatten made it
//      a direction-asymmetric height test in disguise.
//   2. IT LIED BY TRUNCATION. `kMaxSegChecks = 24` capped the walk at 24 legs, then the caller logged
//      "portal-crossing path is clear" -- a claim about 24 legs printed as a claim about all of them. On
//      the Giza route the accepted path had 140.
//   3. IT COULD ONLY DEGRADE. Sitting after the search, its options were accept, swap in one
//      pre-computed alternative, or accept the thing it had just disproved. It never could route around
//      anything; that is the caller's job now (ban the offending portal and search again).
//
// So this reports honestly and completely: how many legs it checked, how many exist, which one failed,
// and what it spent. The caller decides what to do about it.
namespace PathValidate {

struct LegReport {
    bool   ok       = true;    // no breach found in the legs actually checked
    size_t checked  = 0;       // legs tested
    size_t total    = 0;       // legs in the path
    size_t firstBad = 0;       // 1-based index of the first failing leg; 0 == none
    int    probes   = 0;       // body sweeps + footprint tests spent
    float  worstFraction = 1.0f;  // smallest achieved/requested seen on any leg
    // True when the budget ran out before every leg was tested. The caller MUST NOT describe such a
    // path as verified -- that is exactly the claim the old cap made silently.
    bool   truncated = false;
};

// Walk the polyline and test each leg with the engine's own body sweep, plus a footprint test at each
// interior corner. `probeCap` bounds the work; when it is hit the report comes back `truncated` and the
// caller says so rather than rounding up to "clear".
//
// GAME THREAD, on a nav-safe frame -- it makes engine calls.
LegReport CheckLegs(const std::vector<FVec3>& path, int probeCap);

} // namespace PathValidate
