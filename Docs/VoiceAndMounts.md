# Voiced dialogue and mounted autowalk investigation

Investigated 2026-10-15 by Sol. User authorized deeper offline investigation/RE, **no mod-side code
changes**. User clarified that mounted autowalk is a requested capability, not a tested failure.

Before investigating, committed the outstanding October 4 core fixes/tests and project notes as
`5207c1f` and pushed to the existing `origin/master`. All three isolated core/speech tests passed.
No deployment, game launch/control, live memory probe, actual screen-reader test or tool installation.

## Evidence and limits

Local RE archive: `../../FFXII-Decompile/notes/20261015_voice_mounts_01/`.
Its README registers the read-only PE/decompile queries, actual extracted-script census, unchanged
production-code fixture, logs and input hashes. Commercial code/data and raw outputs stay outside
this repository. The current executable SHA256 is
`73461d0896204b1c259c52f61aef4efbc95a2ffc388cadb36493d5d1d89721f8`; image base `0x120000`,
PE timestamp `1573552087`. Sampled native-table/thunk instructions agree with existing exports;
this is not certification of every historical decompile body. Native addresses below are RVAs.

## Voiced dialogue suppression

**Feasible, but not yet a verified quick toggle-only change.** Existing settings and the dialogue
output boundary are straightforward. The unresolved work is positive ownership of a displayed page's
audible voice, including failed/muted/paused playback and subtitle-before-voice ordering.

The existing `DialogueReader::HookedTextWalk` (`0x188C50`) detects page changes; `EmitPage` handles
choice ordering, repeat storage and automatic output. `HookedMsgSet` (`0x1C16B0`) retires recycled
window page identities. A later suppression policy must preserve these event guards, ChoiceReader,
the complete current repeat line and repeated identical lines from distinct valid events.

Native script voice flow recovered:

| Function | Role established from exports/current PE |
| --- | --- |
| `0x2381B0` (`FUN_003581b0`) | Common script voice core; decodes bank/entry and extra arguments, then prepares or requests playback. Multiple `voiceplay` wrappers and `voicestandby` converge here. |
| `0x2CA180` | Stores the current script-container `+0x28` word and an extra argument in a per-entry record. This is a possible ownership lead; field semantics and the message-page join remain unproved. |
| `0x2C9C10` | Prepares bank entry, sample handle, pending-start flag, pan and volume (clamped 0..127). Preparation alone is not audible playback. |
| `0x2C9780` | Services 64 voice banks; pending playback can start later and completed handles are retired. |
| `0x1052C0` | Backend start path; a nonzero SDK start return records state 2. Exact SDK failure/stop semantics still need qualification. |
| `0x1053D0` | Existing status query treats states other than 2 as active; not sufficient by itself to establish an audible, unpaused line. |

The targeted census decoded declared instructions in **1,115 extracted US event/map EBP files,
zero parser errors**. Native 651 accounts for 2,218 `voiceplay` sites, whereas native 135 has 262;
hooking only the obvious single-argument wrapper would miss the common path. Voice standby is
also widely used, so an allocated/ready handle must not automatically suppress speech.

In the actual `alc_a0100` script, captions (`amest`, native 130) and voices run in **different
routines**, both initially waiting 40 script ticks. Caption message 1 and voice entry 0 are separate
identifiers. Do not infer identity from equal indices, first-callback order, timing or text equality.
Exact local file hashes and instruction contexts are archived; no game script bodies are committed.

**Rejected shortcut:** `amesv` (1089) writes registry `+0x5C` before window construction, and
`mesvgetvoiceid` (1168) exposes a separate battle voice-work table. Neither native appears in this
US script corpus. The registry byte is copied into a construction descriptor and cleared afterward;
it is not an established persistent, per-page audible-voice flag. The earlier apparent registry link
does not establish the requested policy. The old `e5f0` dialogue notes are also superseded:
`message_reader.cpp` documents why that surface is the world map.

Before implementation, finish the message-owner/voice-owner join on the **actual `amest` path**,
prove page/voice coverage and retirement, and trace effective native voice mute/pause/global volume.
Handle deferred start and failure without waiting on the game thread. Observe bounded ready state
on its native owner; send copied identity/state/text to a mod worker. Do not retain engine pointers
on workers, scan the sound-manager list from a worker, or gate dialogue on unrelated audio activity.
If a page's voice is uncertain, preserve speech. No live capture is requested at this stage.

Requested UI remains a persistent suppression toggle (proposed default Off), independent of mute
and Autodetail. Suppress only automatic matching dialogue; choices/unvoiced lines remain spoken and
`t` explicitly repeats. BDFFHD/FFX establish behavior, not portable FFXII offsets or ownership.

