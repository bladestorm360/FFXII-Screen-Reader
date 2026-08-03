#include "ui/save_reader.h"

#include "battle/battle_state.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "navigation/map_names.h"
#include "speech/phrasebook.h"
#include "speech/speech.h"

#include <Windows.h>
#include <cstdint>
#include <mutex>
#include <string>

namespace {

using MemRead::Obj0;
using MemRead::PtrAt;
using MemRead::SafeReadU8;
using MemRead::SafeReadU16;
using MemRead::SafeReadU32;

// ---- class + layout (every one confirmed live, probe_save_and_primer) ---------------------------
constexpr uint32_t RVA_SLOT_LIST = 0x45FE80;  // FUN_0057fe80 -- the slot list window class
// FUN_003153f0(rank) -> the rank's NAME as a codec string. Not an id: the probe caught
// FUN_002f9860(1254) -> "Knight of the Round" resolving INSIDE this call and the pointer coming back
// out of it, which is why no rank->id table is needed here (and why the two-point "1243 + rank"
// formula this replaced was never built).
constexpr uint32_t RVA_CLANRANK_NAME = 0x1F53F0;
typedef const uint8_t* (*Pfn_ClanRankName)(int rank);

constexpr uint32_t OFF_W_RECBASE = 0x1C0;   // win+0x1C0 -> the 200 x 0xA0 preview array
constexpr uint32_t OFF_W_ROWMAP  = 0xEF;    // u8[win + 0xEF + row] = index into that array
constexpr uint32_t REC_STRIDE    = 0xA0;
constexpr int      MAX_SLOTS     = 200;

constexpr uint32_t OFF_R_GIL     = 0x08;    // u32  -- CONFIRMED against a screenshot: the 92h save
                                            //         reads 2,782,150 here and the panel shows
                                            //         "GIL 2782150G". +0x0C is a different counter.
constexpr uint32_t OFF_R_HOURS   = 0x18;    // u16
constexpr uint32_t OFF_R_MINUTES = 0x1A;    // u8
constexpr uint32_t OFF_R_SECONDS = 0x1B;    // u8   (read but not spoken -- see below)
constexpr uint32_t OFF_R_PARTY   = 0x20;    // + n*4: charId / level / gauges / flags
constexpr uint32_t OFF_R_CLANRANK = 0x44;   // u32  1..12; 0 = no clan yet
constexpr uint32_t OFF_R_CLANPTS = 0x40;    // u32  -- CONFIRMED: the panel reads "POINTS 13422868"
                                            //         for the record holding 13,422,868. An earlier
                                            //         note struck this because the number "looked
                                            //         absurd"; the game displays it verbatim.
constexpr uint32_t OFF_R_MAPID   = 0x4C;    // u32

constexpr int      PARTY_SLOTS   = 7;
constexpr uint32_t PARTY_STRIDE  = 4;
constexpr uint8_t  CHAR_EMPTY    = 0xFF;    // charId 0xFF = this party slot is unused
constexpr uint8_t  FLAG_LEADER   = 0x01;    // flags bit 0 = this member is the party leader

// The record for a focused row, or null. Two indirections, both from the game's own code: the row
// the cursor is on is not the slot it shows (the list is ordered by recency, so row 7 held slot 0 on
// the probe run), and the map that resolves it is a plain byte table on the window.
void* RecordForRow(void* owner, int row, int* outSlot) {
    if (row < 0 || row >= MAX_SLOTS) return nullptr;
    void* base = PtrAt(owner, OFF_W_RECBASE);
    if (!base) return nullptr;
    uint8_t slotIdx = 0;
    if (!SafeReadU8(owner, OFF_W_ROWMAP + static_cast<uint32_t>(row), &slotIdx)) return nullptr;
    if (slotIdx >= MAX_SLOTS) return nullptr;
    if (outSlot) *outSlot = static_cast<int>(slotIdx);
    return static_cast<char*>(base) + static_cast<size_t>(slotIdx) * REC_STRIDE;
}

// "26 hours 34 minutes". SECONDS ARE READ AND DELIBERATELY DROPPED: the field exists and is correct,
// but nobody choosing between saves needs them, and they would push the useful part of the line
// (which character, what level) further behind a wall of numbers.
std::wstring Playtime(void* rec) {
    uint16_t h = 0; uint8_t m = 0;
    if (!SafeReadU16(rec, OFF_R_HOURS, &h) || !SafeReadU8(rec, OFF_R_MINUTES, &m)) return std::wstring();
    if (h > 999 || m > 59) return std::wstring();          // the game's own clamps; anything else is garbage
    std::wstring s;
    if (h) { s += std::to_wstring(h); s += Phrase::Get(Phrase::Id::HoursSuffix); }
    if (m || !h) {
        if (!s.empty()) s += L" ";
        s += std::to_wstring(m);
        s += Phrase::Get(Phrase::Id::MinutesSuffix);
    }
    return s;
}

// Game call, kept in its own function so no C++ object is live inside the __try scope (SEH rule) --
// the same shape inventory_reader.cpp uses for FUN_002f9860, and this lands in that very function a
// few instructions later. Game thread only; TryFocus is called from the FUN_00247510 dispatch hook.
const uint8_t* ResolveClanRankCodec(int rank) {
    auto fn = reinterpret_cast<Pfn_ClanRankName>(Hooks::ResolveRva(RVA_CLANRANK_NAME));
    if (!fn) return nullptr;
    __try { return fn(rank); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// The clan rank's NAME, e.g. "Knight of the Round". Empty when there is no rank or the text does not
// decode -- and that empty is the whole safety argument for calling an 0.98 function rather than a
// 1.00 one. If the return were ever NOT the name string, the decode fails IsMostlyPrintable and this
// says nothing; there is no path from a wrong pointer to a wrong LABEL, only to silence.
// The clan rank NUMBER, or 0 for "no clan yet". 0 is not a rank -- it is the game's own "this save
// has not joined a clan", and CONFIRMED by screenshot: on a rank-0 save the panel draws NO CLAN RANK
// row and NO POINTS row at all, just LEVEL and GIL. So 0 suppresses both fields here, matching what a
// sighted player sees. (It also keeps FUN_003153f0(0) uncalled -- unobserved, and there is no reason
// to find out in play what it returns.)
int ClanRank(void* rec) {
    uint32_t rank = 0;
    if (!SafeReadU32(rec, OFF_R_CLANRANK, &rank)) return 0;
    return (rank >= 1 && rank <= 12) ? static_cast<int>(rank) : 0;
}

std::wstring ClanRankName(void* rec) {
    const int rank = ClanRank(rec);
    if (rank == 0) return std::wstring();
    const uint8_t* codec = ResolveClanRankCodec(rank);
    if (!codec) return std::wstring();
    std::wstring s = GameText::Decode(codec, 128);
    if (GameText::IsMostlyPrintable(s)) return s;
    s = GameText::Decode(GameText::SkipVariantPrefix(codec), 128);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// "Vaan, level 11" for whichever party slot carries the leader bit. Empty when no slot claims it --
// an empty or corrupt record must produce silence here, not a guess at slot 0.
std::wstring Leader(void* rec) {
    for (int n = 0; n < PARTY_SLOTS; ++n) {
        const uint32_t b = OFF_R_PARTY + static_cast<uint32_t>(n) * PARTY_STRIDE;
        uint8_t charId = 0, level = 0, flags = 0;
        if (!SafeReadU8(rec, b, &charId) || charId == CHAR_EMPTY) continue;
        if (!SafeReadU8(rec, b + 3, &flags) || !(flags & FLAG_LEADER)) continue;
        // CharacterName is PURE MEMORY READS (battle_state.h) -- DefName(0x02, id) would answer the
        // same question by calling FUN_0035d330, which stages its arguments in a static record.
        std::wstring name = BattleState::CharacterName(charId);
        if (name.empty()) return std::wstring();           // unresolvable -> say nothing, never a number
        if (SafeReadU8(rec, b + 1, &level) && level > 0 && level <= 99) {
            name += L", ";
            name += Phrase::Get(Phrase::Id::LevelPrefix);
            name += std::to_wstring(level);
        }
        return name;
    }
    return std::wstring();
}

// The row the cursor is on, so keys 4-9 can address THIS save's party. Deliberately (owner, row) and
// NOT a record pointer: the record is re-derived from the live window on every keypress, so there is
// no cached pointer that can outlive the screen. Three guards in S126 went silent for exactly that
// reason. Game thread writes it; the input thread reads it, hence the lock.
std::mutex g_mutex;
void*      g_rowOwner = nullptr;
int        g_rowIndex = -1;

} // namespace

namespace SaveReader {

bool OwnsSurface(void* w) {
    return w && Obj0(w) == Hooks::ResolveRva(RVA_SLOT_LIST);
}

bool TryFocus(void* owner, int index) {
    STALL_SCOPE("SaveReader::TryFocus");
    if (!OwnsSurface(owner) || index < 0) return false;

    int slotIdx = -1;
    void* rec = RecordForRow(owner, index, &slotIdx);
    if (!rec) return false;

    // WHERE. The same two lookups the row itself paints, so a slot that reads "Rabanastre: Southgate"
    // on screen reads the same here -- this is not a second naming scheme.
    uint32_t mapId = 0;
    if (!SafeReadU32(rec, OFF_R_MAPID, &mapId)) return false;
    const std::wstring where = MapNames::ResolveFullAreaName(static_cast<int>(mapId));
    if (where.empty()) return false;   // empty/corrupt file -> let the game's own "empty" text speak

    // THE NUMBER THE SCREEN SHOWS IS THE ARRAY INDEX, not `+0x54`. Confirmed against a screenshot:
    // the rows read 004, 005, 006, 007, <icon>, 008 in exactly the order the row map gives
    // (...04 05 06 07 00 08), and the record at index 8 is the one whose +0x54 holds 3. Index 0 is
    // the slot the game marks with an icon instead of a number, so it gets no number here either --
    // what that icon MEANS is not established, and naming it would be inventing a label.
    std::wstring line;
    if (slotIdx > 0) { line += std::to_wstring(slotIdx); line += L", "; }
    line += where;
    const std::wstring when = Playtime(rec);
    if (!when.empty()) { line += L", "; line += when; }
    const std::wstring who = Leader(rec);
    if (!who.empty())  { line += L", "; line += who; }
    // Gil last: it is the least useful field for TELLING TWO SAVES APART, which is what this line is
    // for, so it must not push the leader and playtime further back in the utterance.
    uint32_t gil = 0;
    if (SafeReadU32(rec, OFF_R_GIL, &gil) && gil <= 99999999u) {
        line += L", ";
        line += std::to_wstring(gil);
        line += Phrase::Get(Phrase::Id::GilSuffix);
    }
    // Clan rank, then its points -- the order the panel itself uses ("CLAN RANK ... / POINTS ...").
    // BOTH are gated on the rank, not on the points value: a rank-0 save draws neither row, and
    // suppressing points because they HAPPEN to be 0 there would be luck rather than a rule.
    if (ClanRank(rec) != 0) {
        const std::wstring rankName = ClanRankName(rec);
        if (!rankName.empty()) { line += L", "; line += rankName; }
        uint32_t clanPts = 0;
        if (SafeReadU32(rec, OFF_R_CLANPTS, &clanPts)) {
            line += L", ";
            line += std::to_wstring(clanPts);
            line += Phrase::Get(Phrase::Id::ClanPointsSuffix);
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_rowOwner = owner;
        g_rowIndex = index;
    }

    Log::WriteW("SAVE", "slot:", owner, line);
    Speech::Output(line, /*interrupt=*/true);
    return true;
}

bool PartyMemberKey(int n) {
    if (n < 1 || n > PARTY_SLOTS) return false;

    void* owner; int row;
    { std::lock_guard<std::mutex> lk(g_mutex); owner = g_rowOwner; row = g_rowIndex; }
    // STRUCTURAL liveness, the same shape the equipment-column gate uses: the window must still BE
    // the slot list. A closed or recycled one fails the class check and these keys go back to meaning
    // party status, with no flag to get stuck.
    if (!OwnsSurface(owner) || row < 0) return false;

    void* rec = RecordForRow(owner, row, nullptr);
    if (!rec) return false;

    const uint32_t b = OFF_R_PARTY + static_cast<uint32_t>(n - 1) * PARTY_STRIDE;
    uint8_t charId = 0, level = 0;
    if (!SafeReadU8(rec, b, &charId) || charId == CHAR_EMPTY) return true;   // empty slot -> SILENT
    std::wstring name = BattleState::CharacterName(charId);
    if (name.empty()) return true;                                          // unresolvable -> SILENT

    std::wstring line = name;
    if (SafeReadU8(rec, b + 1, &level) && level > 0 && level <= 99) {
        line += L", ";
        line += Phrase::Get(Phrase::Id::LevelPrefix);
        line += std::to_wstring(level);
    }
    Log::WriteW("SAVE", "member:", owner, line);
    Speech::Output(line, /*interrupt=*/true);
    return true;
}

} // namespace SaveReader
