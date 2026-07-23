#include "navigation/path_planner.h"
#include "navigation/player_state.h"
#include "navigation/map_query.h"
#include "navigation/nav_grid.h"
#include "navigation/path_search.h"
#include "navigation/nav_reach.h"
#include "navigation/nav_trace.h"
#include "navigation/map_names.h"
#include "navigation/path_directions.h"
#include "navigation/nav_common.h"
#include "speech/speech.h"
#include "core/logger.h"

#include <atomic>
#include <mutex>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace PathPlanner {

namespace {

// ---- request state (input thread writes, game thread reads) ------------------
std::atomic<uint32_t> g_epoch{1};          // map generation; bumped on teardown
std::atomic<bool>     g_hasRequest{false};  // cheap O(1) idle check on the game thread
std::mutex            g_mutex;              // guards the fields below
FVec3                 g_target;
std::wstring          g_label;
bool                  g_isTransition = false;   // target is a map-jump surface: arriving == crossing
uint32_t              g_reqEpoch = 0;       // g_epoch captured at Request()
uint64_t              g_reqSeq   = 0;       // distinguishes successive requests
int                   g_framesLeft = 0;     // retry countdown while not yet safe
uint64_t              g_notSafeLoggedSeq = 0; // game-thread only: dedupe the per-frame not-safe log to once/request

// ~1.5 s: keep retrying a request while the map is still fading in, then give up out loud.
constexpr int kWaitFrames = 90;

// How near an exit counts as BEING AT IT, in X/Z metres. Y is deliberately ignored: on a multi-level map
// the arrival's height is unreliable even after the walkmap projection, and a doorway at your feet must
// not fail this test over a metre of slope.
//
// 3 m is two fine nav cells (NavGrid::kFineCell = 1.5 m). Measured need: on Muthru Bazaar the tester
// stood 0.78 m from the Rabanastre East End arrival and was still fed "East 3. 3 steps" on ten
// consecutive route presses, cycling through East / Northeast / Southeast / North and never converging,
// because the last two metres are a transition trigger and not a walk.
constexpr float kAtExitDist = 3.0f;
// Inside this, the player is standing IN the doorway and a bearing to it would be noise.
constexpr float kOnExitDist = 1.0f;

// Clear the pending flag only if no newer request arrived while we were planning.
void ClearIfSeq(uint64_t seq) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_reqSeq == seq) g_hasRequest.store(false, std::memory_order_release);
}

} // namespace

bool Init()  { return true; }
void Shutdown() { g_hasRequest.store(false, std::memory_order_release); }

void Request(const FVec3& target, const std::wstring& label, bool isTransition) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_target       = target;
    g_label        = label;
    g_isTransition = isTransition;
    g_reqEpoch   = g_epoch.load(std::memory_order_acquire);
    g_framesLeft = kWaitFrames;
    ++g_reqSeq;
    g_hasRequest.store(true, std::memory_order_release);
    char m[128];
    snprintf(m, sizeof(m), "request: target=(%.2f,%.2f,%.2f) epoch=%u seq=%llu (input thread)",
             target.x, target.y, target.z, g_reqEpoch, (unsigned long long)g_reqSeq);
    Log::Write("NAV-ROUTE", m);
}

void OnMapTeardown() {
    g_epoch.fetch_add(1, std::memory_order_acq_rel);   // any pending request is now stale
    NavGrid::Invalidate();                             // drop the whole-map overlay for the dead map
    NavReach::Invalidate();                            // and the reachable-set answer built on it
    std::lock_guard<std::mutex> lk(g_mutex);
    g_hasRequest.store(false, std::memory_order_release);
}

