#include "navigation/sneak_assist.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>

#include "core/hooks.h"
#include "core/logger.h"
#include "navigation/entity_list.h"
#include "navigation/map_names.h"
#include "navigation/nav_rva.h"
#include "navigation/path_danger.h"

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
// THE MAP THE COUNTER BELONGS TO, latched on the field tick (Session 115). `OnMapTeardown` used to
// print `MapNames::CurrentMapId()`, which by then has ALREADY ADVANCED -- so the log read
// `map 569 census: ... 2 time(s)` for a count that was 568's, and every census line in the file
// named the wrong map by one. Latched here, printed from here.
std::atomic<int> s_censusMapId{-1};
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

} // namespace

void OnFieldFrame() {
    // GAME THREAD, once per field tick. Off a danger map -- every map but two -- this is a table
    // lookup and a store of 0, after which the touch hook can never match anything.
    //
    // Refreshed EVERY tick, not once: the guards move (that is the whole minigame), so a snapshot
    // taken at map load would go stale. Pointers, not positions, so a moving actor does not
    // invalidate it -- only despawning does.
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
        char m[176];
        snprintf(m, sizeof(m), "map %d census: the distance native was called %u time(s) while it "
                               "was loaded (the map being LEFT, not the one being entered)",
                 s_censusMapId.load(std::memory_order_relaxed), n);
        Log::Write("SNEAK", m);
    }
    // The snapshot and the log latches belong to the map being left. Cleared unconditionally, so a
    // stale scene-object pointer can never be matched against an object on the next map, and the
    // next covered map gets its own first-clamp and first-suppression evidence.
    s_guardN.store(0, std::memory_order_release);
    s_touchedN.store(0, std::memory_order_relaxed);
    s_loggedSuppress.store(false, std::memory_order_relaxed);
    s_loggedThisMap.store(false, std::memory_order_relaxed);
}

} // namespace SneakAssist
