#include "navigation/path_planner.h"
#include "navigation/player_state.h"
#include "navigation/map_query.h"
#include "navigation/map_seams.h"
#include "navigation/nav_mesh.h"
#include "navigation/path_search.h"
#include "navigation/path_danger.h"
#include "navigation/sneak_assist.h"
#include "navigation/nav_blocked.h"
#include "navigation/nav_reach.h"
#include "navigation/reach_gate.h"
#include "navigation/route_query.h"
#include "navigation/nav_trace.h"
#include "navigation/map_names.h"
#include "navigation/path_directions.h"
#include "navigation/nav_common.h"
#include "navigation/audio_beacon.h"
#include "speech/speech.h"
#include "speech/phrasebook.h"
#include "core/logger.h"

#include <windows.h>          // GetTickCount64 -- the wait below is a wall-clock deadline

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
uint64_t              g_waitDeadlineMs = 0; // wall-clock expiry of the wait-for-nav-safe window
uint64_t              g_notSafeLoggedSeq = 0; // game-thread only: dedupe the per-frame not-safe log to once/request
// Arm the audio beacon when this route lands. Set by `\`, clear for `p` -- see the header.
bool                  g_seedBeacon = false;
// The target's map-jump group, 0 when it is not a walk-onto surface. Read ONLY on the failure path,
// to turn `target` -- one vertex of a seam -- into the whole seam. See PathSearch::Run's seam pass.
int                   g_seamGroup = 0;
// Suppress ALL speech for this request, whatever the outcome. Only RequestReplan sets it: the
// beacon re-aiming itself is not something the player asked to hear, and "No path" spoken out of
// nowhere because they walked round a corner would be worse than the stale beacon it replaced.
bool                  g_silent = false;

// ---- the BEACON'S OBJECTIVE (guarded by g_mutex, same as the request above) --------------------
//
// A SEPARATE MEMORY FROM "the last request", and that separation is the Session 95 fix. `\` and `p`
// both call Request(), but only `\` arms the beacon -- and the beacon's off-route recovery used to
// re-run whatever was requested last, so a mid-fight `p` at an enemy quietly became the destination the
// beacon led to once the fight ended. It then "arrived" a few metres later and stopped, with the real
// objective still tens of metres away. See RequestReplan in the header for the log evidence.
//
// Only a `seedBeacon` request writes these; `p` cannot touch them. Cleared on map teardown along with
// everything else that is a coordinate on the dead map.
FVec3                 g_objTarget;
std::wstring          g_objLabel;
bool                  g_objIsTransition = false;
float                 g_objBandLo = 1.0f, g_objBandHi = -1.0f;
float                 g_objReach = 0.0f;
int                   g_objSeamGroup = 0;
bool                  g_haveObjective = false;

// Keep retrying a request while the map is still fading in, then give up out loud.
//
// THIS IS WALL-CLOCK, NOT A FRAME COUNT, AND THAT IS THE WHOLE POINT. It used to be `kWaitFrames =
// 90` with a comment claiming ~1.5 s, which is only true at exactly 60 fps: the field tick runs
// once per call to FUN_0022a770, so at any other rate the same 90 frames is a different amount of
// real time and a route asked for during a fade could expire before the map was ready. Expiry here
// SPEAKS a refusal, so the frame rate could change the answer, not merely its timing. Game speed is
// NOT a factor -- 2x/4x runs the sim loop INSIDE that one call more times, so the hook fires once
// per rendered frame at every speed. See Docs\PerFrameAudit.md.
constexpr uint64_t kWaitMs = 1500;

// "At the exit" -- how near, in X/Z and in Y -- is RouteQuery::AtTransition's (S182), shared with the
// Unreachable filter so the two can never disagree about arriving.
// Inside this, the player is standing IN the doorway and a bearing to it would be noise.
constexpr float kOnExitDist = 1.0f;

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

uint32_t CurrentEpoch() { return g_epoch.load(std::memory_order_acquire); }

