#include "navigation/path_validate.h"
#include "navigation/map_query.h"
#include "navigation/nav_footprint.h"
#include "navigation/nav_mesh.h"

#include <cmath>

namespace PathValidate {

namespace {

// A leg counts as walkable when the engine gets essentially all of the way along it. Not a tuned
// threshold: MapQuery::BodySweep returns the ENGINE's own achieved/requested fraction (the quantity it
// stores at ctrl+0x120), and anything short of ~1 means the character was pushed off the line. The slack
// absorbs the depenetration pull-back at the far end, which lands the body one radius short of a wall it
// is legitimately walking up to -- a route that ends against a shopfront is not a breach.
constexpr float kMinFraction = 0.90f;

// Body height, matching NavMesh::StraddleAt and entity_commands.cpp. The sweep is a body sweep so it has
// its own radius, but the LINE still has to start somewhere sane: at the feet it grazes the floor the
// path is standing on, which is how an earlier version of this check reported a breach on 100% of routes
// including a 0.9 m leg from the player's own position.
constexpr float kBodyPad = 0.9f;

} // namespace

LegReport CheckLegs(const std::vector<FVec3>& path, int probeCap) {
    LegReport r;
    if (path.size() < 2) return r;
    r.total = path.size() - 1;

    for (size_t i = 1; i < path.size(); ++i) {
        if (r.probes >= probeCap) { r.truncated = true; break; }

        const FVec3 a{ path[i - 1].x, path[i - 1].y + kBodyPad, path[i - 1].z };
        const FVec3 b{ path[i].x,     path[i].y     + kBodyPad, path[i].z     };

        MapQuery::BodyMove mv;
        const bool clear = MapQuery::BodySweep(a, b, mv);
        ++r.probes;
        ++r.checked;
        if (mv.valid && mv.fraction < r.worstFraction) r.worstFraction = mv.fraction;

        if (!clear && mv.valid && mv.fraction < kMinFraction) {
            r.ok = false;
            r.firstBad = i;
            return r;
        }

        // The corner the leg ARRIVES at, when it is an interior corner. A leg can be perfectly sweepable
        // and still end on a point the engine pushes the body off -- that is the whole reason the corner
        // inset exists, and checking it here is what makes the inset's effect visible rather than
        // theoretical. The final point is the target itself and is deliberately exempt: routes to a
        // shopfront or a wall-mounted notice board legitimately end hard against a boundary.
        if (i + 1 < path.size()) {
            if (r.probes >= probeCap) { r.truncated = true; break; }
            ++r.probes;
            const NavMesh::PolyId at = NavMesh::FindPolyAt(path[i].x, path[i].y, path[i].z);
            if (at != NavMesh::kNoPoly && !NavFootprint::Clears(path[i], at, nullptr)) {
                r.ok = false;
                r.firstBad = i;
                return r;
            }
        }
    }

    if (r.checked < r.total) r.truncated = true;
    return r;
}

} // namespace PathValidate
