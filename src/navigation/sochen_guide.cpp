#include "navigation/sochen_guide.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/hooks.h"
#include "core/logger.h"
#include "navigation/entity_list.h"
#include "navigation/map_names.h"
#include "navigation/map_script.h"
#include "navigation/map_script_routines.h"
#include "navigation/shout_script.h"
#include "speech/phrasebook.h"
#include "speech/speech.h"

namespace SochenGuide {
namespace {

using Phrase::Id;

constexpr char kPalacePrefix[] = "rui_";
constexpr int  kMaxModules     = 5;

// The save block (S180): FUN_002ef2b0's return. Bit 0x02 = waterfall solved, 0x01 = door puzzle solved.
constexpr uint32_t RVA_SAVE_BLOCK = 0x2044480;
constexpr uint32_t kFlagOff       = 0x918;
constexpr uint8_t  kClockSolved   = 0x01;
constexpr uint8_t  kWaterSolved   = 0x02;

constexpr int kFallsOfTime = 184;
constexpr int kMirror      = 185;
constexpr int kDestiny     = 192;

// ---- the waterfall puzzle's four legs -----------------------------------------------------------
//
// `outMap`/`outEntrance` identify the exit to take OUT of Falls of Time by its own `mapjump` literal,
// and `backEntrance` identifies the exit on that map that arrives BACK at Falls of Time by the
// entrance the director is waiting for. Both are matched against the live script's own exit table, so
// nothing here depends on authoring order or on a label.
//
// Storage class 5 is the cross-script work area every module shares, which is why this state survives
// the map changes the puzzle is made of.
struct Leg {
    int      outMap;
    int      outEntrance;
    int      backEntrance;
    uint32_t leftViaOff;   // class 5: this leg's exit has been taken
    uint32_t stageOff;     // class 5: this leg has been completed
};
constexpr Leg kLegs[4] = {
    { kMirror,  5, 3, 0x80, 0x84 },
    { kDestiny, 1, 5, 0x81, 0x85 },
    { kMirror,  4, 7, 0x82, 0x86 },
    { kDestiny, 2, 9, 0x83, 0x87 },
};
constexpr int kLegCount = 4;

// ---- the door puzzle's eight doors, in order ----------------------------------------------------
//
// Named by SCRIPT ROUTINE, never by label: the two sides of one door are two routines (`…b` is the
// far side) and the game gives both the same name, so a label could not tell them apart. `flagOff` is
// the class-1 cell that door raises when opened; the first door raises one nobody tests, so it is
// listed as 0 and never read. A step counts only while every flag AFTER it is still clear.
struct ClockStep {
    const char* routine;
    uint32_t    flagOff;
};
constexpr ClockStep kClock[8] = {
    { "gim_door08b", 0x00 },   // script id 23 -- restarts the count
    { "gim_door15",  0x2F },   // 15
    { "gim_door14b", 0x38 },   // 26
    { "gim_door12",  0x2C },   // 12
    { "gim_door09b", 0x36 },   // 24
    { "gim_door02",  0x22 },   // 2
    { "gim_door03b", 0x33 },   // 21
    { "gim_door05",  0x25 },   // 5  -- completes the puzzle at count 7
};
constexpr int      kClockCount    = 8;
constexpr uint32_t kClockCounter  = 0x3C;   // class 1: steps counted so far
constexpr uint8_t  kCellElemType  = 1;      // every cell above is an s8

std::atomic<bool> s_request{false};

// ---- reading the state --------------------------------------------------------------------------

struct State {
    bool haveModule  = false;
    ShoutScript::RawModule mod;
    int  mapId       = -1;
    bool waterSolved = false;
    bool clockSolved = false;
    bool haveFlags   = false;   // the save byte read
    int  stage       = 0;       // completed waterfall legs, 0..4
    bool leftVia[kLegCount] = {};
    bool haveWater   = false;   // the class-5 cells read
    bool haveClock   = false;   // the class-1 cells read (Destiny's March only)
    int  clockDone   = 0;       // steps counted so far, 0..7
    bool clockFlag[kClockCount] = {};
};

bool ReadCell(void* base, uint32_t off, bool* out) {
    int32_t v = 0;
    if (!base || !ShoutScript::ReadVar(static_cast<char*>(base) + off, kCellElemType, &v)) return false;
    *out = (v != 0);
    return true;
}

State Read() {
    State s;
    ShoutScript::RawModule mods[kMaxModules];
    const int n = ShoutScript::FindModulesBySrcPrefix(kPalacePrefix, mods, kMaxModules);
    if (n <= 0) return s;
    s.haveModule = true;
    s.mod   = mods[0];
    s.mapId = MapNames::CurrentMapId();

    void* block = Hooks::ResolveRva(RVA_SAVE_BLOCK);
    void* base0 = ShoutScript::ClassBaseRaw(s.mod.record, s.mod.ebpBase, 0);
    // The same agreement S180's writer requires before it touches anything: the module's own class-0
    // base must be the save block. A mismatch means the layout moved, and a guide reading on would
    // report confident numbers off an unrelated byte.
    if (block && base0 == block) {
        int32_t v = 0;
        if (ShoutScript::ReadVar(static_cast<char*>(block) + kFlagOff, 0, &v)) {
            s.haveFlags   = true;
            s.waterSolved = (v & kWaterSolved) != 0;
            s.clockSolved = (v & kClockSolved) != 0;
        }
    }

    void* base5 = ShoutScript::ClassBaseRaw(s.mod.record, s.mod.ebpBase, 5);
    if (base5) {
        s.haveWater = true;
        for (int i = 0; i < kLegCount; ++i) {
            bool done = false;
            if (!ReadCell(base5, kLegs[i].leftViaOff, &s.leftVia[i]) ||
                !ReadCell(base5, kLegs[i].stageOff, &done)) {
                s.haveWater = false;
                break;
            }
            if (done) s.stage = i + 1;   // the legs complete in order; the highest one set is the stage
        }
    }

    // Class 1 is PER MODULE, so the door puzzle's counters exist only while its own map script is the
    // live one. Off that map the guide reports the puzzle's solved bit and nothing else.
    if (s.mapId == kDestiny) {
        void* base1 = ShoutScript::ClassBaseRaw(s.mod.record, s.mod.ebpBase, 1);
        int32_t c = 0;
        if (base1 && ShoutScript::ReadVar(static_cast<char*>(base1) + kClockCounter, kCellElemType, &c)) {
            s.haveClock = true;
            s.clockDone = (c < 0) ? 0 : (c > kClockCount - 1 ? kClockCount - 1 : c);
            for (int i = 0; i < kClockCount; ++i)
                if (kClock[i].flagOff) ReadCell(base1, kClock[i].flagOff, &s.clockFlag[i]);
        }
    }
    return s;
}

// ---- finding the entity a step names -------------------------------------------------------------

// A set rather than one group, so the "any way back" case below can let FocusWhere pick the NEAREST
// of several doors instead of whichever the script table happens to list first.
constexpr int kMaxGroups = 16;
struct SeamCtx { int groups[kMaxGroups]; int n; };
bool TestSeam(void* /*sceneObj*/, int seamGroup, void* ctx) {
    if (seamGroup == 0) return false;
    const SeamCtx* c = static_cast<SeamCtx*>(ctx);
    for (int i = 0; i < c->n; ++i) if (c->groups[i] == seamGroup) return true;
    return false;
}

struct RoutineCtx {
    const std::vector<MapScript::RoutineFacts>* facts;
    int index;
};
bool TestRoutine(void* sceneObj, int /*seamGroup*/, void* ctx) {
    RoutineCtx* rc = static_cast<RoutineCtx*>(ctx);
    if (!sceneObj || !rc->facts || rc->index < 0) return false;
    return MapScript::RoutineIndexOfObject(*rc->facts, sceneObj) == rc->index;
}

// Focus the exit whose own `mapjump` goes to `destMap` entrance `entrance`, and return its label.
// `entrance` < 0 matches any exit to that map -- the "just get back to Falls of Time" case, where the
// arrival the director wants has not been earned yet and any door will do.
bool FocusExit(int destMap, int entrance, std::wstring* outLabel, const char** why) {
    std::vector<MapScript::ExitDest> dests;
    if (!MapScript::ReadExitDests(dests, /*logDetail=*/false)) { *why = "no script exit table"; return false; }
    SeamCtx ctx{};
    for (const MapScript::ExitDest& d : dests) {
        if (d.destMapId != destMap) continue;
        if (entrance >= 0 && d.entrance != entrance) continue;
        if (d.group <= 0 || ctx.n >= kMaxGroups) continue;
        ctx.groups[ctx.n++] = d.group;
    }
    if (ctx.n == 0) { *why = "no exit with that destination in the script table"; return false; }
    if (EntityList::FocusWhere(&TestSeam, &ctx, outLabel)) return true;
    *why = "the exit's map-jump group is not in the list";
    return false;
}

// THE SIDE-TRACK CASE. The player left the puzzle's rooms — a chest, a fight, a look around —
// and pressed the key to get back on it. Focus the nearest exit that leads to any of `maps`, tried in
// the caller's order of preference, so the repeat key answers with a way back rather than a bare step
// number. Nothing routes across maps, so this points at the door out of the room they are in.
bool FocusTowardMaps(const int* maps, int n, std::wstring* outLabel, const char** why) {
    for (int i = 0; i < n; ++i)
        if (FocusExit(maps[i], -1, outLabel, why)) return true;
    return false;
}

bool FocusDoor(const char* routineName, std::wstring* outLabel, const char** why) {
    std::vector<MapScript::RoutineFacts> facts;
    if (!MapScript::ReadRoutineFacts(facts)) { *why = "no routine table"; return false; }
    int idx = -1;
    for (const MapScript::RoutineFacts& f : facts)
        if (f.name == routineName) { idx = f.index; break; }
    if (idx < 0) { *why = "routine not found on this map"; return false; }
    RoutineCtx ctx{ &facts, idx };
    if (EntityList::FocusWhere(&TestRoutine, &ctx, outLabel)) return true;
    *why = "no listed object runs that routine";
    return false;
}

// ---- speech --------------------------------------------------------------------------------------

std::wstring StepLine(Id puzzle, int step, int of, const std::wstring& label) {
    std::wstring t = Phrase::Get(puzzle);
    t += L", ";
    t += Phrase::Get(Id::SochenStep);
    t += L' ';
    t += std::to_wstring(step);
    t += Phrase::Get(Id::OfJoiner);
    t += std::to_wstring(of);
    t += L'.';
    if (!label.empty()) { t += L' '; t += label; t += L'.'; }
    return t;
}

std::wstring SolvedLine(Id puzzle) {
    return std::wstring(Phrase::Get(puzzle)) + L", " + Phrase::Get(Id::StatueSolved) + L".";
}

// A puzzle and a place, with no step number — the answer when the counters that would number the step
// are not readable from where the player is standing (the door puzzle keeps its count in its own map
// script's storage). Naming the way back is still the useful half.
std::wstring TargetLine(Id puzzle, const std::wstring& label) {
    std::wstring t = Phrase::Get(puzzle);
    t += L'.';
    if (!label.empty()) { t += L' '; t += label; t += L'.'; }
    return t;
}

// ---- the key --------------------------------------------------------------------------------------

// The WATERFALL step, from wherever the player is standing. Only called while the puzzle is unsolved:
// `Answer` owns what is said once it is done.
bool SpeakWaterfall(const State& s) {
    if (!s.haveWater) return false;

    const int legIdx = s.stage;                      // 0-based index of the leg being worked on
    if (legIdx >= kLegCount) return false;           // all four legs done but the bit is not set yet
    const Leg& leg = kLegs[legIdx];
    const bool midLeg = s.leftVia[legIdx];

    std::wstring label;
    const char* why = nullptr;
    if (s.mapId == kFallsOfTime) {
        FocusExit(leg.outMap, leg.outEntrance, &label, &why);
    } else if (midLeg && s.mapId == leg.outMap) {
        FocusExit(kFallsOfTime, leg.backEntrance, &label, &why);
    } else if (s.mapId == kMirror || s.mapId == kDestiny) {
        // Not mid-leg: the sequence starts from Falls of Time, so any way back is the right way.
        FocusExit(kFallsOfTime, -1, &label, &why);
    } else {
        // Somewhere else in the palace. Head for Falls of Time if this room reaches it, else for
        // either of the other two, which do.
        const int toward[3] = { kFallsOfTime, kMirror, kDestiny };
        FocusTowardMaps(toward, 3, &label, &why);
    }

    char m[224];
    snprintf(m, sizeof(m), "waterfall: map %d stage %d leg %d midLeg %d -> target %s (%s)",
             s.mapId, s.stage, legIdx + 1, midLeg ? 1 : 0,
             label.empty() ? "NONE" : "found", why ? why : "-");
    Log::Write("SOCHEN", m);

    Speech::Output(StepLine(Id::SochenWaterfall, legIdx + 1, kLegCount, label), true);
    return true;
}

// The DOOR step. Only answerable on Destiny's March, where its counters live.
bool SpeakClock(const State& s) {
    if (!s.haveClock) return false;

    const int next = s.clockDone + 1;                // 1-based step, 1..8
    if (next < 1 || next > kClockCount) return false;

    // STRAYED: a door LATER in the order has been opened, so nothing from here on can count until the
    // map reloads. The game's own inscription says the same thing; the mod is not inventing a rule.
    bool strayed = false;
    for (int i = next; i < kClockCount; ++i) if (s.clockFlag[i]) strayed = true;

    std::wstring label;
    const char* why = nullptr;
    if (!strayed) FocusDoor(kClock[next - 1].routine, &label, &why);

    char m[224];
    snprintf(m, sizeof(m), "doors: map %d counted %d next %d (%s) strayed %d -> target %s (%s)",
             s.mapId, s.clockDone, next, kClock[next - 1].routine, strayed ? 1 : 0,
             label.empty() ? "NONE" : "found", why ? why : "-");
    Log::Write("SOCHEN", m);

    if (strayed) {
        Speech::Output(std::wstring(Phrase::Get(Id::SochenDoorPuzzle)) + L". " +
                       Phrase::Get(Id::SochenOutOfTurn), true);
        return true;
    }
    Speech::Output(StepLine(Id::SochenDoorPuzzle, next, kClockCount, label), true);
    return true;
}

// The door puzzle from ANYWHERE ELSE in the palace: its count lives in its own map script's storage,
// so there is no step number to give — but the way back to it is still worth naming, which is the
// whole point of a repeat key after a side track.
bool SpeakClockRemote(const State& s) {
    std::wstring label;
    const char* why = nullptr;
    const int toward[1] = { kDestiny };
    FocusTowardMaps(toward, 1, &label, &why);

    char m[192];
    snprintf(m, sizeof(m), "doors: map %d has no counters here -> way back %s (%s)",
             s.mapId, label.empty() ? "NONE" : "found", why ? why : "-");
    Log::Write("SOCHEN", m);

    Speech::Output(TargetLine(Id::SochenDoorPuzzle, label), true);
    return true;
}

void Answer() {
    const State s = Read();
    if (!s.haveModule) {
        // Not in the palace: `B` belongs to the shout meter and the statue guide elsewhere.
        Log::Write("SOCHEN", "B: no Sochen script live -- silent no-op");
        return;
    }
    if (!s.haveFlags) {
        Log::Write("SOCHEN", "B: the save block did not read -- staying silent");
        return;
    }

    // WHICH PUZZLE. The door puzzle belongs to one room, so it answers there -- unless the waterfall
    // sequence is in flight, which means the player is mid-run through all three rooms and the next
    // move is theirs to finish. Everywhere else in the palace the waterfall puzzle is the only one.
    bool inFlight = false;
    for (int i = 0; i < kLegCount; ++i) if (s.leftVia[i]) inFlight = true;
    const bool clockFirst = (s.mapId == kDestiny) && !s.clockSolved && !inFlight;

    if (clockFirst && SpeakClock(s)) return;
    if (!s.waterSolved && SpeakWaterfall(s)) return;
    if (!s.clockSolved) {
        if (s.mapId == kDestiny && SpeakClock(s)) return;
        // Only once the waterfall one is actually DONE. Reaching here with it unsolved means its cells
        // did not read, and naming the other puzzle would be a confident answer to a question that
        // failed -- the log below says so instead.
        if (s.waterSolved && SpeakClockRemote(s)) return;
    }
    if (s.waterSolved && s.clockSolved) {
        Speech::Output(SolvedLine(Id::SochenWaterfall) + L" " + SolvedLine(Id::SochenDoorPuzzle), true);
        return;
    }

    // Nothing could be read. Silence is the honest answer, and the log says which half failed.
    char m[192];
    snprintf(m, sizeof(m), "B: nothing answerable -- map %d water(solved %d, read %d) clock(solved %d, read %d)",
             s.mapId, s.waterSolved ? 1 : 0, s.haveWater ? 1 : 0,
             s.clockSolved ? 1 : 0, s.haveClock ? 1 : 0);
    Log::Write("SOCHEN", m);
}

} // namespace

void OnFieldFrame() {
    if (!s_request.exchange(false, std::memory_order_acq_rel)) return;
    Answer();
}

void OnMapTeardown() {
    // Drop a request raised against the map that is going away: its answer would name an exit that no
    // longer exists.
    s_request.store(false, std::memory_order_relaxed);
}

void RequestCheck() { s_request.store(true, std::memory_order_release); }

} // namespace SochenGuide
