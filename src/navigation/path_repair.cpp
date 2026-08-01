#include "navigation/path_repair.h"
#include "navigation/nav_footprint.h"
#include "core/logger.h"

#include <cstdio>

namespace PathRepair {

Result Mend(const std::vector<FVec3>& poly,
            const std::vector<int>& polyIdx,
            const std::vector<PathFunnel::Portal>& portals,
            const PathValidate::LegReport& rep,
            const FVec3& from, const FVec3& to,
            int probeBudget, float arrivalTol) {
    Result out;
    if (rep.ok || rep.firstBad == 0 || portals.empty() || probeBudget <= 0) return out;

    const size_t bad = rep.firstBad;
    int budget = probeBudget;

    // Validate a candidate and keep it only if the body walked the whole thing. `budget` and
    // `out.probes` move together so the caller's accounting cannot drift from ours.
    auto tryPoly = [&](std::vector<FVec3>& cand, const char* how, int detail) -> bool {
        if (cand.size() < 2 || budget <= 0) return false;
        // `inset` runs BEFORE the body sweep, so anything it did to the candidate is already baked
        // into the verdict below -- which is exactly why its numbers belong on this line and not on
        // one of its own. See PathFunnel::InsetStats: `left` is the S117 measurement, and the
        // prediction it tests is that it stays 0 on the rungs that report OK and grows on the dense
        // candidates that report "still breaching".
        PathFunnel::InsetStats ins;
        PathFunnel::InsetCorners(cand, &ins);
        const PathValidate::LegReport r2 = PathValidate::CheckLegs(cand, budget, arrivalTol);
        budget      -= r2.probes;
        out.probes  += r2.probes;
        const bool good = r2.ok && !r2.truncated;
        char rm[352];
        snprintf(rm, sizeof(rm),
                 "repair[%s]: leg %zu, %d -- %zu->%zu points, inset %d/%d corner(s) moved, "
                 "%d LEFT their poly, probes=%d -> %s",
                 how, bad, detail, poly.size(), cand.size(), ins.moved, ins.corners, ins.leftHome,
                 r2.probes, good ? "OK" : "still breaching");
        Log::Write("NAV-ROUTE", rm);
        if (!good) return false;
        out.ok     = true;
        out.detail = detail;
        out.rung   = how;
        out.poly   = cand;
        out.report = r2;
        return true;
    };

    // 1a. Un-pull the failing leg (and, when it is interior, the corner it aimed at).
    //     UNCHANGED, AND FIRST, DELIBERATELY. This rung repaired 17 of 17 breaches it was offered in
    //     the failing session; nothing below is allowed to get in front of it.
    {
        std::vector<FVec3> mended;
        const int spliced = PathFunnel::Unpull(poly, polyIdx, portals, bad, mended);
        if (spliced > 0 && tryPoly(mended, "unpull", spliced)) return out;
    }

    // 1b. ...OR THE CORNER IT DEPARTED FROM (Session 97). The mirror image of 1a, and the one that had
    //     no rung at all: 1a can only move the corner a leg AIMS AT, so when the body cannot leave the
    //     corner it STARTS on there was nothing to try. It reaches here only when 1a declined (the two
    //     corners come off adjacent portals, so there is nothing to splice -- the usual shape of a
    //     final-leg breach) or tried and still breached. 7 of the 11 "No path" results in the tester's
    //     session were that case, every one of them the same corner that repairs cleanly when the
    //     breach happens to land one leg earlier.
    //
    //     STRICTLY ADDITIVE, ON PURPOSE. An earlier draft ordered 1a and 1b by `badReached`, on the
    //     reasoning that a body which never left its corner cannot be helped by waypoints further down
    //     the leg. True, and it would have re-ordered two routes that already repair (`reached=0.17m`,
    //     map 311) for no measured gain. After Session 96, a change that can alter a working route buys
    //     its way in with evidence or it does not go in.
    {
        std::vector<FVec3> moved;
        const int n = PathFunnel::UnpullDeparture(poly, polyIdx, portals, bad, moved);
        if (n > 0 && tryPoly(moved, "unpull-departure", n)) return out;
    }

    // 2. RETREAT TO WHERE THE BODY ACTUALLY GOT. `badStopAt` is the engine's own resolved position, so
    //    it is reachable whatever is in the way -- floor border, wall volume, or something we have not
    //    thought of. That is the point: this rung needs no theory.
    //
    //    ON THE FINAL LEG IT INSERTS RATHER THAN REPLACES (Session 97). The old guard
    //    (`bad + 1 < poly.size()`) refused the last leg outright, on the correct principle that the
    //    destination is not ours to move -- but the fix for that is to keep the destination and put the
    //    reachable point BEFORE it, not to skip the rung. The final leg's own arrival tolerance then
    //    covers the stub that is left.
    if (rep.badReached > NavFootprint::BodyRadius() && bad <= poly.size()) {
        const bool interior = (bad + 1 < poly.size());
        std::vector<FVec3> pulled(poly.begin(), poly.begin() + static_cast<ptrdiff_t>(bad));
        pulled.push_back(rep.badStopAt);
        // Interior: the stop stands in for the corner. Final: the stop is an extra waypoint and
        // `poly[bad]` -- the destination, the same FVec3 `/` measures to (S76) -- still ends it.
        pulled.insert(pulled.end(),
                      poly.begin() + static_cast<ptrdiff_t>(interior ? bad + 1 : bad),
                      poly.end());
        if (tryPoly(pulled, "retreat", static_cast<int>(rep.badReached * 100.0f))) return out;
    }

    // 3. LAST RUNG: the whole corridor, un-pulled -- every point in it one the body was measured to fit
    //    through, which is exactly what the chord across them is not.
    //
    //    NO LONGER RESERVED FOR `bad == 1` (Session 97). That gate was written for the case that had no
    //    other move at all (a leg-1 breach, where re-costing is refused because the portal is the start
    //    poly's own and the proven prefix is a single point), and it read as a restriction on when the
    //    corridor is TRUSTWORTHY. It is not: the corridor is walkable by construction on every leg.
    //    This is the last rung, reached only when every cheaper one has already failed its own
    //    re-validation, so there is nothing to protect by denying it to leg 7.
    {
        std::vector<FVec3> full;
        const int pts = PathFunnel::FullCorridor(from, to, portals, full);
        if (pts > 0 && tryPoly(full, "full-corridor", pts)) return out;
    }

    return out;
}

} // namespace PathRepair
