#include "navigation/route_keep.h"
#include "navigation/audio_beacon.h"
#include "navigation/nav_common.h"
#include "navigation/nav_mesh.h"
#include "navigation/reach_gate.h"
#include "core/logger.h"

#include <cstdio>

namespace RouteKeep {
namespace {

// Does the rest of the live route -- the player's position, then each remaining corner -- cross a floor a
// script has closed? Each straight segment is sampled every 0.5 m and the walkmap poly under each sample is
// asked S182's raw-clear / effective-set question. The target's own closed material is exempt, exactly as
// the search exempts it, or a route to a shut door could never be kept. A few dozen memory reads; no
// engine calls.
bool CrossesClosedFloor(const FVec3& from, const std::vector<FVec3>& corners, const FVec3& target,
                        int& outMaterial, FVec3& outAt) {
    outMaterial = -1;
    const uint32_t bit = ReachGate::PartyRefuseBit();
    uint32_t exempt = 0;
    const NavMesh::PolyId goal = NavMesh::FindPolyAt(target.x, target.y, target.z);
    uint32_t gr = 0, ge = 0;
    if (goal != NavMesh::kNoPoly && NavMesh::PolyFlags(goal, gr, ge) && ReachGate::ScriptClosedFlags(gr, ge, bit))
        exempt |= 1u << ReachGate::Material(gr);

    constexpr float kStep = 0.5f;
    FVec3 a = from;
    for (const FVec3& b : corners) {
        const int n = static_cast<int>(NavCommon::Distance2D(a, b) / kStep) + 1;
        for (int i = 1; i <= n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            const FVec3 s{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
            const NavMesh::PolyId p = NavMesh::FindPolyAt(s.x, s.y, s.z);
            uint32_t raw = 0, eff = 0;
            if (p == NavMesh::kNoPoly || !NavMesh::PolyFlags(p, raw, eff)) continue;
            if (!ReachGate::ScriptClosedFlags(raw, eff, bit)) continue;
            if ((exempt >> ReachGate::Material(raw)) & 1u) continue;
            outMaterial = static_cast<int>(ReachGate::Material(raw));
            outAt = s;
            return true;
        }
        a = b;
    }
    return false;
}

} // namespace

bool Decide(bool sameObjective, const FVec3& from, const FVec3& target, uint32_t epoch,
            const PathSearch::Stats& st, uint64_t seq, std::vector<FVec3>& corners) {
    corners.clear();
    if (!sameObjective) return false;                          // a different destination: nothing to keep

    AudioBeacon::LegSnapshot snap;
    if (!AudioBeacon::GetLegSnapshot(snap) || snap.epoch != epoch) return false;   // no live route here
    if (!AudioBeacon::RemainingCorners(corners)) return false;

    char m[320];
    if (st.goalConnected == 0) {
        snprintf(m, sizeof(m),
                 "keep-route: seq=%llu NOT kept -- the oracle proves the goal is not in the player's walkable "
                 "component (start poly %d); the live route ends", (unsigned long long)seq, st.startPoly);
        Log::Write("NAV-ROUTE", m);
        corners.clear();
        return false;
    }
    int mat = -1;
    FVec3 at{};
    if (CrossesClosedFloor(from, corners, target, mat, at)) {
        snprintf(m, sizeof(m),
                 "keep-route: seq=%llu NOT kept -- the rest of the live route now crosses a script-closed floor "
                 "(material %d at (%.1f,%.1f,%.1f)); the live route ends", (unsigned long long)seq, mat,
                 at.x, at.y, at.z);
        Log::Write("NAV-ROUTE", m);
        corners.clear();
        return false;
    }
    snprintf(m, sizeof(m),
             "keep-route: seq=%llu KEPT -- the search from (%.1f,%.1f,%.1f) found no route (pass=%s, oracle=%s) "
             "but the live route to the same objective stands: leg %zu/%zu, %zu corner(s) ahead, %.1fm left",
             (unsigned long long)seq, from.x, from.y, from.z, st.pass,
             st.goalConnected == 1 ? "connected" : "not run", snap.legIndex + 1, snap.legCount,
             corners.size(), snap.routeLenM);
    Log::Write("NAV-ROUTE", m);
    return true;
}

} // namespace RouteKeep
