# Navigation audit and baked-map refactoring request

Recorded 2026-10-15. **Documentation/source inspection only; audit and implementation pending.**
User requests a deep audit of navigation physics, blockers and routing, and has selected a complete
pathfinding replacement using baked maps as **priority 1** under the maintained template. Mounted
autowalk/voiced dialogue are priority 2; broader mod threading/centralization review is priority 3.
The [current maintenance plan](plan.md#current-maintenance-requests--2026-10-15) owns that order.
No mod-side code change,
deployment, game execution or new runtime capture is authorized by this record.

## Tester report and corrected scope

The user confirms that **mounted autowalk failed for a tester**. Scope the repair to mounts/vehicles,
initially chocobos. The user's own lack of testing does not invalidate that report. Earlier notes
calling this an untested requested capability were incorrect; native shared movement support is
implementation evidence, not gameplay confirmation or a reason to dismiss the failure.

Tester `tidus10`, supplied by the user with time 4:52 AM (original message date not supplied):

> I’m currently trying to do the side quest in FF 12 to get to Exodus but I’m having trouble navigating to where I need to go on the choker boat. Has anyone else done this quest and if so can give any advice about the best way to navigate there? I need to cross that floating bridge or something like that.

User confirms **“choker boat” means chocobo**. This identifies the Exodus side quest, not a general
quest to unlock chocobo riding. Preserve both the mounted-autowalk report and objective/navigation
difficulty. The exact symptom (missing selectable target, rejected route, incorrect route, no injected
movement, wrong steering or an unsatisfied puzzle prerequisite) is still unresolved. No tester logs
or saves were accessed; this supplied report does not authorize searching them.

The [Zodiac Age Prima guide](https://primagames.com/eguides/final-fantasy-xii-the-zodiac-age-eguide/side-events/side-events-part-3)
locates this sequence in **Mosphoran Highwaste**, with shrine-dependent floatweed passage, a chocobo
section and subsequent shrine/rock interactions. This is gameplay context for the audit, not proof
of native collision layouts, the tester's exact position or current puzzle state. Recover those from
the matching game data/scripts. Do not hardcode a permanently open bridge from a walkthrough.

## What the current implementation actually does

- **Movement:** `AutoWalk::OnDevicePoll` in `src/navigation/auto_walk.cpp` ORs W/A/S/D bits into the
  DirectInput keyboard state after real input is observed. `src/proxy/dinput8_proxy.cpp` calls it
  from `HookedGetDeviceState`. It sends ordinary movement input; there is no gambit/party-AI move
  command or direct actor-position write in this autowalk path. The separate SDL controller bridge
  does not change how autowalk currently injects movement.
- **Planning:** `PathPlanner::OnGameFrame` -> `RouteQuery::Search` -> `PathSearch::Run` computes
  custom A* routes, corridors/funnel/repairs and validation. It does not ask native party AI to plan
  the route. Native movement/collision queries are used by parts of validation.
- **Map source:** `MapQuery::GetGridInfo` obtains the current native collision context's vertex,
  polygon, CSR and primitive arrays. `NavMesh::Grid` caches descriptors/pointers by map epoch;
  vertices, adjacency and live effective flags are read from game memory. The game's own mesh is
  authored map data, but **the mod does not currently load an offline-baked navigation-map package**.
  Cached native pointers, baked labels and exit metadata do not establish that architecture.
- **Runtime cost:** `nav_hooks.cpp` invokes planning from the field-frame hook. The audit must trace
  the full search/validation pipeline and its native dependencies against the current nonblocking
  contract; moving live pointers or thread-affine engine queries to a worker is not a valid repair.

## Deep audit required

1. Trace actual player/rider/mount identity, movement class, controlled input consumer, keyboard/pad
   ownership and masking, focus/readiness gates, speed and camera steering. Verify the mounted
   target/origin and delivered input together. The 250 ms class-cache fixture in
   [VoiceAndMounts.md](VoiceAndMounts.md) establishes a transition risk, not the reported failure's cause.
2. Recover native physics/query semantics and compare the production consumers: coordinate transforms,
   units, elevation/layers, slopes, adjacency, clearance/footprints, floor versus segment-query classes,
   static/dynamic volumes, passability masks and failure/unavailable results. Check both false blockers
   and paths incorrectly admitted. Preserve working maps and existing evidence-backed distinctions.
3. Examine the Exodus floatweed/chocobo sequence specifically: puzzle state and reveal prerequisites,
   enabled/disabled connections, bridge elevation, mounted-only approaches, exits and interaction
   regions. Determine from native evidence whether the bridge is a changed floor connection, separate
   geometry, dynamic object or another mechanism; its appearance/name does not settle this.
4. Trace objective discovery separately from reachability: quest/event/shrine/bridge targets, labels,
   selection, current presence/reveal gates and approach positions. A valid path cannot fix an absent
   or misidentified target. Do not expose future or hidden objectives from extracted data.
5. Audit A* costs/heuristics, corridor/funnel and height preservation, repair/fallback acceptance,
   work limits and truthful outcomes. Reject stale routes on mount, target, map, puzzle or blocker
   changes; all route speech, beacon and autowalk consumers must share the resulting state.
6. Validate state/counts and bound total work, queues and retained data. Establish actual ownership,
   cancellation and native callback/aggregate cost plus request-to-usable/audible response, data age
   and tail delays. Existing workers, caches or small batches alone are not performance evidence.

## Selected baked-map replacement

The user selected replacement of the current live-mesh search/repair implementation, with the goal
of addressing the recurring route and elevation failures structurally. This supersedes the earlier
recommendation merely to evaluate adoption. Implementation is still pending; the current turn orders
the documented work. Validate the replacement and retire the superseded planning path.

Use the template's [offline extraction and live reconciliation contract](../../../../codex/templates/accessibility-mod/references/offline-discovery.md#extract-map-data-offline):
extract the actual native geometry/layers, connections, placements, interaction approaches and
script/state conditions into versioned, hashed mod-owned maps. Retain useful native mesh structure
and bridge elevations rather than forcing a flat grid. Bake reusable graph/topology data offline;
ordinary builds/runtime must not repeat extraction.

**Baking maps and computing routes are complementary.** A planner still computes a route for the
current start, destination, movement mode and puzzle state using that map. A baked base can reduce
live native reads, support owned worker data and enable repeatable offline regression tests. It
does not automatically correct faulty extraction, collision semantics, steering, missing objectives
or dynamic connectivity. Reconcile native quest/reveal, bridge/shrine/gate state, moving units and
collection/despawn state; invalidate stale plans promptly. Static connectivity alone is insufficient
for this reported sequence.

Validate the full replacement against the current production planner on the same extracted game data
and faithfully recovered native semantics. Preserve ABI/numeric/order/lifetime evidence and make
unavailable engine boundaries explicit. Include working-map controls and discriminating foot/mount,
bridge open/closed, overlapping-height, false-blocker, cancellation and map-return cases. User gameplay
confirmation is distinct. No claim that most existing failures are solved merely by changing map storage.

## Optional native pathing/movement assessment — priority 2

Assess actual game-side pathfinding and movement, including whether party/gambit movement can safely
serve the player-controlled mount, against the existing W/A/S/D simulation. No suitable callable route
or efficiency advantage is established yet. Recover ABI/ownership, thread affinity, manual priority,
cancellation and control/physics behavior; compare equivalent end-to-end work and native callback cost,
usable response, mounted/controller coverage and maintenance burden. Keep input simulation if native
control is less efficient or provides no justified benefit. This option must fit the selected baked-map
architecture; it is not a requirement to switch movement mechanisms.

## Prioritized eventual acceptance

After offline implementation checks, test the reported Exodus sequence first when reachable:
identify the currently available next interaction/exit, request its route while on foot and mounted
where legal, cross the enabled floatweed passage, and stop for required manual interactions. Expect
correct target naming, route/steering and cancellation; never report arrival across an unavailable
connection. Compare closed/open puzzle states, mounting/dismounting, normal/high game speed and
keyboard/controller manual override. Record missing-target, No-path, wrong-level and no-movement
symptoms separately. Accumulate these with the broader maintenance acceptance procedure.

**Status:** tester failures preserved; current architecture source-verified; deep audit, exact cause,
bake coverage/format, optional native-control decision, implementation and gameplay/performance
validation pending. Baked-map replacement is selected as the first priority.
