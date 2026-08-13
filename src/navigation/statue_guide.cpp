#include "navigation/statue_guide.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "navigation/map_names.h"
#include "navigation/shout_script.h"
#include "navigation/statue_table.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "speech/phrasebook.h"
#include "speech/speech.h"

namespace StatueGuide {
namespace {

using StatueTable::Room;
using StatueTable::kRoomCount;

// The authoring prefix shared by every Stilshrine of Miriam room script. Measured, not guessed --
// `ebp_statue_census.py` binds `mrm_b04` -> map 600, `mrm_b03` -> 599 and `mrm_b02` -> 598 by exact
// match against the routine-name pools our own play log printed. The gate is the SCRIPT, not the
// area name: an area name is localized text in twelve languages, and the game calls this place
// "Stilshrine of Miriam" in ours.
constexpr char kDungeonPrefix[] = "mrm_";
constexpr int  kMaxDungeonModules = 5;   // the controller array is five slots

// A script's variable table is small and the descriptor API takes a uint8_t index, so 256 is the
// ceiling the format itself imposes.
constexpr uint32_t kMaxVars = 256;

// Bounded, so an unrecognised module cannot flood the log. Not a clock -- a work budget (L-24).
constexpr int kMaxCensusLines = 48;

// How much of the save block to show either side of a known cell, for the ADJACENCY QUESTION below.
constexpr int32_t kNeighbourSpan = 6;

std::atomic<bool> s_request{false};

// The live Stilshrine module, refreshed each field tick. `s_liveRoom` is its table row, or null when
// the room is one of this dungeon's non-statue rooms (most of them).
ShoutScript::RawModule s_live;
const Room* s_liveRoom = nullptr;

// SESSION-LIFETIME, deliberately not cleared on map teardown. A save-block offset is a property of a
// module's descriptor table and of a block at a fixed address, so once it is measured it stays true;
// clearing it would make a statue readable in its own room and nowhere else, which is the whole
// thing this feature exists to avoid.
int32_t s_learnedFacingOff[kRoomCount];
int32_t s_learnedFlagOff[kRoomCount];
bool    s_learnedInit = false;

// Per map visit.
bool s_censusDone    = false;   // the class-0 inventory for an unmeasured room
bool s_neighbourDone = false;   // the save-block neighbourhood dump
bool s_headerDone    = false;

void InitLearned() {
    if (s_learnedInit) return;
    s_learnedInit = true;
    for (int i = 0; i < kRoomCount; ++i) {
        s_learnedFacingOff[i] = StatueTable::kUnmeasuredOff;
        s_learnedFlagOff[i]   = StatueTable::kUnmeasuredOff;
    }
}

void* Class0Base() {
    if (!s_live.valid) return nullptr;
    return ShoutScript::ClassBaseRaw(s_live.record, s_live.ebpBase, 0);
}

// The offset this session knows for a room's cell: the measured constant when there is one,
// otherwise whatever a visit to that room has taught us, otherwise unmeasured.
int32_t FacingOff(int i) {
    const int32_t baked = StatueTable::Rooms()[i].facingOff;
    return baked != StatueTable::kUnmeasuredOff ? baked : s_learnedFacingOff[i];
}
int32_t FlagOff(int i) {
    const int32_t baked = StatueTable::Rooms()[i].flagOff;
    return baked != StatueTable::kUnmeasuredOff ? baked : s_learnedFlagOff[i];
}

// ---- resolving one statue's two cells ----------------------------------------------------------

struct Reading {
    bool ok       = false;   // at least one cell was read
    bool haveFlag = false;
    bool haveFacing = false;
    bool solved   = false;
    int  facing   = 0;       // 1..4
    int  target   = 0;       // 0 when unmeasured
};

// Resolve through the LIVE module's descriptor table, and learn the offsets while we are there.
// This is the path that needs no hardcoded address at all, so it is preferred whenever the player
// happens to be standing in the room.
bool ResolveLive(int i, void** outFacing, void** outFlag) {
    const Room& r = StatueTable::Rooms()[i];
    if (!s_live.valid || s_liveRoom != &r) return false;
    if (r.facingVar == StatueTable::kUnmeasuredVar) return false;

    void* fa = nullptr; void* fl = nullptr;
    uint8_t ta = 0xFF, tl = 0xFF;
    uint32_t ra = 0, rl = 0;
    if (!ShoutScript::VarAddressRaw(s_live.record, s_live.ebpBase, r.facingVar, &fa, &ta, &ra))
        return false;
    if (!ShoutScript::VarAddressRaw(s_live.record, s_live.ebpBase, r.flagVar, &fl, &tl, &rl))
        return false;

    // THE DESCRIPTOR MUST STILL SAY CLASS 0. If it does not, this module's table is not the one that
    // was measured -- a game patch, or the wrong module -- and reading on anyway would report a
    // confident wrong state off some unrelated byte.
    const uint8_t clsA = static_cast<uint8_t>((ra >> 24) & 7);
    const uint8_t clsB = static_cast<uint8_t>((rl >> 24) & 7);
    if (clsA != 0 || clsB != 0) {
        char m[192];
        snprintf(m, sizeof(m),
                 "%s: var 0x%02X/0x%02X resolve to storage class %u/%u, expected 0 -- declining",
                 r.srcName, r.facingVar, r.flagVar, clsA, clsB);
        Log::Write("STATUE-GUIDE", m);
        return false;
    }

    // LEARN THE OFFSETS. This is what makes a single walk through the room enough to read that
    // statue from anywhere for the rest of the session, and the log line is what lets a future
    // session bake the constant into statue_table.cpp.
    void* base = Class0Base();
    if (base && (s_learnedFacingOff[i] == StatueTable::kUnmeasuredOff ||
                 s_learnedFlagOff[i]   == StatueTable::kUnmeasuredOff)) {
        const int32_t offA = static_cast<int32_t>(static_cast<char*>(fa) - static_cast<char*>(base));
        const int32_t offB = static_cast<int32_t>(static_cast<char*>(fl) - static_cast<char*>(base));
        s_learnedFacingOff[i] = offA;
        s_learnedFlagOff[i]   = offB;
        char m[256];
        snprintf(m, sizeof(m),
                 "MEASURED %s (map %d): facing var 0x%02X @%p = class0+0x%03X, "
                 "flag var 0x%02X @%p = class0+0x%03X  <== bake these into statue_table.cpp",
                 r.srcName, MapNames::CurrentMapId(), r.facingVar, fa, offA,
                 r.flagVar, fl, offB);
        Log::Write("STATUE-GUIDE", m);
    }

    if (outFacing) *outFacing = fa;
    if (outFlag)   *outFlag   = fl;
    return true;
}

Reading ReadRoom(int i) {
    Reading out;
    const Room& r = StatueTable::Rooms()[i];
    out.target = r.target;

    void* facingAddr = nullptr;
    void* flagAddr   = nullptr;
    if (!ResolveLive(i, &facingAddr, &flagAddr)) {
        void* base = Class0Base();
        if (!base) return out;
        const int32_t fo = FacingOff(i);
        const int32_t go = FlagOff(i);
        if (fo != StatueTable::kUnmeasuredOff) facingAddr = static_cast<char*>(base) + fo;
        if (go != StatueTable::kUnmeasuredOff) flagAddr   = static_cast<char*>(base) + go;
    }

    int32_t v = 0;
    if (facingAddr && ShoutScript::ReadVar(facingAddr, StatueTable::kCellElemType, &v)) {
        out.haveFacing = true;
        out.facing = static_cast<int>(v);
    }
    if (flagAddr && ShoutScript::ReadVar(flagAddr, StatueTable::kCellElemType, &v)) {
        out.haveFlag = true;
        out.solved   = (v != 0);
    }
    out.ok = out.haveFacing || out.haveFlag;
    return out;
}

// ---- turning a reading into words --------------------------------------------------------------

// THE FLAG IS THE VERDICT AND THE TARGET IS ONLY A COUNT. Solved-ness is never derived from the
// facing: the game publishes its own per-statue answer and that is what gets read (L-07).
//
// Turns, once the flag says "not yet": four facings, clockwise increments, so the shortest way round
// is (target - facing) mod 4 -- one turn clockwise, two turns (the same either way, said clockwise),
// or one turn counterclockwise. There is no three-turn case; going the other way is always shorter.
std::wstring Verdict(const Reading& rd, const char* srcName) {
    if (!rd.haveFlag) return Phrase::Get(Phrase::Id::StateUnknown);
    if (rd.solved)    return Phrase::Get(Phrase::Id::StatueSolved);

    if (rd.target == StatueTable::kUnmeasuredTarget || !rd.haveFacing ||
        rd.facing < 1 || rd.facing > StatueTable::kFacings) {
        // The flag is readable and says "not right", but there is no measured target to count
        // against. Say the half that is known rather than claiming ignorance of the whole thing.
        return Phrase::Get(Phrase::Id::StatueNotSolved);
    }

    const int delta = ((rd.target - rd.facing) % StatueTable::kFacings + StatueTable::kFacings)
                      % StatueTable::kFacings;
    if (delta == 0) {
        // The flag says not solved while the facing already equals the measured target. One of the
        // two numbers is wrong, and the FLAG WINS because it is the game's own word -- but this must
        // never pass silently, because it means a baked target or offset has gone stale.
        char m[192];
        snprintf(m, sizeof(m),
                 "DISAGREEMENT on %s: flag says NOT solved but facing %d == target %d. "
                 "Reporting not solved; a baked offset or target may be stale.",
                 srcName, rd.facing, rd.target);
        Log::Write("STATUE-GUIDE", m);
        return Phrase::Get(Phrase::Id::StatueNotSolved);
    }

    std::wstring t;
    if (delta == 1)      t  = Phrase::Get(Phrase::Id::Clockwise);
    else if (delta == 2) t  = Phrase::Get(Phrase::Id::Clockwise);          // two turns either way
    else                 t  = Phrase::Get(Phrase::Id::Counterclockwise);   // delta 3
    t += L" ";
    t += Phrase::Get(delta == 2 ? Phrase::Id::Twice : Phrase::Id::Once);
    return t;
}

// ---- the one-per-visit measurement logging -----------------------------------------------------
//
// Everything below is LOG-ONLY and exists so the third guardian costs no special trip. It is the
// build note from Session 156: have the mod record what a future session needs the first time the
// player walks in, rather than sending them back for a capture run.

// The class-0 (save block) variables a module DECLARES, with their offsets. For `mrm_c01` this is
// the whole missing measurement: its facing and flag cells are two of these, and a rotation seen in
// StatueDiag's raw class-0 diff picks them out by index.
void LogClass0Census() {
    if (!s_live.valid) return;
    void* base = Class0Base();
    const uint32_t declared = ShoutScript::VarCount(s_live.record);
    char head[224];
    snprintf(head, sizeof(head),
             "==== class-0 census: %s (slot %d, map %d) declares %u variables, save block @%p ====",
             s_live.srcName, s_live.slot, MapNames::CurrentMapId(), declared, base);
    Log::Write("STATUE-GUIDE", head);
    if (declared == 0 || declared > kMaxVars || !base) return;

    int lines = 0;
    for (uint32_t i = 0; i < declared; ++i) {
        if (lines >= kMaxCensusLines) {
            // NO SILENT CAP: a reader must never mistake a truncated census for a short one.
            Log::Write("STATUE-GUIDE", "class-0 census budget reached -- remaining variables NOT "
                                       "listed. Re-enter the map for a fresh budget.");
            break;
        }
        void* addr = nullptr;
        uint8_t type = 0xFF;
        uint32_t raw = 0;
        if (!ShoutScript::VarAddressRaw(s_live.record, s_live.ebpBase,
                                        static_cast<uint8_t>(i), &addr, &type, &raw)) continue;
        if (static_cast<uint8_t>((raw >> 24) & 7) != 0) continue;
        int32_t val = 0;
        if (!ShoutScript::ReadVar(addr, type, &val)) continue;
        ++lines;
        char m[224];
        snprintf(m, sizeof(m), "  var 0x%02X type=%u @%p = class0+0x%03X  value=%d",
                 i, type, addr,
                 static_cast<unsigned>(static_cast<char*>(addr) - static_cast<char*>(base)), val);
        Log::Write("STATUE-GUIDE", m);
    }
}

// THE ADJACENCY QUESTION, asked once per visit and answered by bytes. The two measured cells sit at
// class0+0x9B1 (a facing) and class0+0x882 (a flag), in two different sub-regions of the block. If
// the three guardians' facings are neighbours in one array and their flags neighbours in another --
// which is how one authoring template instantiated three times usually lands -- then these two
// windows already contain all six cells, and the third statue can be bound with no visit at all.
// It is a HYPOTHESIS and nothing reads on it; this dump is how it gets tested or killed.
void LogNeighbourhood() {
    void* base = Class0Base();
    if (!base) return;
    for (int i = 0; i < kRoomCount; ++i) {
        const int32_t offs[2] = { FacingOff(i), FlagOff(i) };
        const char*   what[2] = { "facing", "flag" };
        for (int k = 0; k < 2; ++k) {
            if (offs[k] == StatueTable::kUnmeasuredOff) continue;
            const int32_t start = offs[k] - kNeighbourSpan;
            if (start < 0) continue;
            uint8_t buf[kNeighbourSpan * 2 + 1] = {};
            if (!MemRead::SafeReadBytes(static_cast<char*>(base) + start, buf, sizeof(buf))) continue;
            char hex[128]; int used = 0;
            for (size_t b = 0; b < sizeof(buf); ++b)
                used += snprintf(hex + used, sizeof(hex) - static_cast<size_t>(used),
                                 "%s%02X", b ? " " : "", buf[b]);
            char m[256];
            snprintf(m, sizeof(m), "save block around %s %s (class0+0x%03X, centre byte %d of %d): %s",
                     StatueTable::Rooms()[i].srcName, what[k], offs[k],
                     kNeighbourSpan + 1, static_cast<int>(sizeof(buf)), hex);
            Log::Write("STATUE-GUIDE", m);
        }
    }
}

// ---- the key ------------------------------------------------------------------------------------

void SpeakAll() {
    std::wstring text;
    char detail[352]; int used = 0;
    int resolved = 0;

    for (int i = 0; i < kRoomCount; ++i) {
        const Room& r = StatueTable::Rooms()[i];
        const Reading rd = ReadRoom(i);
        if (rd.ok) ++resolved;

        if (!text.empty()) text += L" ";
        text += Phrase::Get(Phrase::Id::Statue);
        text += L" ";
        text += std::to_wstring(i + 1);
        text += L": ";
        text += rd.ok ? Verdict(rd, r.srcName) : std::wstring(Phrase::Get(Phrase::Id::StateUnknown));
        text += L".";

        // The evidence trail for every press: the raw cells, so a wrong announcement is traceable to
        // a number rather than argued about. The AREA NAME is here rather than in the speech --
        // the spoken line is the shape the tester asked for.
        used += snprintf(detail + used, sizeof(detail) - static_cast<size_t>(used),
                         "%s#%d %s facing=%s%d flag=%s target=%d",
                         used ? " | " : "", i + 1, r.srcName,
                         rd.haveFacing ? "" : "?", rd.facing,
                         rd.haveFlag ? (rd.solved ? "1" : "0") : "?", r.target);
    }

    Log::Write("STATUE-GUIDE", detail);

    // NOT ONE STATUE READ. That is a broken save-block read, not a puzzle state, and three
    // "unknown"s would be filler dressed as an answer -- so the key says nothing and the log says
    // why (the silence-is-normal rule).
    if (resolved == 0) {
        Log::Write("STATUE-GUIDE", "no statue resolved -- save block unreadable, staying silent");
        return;
    }

    Log::WriteW("STATUE-GUIDE", "B: ", text);
    Speech::Output(text, true);
}

void RefreshModule() {
    ShoutScript::RawModule mods[kMaxDungeonModules];
    const int n = ShoutScript::FindModulesBySrcPrefix(kDungeonPrefix, mods, kMaxDungeonModules);
    if (n <= 0) {
        s_live = ShoutScript::RawModule();
        s_liveRoom = nullptr;
        return;
    }

    // The FIRST matching slot wins, held by RECORD pointer so a second dungeon script loading beside
    // it cannot silently swap what we are reading mid-visit.
    const bool changed = (!s_live.valid || s_live.record != mods[0].record);
    s_live = mods[0];
    s_liveRoom = StatueTable::FindBySrc(s_live.srcName);
    if (!changed && s_headerDone) return;

    s_headerDone = true;
    char m[256];
    snprintf(m, sizeof(m), "in the Stilshrine: map %d, module %s (slot %d)%s",
             MapNames::CurrentMapId(), s_live.srcName, s_live.slot,
             s_liveRoom ? " -- a guardian room" : "");
    Log::Write("STATUE-GUIDE", m);

    if (!s_neighbourDone) { s_neighbourDone = true; LogNeighbourhood(); }

    // The census runs for a guardian room whose cells are NOT yet measured -- today that is only
    // `mrm_c01` -- and for any Stilshrine module the table does not know at all, because a fourth
    // statue script would show up exactly that way.
    const bool unmeasuredRoom = s_liveRoom && s_liveRoom->facingVar == StatueTable::kUnmeasuredVar;
    if (!s_censusDone && (unmeasuredRoom || !s_liveRoom)) {
        s_censusDone = true;
        if (unmeasuredRoom)
            Log::Write("STATUE-GUIDE", "this guardian's cells have NEVER been measured -- listing "
                                       "every save-block variable it declares");
        LogClass0Census();
    }
}

} // namespace

void OnFieldFrame() {
    InitLearned();
    RefreshModule();

    if (!s_request.exchange(false, std::memory_order_acq_rel)) return;
    if (!s_live.valid) {
        // Not in this dungeon: `B` belongs to the shout minigame here, and this half stays quiet.
        Log::Write("STATUE-GUIDE", "B: no Stilshrine script live -- silent no-op");
        return;
    }
    SpeakAll();
}

void OnMapTeardown() {
    s_live = ShoutScript::RawModule();
    s_liveRoom = nullptr;
    s_censusDone    = false;
    s_neighbourDone = false;
    s_headerDone    = false;
    // s_learnedFacingOff / s_learnedFlagOff are NOT cleared -- see statue_guide.h.
}

void RequestCheck() { s_request.store(true, std::memory_order_release); }

} // namespace StatueGuide
