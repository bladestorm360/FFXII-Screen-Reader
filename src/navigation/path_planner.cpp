#include "navigation/path_planner.h"
#include "navigation/player_state.h"
#include "navigation/map_query.h"
#include "navigation/nav_mesh.h"
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
// The target's INTERACTION BAND (InteractTarget::ReadBandFor): the range of player Y from which the
// engine will let you interact with it. Captured at Request() on the input thread, where the target
// object is still in hand, and handed to PathSearch so the goal becomes "a cell you could stand in
// and interact from" rather than "the target's own cell". Inverted (lo > hi) = no band known, which
// PathSearch treats as the pre-Session-73 behaviour.
float                 g_bandLo = 1.0f, g_bandHi = -1.0f;
// The engine's own horizontal interaction reach for the target (InteractTarget::ReadReachFor,
// radiusMin -- the direction-independent bound). 0 = unknown, which routes to the target's own
// poly exactly as before.
float                 g_reach = 0.0f;
uint32_t              g_reqEpoch = 0;       // g_epoch captured at Request()
uint64_t              g_reqSeq   = 0;       // distinguishes successive requests
int                   g_framesLeft = 0;     // retry countdown while not yet safe
uint64_t              g_notSafeLoggedSeq = 0; // game-thread only: dedupe the per-frame not-safe log to once/request

// ~1.5 s: keep retrying a request while the map is still fading in, then give up out loud.
constexpr int kWaitFrames = 90;

// How near an exit counts as BEING AT IT, in X/Z metres. Measured need: on Muthru Bazaar the tester
// stood 0.78 m from the Rabanastre East End arrival and was still fed "East 3. 3 steps" on ten
// consecutive route presses, cycling through East / Northeast / Southeast / North and never
// converging, because the last two metres are a transition trigger and not a walk.
constexpr float kAtExitDist = 3.0f;
// Inside this, the player is standing IN the doorway and a bearing to it would be noise.
constexpr float kOnExitDist = 1.0f;
// ...and it must ALSO be within this much of the seam's own height (Session 74).
//
// Y used to be deliberately ignored here, on the reasoning that an exit's height was unreliable. That
// was true when the height came from the map-control blob, and it stopped being true in Session 64
// when an exit became a walkmap FLOOR POLYGON -- its Y is now a floor. Ignoring it meant Upper
// Apartments announced "At the exit" for the Highhall seam while the player stood 7.8 m beneath it,
// because the two exits there are 4 m apart horizontally and 9.7 m apart vertically.
//
// Generous on purpose: it only has to separate storeys, never to judge a slope or a doorstep. The
// old rule survives for anything blob-derived; this applies to walkmap seams, which is all of them.
constexpr float kAtExitDy   = 3.0f;

// THE DESTINATION NAME IS NOT SPOKEN. A route is requested for the thing the player just heard
// named by `[`/`]`, or for the target they just locked, so "Eastgate. 3 north, 2 east" spends the
// first second of every route repeating something they already know before reaching the part they
// pressed the key for. Requested by the tester, 2026-07-28: speak the RESULT, nothing else -- and
// the same rule for all four outcomes ("At the exit", "No path", "Route unavailable", the legs), so
// there is nothing to remember about which one names its target.
//
// The label is still CARRIED, because the log has to be able to say which target a route was for.
// It goes to the NAV-ROUTE drain line instead of to speech; see LabelForLog.
void LabelForLog(const std::wstring& label, char* out, size_t cap) {
    size_t n = 0;
    for (wchar_t wc : label) { if (n + 1 >= cap) break; out[n++] = (wc < 128) ? static_cast<char>(wc) : '?'; }
    out[n] = '\0';
}

// Clear the pending flag only if no newer request arrived while we were planning.
void ClearIfSeq(uint64_t seq) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_reqSeq == seq) g_hasRequest.store(false, std::memory_order_release);
}

} // namespace

bool Init()  { return true; }
void Shutdown() { g_hasRequest.store(false, std::memory_order_release); }

void Request(const FVec3& target, const std::wstring& label, bool isTransition,
             float bandLo, float bandHi, float reachRadius) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_target       = target;
    g_label        = label;
    g_isTransition = isTransition;
    g_bandLo       = bandLo;
    g_bandHi       = bandHi;
    g_reach        = reachRadius;
    g_reqEpoch   = g_epoch.load(std::memory_order_acquire);
    g_framesLeft = kWaitFrames;
    ++g_reqSeq;
    g_hasRequest.store(true, std::memory_order_release);
    char m[128];
    snprintf(m, sizeof(m), "request: target=(%.2f,%.2f,%.2f) epoch=%u seq=%llu (input thread)",
             target.x, target.y, target.z, g_reqEpoch, (unsigned long long)g_reqSeq);
    Log::Write("NAV-ROUTE", m);
    char bm[128];
    snprintf(bm, sizeof(bm), "request: interaction band=[%.2f,%.2f] reach=%.2f%s",
             bandLo, bandHi, reachRadius,
             (bandHi < bandLo) ? " (no band -> target's own poly)"
                              : (reachRadius <= 0.01f ? " (no reach -> target's own poly)" : ""));
    Log::Write("NAV-ROUTE", bm);
}

