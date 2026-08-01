#include "navigation/sneak_assist.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>

#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "navigation/entity_list.h"
#include "navigation/map_names.h"
#include "navigation/map_script.h"
#include "navigation/nav_common.h"
#include "navigation/nav_rva.h"
#include "navigation/path_danger.h"
#include "navigation/player_state.h"

#include <string>

namespace SneakAssist {
namespace {

// FUN_003448f0(ctx, _, _, vmState) -- the script native the 0x0290 slot dispatches. Only the first
// argument is read here (the VM context that carries the return slot); the rest are passed through.
using ScriptDistanceFn = void(__fastcall*)(void*, void*, void*, void*);
ScriptDistanceFn s_orig = nullptr;

// Where FUN_0026b4c0 stores a native's result, replicated exactly:
//     base = *(u64*)(ctx + 0xA8);  idx = *(i8*)(ctx + 0x11);
//     *(u32*)(base + 0xC + idx*0x28) = value;   *(u8*)(base + 0x15 + idx*0x28) = 3;
// We rewrite ONLY the value word, leaving the type tag the game just wrote.
constexpr uintptr_t OFF_CTX_SLOTBASE = 0xA8;
constexpr uintptr_t OFF_CTX_SLOTIDX  = 0x11;
constexpr uintptr_t OFF_SLOT_VALUE   = 0x0C;
constexpr size_t    SLOT_STRIDE      = 0x28;

// The clamp. Map 568's room is ~40 m across and the mod's own routes speak tens of metres, so this
// is orders of magnitude beyond any threshold a proximity check could carry -- while staying far
// from float extremes, so an unexpected downstream arithmetic use cannot produce an inf/NaN.
constexpr float kClampMetres = 9999.0f;

// ---- IS THE HOOKED NATIVE EVER CALLED? (S110 -- log-only, gate-independent) ----------------------
// The first play test armed the feature on map 568, ran the minigame through three complete
// shout/capture cycles, and produced ZERO clamp lines. Map covered + hook installed should have
// logged one. Something upstream of the clamp is wrong, and exactly two hypotheses fit: the script
// never calls this native during the sequence (so the capture is driven by something else -- the
// map's rect/touch tests are the obvious candidate), or the hook is not on the function the VM
// dispatches. **These counters tell the two apart with one play session**, and they run ABOVE the
// map gate so the answer does not depend on the feature being applicable here. Both were answered
// in S112: `native FIRED on map 568`, and the clamp works.
std::atomic<uint32_t> s_callsThisMap{0};
std::atomic<bool>     s_loggedFirstCall{false};
// THE MAP THE COUNTER BELONGS TO. `OnMapTeardown` used to print `MapNames::CurrentMapId()`, which by
// then has ALREADY ADVANCED -- so the log read `map 569 census: ... 2 time(s)` for a count that was
// 568's, and every census line in the file named the wrong map by one.
//
// S115 LATCHED IT ON THE FIELD TICK AND THAT DID NOT WORK EITHER -- the play log still says
// `map 569 census` for 568's two calls, because **the field tick has already run for the NEW map by
// the time teardown fires**, so the latch advanced with it. A fix for an ORDERING bug has to be
// verified against the ordering, not against the read.
//
// So the latch is WRITE-ONCE PER MAP and CONSUMED BY THE PRINT: the field tick fills it only when it
// is empty, and teardown empties it after printing. The new map's early ticks find it full and leave
// 568's id alone; teardown prints 568 and clears; the next tick latches 569. No ordering assumption
// survives in it at all -- whoever runs first, the id printed is the one the counter was counting on.
constexpr int kNoCensusMap = -1;
std::atomic<int> s_censusMapId{kNoCensusMap};
// Log-only, and reset on every map change: the watcher polls per frame, so without this the
// first-clamp evidence would be O(frames). This suppresses a LOG line, never speech (CLAUDE.md's
// log-volume exception -- the per-frame producer is the map script's own watcher routine).
std::atomic<bool> s_loggedThisMap{false};

// ---- THE GUARDS' OWN TRIGGER VOLUME (S113) ------------------------------------------------------
// FUN_002677f0(object, mode) answers "is the party LEADER inside THIS object's volume?" and is the
// shared choke point under both of the script's touch tests -- the instant one and the 21 waiting
// ones. Clamping `distance` alone was not enough: it stretched the distraction window from 4-7
// seconds to 3m02s, and then the sequence still ended, because this is the second catch path.
//
// IT IS ANSWERED PER OBJECT, WHICH IS THE WHOLE POINT. `object` is param_1, so the guards can be
// silenced while every other trigger volume on the map -- doors, event rects, the servant's own
// conversation triggers, the advance-urging rects -- keeps answering truthfully. A map-wide
// suppression would risk stopping the sequence from starting at all; this cannot.
using TouchTestFn = bool(__fastcall*)(void*, int);
TouchTestFn s_origTouch = nullptr;

// Snapshot of the current map's danger actors, refreshed on the field tick. Read from the VM thread
// inside a test the script polls tens of times a frame, so it must not take a lock: a tiny fixed
// array plus a relaxed count, and a stale entry can only mean one frame of vanilla behaviour.
//
// SIXTEEN, NOT EIGHT (Session 115): map 569 carries FOURTEEN "Imperial" actors on the one npcdic id,
// against 568's two, and a snapshot that truncated would silently leave real guards answering
// truthfully -- the one failure mode this array has.
constexpr int kMaxGuards = 16;
void*                 s_guards[kMaxGuards] = {};
std::atomic<int>      s_guardN{0};
std::atomic<bool>     s_loggedSuppress{false};

bool IsGuardObject(void* obj) {
    const int n = s_guardN.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < kMaxGuards; ++i)
        if (s_guards[i] == obj) return true;
    return false;
}

// THE ONE GATE (Session 115). There is no toggle any more: a map with a PathDanger row is a map
// this feature acts on, and every other map is one where the writes below are unreachable.
bool CoveredMap() {
    return PathDanger::MapHasRow(static_cast<uint32_t>(MapNames::CurrentMapId()));
}

// SEH-guarded: the ctx comes from the game's VM, and a torn/streaming pointer must degrade to
// "leave the value alone", never to a fault inside a native call.
bool ClampResultSlot(void* ctx, float* outOriginal, bool* outWasFloat) {
    __try {
        auto base = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(ctx) + OFF_CTX_SLOTBASE);
        if (!base) return false;
        const int idx = *reinterpret_cast<int8_t*>(reinterpret_cast<uint8_t*>(ctx) + OFF_CTX_SLOTIDX);
        if (idx < 0 || idx > 63) return false;
        auto* value = reinterpret_cast<uint32_t*>(base + OFF_SLOT_VALUE +
                                                  static_cast<uintptr_t>(idx) * SLOT_STRIDE);

        // WHICH REPRESENTATION -- MEASURED PER CALL, NOT GUESSED. FUN_004686d0 returns sqrtf's float,
        // and FUN_0026b4c0 stores a 32-bit word; the decompile does not show whether the float is
        // stored as bits or converted to an int, and a wrong guess would write a nonsense number. The
        // two readings are cleanly separable for a real distance, so the original value decides:
        // 8.13 m is 0x41022D0E, which reads as 1.09e9 when taken as an int; the integer 8 reads as
        // 1.1e-44 when taken as a float. Anything in [0.001, 100000) is the float reading.
        const uint32_t raw = *value;
        float asFloat = 0.0f;
        memcpy(&asFloat, &raw, sizeof(asFloat));
        const bool isFloat = std::isfinite(asFloat) &&
                             std::fabs(asFloat) >= 0.001f && std::fabs(asFloat) < 100000.0f;

        if (isFloat) {
            float clamp = kClampMetres;
            uint32_t bits = 0;
            memcpy(&bits, &clamp, sizeof(bits));
            *value = bits;
            if (outOriginal) *outOriginal = asFloat;
        } else {
            *value = static_cast<uint32_t>(kClampMetres);
            if (outOriginal) *outOriginal = static_cast<float>(raw);
        }
        if (outWasFloat) *outWasFloat = isFloat;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall HookedScriptDistance(void* ctx, void* a2, void* a3, void* vm) {
    // Log-only census (S110). One relaxed increment per call, and ONE log line per map load -- the
    // script polls this, so anything per-call would be O(frames). Deliberately ABOVE the toggle
    // check: the question it answers ("is this native called at all?") must not depend on the
    // feature being switched on.
    s_callsThisMap.fetch_add(1, std::memory_order_relaxed);
    if (!s_loggedFirstCall.exchange(true, std::memory_order_relaxed)) {
        char m[160];
        snprintf(m, sizeof(m), "native FIRED on map %d (first call this map) -- the hook is on a "
                               "function the script actually calls", MapNames::CurrentMapId());
        Log::Write("SNEAK", m);
    }

    // THE OFF-TABLE PATH IS THE FIRST BRANCH -- on every map but the table's, this function is the
    // original plus one table scan, and no write below is reachable.
    if (!CoveredMap()) {
        if (s_orig) s_orig(ctx, a2, a3, vm);
        return;
    }

    // Run the game's own native first: it pops its arguments and writes the result slot, so the VM
    // stack stays exactly as the engine left it. Only then is the stored value replaced.
    if (s_orig) s_orig(ctx, a2, a3, vm);
    if (!ctx) return;

    float original = 0.0f;
    bool  wasFloat = false;
    const bool ok = ClampResultSlot(ctx, &original, &wasFloat);

    if (!s_loggedThisMap.exchange(true, std::memory_order_relaxed)) {
        char m[208];
        snprintf(m, sizeof(m),
                 "clamp ACTIVE on map %d: script distance %.2f -> %.0f (%s slot)%s -- "
                 "first clamp this map; the watcher polls per frame so later ones are silent",
                 MapNames::CurrentMapId(), original, kClampMetres,
                 wasFloat ? "float" : "int", ok ? "" : "  <== SLOT UNREADABLE, value left alone");
        Log::Write("SNEAK", m);
    }
}

// The FALSIFIER (log-only). The capture volume may not belong to the guard NPCs at all -- map 569's
// own actor names include `捕獲レクト兵士` ("capture rect soldier"), and a rect actor carries no
// npcdic name, so the entity scan cannot see it and the rule above would never match it. Every
// OTHER object that reports the leader inside it, on a danger map, is logged once. If a
// capture still happens, this names the object that caused it -- one table entry, not another
// guessing round. Deliberately does NOT suppress: an unnamed object may be a story rect, and
// silencing one could stop the sequence starting.
void*             s_touchedSeen[16] = {};
std::atomic<int>  s_touchedN{0};

void NoteTouchingObject(void* object, int mode) {
    int n = s_touchedN.load(std::memory_order_relaxed);
    for (int i = 0; i < n && i < 16; ++i) if (s_touchedSeen[i] == object) return;
    if (n >= 16) return;
    s_touchedSeen[n] = object;
    s_touchedN.store(n + 1, std::memory_order_relaxed);
    char m[224];
    snprintf(m, sizeof(m),
             "touch REPORTED on map %d by a NON-guard object (obj=%p, mode=%d) -- left alone. If a "
             "capture happened around now, this is the candidate to add to the danger table",
             MapNames::CurrentMapId(), object, mode);
    Log::Write("SNEAK", m);
    const std::wstring label = EntityList::LabelForSceneObject(object);
    if (!label.empty()) Log::WriteW("SNEAK", "  object is: ", label);
}

bool __fastcall HookedTouchTest(void* object, int mode) {
    // THE SNAPSHOT IS THE MAP GATE, and it is also the cheapest thing this function could ask. It is
    // only ever populated on a map PathDanger names, so an empty one means "nothing here is a guard"
    // and costs a single acquire load -- after which the override below is unreachable rather than
    // merely skipped, the same shape as the distance clamp. This is the hot one: the script polls it
    // tens of times a frame.
    if (s_guardN.load(std::memory_order_acquire) == 0)
        return s_origTouch ? s_origTouch(object, mode) : false;

    const bool inside = s_origTouch ? s_origTouch(object, mode) : false;
    if (!inside) return false;                       // nothing to hide; the common case

    if (!IsGuardObject(object)) { NoteTouchingObject(object, mode); return inside; }

    // A GUARD'S OWN VOLUME, and the leader is in it. Answer no. Every other trigger on the map --
    // doors, event rects, the servant's conversation triggers -- answered truthfully above.
    if (!s_loggedSuppress.exchange(true, std::memory_order_relaxed)) {
        char m[224];
        snprintf(m, sizeof(m),
                 "touch SUPPRESSED on map %d for a danger actor (obj=%p, mode=%d) -- the guard's own "
                 "volume reported the leader inside and was answered no; first suppression this "
                 "map, later ones are silent (the script polls this every frame)",
                 MapNames::CurrentMapId(), object, mode);
        Log::Write("SNEAK", m);
        const std::wstring label = EntityList::LabelForSceneObject(object);
        if (!label.empty()) Log::WriteW("SNEAK", "  suppressed object is: ", label);
    }
    return false;
}

// ---- THE OTHER CATCH: A TRIGGER VOLUME THAT STARTS A ROUTINE (Session 117) ----------------------
//
// MAP 569 CALLS NONE OF THE TOUCH NATIVES. `rrp_a03` contains ZERO `0x26D` and ZERO `0x525` -- S115
// measured that and this file claimed 569 covered anyway; S116's play test then produced ZERO touch
// lines of either kind on 569, exactly as the census predicted. The three natives that funnel into
// `FUN_002677f0` are simply never dispatched there, so nothing above can suppress anything.
//
// What 569 uses instead is the ENGINE's own trigger update, `FUN_0025c830`: it walks the four party
// actors against each volume and, on the frame the volume goes from empty to occupied, calls
// `FUN_003dbb60(object, 4, routineIdx, 0)` -- START THIS OBJECT'S ROUTINE. That is the capture; the
// twelve `捕獲…` routines in `rrp_a03` are its targets, and the `捕獲レクト兵士01..07` ("capture rect
// soldier") actors are the volumes. No script native is involved at any point.
//
// **THE SUPPRESSION IS KEYED ON THE ROUTINE THE FIRE WOULD START, NOT ON THE OBJECT.** That is the
// only identity available: the volumes are rect actors with no npcdic name, so the entity scan cannot
// see them and `IsGuardObject` can never match them. The routine, by contrast, names itself.
//
// **THE FIRED INDEX IS OBJECT-LOCAL (Session 118).** S117 read it as a routine-table index and the
// play log refuted that in one screen: objects fired CONSECUTIVE SMALL indices (1,2,3 on one object,
// 2,3,4 on the next) whose "names" resolved to `setup` and the map's resident director -- routines no
// trigger volume could be starting -- while the real capture never matched and the player was caught.
// `FUN_003dbcf0` bounds the index against the object's OWN event table at `object+0x48`, whose
// entries are NAME-POOL OFFSETS. `MapScript::FiredRoutineName` walks that chain; the name it returns
// is the map author's own string for the routine that would actually run.
//
// It is also the GLOBAL rule the per-map table never was: "do not start a routine the map's own
// author named a capture" holds on both palace maps (568's single `ヴァン捕獲` and 569's twelve) with
// no map id, offset or index in it. The map table stays as the containment gate, not as the answer.
//
// FAILS OPEN BY CONSTRUCTION. An unreadable blob, an index the table does not hold, or a name that
// does not match all take the same path: call the original. If the index space turns out not to be
// this table's, the mod does nothing different and the log below says so, naming every index it saw.
using EventFireFn = int(__fastcall*)(void*, uint32_t, uint32_t, int, int);
EventFireFn s_origFire = nullptr;

// ONLY A FIRE THAT CAME FROM A TRIGGER VOLUME MAY BE DECLINED, and this makes that a fact rather than
// a guess at the kind byte. `FUN_003dbb60` has ~20 call sites and they are not all volumes --
// conversation starts (`FUN_00269640`/`FUN_00269860`) and script-side event calls
// (`FUN_00269a90`/`FUN_00269ba0`/`FUN_00266530`) go through it too. **A routine the SCRIPT asks for
// must run:** if the map's own setup starts `捕獲監視監督` ("capture watch supervisor"), declining it
// would stall the sequence rather than save it. So the trigger update is bracketed and only fires
// issued inside it are candidates. Game thread on both sides; the flag is thread-local, so nothing
// else can observe or race it.
//
// ---- AND SINCE SESSION 119 THE UPDATE ITSELF IS THE SUPPRESSION POINT. --------------------------
// The S118 play forced the question the fire hook cannot answer: `FUN_0025c830` fires `FUN_003dbb60`
// ONLY for objects whose class byte `+0x18 != 1`. For `+0x18 == 1` (script-created objects -- which
// rects are) every branch either skips the fire or returns before it, and the update's outputs are
// the inside-mask at `node+0x60`, the status bits on `object+0xC`, and the notification registers
// `FUN_003df760` writes (`DAT_02b59c40..ce0`, slots 1/2) for the script to poll. A capture rect of
// that class NEVER passes through the fire hook, and no decline there can reach it.
//
// So the update is skipped outright -- mask never set, bits never set, registers never written, and
// the fire (when the class does fire one) never issued -- for exactly two object identities, both
// MEASURED, on danger-table maps only:
//   1. A GUARD'S OWN OBJECT (`IsGuardObject`, the npcdic snapshot). This is S113's user-approved
//      design -- "silence the guards' own trigger volume, per object" -- moved from the reader
//      (`FUN_002677f0`, which 569's script never calls) to the writer, which every read path shares.
//   2. AN OBJECT WHOSE OWN EVENT TABLE NAMES A `捕獲` ROUTINE (`ObjectNamesCapture` below) -- the
//      map author's own word for the fail branch, the same rule the fire hook applies, evaluated
//      over the identity a rect actually carries.
// Everything else runs the original unchanged. Fail-open: an unreadable table or names mean NO skip.
using TriggerUpdateFn = void(__fastcall*)(void*, void*);
TriggerUpdateFn s_origTrigger = nullptr;
thread_local bool t_inTriggerUpdate = false;

// `捕獲` ("capture") in Shift-JIS, the encoding the name pool stores. This is the GAME's word for its
// own fail branch, read out of the game's own script -- not a mod-authored label, and never spoken.
constexpr char kCaptureMarker[] = "\x95\xDF\x8A\x6C";
constexpr size_t kCaptureMarkerLen = 4;

// The engine's own "no event slot was free" answer (`FUN_003dbcf0` returns it when the object's
// current-event slot is -1). Returned instead of firing, because it is a value the callers already
// handle: `FUN_0025c830` continues only on 1, so 2 makes its ENTER branch return before the scene
// takes over, and its LEAVE branch runs the cleanup it runs whenever an event could not start.
constexpr int kFireDeclined = 2;

// NO VERDICT CACHE ANY MORE (Session 118). S117 cached "is routine N a capture?" keyed on the bare
// index -- which the corrected model makes an identity error: the index is object-LOCAL, so the same
// number names different routines on different objects, and a cached verdict for one object would be
// replayed for another. Fires are event-driven (a crossing, not a frame), so resolving the name per
// fire is a handful of guarded reads and costs nothing that matters.

// Log latch: fires are event-driven, not per-frame, but a volume the player paces in and out of
// would still repeat. One line per (object, kind, index) triple per map -- THE OBJECT IS PART OF THE
// KEY (Session 118): object-local indices collide across objects by design, and S117's object-less
// key deduplicated DIFFERENT objects' fires into one line. That is precisely how the capture rect's
// own fire went unlogged on the play test that refuted S117: an earlier object had already used its
// (kind, index) pair. 96 slots, not 32 -- map 569 carries seventy rects, and on the two danger maps
// a fuller file log is the point.
struct FireSeen { void* obj; uint32_t key; };
constexpr int kMaxFiresSeen = 96;
FireSeen         s_firesSeen[kMaxFiresSeen] = {};
std::atomic<int> s_firesSeenN{0};

// The spam tier's own budget (script/other fires -- map boot REQs, conversations). Separate from the
// trigger-volume dedup so load-time volume can never starve the lines that decide anything.
constexpr int    kScriptFireLogBudget = 16;
std::atomic<int> s_scriptFiresLogged{0};

bool NoteFireOnce(void* object, uint32_t kind, uint32_t routineIdx) {
    const uint32_t key = (kind << 24) | (routineIdx & 0xFFFFFFu);
    const int n = s_firesSeenN.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < kMaxFiresSeen; ++i)
        if (s_firesSeen[i].obj == object && s_firesSeen[i].key == key) return false;
    if (n >= kMaxFiresSeen) return false;
    s_firesSeen[n] = { object, key };
    s_firesSeenN.store(n + 1, std::memory_order_release);   // publish after the record is written
    return true;
}

// Routine names are Shift-JIS; the log is ASCII. Printable bytes pass through, everything else
// becomes its hex value, so the LOG CARRIES THE BYTES -- a name that did not match can be checked
// against the script offline instead of being an unreadable row of question marks.
std::string NameForLog(const std::string& raw) {
    std::string o;
    char hex[5];
    for (unsigned char c : raw) {
        if (c >= 0x20 && c < 0x7f) { o.push_back(static_cast<char>(c)); continue; }
        snprintf(hex, sizeof(hex), "\\x%02X", c);
        o += hex;
        if (o.size() > 96) break;
    }
    return o;
}

// True when the routine THIS fire would start is one the map's own script names a capture.
//
// `name` is filled whenever the chain could be read, matched or not -- an unmatched fire has to stay
// evidence rather than become a silent pass, because "the resolution chain is wrong" and "this
// volume is not a capture" would otherwise print identically. That distinction is the whole
// falsifier, and it is exactly how S117's wrong index model was caught in one play session.
// Unreadable => false: fail open, always.
bool IsCaptureRoutine(void* object, uint32_t idx, std::string& name) {
    if (!MapScript::FiredRoutineName(object, idx, name)) return false;
    return name.find(kCaptureMarker, 0, kCaptureMarkerLen) != std::string::npos;
}

// ---- The per-object trigger census (Session 119, danger maps only, file-only) -------------------
//
// TWO PLAYS FAILED FOR TWO DIFFERENT LOGGING REASONS and the capture's mechanism has still never
// been OBSERVED -- S118's object-less dedup hid it, then S118's fix spent all 96 log slots on the
// load-time `init`/`main` spam before the player took a step. This census cannot miss: ONE line per
// object the trigger update touches, the first time it touches it, carrying every identity surface
// the object has -- class byte, flag bytes, event-table names, position, distance to the nearest
// guard, and the skip verdict. Whatever catches the player next is IN this table, and turning it
// into a rule is one read.
struct TriggerObj {
    void* obj          = nullptr;
    bool  namesCapture = false;   // its event table names a `捕獲` routine (cached verdict)
    bool  skipLogged   = false;   // the skip line for this object has been written
};
constexpr int kMaxTriggerObjs = 160;
TriggerObj s_trigObjs[kMaxTriggerObjs] = {};
int        s_trigObjN = 0;                 // game thread only -- no atomics needed
bool       s_trigObjOverflow = false;

// Does this object's OWN event table name a capture routine? Walks every entry through the same
// chain the fire hook resolves one entry with. Unreadable table or names => false (fail open).
bool ScanObjectForCaptureNames(void* object, std::string* firstNames, int* outCount) {
    bool hit = false;
    void* tbl = MemRead::PtrAt(object, 0x48);
    uint32_t count = 0;
    if (tbl) MemRead::SafeReadU32(tbl, 0, &count);
    if (count > 64) count = 0;             // torn read; treat as no table
    if (outCount) *outCount = static_cast<int>(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::string nm;
        if (!MapScript::FiredRoutineName(object, i, nm)) continue;
        if (nm.find(kCaptureMarker, 0, kCaptureMarkerLen) != std::string::npos) hit = true;
        if (firstNames && i < 6) {
            if (!firstNames->empty()) *firstNames += "|";
            *firstNames += NameForLog(nm);
        }
    }
    return hit;
}

// First sight of an object: census-log it and cache the capture-name verdict. Returns its record.
TriggerObj* CensusOnce(void* object) {
    for (int i = 0; i < s_trigObjN; ++i)
        if (s_trigObjs[i].obj == object) return &s_trigObjs[i];
    if (s_trigObjN >= kMaxTriggerObjs) {
        if (!s_trigObjOverflow) {
            s_trigObjOverflow = true;
            Log::Write("SNEAK", "trigger census FULL (160 objects) -- later objects are NOT counted; "
                                "if the capture is still unexplained, raise kMaxTriggerObjs");
        }
        return nullptr;
    }
    TriggerObj* rec = &s_trigObjs[s_trigObjN++];
    rec->obj = object;

    std::string names;
    int evtCount = 0;
    rec->namesCapture = ScanObjectForCaptureNames(object, &names, &evtCount);

    uint8_t cls = 0, f8 = 0, fB = 0, fC = 0;
    MemRead::SafeReadU8(object, 0x18, &cls);
    MemRead::SafeReadU8(object, 0x08, &f8);
    MemRead::SafeReadU8(object, 0x0B, &fB);
    MemRead::SafeReadU8(object, 0x0C, &fC);

    FVec3 pos{};
    const bool posOk = PlayerState::ReadSceneObjectPos(object, pos);
    float guardDist = -1.0f;
    if (posOk) {
        const int n = s_guardN.load(std::memory_order_acquire);
        for (int i = 0; i < n && i < kMaxGuards; ++i) {
            FVec3 gp{};
            if (!PlayerState::ReadSceneObjectPos(s_guards[i], gp)) continue;
            const float d = NavCommon::Distance2D(pos, gp);
            if (guardDist < 0.0f || d < guardDist) guardDist = d;
        }
    }

    char m[400];
    snprintf(m, sizeof(m),
             "trigger census obj=%p class=0x%02X f8=0x%02X fB=0x%02X fC=0x%02X pos=(%.1f,%.1f,%.1f) "
             "nearestGuard=%.1fm evt=%d names:%s%s",
             object, cls, f8, fB, fC,
             posOk ? pos.x : 0.0f, posOk ? pos.y : 0.0f, posOk ? pos.z : 0.0f,
             guardDist, evtCount, names.empty() ? "(none)" : names.c_str(),
             rec->namesCapture ? "  <== NAMES A CAPTURE ROUTINE" : "");
    Log::Write("SNEAK", m);
    return rec;
}

void __fastcall HookedTriggerUpdate(void* container, void* object) {
    // Off the danger table this is the original plus one two-row scan -- no census, no skip, and the
    // bracket still marks the extent so the fire hook's `src=` stays truthful everywhere.
    if (!CoveredMap()) {
        const bool prev = t_inTriggerUpdate;
        t_inTriggerUpdate = true;
        if (s_origTrigger) s_origTrigger(container, object);
        t_inTriggerUpdate = prev;
        return;
    }

    TriggerObj* rec = CensusOnce(object);

    // THE SKIP. A guard's own volume, or a volume whose event table names a capture routine, is
    // inert: mask never written, bits never set, notification registers never posted, fires never
    // issued. Both identities are measured; everything else runs the original unchanged.
    const bool guard = IsGuardObject(object);
    if (guard || (rec && rec->namesCapture)) {
        if (rec && !rec->skipLogged) {
            rec->skipLogged = true;
            char m[224];
            snprintf(m, sizeof(m),
                     "trigger update SKIPPED for obj=%p on map %d -- %s; its volume reports nobody, "
                     "on every read path at once",
                     object, MapNames::CurrentMapId(),
                     guard ? "a guard's own object (npcdic snapshot)"
                           : "its event table names a capture routine");
            Log::Write("SNEAK", m);
            if (guard) {
                const std::wstring label = EntityList::LabelForSceneObject(object);
                if (!label.empty()) Log::WriteW("SNEAK", "  skipped object is: ", label);
            }
        }
        return;
    }

    const bool prev = t_inTriggerUpdate;
    t_inTriggerUpdate = true;
    if (s_origTrigger) s_origTrigger(container, object);
    t_inTriggerUpdate = prev;
}

int __fastcall HookedEventFire(void* object, uint32_t kind, uint32_t routineIdx, int mode, int flag) {
    // OFF-TABLE IS THE FIRST BRANCH, exactly as the two hooks above: on every map but the danger
    // table's this is the original plus one table scan, and nothing below is reachable. Trigger fires
    // happen on every map in the game -- doors, chests, conversations -- so this early-out is what
    // keeps the hook from having a blast radius at all.
    if (!CoveredMap())
        return s_origFire ? s_origFire(object, kind, routineIdx, mode, flag) : kFireDeclined;

    std::string name;
    const bool fromVolume = t_inTriggerUpdate;
    const bool capture    = IsCaptureRoutine(object, routineIdx, name);
    const bool suppress   = capture && fromVolume;

    // THE LOG BUDGET IS TIERED BY WHAT THE LINE CAN PROVE (Session 119). The S118 play burned all 96
    // shared slots on load-time `init`/`main` fires inside 18 seconds, so the one line that mattered
    // could never print -- the cap deleted the evidence exactly as the object-less key had the play
    // before. Now: a CAPTURE-named fire always logs, budget be damned (there are twelve on the worst
    // map); a trigger-volume fire gets the per-object dedup and the big budget (these are the
    // falsifier); script/other fires -- the spam tier, and the tier that is never declined -- get a
    // small count so the file still shows the map booting without drowning the rest.
    bool logIt;
    if (capture) {
        logIt = true;
    } else if (fromVolume) {
        logIt = NoteFireOnce(object, kind, routineIdx);
    } else {
        logIt = (s_scriptFiresLogged < kScriptFireLogBudget) && ++s_scriptFiresLogged;
    }
    if (logIt) {
        char m[320];
        snprintf(m, sizeof(m),
                 "event fire on map %d: obj=%p kind=%u routine=%u src=%s name=\"%s\" -- %s",
                 MapNames::CurrentMapId(), object, kind, routineIdx,
                 fromVolume ? "trigger-volume" : "script/other",
                 name.empty() ? "<unreadable>" : NameForLog(name).c_str(),
                 suppress ? "CAPTURE, SUPPRESSED (the routine the map's own author named a capture)"
                 : capture ? "capture routine, but NOT from a trigger volume -- passed through"
                           : "passed through");
        Log::Write("SNEAK", m);
    }

    if (suppress) return kFireDeclined;
    return s_origFire ? s_origFire(object, kind, routineIdx, mode, flag) : kFireDeclined;
}

} // namespace