## Mounted autowalk

**The fundamental native path is already compatible in design; a gameplay defect is not established.**
Do not write a new vehicle controller merely because the user requested coverage.

- Native 894/895 are mount/dismount thunks (`0x227230`/`0x227820` to `0x25CFF0`/`0x25CFC0`),
  which enable/disable the chocobo mode through `0x1E3D90`.
- Mode getter `0x1ED9A0` distinguishes mounted mode **2**, with higher-priority modes taking
  precedence; do not substitute a bare bit test for the full verdict.
- `0x2066C0` installs chocobo character ID `0x1A` in party slot **3**. `0x1E4060` attaches the
  rider to that actor. `0x206500` selects it as controlled leader, `0x2394B0` requests the handle
  change, and `0x238BC0` publishes it to `DAT_022c7fe0` (RVA `0x21A7FE0`). The mod already reads
  that handle. Blindly changing every player read to slot 3 would break walking and teardown.
- Shared driver `0x238CB0` resolves that controlled handle, reads normal movement input through
  `0x2394E0`, uses the established camera-relative transform, and updates the actor. Speed helper
  `0x115EA0` selects the mounted speed path. No separate steering/injection API was identified.
- Native class writer `0x1481E0` (guard wrapper `0x1481D0`) supplies mounted class **5**.
  Native floor predicate `0x110A40` uses effective bit 24 for class 5 and bit 23 for foot class 0.
  Existing `PartyMovementClass`, terrain/march checks and ReachGate already use the live class.

### Verified source risks, distinct from a reported mounted failure

1. **Class transition cache:** unchanged production `player_state.cpp` was compiled into an isolated
   x64 fixture with owned memory and a controlled clock. It returns old foot class 0 after the
   native byte changes to 5, until its 250 ms TTL expires; dismount shows the reverse. The cache has
   no actor/mount identity key. This can affect a route requested immediately after a transition.
   The fixture confirms source semantics, not frequency or consequences in gameplay.
2. **Route lifecycle:** beacon/autowalk snapshots carry map epoch but no mount/class identity.
   A same-map mounting change does not by itself retire a route planned for the previous movement
   class. Do not resume it automatically after a control transition without revalidation.
3. **Safety predicate:** `IsFieldNavSafe` requires party **slot 0** to be non-null, even though it
   separately resolves the actual controlled leader. The fixture proves rejection if slot 0 is null
   and the leader is valid. It does **not** prove slot 0 becomes null while riding; do not delete
   this teardown gate without recovering the real ownership/lifetime replacement.
4. **Existing manual priority gap:** autowalk injection observes keyboard movement cancellation,
   with no AutoWalk call from the SDL pad path. Physical stick override is not established by this
   code. Preserve all keyboard stops; controller cancellation must be covered before claiming
   complete controller-compatible mounted autowalk.

Steering uses fixed corner/rejoin/stuck constants; body sweep uses class-4 segment semantics and
0.27 radius. Neither should be changed just because mounted movement is faster. Native mover
footprint/turn behavior and normal/high-speed corner/arrival evidence are still required. A class
transition repair is likely a small maintenance change; general riding performance remains untested.

Scope the initial transport work to **chocobos**. Dedicated native/script support is established;
the private airship destination map is a separate menu (debug.md, Session 176). The inspected corpus
does not prove an exhaustive list of every controllable transport, so do not claim exclusivity.

## Next implementation pass and acceptance

Prioritize mounted transition/cache/route identity handling, then the existing mounted path, since
there is a concrete bounded issue and no evidence requiring a new steering system. Voice policy is
moderate remaining RE/integration, not just a settings edit; finish its exact owner join first.
Keep the broader refactoring audit separate, while obeying the current nonblocking contract in
touched paths. No implementation or runtime/performance success is claimed here.

After implementation/offline checks, the first user-side scenarios should be:

1. On foot, then mounted: select a reachable exit, request a route and follow a straight section
   and a corner. Expect correct motion and spoken stopping; manual movement must win. Repeat
   immediately after mounting/dismounting and at normal/high game speed where available.
2. Enable suppression for a voiced scene, then an unvoiced conversation/choice. Expect native voice
   without duplicate automatic speech, normal unvoiced/choice speech and explicit `t` repeat.
   Check Off, native voice volume zero/pause/failure, page changes and reload persistence.

The [maintenance plan](plan.md#current-maintenance-requests--2026-10-15) retains the accumulated
failure symptoms and safety checks. The user directs gameplay; Sol owns implementation correctness,
offline verification and technical evidence. No time or usage estimate has been measured.
