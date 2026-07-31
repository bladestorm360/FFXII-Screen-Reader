#pragma once

#include <cstddef>
#include <cstdint>
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

// What ended the sub-step walk of a leg. A breach reads completely differently depending on which of
// these it was, and until Session 96 all three printed identically.
// `Wall` IS GONE, NOT RENAMED (Session 97). It meant "a `PointInVolume` sample landed inside
// something", and that verdict refused 16 of 16 routes on map 315 while the body sweep objected to
// none of them. Nothing can produce it any more, and this project has been bitten twice by counters and
// enum values that are written by a comment and read by nothing -- so it is deleted rather than left
// permanently zero. If the class-aware wall test ever lands (`FUN_0022d4b0` via `FUN_002315e0`), it
// gets its own cause then.
enum class StopCause {
    None = 0,
    Sweep,     // the engine's body sweep would not carry the body that far -- floor or depenetration
    Budget,    // ran out of probes. NOT a breach: this sets `truncated`, never `ok = false`
};
const char* CauseName(StopCause c);

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
    // A TIGHT CORNER IS MEASURED, NOT FATAL (Session 95). See the note above CheckLegs.
    int    tightCorners = 0;   // interior corners whose footprint overlaps a boundary edge
    size_t firstTight   = 0;   // 1-based index of the first such corner; 0 == none
    // THE SUB-STEP RE-ASK (Session 95). `resweeps` counts legs the one-shot sweep called blocked and
    // that were therefore re-walked in character-sized steps; `rescued` how many of those turned out
    // to be walkable after all. A high `rescued` is the long-sweep domain error being caught; a high
    // `resweeps` with `rescued` 0 means the obstructions are real.
    int    resweeps = 0;
    int    rescued  = 0;
    float  badLength  = 0.0f;  // breaching leg's length in metres  } printed together so a breach can
    float  badReached = 0.0f;  // how far along it the body got     } be read without a second session
    // WHERE it stopped, in world coordinates, ON THE GROUND -- pinned to the walkmap under the stop,
    // never the lifted frame the sweep works in. The caller uses this as a route waypoint, so the
    // frame is part of the contract and not a detail: the two branches of the walk used to disagree
    // about it and the log printed both.
    FVec3  badStopAt{};
    // WHY IT STOPPED, AND WHAT IT STOPPED ON (Session 96). `badStopAt` said where and the log then
    // could not say what: a wall volume met mid-leg and a body sweep refused by the floor produce the
    // same coordinates, and the only wall counter (`walls`) covers the whole-leg pre-test, never the
    // sub-step walk. On map 311 four independent attempts stopped at the identical point 6.23 m along
    // a 9.00 m leg and the log could not say which of the two it was. One field ends that.
    StopCause badCause = StopCause::None;
    int    badStopPoly = -1;      // walkmap poly under the stop point, -1 == off-mesh
    bool   badStopWalk = false;   // ...and whether the party's own floor class may stand on it
    uint32_t badStopFlags = 0;    // its effective flags, so a bit-23 refusal is readable at a glance
    // FLOOR BORDER OR WALL? Filled only on a breach (see Diagnose in the .cpp). `badCornerClear` is
    // the footprint test at the corner the leg was aiming AT -- the question `tightCorners` looks like
    // it answers and does not, because the loop returns on a breach before its corner check runs.
    // The two volume probes bracket the stop: at it, and 0.3 m further along the leg.
    int    badCornerPoly    = -1;
    bool   badCornerClear   = false;
    float  badCornerMargin  = 0.0f;   // metres from the corner to the nearest hard border
    bool   badStopInVolume  = false;
    bool   badAheadInVolume = false;
    // WAS THE CHECK EVEN AWAKE? (Session 96). Every failure path in the stack -- null collision world,
    // unresolved RVA, SEH fault, unreadable straddle -- returns CLEAR, which is right (never invent a
    // block) and was invisible, which is not: a route validated against a dead world logged exactly
    // like a genuinely clear one. `blind` counts legs whose sweep never reached the engine.
    int    swept = 0;
    int    blind = 0;
    // WHAT THE VOLUME PROBE WOULD HAVE REFUSED (Session 97). `volHit` counts legs whose midpoint sits
    // inside a `PointInVolume` hit; `volWalked` how many of those the body then walked anyway. Until
    // this session a single `volHit` WAS the verdict, and on map 315 that refused 16 of 16 routes while
    // the body sweep objected to none of them. The pair is that test's own falsifier: `volWalked` equal
    // to `volHit` on routes the tester then walks means the probe was measuring nothing the party
    // collides with. **These two are READ AND PRINTED on the `validate:` line** -- if a later change
    // stops printing them, delete them; a counter nothing reads is a defect this project has shipped
    // twice. See WallSuspect in the .cpp for why it cannot answer the question it was asked.
    int    volHit    = 0;
    int    volWalked = 0;
};

