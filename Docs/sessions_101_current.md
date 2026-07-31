# FFXII-Screen-Reader — Session Log (Sessions 101–current)

Continues `sessions_051_100.md`, which is closed at **Session 100** (the adjacency march +
auto-walk build).

Entry format is mandatory: `## Session N — YYYY-MM-DD — [track] <title>`, where `N` is a single,
global, monotonically increasing integer shared by all parallel tracks. Never a date-only header,
never a letter sub-session. Before appending, grep this file for the highest `## Session N` AND
check `git log` for an unlogged session after it; the next session takes `N+1`. Split again after
Session 150 (`sessions_101_150.md` + `sessions_151_current.md`). Every entry carries a KEYWORDS
line for grep.

## Session 101 — 2026-07-31 — [navigation] The goal is a SURFACE: the route now ends where walking first touches the seam

KEYWORDS: surface goal seam poly set map jump transition endpoint portal entry point path_surface_goal
PathSurfaceGoal::Route surfaceTouch first pop pass=surface-goal 315 northern sluiceway replan frontier
suppressed no path 16.4m short vertex boundary S98 diagnosis S99 circularity InsetCorners kArrivalTol

**The one residual defect from the S100 breakthrough.** Map 315 routes and the tester has walked it
(`319c0bb`), but a mid-route replan from certain approach directions still failed: the route target
is `MapQuery::NearestPointOnSurface` — the nearest tagged seam **vertex** in XZ — and on a 27 m seam
that is the corner the walkable approach reaches LAST. The final leg then runs along the exit surface,
validation fails, `PathSearch::Run` returns `Plan::Frontier` 16.4 m short, the planner suppresses it
and speaks "No path", and the beacon stops until the player presses `\` again.

### What shipped

`src/navigation/path_surface_goal.{h,cpp}` — a route that ends where walking first TOUCHES the
transition surface:

- **The endpoint is the PORTAL the corridor crosses onto the surface**, not a distance to anything.
  `EdgeClearSpan` (falling back to `EdgePortal`) → midpoint → step `kStepIn = 0.5 m` toward the
  surface poly's centroid so the arrival poly is unambiguously the transition → `ClosestPointOnPoly`
  to clamp back onto the triangle and take Y from its own plane.
- **The corridor is rebuilt for that end poly** via the existing `PathCorridor::Build`, the same
  helper `BuildFrontier` uses and for the same reason: funnelling one poly's portal sequence toward a
  point on another is not a path the funnel can repair.
- **Validation is the ordinary one, not a lighter one.** `BestPolarity` → `InsetCorners` →
  `DropPassedWaypoints` → `PathValidate::CheckLegs(poly, budget, kArrivalTol)`, in that order, with
  the corner footprint tests, the adjacency march and the pinned-corner acceptance all included.
  Accepted **only** on `ok && !truncated && size >= 2`; anything else falls through to today's
  frontier. No partial acceptance, no relaxed tolerance, no repair-ladder shortcut.

`path_search.cpp` grew by ~15 lines (it is already 789 and over the 500-line cap, which is why the
work went in its own file): an `unordered_set` built once per `Run` from `seamPolys`; `surfaceTouch`
recorded on the **first pop of any member**, reset beside `came.clear()` so the touch and the parent
links can never come from different passes; and the consumer replacing the `(void)seamPolys;` line.

### Why this cannot change a route that works today — the property, not the promise

1. `seamPolys` is `nullptr` for every request that is not a walk-onto transition
   (`path_planner.cpp` only fills it when `seamGroup != 0`), so the set is empty, `surfaceTouch`
   stays `kNoPoly`, and the whole block is unreachable.
2. The block sits **below** the `if (best.reachedGoal && bestReport.ok && !truncated) return
   Plan::Route`, so a route that validates never reaches it.
3. Observing the set costs one hash probe per pop and changes no A\* decision.

Only two outcomes are reachable: a validated `Route` where there is a suppressed Frontier / "No path"
today, or the frontier path exactly as it runs today.

### The S99 rule, and why this is not S98 again

**A route may never be validated against a point derived from that same route's own progress.** S98
aimed at the seam member nearest the banked proven prefix's end; the prefix already ended on a seam
poly, so the aim point WAS the reference (`0.0m from ref` ×18) and the re-run validated the prefix it
came from. Here the endpoint is a portal between two mesh triangles — geometry that exists whether or
not this search ran — and the proof is a body walk, not a distance comparison. `pass=seam` remains
grep-dead; this pass logs **`pass=surface-goal`**.

### State

**BUILT, DEPLOYED, NOT PLAY-CONFIRMED.** Regression gate to check first in the next tester log, before
anything else: `pass=seam` absent; the working 315 route still `pass=mesh` with `inset=` non-zero,
`validate: … OK` and ~20/20 legs; and **no `surface-goal:` line on it** — a validated route must never
reach the new block. Then the fix itself: replan from mid-bank should log `surface-goal: … ACCEPTED`
and `plan=Route` where it logged `FRONTIER SUPPRESSED … 16.4m short`.

Done on the `nav/surface-goal` worktree so it can be reverted without touching Session 102's
event-bound transition work. `debug.md`'s Sluiceway section had its stale "STILL BLOCKED" title struck
and rows 11-14 added — the anatomy of what actually unblocked the map.