bool RequestReplan() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_haveObjective) return false;
    // The whole destination is restored from the OBJECTIVE snapshot -- target, label, band, reach,
    // isTransition -- so anything requested since (a `p` at an enemy, most often) cannot redirect it.
    // Only the delivery changes: silent, and it arms the beacon.
    g_target       = g_objTarget;
    g_label        = g_objLabel;
    g_isTransition = g_objIsTransition;
    g_bandLo       = g_objBandLo;
    g_bandHi       = g_objBandHi;
    g_reach        = g_objReach;
    g_seamGroup    = g_objSeamGroup;
    g_silent       = true;
    g_seedBeacon   = true;
    g_reqEpoch   = g_epoch.load(std::memory_order_acquire);
    g_waitDeadlineMs = GetTickCount64() + kWaitMs;
    ++g_reqSeq;
    g_hasRequest.store(true, std::memory_order_release);
    char lm[96]; LabelForLog(g_objLabel, lm, sizeof(lm));
    char m[208];
    snprintf(m, sizeof(m),
             "replan: silent re-run of the BEACON OBJECTIVE \"%s\" at (%.2f,%.2f,%.2f) seq=%llu",
             lm, g_objTarget.x, g_objTarget.y, g_objTarget.z, (unsigned long long)g_reqSeq);
    Log::Write("NAV-ROUTE", m);
    return true;
}

void Request(const FVec3& target, const std::wstring& label, bool isTransition,
             float bandLo, float bandHi, float reachRadius, bool seedBeacon, int seamGroup) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_target       = target;
    g_label        = label;
    g_isTransition = isTransition;
    g_bandLo       = bandLo;
    g_bandHi       = bandHi;
    g_reach        = reachRadius;
    g_seedBeacon   = seedBeacon;
    g_seamGroup    = seamGroup;
    g_silent       = false;
    // ONLY A REQUEST THAT ARMS THE BEACON BECOMES ITS OBJECTIVE. `p` passes seedBeacon=false because it
    // has no business steering the beacon; that same flag is what stops it redirecting one that is
    // already running.
    if (seedBeacon) {
        g_objTarget       = target;
        g_objLabel        = label;
        g_objIsTransition = isTransition;
        g_objBandLo       = bandLo;
        g_objBandHi       = bandHi;
        g_objReach        = reachRadius;
        g_objSeamGroup    = seamGroup;
        g_haveObjective   = true;
    }
    g_reqEpoch   = g_epoch.load(std::memory_order_acquire);
    g_waitDeadlineMs = GetTickCount64() + kWaitMs;
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
    // The beacon's leg corners are coordinates on the map being torn down. It would notice via the
    // epoch on its next frame anyway, but stopping here cuts a ping mid-transition instead of
    // letting one more fire at a corner that no longer exists.
    AudioBeacon::Stop();
    // Measured obstructions are coordinates on the map being torn down.
    NavBlocked::Clear();
    std::lock_guard<std::mutex> lk(g_mutex);
    g_haveObjective = false;                           // its coordinates belong to the map being torn down
    g_hasRequest.store(false, std::memory_order_release);
}