void OnGameFrame() {
    // Advance the per-map reachability fill BEFORE the pending-request check: it is bounded work that has
    // to make progress whether or not anyone asked for a route, because the exit list filters on it. Once
    // the component is closed this is a couple of atomic loads.
    {
        // For about a second after a map load the position reads as the exact ORIGIN before the party is
        // placed. It passes IsFieldNavSafe, so the flood fill used to burn a whole pass on it ("fill
        // complete -- 1 cells reachable from (0,0)" on every single transition) and the breadcrumb ring
        // recorded a phantom crumb that skewed its own walked box. No real field position is exactly
        // (0,0,0) -- every map's playable area is well away from the origin -- so rejecting it costs
        // nothing and both consumers simply wait a frame longer for the truth.
        FVec3 pp;
        if (PlayerState::IsFieldNavSafe() && PlayerState::ReadPlayerPos(pp) &&
            !(pp.x == 0.0f && pp.y == 0.0f && pp.z == 0.0f)) {
            NavReach::OnGameFrame(g_epoch.load(std::memory_order_acquire), pp);
            // Breadcrumb the walked path. Piggybacks on the position read this block already does, and
            // is the only measurement we have of where a transition ACTUALLY fires -- see nav_trace.h.
            NavTrace::OnFieldFrame(MapNames::CurrentMapId(), pp);
        }
    }

    if (!g_hasRequest.load(std::memory_order_acquire)) return;   // O(1) common case

    FVec3 target; std::wstring label; uint32_t reqEpoch; uint64_t seq;
    bool isTransition;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_hasRequest.load(std::memory_order_relaxed)) return;
        target = g_target; label = g_label; reqEpoch = g_reqEpoch; seq = g_reqSeq;
        isTransition = g_isTransition;
    }

    // Map changed since the request was made -> un-revivably stale; drop silently.
    uint32_t curEpoch = g_epoch.load(std::memory_order_acquire);
    if (reqEpoch != curEpoch) {
        char m[112];
        snprintf(m, sizeof(m), "drain seq=%llu: stale epoch (req=%u cur=%u) -> drop",
                 (unsigned long long)seq, reqEpoch, curEpoch);
        Log::Write("NAV-ROUTE", m);
        ClearIfSeq(seq);
        return;
    }

    // Not fully live yet (fade / partial load) OR the player doesn't resolve -> retry
    // for a bounded window before giving up. Never touch map data while unsafe.
    FVec3 from;
    bool navSafe = PlayerState::IsFieldNavSafe();
    bool posOk   = navSafe && PlayerState::ReadPlayerPos(from);
    if (!navSafe || !posOk) {
        bool giveUp = false;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_reqSeq == seq && --g_framesLeft <= 0) {
                g_hasRequest.store(false, std::memory_order_release);
                giveUp = true;
            }
        }
        // This branch can run up to kWaitFrames times; log the not-safe reason ONCE
        // per request (dedupe on seq) so the file isn't spammed per frame.
        if (seq != g_notSafeLoggedSeq) {
            g_notSafeLoggedSeq = seq;
            // Name WHICH gate condition(s) failed (0 == would be safe; then posOk was the
            // blocker). This is what turns a silent route into a diagnosable one.
            uint8_t fm = PlayerState::NavSafeFailMask();
            char names[96];
            PlayerState::FormatNavSafeMask(fm, names, sizeof(names));
            char m[192];
            snprintf(m, sizeof(m),
                     "drain seq=%llu: not nav-safe (fieldSafe=%d posOk=%d failMask=0x%02X[%s]) -> retry up to %d frames",
                     (unsigned long long)seq, navSafe ? 1 : 0, posOk ? 1 : 0, fm, names, kWaitFrames);
            Log::Write("NAV-ROUTE", m);
        }
        if (giveUp) {
            Log::Write("NAV-ROUTE", "drain: gave up (never nav-safe within window) -> Route unavailable");
            Speech::Output(label.empty() ? L"Route unavailable"
                                         : (label + L". Route unavailable"), true);
        }
        return;
    }

    // ---- Standing on the transition ---------------------------------------------------------------
    // An exit's target IS its map-jump surface -- the floor the game fires the transition on -- so
    // arriving there means crossing. Within a couple of metres the remaining distance is the seam
    // itself, not a walk, and A* would otherwise keep emitting honest little two-metre legs that to a
    // blind player read as the exit moving around (ten presses, ten directions, no arrival).
    //
    // NO DIRECTION IS SPOKEN. S59 shipped one and it sent the tester east into a wall; there is nothing
    // left to derive anyway now that the route ends on the trigger. Checked BEFORE the search, so
    // standing on an exit can never produce "No path" either.
    const float exitDist = NavCommon::Distance2D(from, target);
    if (isTransition && exitDist <= kAtExitDist) {
        float facing = 0.0f;
        PlayerState::ReadCameraForwardStable(facing);
        std::wstring say = label.empty() ? std::wstring() : (label + L". ");
        say += L"At the exit.";
        // Beyond arm's reach, still say where it is: standing BESIDE the seam rather than on it is a
        // real difference the player can act on.
        if (exitDist > kOnExitDist) {
            say += L" ";
            say += NavCommon::CardinalBearingRelative(from, target, facing);
            say += L" ";
            say += std::to_wstring(NavCommon::DistanceToSteps(exitDist));
            say += L".";
        }
        char m[192];
        snprintf(m, sizeof(m), "drain seq=%llu: at transition %.2fm from=(%.2f,%.2f,%.2f) tgt=(%.2f,%.2f,%.2f)",
                 (unsigned long long)seq, exitDist, from.x, from.y, from.z, target.x, target.y, target.z);
        Log::Write("NAV-ROUTE", m);
        Speech::Output(say, true);
        ClearIfSeq(seq);
        return;
    }

    std::vector<FVec3> rawPoly, poly;
    PathSearch::Stats st;
    PathSearch::Plan r = PathSearch::Run(from, target, curEpoch, rawPoly, poly, st);

    const char* planName = (r == PathSearch::Plan::Route) ? "Route" : "NoPath";
    char m[160];
    snprintf(m, sizeof(m), "drain seq=%llu: from=(%.2f,%.2f,%.2f) plan=%s legs=%zu",
             (unsigned long long)seq, from.x, from.y, from.z, planName, poly.size());
    Log::Write("NAV-ROUTE", m);
    // Search stats — diagnoses a NoPath (startFloor=0 => the player's own fine cell has no
    // floor; expands maxed => budget/maze). gridSamples = fine GroundAt samples this map
    // (cache size), fineCell = routing resolution.
    char ms[288];
    snprintf(ms, sizeof(ms),
             "drain seq=%llu: stats plan=%s pass=%s tgtCell=(%d,%d) rays=%d startFloor=%d expands=%d touched=%d nearest=(%d,%d) nearDist=%.1fm | fineCell=%.1fm gridSamples=%d",
             (unsigned long long)seq, planName, st.pass, st.tx, st.tz, st.rays,
             st.startFloorHit ? 1 : 0, st.expands, st.cells,
             st.nearC, st.nearR, st.nearDist, NavGrid::kFineCell, NavGrid::SamplesThisMap());
    Log::Write("NAV-ROUTE", ms);

    // Instrumentation: target + raw/smoothed polyline sizes + the first leg, for diagnosing
    // a route. Directions are WORLD-ABSOLUTE (no facing/camera frame).
    {
        const FVec3 s1 = (poly.size() > 1) ? poly[1] : target;
        char mg[192];
        snprintf(mg, sizeof(mg),
                 "drain seq=%llu: tgt=(%.1f,%.1f) rawPts=%zu smPts=%zu firstLeg=(%.1f,%.1f)",
                 (unsigned long long)seq, target.x, target.z, rawPoly.size(), poly.size(), s1.x, s1.z);
        Log::Write("NAV-ROUTE", mg);
    }

    // Leg directions are EGOCENTRIC, in egocentric words ("ahead 12, right 4"). Movement is
    // camera-relative, so this is the only frame the player can act on. Reference comes from the
    // STABLE getter (live value, else the last good one) and its bool is HONOURED -- the old code
    // defaulted `facingRad` to 0 and ignored the return, and CompassFaceDeg(0) == 180, so a frame
    // with no refreshed camera row spoke every leg 180 degrees reversed.
    float facingRad = 0.0f;
    const char* refSrc = "none";
    PlayerState::ReadCameraForwardStable(facingRad, &refSrc);   // always yields a reference; see player_state.h

    // Log the reference and how far it moved since the LAST route. This is the line that turns "the
    // directions reversed" from a session of guesswork into a glance: a big `dref` between two drains is
    // the game swinging its camera (expected; documented in the ReadMe), a near-zero `dref` under flipped
    // words would be a mod bug. Game-thread only, so a plain static is fine.
    {
        static bool  s_havePrev = false;
        static float s_prevRef  = 0.0f;
        const float deg = facingRad * 57.2957795f;
        float dref = 0.0f;
        if (s_havePrev) {
            dref = deg - s_prevRef;
            while (dref > 180.0f)  dref -= 360.0f;
            while (dref < -180.0f) dref += 360.0f;
        }
        s_prevRef = deg; s_havePrev = true;
        char m[128];
        snprintf(m, sizeof(m), "drain seq=%llu: ref=%.1fdeg src=%s dref=%.1fdeg",
                 (unsigned long long)seq, deg, refSrc, dref);
        Log::Write("NAV-ROUTE", m);
    }

    // Legs come from the RAW cell path, NOT the smoothed `poly`. The string-pull above collapses a
    // route to line-of-sight chords, which is right for geometry and wrong for instructions: an
    // L-shaped route across open ground becomes one diagonal chord, and the player is told to walk a
    // line the route never takes -- then any drift off that imaginary diagonal comes back as a
    // completely different direction. `poly` stays the validated geometry and the diagnostic below.
    std::wstring say = label.empty() ? std::wstring() : (label + L". ");
    if (r == PathSearch::Plan::Route) say += PathDirections::Describe(rawPoly, facingRad);
    else                  say += L"No path";

    // Log the spoken directions (ASCII cardinals/digits) so the exact leg text is diagnosable.
    {
        char t[192]; size_t n = 0;
        for (wchar_t wc : say) { if (n + 1 >= sizeof(t)) break; t[n++] = (wc < 128) ? static_cast<char>(wc) : '?'; }
        t[n] = '\0';
        char mt[224];
        snprintf(mt, sizeof(mt), "drain seq=%llu: say=\"%s\"", (unsigned long long)seq, t);
        Log::Write("NAV-ROUTE", mt);
    }

    Speech::Output(say, true);
    ClearIfSeq(seq);
}

} // namespace PathPlanner
