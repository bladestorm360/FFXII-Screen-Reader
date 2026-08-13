#include "navigation/statue_diag.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "navigation/entity_list.h"
#include "navigation/map_names.h"
#include "navigation/map_script.h"
#include "navigation/nav_rva.h"
#include "navigation/shout_script.h"
#include "core/logger.h"
#include "core/mem_read.h"

namespace StatueDiag {
namespace {

// The authoring prefix shared by every Stilshrine of Miriam room script. Measured, not guessed:
// `ebp_statue_census.py` binds `mrm_b04` -> map 600, `mrm_b03` -> 599 and `mrm_b02` -> 598 by exact
// match against the routine-name pools our own play log printed, and the only four scripts in the
// game that mention a statue at all are `mrm_b02/b03/b04/c01`.
constexpr char kDungeonPrefix[] = "mrm_";

// A script's variable table is small (the shout census works in indices under 0x40), and the
// descriptor API takes a uint8_t index, so 256 is the hard ceiling the format itself imposes.
constexpr uint32_t kMaxVars = 256;

// Log volume. These are WORK budgets, not clocks (L-24) -- they bound how much a single map visit
// may write, and they are counts on purpose.
// ONE BUDGET PER NET, NEVER A SHARED ONE. The first run of this capture shared a single 400-line
// budget between the variable diff and the object diff; wandering ENEMIES burned all 400 on yaw
// jitter and the budget closed 22 seconds BEFORE the player rotated the statue. The whole point of
// the visit was lost, and the log looked like a clean negative result rather than a blinded one.
// A net that goes noisy must only ever starve itself.
constexpr int kMaxVarLines = 400;      // the variable diff, per map visit
constexpr int kMaxObjLines = 80;       // the object diff, per map visit
constexpr int kNoisyStreak    = 3;     // changed on N consecutive samples => a per-frame counter
// Orientation floats are compared with an EPSILON, not `!=`. The baselines came back as
// `+0xAC=-0.0000` -- values sitting a denormal away from zero, which `!=` reports as a change on
// every single frame. A quarter turn is ~1.57 radians; nothing near this threshold is a rotation.
constexpr float kYawEpsilon = 0.001f;

// Candidate orientation slots on the transform node the mod already reads position from
// (`sceneObj+0xB8`). WE READ THEM RAW AND WE DO NOT CALL ANYTHING.
//
// The engine has a class-agnostic yaw getter, and the tempting move is to call it. It is NOT worth
// it here: its identity is inferred, not confirmed to this project's 0.98 bar, and a wrong
// signature on a game call is a crash rather than a bad number (the S129 shop crash was exactly
// that). Reading floats through the guarded reader answers the same question -- "does any
// orientation field move when the statue turns" -- and cannot fault.
//
// The two documented layouts disagree about which slot is live, and which applies depends on the
// object's scene class, which is itself unestablished for this object. So dump both families and
// let the diff say which one moved:
//   class 1 (gimmick / volume) : node+0x38 body yaw, mirror +0x48, second angle +0x54 / +0x58
//   class 3 (character)        : node+0xA4 move heading, +0xA8 body yaw, +0xA0 / +0xAC pitch pair
constexpr uint32_t kYawSlots[] = { 0x38, 0x48, 0x54, 0x58, 0xA0, 0xA4, 0xA8, 0xAC };
constexpr int kYawSlotCount = static_cast<int>(sizeof(kYawSlots) / sizeof(kYawSlots[0]));

// ---- THE ROUTINE WATCH LIST --------------------------------------------------------------------
//
// The script names its own state, so a fire of one of these IS the answer. Names are raw cp932
// bytes exactly as they sit in the module's name pool: `MapScript::FiredRoutineName` returns them
// untranscoded and its header is explicit that callers compare BYTES -- nothing here is user-facing
// text, and no locale is involved. The Japanese is reproduced in the comment column so the table can
// be checked against `ebp_statue_census.py` by eye.
//
// Matching is EXACT, so `全方向` and `全方向NG` cannot shadow one another and order does not matter.
// `token`, when non-null, puts this event in the running SEQUENCE line -- the one line a
// reader has to find. Everything else is detail.
struct Watch { const char* bytes; const char* label; const char* token; };
const Watch kWatch[] = {
    // ---- the verdict. This is the whole reason the tap exists. ----
    { "\x91\x53\x95\xFB\x8C\xFC\x4E\x47", "ALL DIRECTIONS NG -- the game says NOT solved", "not-solved" }, // 全方向NG
    { "\x91\x53\x95\xFB\x8C\xFC",         "ALL DIRECTIONS -- the game says SOLVED", "SOLVED" },       // 全方向
    // ---- the facing a statue settles into: the routine name IS the facing ----
    { "\x96\x6B\x95\xFB\x8C\xFC",         "settled facing NORTH", "N" },                         // 北方向
    { "\x93\xEC\x95\xFB\x8C\xFC",         "settled facing SOUTH", "S" },                         // 南方向
    { "\x93\x8C\x95\xFB\x8C\xFC",         "settled facing EAST", "E" },                          // 東方向
    { "\x90\xBC\x95\xFB\x8C\xFC",         "settled facing WEST", "W" },                          // 西方向
    // ---- which way it turned. Clockwise family. ----
    { "\x96\x6B\x81\x60\x93\x8C",         "turn CW  north->east", "cw" },                         // 北～東
    { "\x93\x8C\x81\x60\x93\xEC",         "turn CW  east->south", "cw" },                         // 東～南
    { "\x93\xEC\x81\x60\x90\xBC",         "turn CW  south->west", "cw" },                         // 南～西
    { "\x90\xBC\x81\x60\x96\x6B",         "turn CW  west->north", "cw" },                         // 西～北
    // ---- counterclockwise family ----
    { "\x96\x6B\x81\x60\x90\xBC",         "turn CCW north->west", "ccw" },                         // 北～西
    { "\x90\xBC\x81\x60\x93\xEC",         "turn CCW west->south", "ccw" },                         // 西～南
    { "\x93\xEC\x81\x60\x93\x8C",         "turn CCW south->east", "ccw" },                         // 南～東
    { "\x93\x8C\x81\x60\x96\x6B",         "turn CCW east->north", "ccw" },                         // 東～北
    // ---- the presentation events, incl. the completion ----
    { "\x91\x9C\x89\xF1\x93\x5D\x90\x55\x93\xAE\x8A\x4A\x8E\x6E", "statue rotation shake", nullptr }, // 像回転振動開始
    { "\x8C\x95\x8E\x9D\x82\xBF\x8F\xE3\x82\xB0\x90\x55\x93\xAE\x8A\x4A\x8E\x6E",
                                          "SWORD LIFT -- puzzle completion", "SWORD-LIFT" },              // 剣持ち上げ振動開始
    { "\x90\xCE\x94\xE0\x90\x55\x93\xAE\x8A\x4A\x8E\x6E", "stone door shake", nullptr },              // 石扉振動開始
    { "\x8A\x4B\x92\x69\x90\x55\x93\xAE\x8A\x4A\x8E\x6E", "stairway shake", nullptr },                // 階段振動開始
    // ---- the ASCII half of the same machinery ----
    { "statue_farst",           "statue interaction entry", nullptr },
    { "statue_fs_ALL",          "statue fs ALL", nullptr },
    { "statue_fs_ALL_off",      "statue fs ALL off", nullptr },
    { "fs_change",              "fs change", nullptr },
    { "fs_on",                  "fs on", nullptr },
    { "fs_off",                 "fs off", nullptr },
    { "sml_statue",             "sml statue", nullptr },
    { "sml_statue_eye_effect",  "statue EYE effect -- the game's own 'this one is right' feedback", "EYE" },
    { "mrm_statue_gimm_se",     "statue gimmick SE", nullptr },
    { "mrm_statue_gimm_eye_se", "statue EYE SE", nullptr },
    { "NG_MES",                 "NG message", nullptr },
    { "END_MES",                "END message", nullptr },
};
constexpr int kWatchCount = static_cast<int>(sizeof(kWatch) / sizeof(kWatch[0]));

// Everything else that fires in the dungeon: a bounded census, so an event we did not anticipate is
// still visible. Map boot alone fires `init`/`main` repeatedly, which is what burned S118's whole
// budget before the one line that mattered could print -- hence two tiers, watched names uncapped.
constexpr int kOtherFireBudget = 120;

struct VarSample {
    int32_t  value    = 0;
    uint8_t  elemType = 0xFF;
    uint8_t  cls      = 0xFF;
    bool     valid    = false;
    int      streak   = 0;      // consecutive samples on which this changed
    bool     noisy    = false;  // retired: a per-frame counter, not puzzle state
};

// ---- WINDOWS, AND WHY CLASS 0 IS THE BIG ONE (Session 156) ----------------------------------
//
// Storage class 0 is NOT module-local: both `mrm_b04` (map 600) and `mrm_b03` (map 599) report the
// same base, `0x02164480` -- which `GameArchitecture.md` already identifies as `FUN_002ef2b0()`
// (`&DAT_02164280 + 0x200`), the game's PERSISTENT SAVE BLOCK. The statue facings live there, which
// is why they survive leaving the room, and it means every statue in the dungeon is readable from
// anywhere once its offset is known.
//
// The first version of this diff covered classes 4 and 5 at 512 bytes and MISSED THE LOT: class 0
// was not swept at all, and map 599's statue cell sits at base + 0x9B1 -- 2481 bytes in, far past a
// 512-byte window. So a flag written into the save block by a script that does not DECLARE it as one
// of its own variables was invisible to every net. `mrm_b03` declares 33 variables and none is a
// flag; `mrm_b04` declares 46 and one of them is `0x08`. Different modules declare different subsets
// of the same shared array, so the declared-variable sweep can never be the whole story.
constexpr uint32_t kGlobalWindow  = 512;    // classes 4 and 5 -- the int/float work arrays
constexpr uint32_t kClass0Window  = 4096;   // the save block: must reach past the statue cells
constexpr int kMaxGlobalLines = 200;

struct GlobalSnap {
    uint8_t bytes[kClass0Window] = {};
    int     streak[kClass0Window] = {};
    bool    retired[kClass0Window] = {};
    uint32_t window = 0;
    bool    valid = false;
};
GlobalSnap s_glob[3];          // [0] = class 0 (SAVE BLOCK), [1] = class 4, [2] = class 5
int  s_globLines = 0;
bool s_globBudgetNoted = false;

struct ObjSample {
    void*  sceneObj = nullptr;
    float  yaw[kYawSlotCount] = {};
    int    streak[kYawSlotCount] = {};   // consecutive samples this slot moved
    bool   retired[kYawSlotCount] = {};  // an animation, not puzzle state
    bool   valid = false;
};

ShoutScript::RawModule s_mod;
std::vector<VarSample> s_vars;
std::vector<ObjSample> s_objs;
int  s_varLines = 0;
int  s_objLines = 0;
bool s_headerDone      = false;
bool s_varBudgetNoted  = false;
bool s_objBudgetNoted  = false;

// THE ARM FLAG for the event-fire tap. Set by the field tick, which already resolves the module.
// The tap runs inside somebody else's hook on a function that fires on every map in the game --
// doors, chests, conversations -- so its cost off this dungeon has to be one bool test, not a
// five-slot scan per fire.
bool s_armed = false;

struct FireSeen { void* object; uint32_t routine; };
std::vector<FireSeen> s_firesSeen;   // dedup for the UNWATCHED tier only
int  s_otherFires = 0;
bool s_otherNoted = false;

// The running sequence of facings and verdicts, so the log answers the question in ONE
// line instead of making a reader reconstruct it from scattered fires. Capped -- a full
// puzzle is a dozen events, so anything longer is a loop and the tail is what matters.
std::string s_sequence;
int s_sequenceCount = 0;

// The storage class the descriptor reports, in words -- the field that decides whether the readout
// can cover all three guardians from anywhere. Class 4 is the cross-script global int array that
// every module shares; class 3 is module-local and dies with the map.
const char* ClassWord(uint8_t cls) {
    switch (cls) {
        case 0: return "storage0";
        case 1: return "storage1";
        case 2: return "per-actor(UNSUPPORTED)";
        case 3: return "module-local";
        case 4: return "GLOBAL-int";
        case 5: return "GLOBAL-float";
        default: return "?";
    }
}

void ResetState() {
    s_mod = ShoutScript::RawModule();
    s_vars.clear();
    s_objs.clear();
    s_varLines = 0;
    s_objLines = 0;
    s_headerDone     = false;
    s_varBudgetNoted = false;
    s_objBudgetNoted = false;
    s_globLines = 0;
    s_globBudgetNoted = false;
    s_glob[0] = GlobalSnap();
    s_glob[1] = GlobalSnap();
    s_glob[2] = GlobalSnap();
    s_armed       = false;
    s_firesSeen.clear();
    s_otherFires  = 0;
    s_otherNoted  = false;
    s_sequence.clear();
    s_sequenceCount = 0;
}

bool VarBudgetLeft() {
    if (s_varLines < kMaxVarLines) return true;
    if (!s_varBudgetNoted) {
        s_varBudgetNoted = true;
        // NO SILENT CAP (the "log what was dropped" rule): a reader must never mistake a truncated
        // capture for a quiet one.
        Log::Write("STATUE", "VARIABLE budget reached -- further variable changes on this map are "
                             "NOT being logged. Re-enter the map for a fresh budget.");
    }
    return false;
}

bool ObjBudgetLeft() {
    if (s_objLines < kMaxObjLines) return true;
    if (!s_objBudgetNoted) {
        s_objBudgetNoted = true;
        Log::Write("STATUE", "OBJECT budget reached -- further orientation changes are NOT being "
                             "logged. The variable capture is unaffected.");
    }
    return false;
}

// ---- the variable table -------------------------------------------------------------------------

void SnapshotVars(bool baseline) {
    uint32_t declared = ShoutScript::VarCount(s_mod.record);
    if (declared == 0 || declared > kMaxVars) {
        // A count outside the format's own ceiling means the record is torn or the table is not
        // where we think. Say so once rather than sweeping 256 garbage descriptors.
        if (baseline) {
            char m[160];
            snprintf(m, sizeof(m),
                     "%s: descriptor table declares %u variables -- outside [1,%u], not sweeping",
                     s_mod.srcName, declared, kMaxVars);
            Log::Write("STATUE", m);
        }
        return;
    }

    if (baseline) {
        s_vars.assign(declared, VarSample());
        char m[192];
        snprintf(m, sizeof(m), "%s (slot %d) declares %u variables -- baseline taken",
                 s_mod.srcName, s_mod.slot, declared);
        Log::Write("STATUE", m);
    }
    if (s_vars.size() != declared) return;   // count moved under us: skip this sample

    for (uint32_t i = 0; i < declared; ++i) {
        VarSample& v = s_vars[i];
        if (v.noisy) continue;

        void*   addr = nullptr;
        uint8_t type = 0xFF;
        uint32_t raw = 0;
        if (!ShoutScript::VarAddressRaw(s_mod.record, s_mod.ebpBase,
                                        static_cast<uint8_t>(i), &addr, &type, &raw)) {
            v.valid = false;
            continue;
        }
        int32_t now = 0;
        if (!ShoutScript::ReadVar(addr, type, &now)) { v.valid = false; continue; }

        const uint8_t cls = static_cast<uint8_t>((raw >> 24) & 7);
        if (baseline || !v.valid) {
            v.value = now; v.elemType = type; v.cls = cls; v.valid = true; v.streak = 0;
            continue;
        }
        if (now == v.value) { v.streak = 0; continue; }

        // CHANGED.
        ++v.streak;
        if (v.streak >= kNoisyStreak) {
            v.noisy = true;
            char m[192];
            snprintf(m, sizeof(m),
                     "var 0x%02X (%s) changes every frame -- retired as a counter, not puzzle state",
                     i, ClassWord(cls));
            Log::Write("STATUE", m);
            v.value = now;
            continue;
        }
        if (VarBudgetLeft()) {
            ++s_varLines;
            // THE ADDRESS IS THE POINT, not the index. Two modules can both call a variable
            // `0x0E` and mean different memory, or the same memory at different indices -- only the
            // resolved address says which. Comparing map 600's statue variable against map 599's is
            // exactly the question "is this dungeon-wide state or one room's own", and that decides
            // whether the readout can cover all three guardians from anywhere.
            char m[288];
            snprintf(m, sizeof(m),
                     "var 0x%02X %-22s type=%u  %d -> %d   @%p  (%s, map %d)",
                     i, ClassWord(cls), type, v.value, now, addr, s_mod.srcName,
                     MapNames::CurrentMapId());
            Log::Write("STATUE", m);
        }
        v.value = now;
    }
}

// ---- the shared global arrays ------------------------------------------------------------------
//
// THE GAP THE OTHER NETS LEAVE. The variable sweep can only see what THIS module's descriptor table
// declares. If the statue flag is declared by another script -- a system module, or the sibling room
// -- it is invisible there, however global its storage. Classes 4 and 5 are the arrays every module
// shares, so diffing them RAW catches a write from any script at all, with no descriptor involved.
//
// Bounded and budgeted separately, like every other net. The window is deliberately small: this is
// looking for a puzzle flag near the start of the work array, not auditing the game's global state.
bool GlobalBudgetLeft() {
    if (s_globLines < kMaxGlobalLines) return true;
    if (!s_globBudgetNoted) {
        s_globBudgetNoted = true;
        Log::Write("STATUE", "GLOBAL-ARRAY budget reached -- further raw global changes are NOT "
                             "being logged. The variable capture is unaffected.");
    }
    return false;
}

// Log where EVERY storage class lives, once per visit. Classes 4 and 5 are known to be the shared
// arrays; 0, 1 and 3 are read from the module record and it has never been established whether they
// are shared or per-module. The statue's facing sits in class 0, so that is now the load-bearing
// question -- and one line of addresses answers it the moment a second room is captured.
void LogClassBases() {
    for (uint8_t cls = 0; cls <= 5; ++cls) {
        if (cls == 2) continue;   // per-actor, no honest address outside a running native
        void* b = ShoutScript::ClassBaseRaw(s_mod.record, s_mod.ebpBase, cls);
        char m[160];
        snprintf(m, sizeof(m), "storage class %u base=%p  (%s, map %d)",
                 cls, b, s_mod.srcName, MapNames::CurrentMapId());
        Log::Write("STATUE", m);
    }
}

void SnapshotGlobals(bool baseline) {
    if (baseline) LogClassBases();
    static const uint8_t  kClasses[3] = { 0, 4, 5 };
    static const uint32_t kWindows[3] = { kClass0Window, kGlobalWindow, kGlobalWindow };
    for (int c = 0; c < 3; ++c) {
        const uint32_t win = kWindows[c];
        void* base = ShoutScript::ClassBaseRaw(s_mod.record, s_mod.ebpBase, kClasses[c]);
        if (!base) continue;
        uint8_t now[kClass0Window] = {};
        if (!MemRead::SafeReadBytes(base, now, win)) continue;

        GlobalSnap& g = s_glob[c];
        if (baseline || !g.valid) {
            memcpy(g.bytes, now, win);
            memset(g.streak, 0, sizeof(g.streak));
            memset(g.retired, 0, sizeof(g.retired));
            g.window = win;
            g.valid = true;
            char m[160];
            snprintf(m, sizeof(m), "global class %u base=%p -- %u-byte baseline taken",
                     kClasses[c], base, win);
            Log::Write("STATUE", m);
            continue;
        }

        for (uint32_t i = 0; i < win; ++i) {
            if (now[i] == g.bytes[i]) { g.streak[i] = 0; continue; }
            if (++g.streak[i] >= kNoisyStreak) {
                if (!g.retired[i]) {
                    g.retired[i] = true;
                    char m[176];
                    snprintf(m, sizeof(m),
                             "global class %u +0x%03X changes continuously -- retired as a counter",
                             kClasses[c], i);
                    Log::Write("STATUE", m);
                }
                g.bytes[i] = now[i];
                continue;
            }
            if (!g.retired[i] && GlobalBudgetLeft()) {
                ++s_globLines;
                char m[192];
                snprintf(m, sizeof(m), "global class %u +0x%03X  %u -> %u   (map %d)",
                         kClasses[c], i, g.bytes[i], now[i], MapNames::CurrentMapId());
                Log::Write("STATUE", m);
            }
            g.bytes[i] = now[i];
        }
    }
}

// ---- the statue objects -------------------------------------------------------------------------

// The guardians carry a per-map custom string rather than an npcdic id, so their name key is -1.
//
// ---- THAT IS NOT ENOUGH ON ITS OWN, AND THE FIRST RUN PROVED IT (2026-08-12) --------------------
//
// `nameIdx == -1` also catches this map's WANDERING ENEMIES -- the capture came back with "Balloon
// 2", "Ghoul" and "Zombie Warrior" in it. Their yaw genuinely changes every frame, they burned the
// whole shared line budget on jitter 22 seconds before the player rotated anything, and because the
// collect cap was 8 they may well have pushed the statue out of the window entirely. The visit
// produced no rotation data at all and looked, in the log, exactly like a clean negative result.
//
// So the selector now also demands the CLASS-1 GIMMICK SHAPE, which the same run measured:
//     Stone Brave / Ancient Door : sceneCat=1 class=1     <- what we want
//     enemies and party          : scene category 5-7     <- characters, never a statue
// This is the object's own type byte, not a name and not a category the mod assigned, so it stays
// locale-independent. The label is still only ever LOGGED, never matched on.
constexpr int16_t kCustomStringNameIdx = -1;
constexpr int kMaxObjs = 16;              // was 8 -- too small once enemies could enter at all
constexpr uint8_t kGimmickClass = 1;      // `sceneObj+0x03 >> 5`

// Is this the class-1 gimmick family (statues, doors, levers), rather than a character?
bool IsGimmick(void* obj) {
    uint8_t typeByte = 0;
    if (!MemRead::SafeReadU8(obj, NavRva::SCENEOBJ_TYPE_BYTE, &typeByte)) return false;
    return static_cast<uint8_t>(typeByte >> 5) == kGimmickClass;
}

void SnapshotObjs(bool baseline) {
    void* raw[kMaxObjs] = {};
    const int rawN = EntityList::CollectSceneObjectsByNameIdx(kCustomStringNameIdx, raw, kMaxObjs);
    if (rawN <= 0) return;

    void* found[kMaxObjs] = {};
    int n = 0;
    for (int i = 0; i < rawN; ++i)
        if (raw[i] && IsGimmick(raw[i])) found[n++] = raw[i];
    if (n <= 0) return;

    // KEYED ON THE OBJECT POINTER, NOT ON THE INDEX. The collect order is not stable across
    // rescans, so an index-keyed snapshot re-baselined constantly and re-dumped the same objects --
    // 54 times in the first run.
    if (baseline || s_objs.empty()) {
        s_objs.clear();
        baseline = true;
    }

    for (int k = 0; k < n; ++k) {
        void* obj = found[k];
        void* node = MemRead::PtrAt(obj, NavRva::SCENEOBJ_XFORM_PTR);
        if (!node) continue;

        float now[kYawSlotCount] = {};
        for (int s = 0; s < kYawSlotCount; ++s) MemRead::SafeReadF32(node, kYawSlots[s], &now[s]);

        ObjSample* found_o = nullptr;
        for (ObjSample& c : s_objs)
            if (c.sceneObj == obj) { found_o = &c; break; }

        if (!found_o) {
            s_objs.push_back(ObjSample());
            ObjSample& o = s_objs.back();
            o.sceneObj = obj;
            memcpy(o.yaw, now, sizeof(now));
            o.valid = true;

            // THE SHAPE DUMP, once per object per visit. Which scene CATEGORY and CLASS this object
            // is decides which orientation slot is even meaningful, and it has never been measured
            // for a statue -- categories 3 and 4 carry a node only 0x20 bytes long, where none of
            // the slots above exist at all.
            uint8_t typeByte = 0, kindByte = 0, ready = 0;
            uint32_t flags = 0;
            MemRead::SafeReadU8 (obj, NavRva::SCENEOBJ_TYPE_BYTE,   &typeByte);
            MemRead::SafeReadU8 (obj, NavRva::SCENEOBJ_ENABLE_OFF,  &kindByte);
            MemRead::SafeReadU8 (obj, NavRva::SCENEOBJ_READY_OFF,   &ready);
            MemRead::SafeReadU32(obj, NavRva::SCENEOBJ_FLAGS_OFF,   &flags);
            char nm[96];
            Log::ToUtf8(EntityList::LabelForSceneObject(obj), nm, sizeof(nm));
            char m[288];
            snprintf(m, sizeof(m),
                     "obj %p \"%s\": sceneCat=%u class=%u kind=%u +0x14=0x%02X +0x1C=0x%08X node=%p",
                     obj, nm, static_cast<unsigned>(typeByte & 0x1F),
                     static_cast<unsigned>(typeByte >> 5),
                     static_cast<unsigned>(kindByte & 0x0F), ready, flags, node);
            Log::Write("STATUE", m);

            char y[288]; int off = 0;
            for (int s = 0; s < kYawSlotCount; ++s)
                off += snprintf(y + off, sizeof(y) - static_cast<size_t>(off),
                                "%s+0x%02X=%.4f", s ? " " : "", kYawSlots[s], now[s]);
            char m2[352];
            snprintf(m2, sizeof(m2), "  node orientation slots: %s", y);
            Log::Write("STATUE", m2);
            continue;
        }

        ObjSample& o = *found_o;
        for (int s = 0; s < kYawSlotCount; ++s) {
            // EPSILON, not `!=`. Baselines read `-0.0000` -- a denormal away from zero, which an
            // exact compare calls a change on every frame. A quarter turn is ~1.57 radians.
            const float delta = now[s] - o.yaw[s];
            if (delta > -kYawEpsilon && delta < kYawEpsilon) { o.streak[s] = 0; continue; }

            // A slot that moves on consecutive samples is an animation, not a puzzle state. Retire
            // it rather than let it spend the budget -- the same rule the variable sweep uses.
            if (++o.streak[s] >= kNoisyStreak) {
                if (!o.retired[s]) {
                    o.retired[s] = true;
                    char nm[96];
                    Log::ToUtf8(EntityList::LabelForSceneObject(obj), nm, sizeof(nm));
                    char m[224];
                    snprintf(m, sizeof(m),
                             "obj \"%s\" node+0x%02X moves continuously -- retired as animation",
                             nm, kYawSlots[s]);
                    Log::Write("STATUE", m);
                }
                o.yaw[s] = now[s];
                continue;
            }
            if (!o.retired[s] && ObjBudgetLeft()) {
                ++s_objLines;
                char nm[96];
                Log::ToUtf8(EntityList::LabelForSceneObject(obj), nm, sizeof(nm));
                char m[256];
                snprintf(m, sizeof(m), "obj %p \"%s\" node+0x%02X  %.4f -> %.4f",
                         obj, nm, kYawSlots[s], o.yaw[s], now[s]);
                Log::Write("STATUE", m);
            }
            o.yaw[s] = now[s];
        }
    }
}

} // namespace

bool InDungeon() {
    ShoutScript::RawModule mods[5];
    return ShoutScript::FindModulesBySrcPrefix(kDungeonPrefix, mods, 5) > 0;
}

void OnEventFire(void* object, uint32_t kind, uint32_t routineIdx) {
    // ONE BOOL TEST on every map in the game that is not this dungeon. `FUN_003dbb60` fires
    // constantly -- doors, chests, conversations -- so nothing below may be reachable off it.
    if (!s_armed) return;

    std::string raw;
    const bool named = MapScript::FiredRoutineName(object, routineIdx, raw);

    const char* label = nullptr;
    const char* token = nullptr;
    if (named) {
        for (int i = 0; i < kWatchCount; ++i) {
            if (raw == kWatch[i].bytes) { label = kWatch[i].label; token = kWatch[i].token; break; }
        }
    }

    if (!label) {
        // UNWATCHED TIER: deduped per (object, routine) and capped. An unresolved name is logged
        // here too rather than dropped -- "the statue's routines fire with no resolvable name" would
        // itself be the finding, and a silent skip would hide it.
        for (const FireSeen& f : s_firesSeen)
            if (f.object == object && f.routine == routineIdx) return;
        if (s_otherFires >= kOtherFireBudget) {
            if (!s_otherNoted) {
                s_otherNoted = true;
                Log::Write("STATUE", "unwatched event-fire census budget reached -- further "
                                     "non-statue fires on this map are NOT being logged");
            }
            return;
        }
        ++s_otherFires;
        s_firesSeen.push_back({ object, routineIdx });
    }

    // ASCII-safe rendering of the raw name: most of these are Shift-JIS, and the log is UTF-8.
    // The LABEL is what a reader acts on; this column only proves which byte string matched.
    char safe[80];
    size_t n = 0;
    for (; n < raw.size() && n < sizeof(safe) - 1; ++n) {
        const unsigned char c = static_cast<unsigned char>(raw[n]);
        safe[n] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
    safe[n] = '\0';

    char m[352];
    snprintf(m, sizeof(m),
             "fire map %d obj=%p kind=%u routine=%u name=\"%s\"%s%s",
             MapNames::CurrentMapId(), object, kind, routineIdx,
             named ? safe : "<unresolved>",
             label ? "  <== " : "", label ? label : "");
    Log::Write("STATUE", m);

    // THE ONE LINE TO LOOK FOR. Reading a log is work, and more so with a screen reader, so the
    // facings and verdicts are also accumulated into a single running sentence. Turning one statue
    // in a full circle produces `N > cw > E > cw > S > cw > W > cw > N` -- which enumerates that
    // statue's four facings AND, if the eye effect fires on one of them, names its target, without
    // solving anything or disturbing the puzzle.
    if (token) {
        if (s_sequenceCount < 48) {
            ++s_sequenceCount;
            if (!s_sequence.empty()) s_sequence += " > ";
            s_sequence += token;
        }
        char sq[512];
        snprintf(sq, sizeof(sq), "SEQUENCE (map %d): %s",
                 MapNames::CurrentMapId(), s_sequence.c_str());
        Log::Write("STATUE", sq);
    }
}

void OnFieldFrame() {
    ShoutScript::RawModule mods[5];
    const int n = ShoutScript::FindModulesBySrcPrefix(kDungeonPrefix, mods, 5);
    if (n <= 0) {
        // Not in this dungeon. Everything below is unreachable, not merely skipped, and the
        // event-fire tap disarms with it.
        if (s_mod.valid) ResetState();
        s_armed = false;
        return;
    }

    // The FIRST matching slot wins and is held by RECORD pointer, so a second dungeon script
    // loading beside it cannot silently swap what we are diffing mid-visit.
    const bool changed = (!s_mod.valid || s_mod.record != mods[0].record);
    if (changed) {
        ResetState();
        s_mod = mods[0];
    }

    if (!s_headerDone) {
        s_headerDone = true;
        char names[192] = {};
        int used = 0;
        for (int i = 0; i < n && used < static_cast<int>(sizeof(names)) - 1; ++i)
            used += snprintf(names + used, sizeof(names) - static_cast<size_t>(used),
                             "%s[%d]=%s", used ? " " : "", mods[i].slot, mods[i].srcName);
        char m[288];
        snprintf(m, sizeof(m),
                 "==== Stilshrine capture: map %d, %d dungeon module(s): %s ====",
                 MapNames::CurrentMapId(), n, names);
        Log::Write("STATUE", m);
    }

    // Arm the event-fire tap only once a dungeon module is confirmed live.
    s_armed = true;

    SnapshotVars(changed);
    SnapshotGlobals(changed);
    SnapshotObjs(changed);
}

void OnMapTeardown() {
    if (s_mod.valid) Log::Write("STATUE", "map teardown -- capture state dropped");
    ResetState();
}

} // namespace StatueDiag
