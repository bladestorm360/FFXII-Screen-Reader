#pragma once

#include <cstdint>
#include <vector>

#include "navigation/nav_types.h"

// The audio beacon: a repeating, panned ping that leads the player along the route `\` just spoke.
//
// Each corner where a spoken leg runs out is a beacon drop point. The ping is panned toward the
// current corner and speeds up from 1 s to 0.2 s as the player closes on it; arriving advances to
// the next corner SILENTLY; arriving at the last one plays the sound pitched up, once, and stops.
// Wander off the route and it silently re-plans.
//
// ---------------------------------------------------------------------------------------------
// THE PAN IS A TRAVEL DIRECTION, NOT A TURN INSTRUCTION
//
// The pan angle answers "which way do I walk", not "how far do I rotate my body". It is the SAME
// angle the spoken leg is, in the same frame, from the same number: if the leg is "Northeast", the
// voice says Northeast and the beacon sits ~45 degrees right. The two are one value in two
// encodings, so `facingRad` here comes from PlayerState::ReadCameraForwardStable -- exactly the call
// PathDirections::Describe is handed -- and never from a second source. That makes "the beacon pans
// right while the voice says West" structurally impossible instead of a bug to test for.
//
// ---------------------------------------------------------------------------------------------
// POLLED MONITOR -- A DOCUMENTED, EXPLICITLY APPROVED EXCEPTION
//
// CLAUDE.md: "NO polling, timers, or per-frame checks -- event-driven hooks only. No exceptions
// outside narrow polled-monitor cases that have been documented and explicitly approved." A beacon
// is definitionally a polled monitor: there is no game event for "the player got two metres closer",
// and the whole feature is the continuous readout of that distance. Approved by the tester on
// 2026-07-29 as part of the original request. This is that documentation.
//
// The cost is bounded and pays for itself: OnGameFrame is one relaxed atomic load and an immediate
// return whenever no beacon is running, which is the overwhelmingly common case.
//
// NO SPEECH. Nothing in this module speaks -- not on leg advance, not on arrival, not on a re-plan.
// The no-dedup rule is therefore not engaged here at all. Diagnostics go to the "BEACON" log
// category (feedback_never_speak_filler_be_silent).
namespace AudioBeacon {

// GAME THREAD. Arm the beacon on a fresh route. `legPoints` is PathDirections::Describe's
// outLegPoints -- one corner per spoken leg, last element the destination. An empty list stops the
// beacon, which is how a failed re-plan turns it off without a special case.
void Seed(const std::vector<FVec3>& legPoints, uint32_t epoch);

// GAME THREAD. Once per field frame from the FUN_0022a770 hook, after PathPlanner::OnGameFrame so a
// route seeded this frame starts pinging immediately rather than a frame later.
void OnGameFrame();

// Why the route last stopped being active (Session 100, for AutoWalk). `External` is any Stop() a
// caller did not label -- teardown, shutdown, a failed re-plan's empty Seed. `Reseeded` means a NEW
// route replaced the old one (not a stop at all from the player's point of view).
enum class StopReason { None, Arrived, MapChange, Reseeded, External };
StopReason LastStopReason();

// One consistent view of the route being followed (Session 100, for AutoWalk). GAME THREAD ONLY --
// reads the same non-atomic leg state OnGameFrame owns; calling it from another thread is a race by
// construction, exactly like Seed. Returns false when no route is active (fields then meaningless
// beyond routeActive/engagedCombat/epoch). `routeLenM` is player -> current corner -> ... -> end.
struct LegSnapshot {
    bool     routeActive   = false;
    bool     engagedCombat = false;   // the beacon's own PartyEngagement edge-latch
    uint32_t epoch         = 0;
    size_t   legIndex      = 0;       // 0-based index of the corner currently steered at
    size_t   legCount      = 0;
    FVec3    legTarget{};
    FVec3    finalTarget{};
    float    routeLenM     = 0.0f;
};
bool GetLegSnapshot(LegSnapshot& out);

// Stop and forget the route. Safe from any thread. The reason defaults to External so an unlabelled
// caller cannot leave a stale, more specific reason standing.
void Stop(StopReason reason = StopReason::External);

// True while a route is loaded and the beacon is running.
bool Active();

} // namespace AudioBeacon