void OnGameFrame() {
    // Capture-distance diagnostic (Session 106, log-only). One int compare per frame on maps without
    // a danger-table row; on table maps it logs the pre-gap player position and actor distances when
    // a scripted scene (a catch, among others) hands control back. See path_danger.h.
    PathDanger::NoteFieldFrame();
    // Refresh which scene objects are this map's guards, so the touch-test override can answer per
    // object (S113). One table lookup and a store while disarmed, which is almost always.
    SneakAssist::OnFieldFrame();
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
            // Sweep the map's transition seams, ONCE PER EPOCH, from inside this gate.
            //
            // This used to pass only the map id, and the comment here claimed IsFieldNavSafe() is false
            // for "the whole of a transition" so the id and the resident walkmap could not disagree.
            // STRUCK (Session 93): it is false for the MIDDLE of a transition. At the LEADING edge the id
            // has already flipped while the previous map's walkmap is still resident, and the tester's
            // Ridorana log caught the sweep running on map 306's polygons and tagging them 1101 -- after
            // which the crossing oracle blamed a "wrong group->destination binding" that was fine.
            // The epoch this function already holds is the only signal that actually brackets a map.
            // ONE line per epoch recording the fail mask on the first nav-safe frame of the map. This
            // is the confirmation for loosening the gate (Session 93): the two manifest bits are now
            // log-only, so if a map reports them the log says so and routing still runs. It is also how
            // a regression in the OTHER six bits would surface -- a map that becomes nav-safe with an
            // unexpected mask is one grep away instead of one play session.
            {
                const uint32_t ep = g_epoch.load(std::memory_order_acquire);
                static uint32_t s_loggedEpoch = 0xFFFFFFFFu;
                if (s_loggedEpoch != ep) {
                    s_loggedEpoch = ep;
                    const uint8_t fm = PlayerState::NavSafeFailMask();
                    char names[96];
                    PlayerState::FormatNavSafeMask(fm, names, sizeof(names));
                    char m[208];
                    snprintf(m, sizeof(m),
                             "gate: first nav-safe frame of epoch %u on map %d -- failMask=0x%02X[%s] "
                             "(bits 2/3 are log-only; routing runs regardless)",
                             ep, MapNames::CurrentMapId(), fm, names);
                    Log::Write("NAV-ROUTE", m);
                }
            }
            MapQuery::PrimeMapJumpSurfaces(MapNames::CurrentMapId(),
                                           g_epoch.load(std::memory_order_acquire));
            NavReach::OnGameFrame(g_epoch.load(std::memory_order_acquire), pp);
            // Breadcrumb the walked path. Piggybacks on the position read this block already does, and
            // is the only measurement we have of where a transition ACTUALLY fires -- see nav_trace.h.
            NavTrace::OnFieldFrame(MapNames::CurrentMapId(), pp);
        }
    }

    if (!g_hasRequest.load(std::memory_order_acquire)) return;   // O(1) common case

    FVec3 target; std::wstring label; uint32_t reqEpoch; uint64_t seq;
    bool isTransition; float bandLo, bandHi, reach; bool silent, seedBeacon; int seamGroup;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_hasRequest.load(std::memory_order_relaxed)) return;
        target = g_target; label = g_label; reqEpoch = g_reqEpoch; seq = g_reqSeq;
        isTransition = g_isTransition;
        bandLo = g_bandLo; bandHi = g_bandHi; reach = g_reach;
        silent = g_silent; seedBeacon = g_seedBeacon; seamGroup = g_seamGroup;
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
            if (g_reqSeq == seq && GetTickCount64() >= g_waitDeadlineMs) {
                g_hasRequest.store(false, std::memory_order_release);
                giveUp = true;
            }
        }
        // This branch runs every frame for up to kWaitMs; log the not-safe reason ONCE
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
                     "drain seq=%llu: not nav-safe (fieldSafe=%d posOk=%d failMask=0x%02X[%s]) -> retry up to %llu ms",
                     (unsigned long long)seq, navSafe ? 1 : 0, posOk ? 1 : 0, fm, names,
                     (unsigned long long)kWaitMs);
            Log::Write("NAV-ROUTE", m);
        }
        if (giveUp) {
            char lm[128]; LabelForLog(label, lm, sizeof(lm));
            char m[208];
            snprintf(m, sizeof(m), "drain: gave up (never nav-safe within window) for \"%s\" -> Route unavailable%s",
                     lm, silent ? " (silent replan; not spoken)" : "");
            Log::Write("NAV-ROUTE", m);
            if (silent) AudioBeacon::Stop();
            else        Speech::Output(Phrase::Get(Phrase::Id::RouteUnavailable), true);
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
    RouteQuery::Params q;
    q.target       = target;
    q.isTransition = isTransition;
    q.bandLo       = bandLo;
    q.bandHi       = bandHi;
    q.reach        = reach;
    q.seamGroup    = seamGroup;
    float exitDist = 0.0f, exitDy = 0.0f;
    if (RouteQuery::AtTransition(q, from, exitDist, exitDy)) {
        float facing = 0.0f;
        PlayerState::ReadCameraForwardStable(facing);
        std::wstring say = Phrase::Get(Phrase::Id::AtTheExit);
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
        // Standing on the destination: there is no route left to lead anybody along.
        if (seedBeacon) AudioBeacon::Stop();
        if (!silent) Speech::Output(say, true);
        if (!silent) ReachGate::NoteRouteResult(label, target, true);   // standing on it: plainly reachable
        ClearIfSeq(seq);
        return;
    }

    // The seam set for the failure path, and the search itself: RouteQuery::Search (S182), the same call
    // the Unreachable filter makes, so the answer it hides on is this answer.
    std::vector<FVec3> rawPoly, poly;
    PathSearch::Stats st;
    PathSearch::Plan r = RouteQuery::Search(q, from, curEpoch, rawPoly, poly, st);
    // THE UNREACHABLE FILTER'S ONLY INPUT (S182): the answer this request is about to SPEAK. A Frontier is
    // spoken as "No path", so for the filter it is exactly that. Not recorded when the player will not hear
    // it (a silent beacon replan), nor when the search could not place the player at all -- that is no
    // answer about the target. Storing it is a lock and a short vector scan; the filter never searches.
    if (!silent && RouteQuery::AnsweredAboutTarget(st))
        ReachGate::NoteRouteResult(label, target, r == PathSearch::Plan::Route);

    const char* planName = (r == PathSearch::Plan::Route)    ? "Route"
                         : (r == PathSearch::Plan::Frontier) ? "Frontier"
                                                             : "NoPath";
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
    // legPoints are the corners where each spoken leg runs out -- the audio beacon's drop points.
    // They come out of the SAME call that produces the words, so the beacon can never aim at a
    // corner the player was not told about.
    // A FRONTIER ROUTE IS SPOKEN, AND ITS SHORTFALL IS SPOKEN WITH IT.
    //
    // The goal could not be reached, so this walks as close as the mesh allows. Saying nothing is not an
    // option -- a sighted player can see which way to go round an obstacle and a blind one cannot, which
    // is exactly why "Route unavailable" was rejected. But speaking it as a plain route is the S73/S74
    // failure that walked the tester confidently to a spot 3 m from an exit 7.8 m overhead, so the legs
    // are followed by "Blocked" and how far short the frontier lands.
    //
    // Both words already exist: `BlockedWord` and `StepsSuffix`. The comma idiom matches the crow-flies
    // blocked phrasing ("Blocked, bear ..."), so no new phrasebook entry and no new permission.
    std::vector<FVec3> legPoints;
    std::wstring say;
    if (r == PathSearch::Plan::Route) {
        say = PathDirections::Describe(rawPoly, facingRad, seedBeacon ? &legPoints : nullptr);
    } else {
        // A PARTIAL ROUTE IS A FAILURE, AND IS NO LONGER SPOKEN AS A ROUTE (Session 96).
        //
        // **This reverses the Session 93 ruling** ("we can not have routes that simply dead end"), on
        // the tester's explicit instruction: *"we aren't looking for partial paths so there's no reason
        // to say 'blocked'... if a path is partial, it either means the player can't access it from
        // this part of the map or the pathfinder failed to find the correct route."*
        //
        // What it looked like in play, from one log: `"Northwest 2. 2 steps. Blocked, 195 steps"`, then
        // "No path" five times, then `"Southeast 12, ... Blocked, 89 steps"` pointing the OTHER way --
        // and walking southeast led straight back into the region that answers "No path". A two-step
        // route that falls 195 steps short is not a route; it is the search's failure mode read aloud,
        // and following it is worse than being told nothing, because it actively misleads.
        //
        // The frontier geometry is still COMPUTED and fully logged -- it is the best evidence we have
        // about where the search ran out -- it is simply not spoken and does not arm the beacon.
        say = std::wstring(Phrase::Get(Phrase::Id::NoPath));
        if (r == PathSearch::Plan::Frontier) {
            char lm[128]; LabelForLog(label, lm, sizeof(lm));
            char m[256];
            snprintf(m, sizeof(m),
                     "drain seq=%llu: FRONTIER SUPPRESSED for \"%s\" -- %zu legs reaching %.1fm short; "
                     "spoken as No path (partial routes are not spoken; see path_planner.cpp)",
                     (unsigned long long)seq, lm, rawPoly.size(), st.shortfall);
            Log::Write("NAV-ROUTE", m);
        }
    }

    if (seedBeacon) {
        // An empty list (no route, or a route under half a step) stops the beacon -- which is also
        // the right answer for a failed silent re-plan, so it needs no separate branch.
        AudioBeacon::Seed(legPoints, curEpoch);
    }

    // Log the spoken directions (ASCII cardinals/digits) so the exact leg text is diagnosable.
    {
        char t[192]; size_t n = 0;
        for (wchar_t wc : say) { if (n + 1 >= sizeof(t)) break; t[n++] = (wc < 128) ? static_cast<char>(wc) : '?'; }
        t[n] = '\0';
        char mt[256];
        snprintf(mt, sizeof(mt), "drain seq=%llu: say=\"%s\"%s beaconLegs=%zu",
                 (unsigned long long)seq, t,
                 silent ? " (SILENT replan -- not spoken)" : "", legPoints.size());
        Log::Write("NAV-ROUTE", mt);
    }

    if (!silent) Speech::Output(say, true);
    ClearIfSeq(seq);
}

} // namespace PathPlanner