void OnFieldFrame() {
    // GAME THREAD, once per field tick. Off a danger map -- every map but two -- this is a table
    // lookup and a store of 0, after which the touch hook can never match anything.
    //
    // Refreshed EVERY tick, not once: the guards move (that is the whole minigame), so a snapshot
    // taken at map load would go stale. Pointers, not positions, so a moving actor does not
    // invalidate it -- only despawning does.
    // Fill the census latch only when it is EMPTY -- see the note on s_censusMapId. Ticks of the map
    // being entered must not overwrite the id of the map whose census has not been printed yet.
    if (s_censusMapId.load(std::memory_order_relaxed) == kNoCensusMap)
        s_censusMapId.store(MapNames::CurrentMapId(), std::memory_order_relaxed);
    const int16_t nameIdx = PathDanger::DangerNameIdx(static_cast<uint32_t>(MapNames::CurrentMapId()));
    if (nameIdx < 0) { s_guardN.store(0, std::memory_order_release); return; }
    void* found[kMaxGuards] = {};
    const int n = EntityList::CollectSceneObjectsByNameIdx(nameIdx, found, kMaxGuards);
    for (int i = 0; i < n && i < kMaxGuards; ++i) s_guards[i] = found[i];
    s_guardN.store(n, std::memory_order_release);   // publish AFTER the pointers are written
}