// Walk the polyline and test each leg with the engine's own body sweep, plus a footprint test at each
// interior corner. `probeCap` bounds the work; when it is hit the report comes back `truncated` and the
// caller says so rather than rounding up to "clear".
//
// A LEG IS WALKED, NOT SPANNED (Session 95, second correction). `MapQuery::BodySweep` is
// `FUN_00230c10`, and it is only a body test over a SHORT displacement -- its third phase fires two
// probes rotated +/-30 degrees about the travel axis and keeps the shortest reach, so at distance d
// those probes are 0.5*d off the line. For that cone to stay inside the 0.27 m body the displacement
// must be <= ~0.54 m, which is exactly the per-frame delta all three of the engine's own call sites
// pass. This file was handing it 26-metre legs, so it swept a 13-metre-wide cone and reported a wall
// in any corridor narrower than that.
//
// Measured, from the tester's log: 26.00 m leg -> fraction 0.38; 26.82 m -> 0.42; 16.24 m -> 0.19.
// Fifteen of eighteen routes in that session ended up as Frontier because of it.
//
// So the one-shot sweep is kept as a FAST PATH -- when it says clear, the leg is clear and that costs
// one probe -- and a leg it calls blocked is RE-ASKED by walking it in character-sized steps with Y
// pinned to the walkmap under each step, carrying the engine's own resolved position forward so
// depenetration slides the body along a wall exactly as it does in play. Only that verdict is final.
//
// AND THE TEST IS A DISTANCE, NOT A RATIO. A fraction penalises short legs: the depenetration
// pull-back at the far end is a fixed ~one body radius, so a leg SHORTER than the radius can never
// achieve any ratio at all. The log had a 0.22 m leg failing at fraction 0.02 fourteen times -- with
// a 0.27 m body that outcome was arithmetically forced. A leg now passes when the body finishes
// within one body radius (plus a little) of where it was asked to go, which is dimensionally the
// right question and is length-independent.
//
// ONLY THE BODY SWEEP DECIDES `ok` (Session 95). The corner footprint test used to fail the whole route,
// and in the tester's log that accounted for 25 of 57 breaches -- every one of them a route whose legs
// INTO and OUT OF the corner had both swept clear. The two tests answer different questions and S93's
// own reading of the engine says so: `NavFootprint` replicates the border test the engine uses to push a
// body back to TANGENCY, so a corner that fails it is a place the player gets nudged, not a place they
// cannot walk through. `MapQuery::BodySweep` -- which returns the engine's own achieved/requested
// fraction, depenetration included -- is the authoritative answer to "did the body get there".
//
// AND THAT NOW INCLUDES THE VOLUME PROBE (Session 97). A whole-leg `MapQuery::PointInVolume` test was
// added in S96 on the premise that "walls are not floor geometry ... so a wall standing inside a floor
// triangle passed every check the router had". **The premise is false and the decompile says so.**
// `FUN_00230c10` -- the body sweep -- runs its ellipsoid push-out passes with CSR layer mask **7**
// (layers 0, 1 AND 2) through `FUN_0022de60`, over the same `0x4000`-tagged, `0x90`-stride volume
// primitives. It has always seen them. Conf 0.97.
//
// Worse, the two do not ask the same question. `FUN_00232490` installs `FUN_0022f8b0`, which tests
// **exactly one bit (31)** of the merged flags word and has no class filter at all. The engine's own
// movement collision reads `merged_flags & 7` against the mover's query class: class 0 always solid,
// class 1 conditional, **class 4 solid only when queryClass != 4** -- and the party's movers pass 4 --
// classes 2/3/5/6/7 never collide, bit 23 a "soft" hit that is recorded and does not block. It also
// hard-excludes the `>= 0x5000` range, i.e. doors and moving platforms. So it counts walls the party
// walks through and misses ones it cannot. Measured on map 315: 16 of 16 routes refused, every one
// `why=wall`, and the sweep never objected to a single leg.
//
// The consequence of conflating them was not cosmetic. A corner failure banned the portal on a leg that
// had swept fine, so each retry detoured around a good opening and the route got LONGER every attempt
// (211.6m -> 230.9m -> 233.0m -> 233.5m on one Garamsythe route) before the run gave up. Tight corners
// are still counted and reported, which was S93's stated reason for adding the test -- to make the
// corner inset's effect visible -- and validation now walks PAST one to find out whether the next leg
// actually sweeps, which is the question that was never being asked.
//
// GAME THREAD, on a nav-safe frame -- it makes engine calls.
// `arrivalTol` is how far short of the FINAL point the body may finish and still count as arrived.
// A target you cannot stand on -- an exit surface, a shopfront, a notice board, an NPC -- always stops
// the body short, so judging the last leg by the mid-route tolerance is a guaranteed false breach.
// Measured (Session 95 log): every single breach was the last leg, the body finishing 2.55 m from an
// exit against a 0.42 m tolerance, and the ban that followed made A* declare the goal unreachable and
// hand back a route ending 15.2 m short instead. Pass 0 to use the mid-route tolerance throughout.
LegReport CheckLegs(const std::vector<FVec3>& path, int probeCap, float arrivalTol = 0.0f);

} // namespace PathValidate