void OnMapTeardown() {
    g_epoch.fetch_add(1, std::memory_order_acq_rel);   // any pending request is now stale
    NavMesh::Invalidate();                             // drop the cached walkmap arrays for the dead map
    NavReach::Invalidate();                            // and the reachable-set answer built on it
    MapQuery::InvalidateMapJumpSurfaces();             // and the seams, which are walkmap geometry too
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
            // Sweep the map's transition seams, ONCE per map, from inside this gate. It has to be
            // here and nowhere else: IsFieldNavSafe() is false for the whole of a transition (the
            // area id reads 0xFFFFFFFF and the leader pointer is zeroed at teardown start), so it
            // is the only cheap proof that the resident walkmap belongs to the map id we are about
            // to tag the answer with. Sweeping on MapQuery::HasWorld() instead -- which only proves
            // SOME walkmap is up -- is what left the seam cache a full map behind and scrambled
            // every exit in Garamsythe Waterway. Runs before the two consumers below, both of which
            // read the seams. See MapQuery::PrimeMapJumpSurfaces.
            MapQuery::PrimeMapJumpSurfaces(MapNames::CurrentMapId());
            NavReach::OnGameFrame(g_epoch.load(std::memory_order_acquire), pp);
            // Breadcrumb the walked path. Piggybacks on the position read this block already does, and
            // is the only measurement we have of where a transition ACTUALLY fires -- see nav_trace.h.
            NavTrace::OnFieldFrame(MapNames::CurrentMapId(), pp);
        }
    }

    if (!g_hasRequest.load(std::memory_order_acquire)) return;   // O(1) common case

    FVec3 target; std::wstring label; uint32_t reqEpoch; uint64_t seq;
    bool isTransition; float bandLo, bandHi, reach;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_hasRequest.load(std::memory_order_relaxed)) return;
        target = g_target; label = g_label; reqEpoch = g_reqEpoch; seq = g_reqSeq;
        isTransition = g_isTransition;
        bandLo = g_bandLo; bandHi = g_bandHi; reach = g_reach;
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
            char lm[128]; LabelForLog(label, lm, sizeof(lm));
            char m[208];
            snprintf(m, sizeof(m), "drain: gave up (never nav-safe within window) for \"%s\" -> Route unavailable", lm);
            Log::Write("NAV-ROUTE", m);
            Speech::Output(L"Route unavailable", true);
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
    const float exitDy   = std::fabs(from.y - target.y);
    if (isTransition && exitDist <= kAtExitDist && exitDy <= kAtExitDy) {
        float facing = 0.0f;
        PlayerState::ReadCameraForwardStable(facing);
        std::wstring say = L"At the exit.";
        // Beyond arm's reach, still say where it is: standing BESIDE the seam rather than on it is a
        // real difference the player can act on.
        if (exitDist > kOnExitDist) {
            say += L" ";
            say += NavCommon::CardinalBearingRelative(from, target, facing);
            say += L" ";
            say += std::to_wstring(NavCommon::DistanceToSteps(exitDist));
            say += L".";
        }
        char lm[128]; LabelForLog(label, lm, sizeof(lm));
        char m[320];
        snprintf(m, sizeof(m), "drain seq=%llu: at transition \"%s\" d2D=%.2fm dY=%.2fm from=(%.2f,%.2f,%.2f) tgt=(%.2f,%.2f,%.2f)",
                 (unsigned long long)seq, lm, exitDist, exitDy, from.x, from.y, from.z,
                 target.x, target.y, target.z);
        Log::Write("NAV-ROUTE", m);
        Speech::Output(say, true);
        ClearIfSeq(seq);
        return;
    }

    std::vector<FVec3> rawPoly, poly;
    PathSearch::Stats st;
    PathSearch::Plan r = PathSearch::Run(from, target, curEpoch, bandLo, bandHi, reach, rawPoly, poly, st);

    const char* planName = (r == PathSearch::Plan::Route) ? "Route" : "NoPath";
    // The label is logged here and NOT spoken -- this line is the only record of which target a
    // route was for, now that the speech is bare directions.
    char lm[128]; LabelForLog(label, lm, sizeof(lm));
    char m[288];
    snprintf(m, sizeof(m), "drain seq=%llu: target=\"%s\" from=(%.2f,%.2f,%.2f) plan=%s legs=%zu",
             (unsigned long long)seq, lm, from.x, from.y, from.z, planName, poly.size());
    Log::Write("NAV-ROUTE", m);
    // Search stats — diagnoses a NoPath (startFloor=0 => the player's own fine cell has no
    // floor; expands maxed => budget/maze). gridSamples = fine GroundAt samples this map
    // (cache size), fineCell = routing resolution.
    char ms[288];
    snprintf(ms, sizeof(ms),
             "drain seq=%llu: stats plan=%s pass=%s startPoly=%d goalPoly=%d endPoly=%d "
             "expands=%d touched=%d volumeRays=%d nearDist=%.1fm",
             (unsigned long long)seq, planName, st.pass, st.startPoly, st.goalPoly, st.endPoly,
             st.expands, st.touched, st.rays, st.nearDist);
    Log::Write("NAV-ROUTE", ms);

    // The route's own shape is now logged by PathSearch itself (the `mesh:` line: start/goal/end poly,
    // path length, expansions and volume checks). The three diagnostics that used to live here -- the
    // direct-line step probe, the 33x33 terrain field and the per-leg step profile -- all measured the
    // GRID: they sampled MapQuery::GroundAt (which is not a floor query) and compared against kMaxStep
    // and kStepDiscont, gates the mesh search does not have. Kept as-is they would print `wmBlock=
    // 5(step>max)` about a limit nothing enforces, which is worse than printing nothing.

    // Instrumentation: target + raw/smoothed polyline sizes + the first leg, for diagnosing
    // a route. (Leg directions themselves are camera-relative -- see the facingRad block below.)
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
    std::wstring say = (r == PathSearch::Plan::Route) ? PathDirections::Describe(rawPoly, facingRad)
                                                      : std::wstring(L"No path");

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