bool Init() {
    const bool okTouch = Hooks::InstallTyped(NavRva::TOUCH_TEST, &HookedTouchTest, &s_origTouch);
    Log::Write("SNEAK", okTouch ? "touch-test hook installed (the guards' own trigger volume)"
                                : "touch-test hook FAILED to install -- guards will still notice you");
    // The trigger-update hook goes in FIRST: it is the scope marker for the fire hook AND, since
    // S119, the suppression point itself -- a guard's or capture-named object's volume is skipped
    // outright, which silences every downstream signal at once (mask, bits, notification registers,
    // fires), including the paths class +0x18==1 objects take that never reach the fire hook.
    const bool okTrig = Hooks::InstallTyped(NavRva::TRIGGER_UPDATE, &HookedTriggerUpdate,
                                            &s_origTrigger);
    Log::Write("SNEAK", okTrig
        ? "trigger-update hook installed (guard/capture volumes are skipped at the writer; also the "
          "fire hook's scope marker, and the per-object census on danger maps)"
        : "trigger-update hook FAILED to install -- rect volumes cannot be silenced");
    const bool okFire = Hooks::InstallTyped(NavRva::EVENT_FIRE, &HookedEventFire, &s_origFire);
    Log::Write("SNEAK", okFire
        ? "event-fire hook installed (a trigger volume starting a routine -- map 569's catch, which "
          "calls no script native at all)"
        : "event-fire hook FAILED to install -- rect-driven captures will still happen");
    const bool ok = Hooks::InstallTyped(NavRva::SCRIPT_DISTANCE, &HookedScriptDistance, &s_orig);
    Log::Write("SNEAK", ok ? "script-distance hook installed (sneak assist acts on the danger-table "
                             "maps only; nothing to switch on)"
                           : "script-distance hook FAILED to install -- sneak assist unavailable");
    return ok;
}

void OnMapTeardown() {
    // The census verdict for the map being left (S110, log-only). A ZERO here on a covered map
    // whose sequence the player actually played is the falsifier: it means the script never calls
    // this native, so no clamp of it could ever have worked and the capture is driven by something
    // else. Printed for EVERY map, because "which maps use it at all" is the same question.
    //
    // THE MAP ID IS THE LATCHED ONE, NOT THE LIVE ONE (Session 115). By the time this runs the
    // engine has already moved `CurrentMapId` on, so the old line credited every count to the map
    // being ENTERED -- 568's two calls were logged as `map 569 census`.
    {
        const uint32_t n = s_callsThisMap.exchange(0, std::memory_order_relaxed);
        s_loggedFirstCall.store(false, std::memory_order_relaxed);
        // Consume the latch: printing it is what frees it for the next map to claim.
        const int mapId = s_censusMapId.exchange(kNoCensusMap, std::memory_order_relaxed);
        char m[176];
        snprintf(m, sizeof(m), "map %d census: the distance native was called %u time(s) while it "
                               "was loaded (the map being LEFT, not the one being entered)",
                 mapId, n);
        Log::Write("SNEAK", m);
    }
    // The snapshot and the log latches belong to the map being left. Cleared unconditionally, so a
    // stale scene-object pointer can never be matched against an object on the next map, and the
    // next covered map gets its own first-clamp and first-suppression evidence.
    s_guardN.store(0, std::memory_order_release);
    s_touchedN.store(0, std::memory_order_relaxed);
    s_loggedSuppress.store(false, std::memory_order_relaxed);
    s_loggedThisMap.store(false, std::memory_order_relaxed);

    // The fires-seen latch, the spam budget and the trigger census all hold scene-object pointers or
    // verdicts that die with the map -- cleared for the same reason the guard snapshot is: an
    // identity is only valid inside the map it was read on. (The S117 per-index verdict cache is
    // GONE, not merely cleared -- the index is object-local, so a cached verdict keyed on the bare
    // index was wrong within a single map, not just across two.)
    s_firesSeenN.store(0, std::memory_order_relaxed);
    s_scriptFiresLogged.store(0, std::memory_order_relaxed);
    for (int i = 0; i < s_trigObjN; ++i) s_trigObjs[i] = TriggerObj{};
    s_trigObjN = 0;
    s_trigObjOverflow = false;
}

} // namespace SneakAssist
