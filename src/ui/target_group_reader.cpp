#include "ui/target_group_reader.h"
#include "ui/battle_target_reader.h"
#include "ui/ingame_menu_reader.h"
#include "ui/text_capture.h"
#include "battle/battle_state.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"
#include "core/stall_probe.h"
#include "speech/speech.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

// THE TARGET LIST, AS THE BINARY BUILDS IT (S184; abs = RVA + 0x120000).
//
// The battle menu root FUN_00279e20 keeps two FUN_00276d30 "parent" slots. When the confirmed command
// needs a target, the root builds a parent with a list KIND in 0x10..0x13, sets parent+0x2FF0 bit 0x400
// (target mode) and broadcasts message 0x1F with that kind at msg+0x08. The parent's controller
// FUN_002778c0 (already hooked by ingame_menu_reader.cpp) handles 0x1F by toggling which of its two
// FUN_0027ad70 panels is live (ctrl+0xF0 / ctrl+0x1710) and, in target mode only, writing the GROUP
// TITLE to parent+0x208 from the kind:
//
//     kind 0x10 -> text 0x4A4A  "FOES"       kind 0x12 -> text 0x4A4B  "RESERVE"   (the else arm)
//     kind 0x11 -> text 0x4A49  "PARTY"      kind 0x13 -> text 0x4A4C  "ALLIES"
//
// (words: menu_expansion.bin entries 17-20, section 19 = first id 190000 / 10000, decoded offline; the
// mod speaks whatever FUN_002f9860 returns, so the live locale's own wording is what is heard). It then
// calls FUN_0027e050(panel, kind) -- a table maps kind to the builder's list kind, and ONLY list kind
// 0xF gets the row draw FUN_0027d5c0 -- and FUN_002d47c0(widget, ..., 1), which sends the new list's
// first-row 0x8000 focus BEFORE the handler returns.
//
// WHICH KIND IS RESERVE -- three independent sites agree (conf 0.98):
//   * FUN_0031eb20 case 0xF (the list builder) walks roster list 3 slots 4..8 (BtlWork+0x5A7E), the
//     reserve, writing each BtlChr's charId (bc+0x04) as the row and FUN_00322a80's verdict as the
//     row flag (bit 1 = not a legal target for this action);
//   * FUN_0027c730 (next/previous group) maps list kind 0xF <-> group 2 <-> kind 0x12, and tests group 2
//     with FUN_0027b6c0, whose whole job is "build list kind 0xF, is it non-empty";
//   * the 0x1F title arm gives 0x12 the RESERVE string.
// FUN_0027d5c0 names a row through FUN_0031c5d0({2, charId}) -- category 2, the character master
// table -- which is exactly the walk BattleState::CharacterName reimplements.
//
// THE SWITCH. FUN_0027b430(ctrl, panel, parent, dir) is the only group stepper. Its three callers are
// the controller's L1/R1 pad bits (0x400/0x800) and cases 4/5 of the FUN_00290520 input source. It
// asks FUN_0027c730 for the next allowed group, and on a CHANGE bubbles 0x21 (the new kind) up to the
// root, closes the parent and returns 1; the root rebuilds a parent for the stored kind, which sends a
// target-mode 0x1F. So "returned 1, then the next target-mode 0x1F" is exactly "a switch landed on
// this group" -- and entering targeting, or the list/cursor toggles that re-send 0x1F with the SAME
// kind, never arm it.
namespace {

using MemRead::PtrAt;
using MemRead::SafeReadInt;
using MemRead::SafeReadU8;
using MemRead::SafeReadU32;
using MemRead::SafeReadU64;

constexpr uint32_t RVA_GROUP_SWITCH = 0x15B430;  // FUN_0027b430(ctrl, panel, parent, dir) -> 1 on change
constexpr uint32_t RVA_DRAW_RESERVE = 0x15D5C0;  // FUN_0027d5c0 -- row draw for list kind 0xF only

constexpr int      MSG_BUILD_LIST   = 0x1F;      // controller: build the next list, kind at msg+0x08
constexpr uint32_t OFF_MSG_ARG0     = 0x08;
constexpr uint32_t OFF_CTRL_PARENT  = 0xD0;      // ctrl+0xD0 -> parent (FUN_00276d30), == ingame_menu_reader's
constexpr uint32_t OFF_PARENT_FLAGS = 0x2FF0;
constexpr uint32_t PARENT_TARGETING = 0x400;     // set by the root for kinds 0x10..0x13
constexpr uint32_t OFF_PARENT_TITLE = 0x208;     // codec* the 0x1F title arm writes

constexpr uint64_t KIND_FOES    = 0x10;
constexpr uint64_t KIND_PARTY   = 0x11;
constexpr uint64_t KIND_RESERVE = 0x12;
constexpr uint64_t KIND_ALLIES  = 0x13;

// Panel = the list struct at panel+0x4C0 (the address FUN_0031eb20 is handed), so builder index k
// (4-byte ints) lands at panel + 0x4C0 + k*4.
constexpr uint32_t OFF_LIST_KIND   = 0x4C0;   // [0x00] -- 0xF for Reserve
constexpr uint32_t OFF_ROW_COUNT   = 0x500;   // [0x10]
constexpr uint32_t OFF_ROW_ID      = 0x510;   // [0x14 + i*2] -- charId (a u8 in an int)
constexpr uint32_t OFF_ROW_FLAGS   = 0x514;   // [0x15 + i*2] -- bit 1 = not a legal target (log only)
constexpr uint32_t ROW_STRIDE      = 8;
constexpr uint32_t LISTKIND_RESERVE = 0xF;
constexpr int      MAX_ROWS        = 64;      // the command reader's own bound for this panel

constexpr int      RESERVE_FIRST_SLOT = 4;    // roster list 3: 0-2 active, 3 guest, 4-8 reserve
constexpr int      RESERVE_LAST_SLOT  = 8;

// NOT FRAME COUNTS. The switch's list arrives on the root's next update; the queue flags only have to
// outlive the title. Both are deadlines, so a flag nobody consumed can never leak into a later,
// unrelated announcement.
constexpr uint64_t SWITCH_ARM_MS = 2000;
constexpr uint64_t QUEUE_ROW_MS  = 1500;

typedef uint64_t (*Pfn_GroupSwitch)(void*, void*, void*, int);
Pfn_GroupSwitch s_origGroupSwitch = nullptr;

std::atomic<uint64_t> g_switchArmedUntil{0};   // FUN_0027b430 returned 1
std::atomic<uint64_t> g_queueRowUntil{0};      // a title just spoke; the Reserve row queues behind it
std::atomic<uint64_t> g_titleCheckKind{0};     // kind whose title was spoken, for AfterCtrlMessage
std::wstring          g_titleSpoken;           // game thread only (Before/After run on one thread)

int TitleIdForKind(uint64_t kind) {
    switch (kind) {
        case KIND_FOES:    return 0x4A4A;
        case KIND_PARTY:   return 0x4A49;
        case KIND_RESERVE: return 0x4A4B;
        case KIND_ALLIES:  return 0x4A4C;
        default:           return 0;
    }
}

void* ParentOf(void* ctrl) { return PtrAt(ctrl, OFF_CTRL_PARENT); }

bool Targeting(void* parent) {
    uint32_t flags = 0;
    return parent && SafeReadU32(parent, OFF_PARENT_FLAGS, &flags) && (flags & PARENT_TARGETING);
}

uint64_t HookedGroupSwitch(void* ctrl, void* panel, void* parent, int dir) {
    const uint64_t ret = s_origGroupSwitch ? s_origGroupSwitch(ctrl, panel, parent, dir) : 0;
    STALL_SCOPE("TargetGroup::HookedGroupSwitch");
    if (ret == 1) {
        g_switchArmedUntil.store(GetTickCount64() + SWITCH_ARM_MS, std::memory_order_relaxed);
        char m[96];
        snprintf(m, sizeof(m), "group switch accepted: dir=%d -- the next target-mode list build is spoken", dir);
        Log::Write("TGTGROUP", m);
    }
    return ret;
}

// The reserve member's BtlChr, found the way the builder found it: roster slots 4-8, matched on the
// charId the row carries. Null when none matches -- the caller then speaks the name alone.
void* ReserveBtlChr(uint32_t charId, int* slotOut) {
    for (int slot = RESERVE_FIRST_SLOT; slot <= RESERVE_LAST_SLOT; ++slot) {
        void* bc = BattleState::BtlChrForSlot(slot);
        uint8_t id = 0xFF;
        if (bc && SafeReadU8(bc, PhyreTypes::BC_CHARID, &id) && id == charId) {
            *slotOut = slot;
            return bc;
        }
    }
    *slotOut = -1;
    return nullptr;
}

} // namespace

namespace TargetGroupReader {

bool Init() {
    const bool ok = Hooks::InstallTyped(RVA_GROUP_SWITCH, &HookedGroupSwitch, &s_origGroupSwitch);
    Log::Write("TGTGROUP", ok ? "TargetGroupReader: group-switch hook installed (FUN_0027b430)"
                              : "TargetGroupReader: the group-switch hook FAILED to install");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_GROUP_SWITCH);
    g_switchArmedUntil.store(0);
    g_queueRowUntil.store(0);
    g_titleCheckKind.store(0);
}

void BeforeCtrlMessage(void* ctrl, int msgId, void* msg) {
    if (msgId != MSG_BUILD_LIST) return;
    const uint64_t armed = g_switchArmedUntil.load(std::memory_order_relaxed);
    if (armed == 0) return;
    STALL_SCOPE("TargetGroup::BeforeCtrlMessage");
    const uint64_t now = GetTickCount64();
    if (now > armed) {
        g_switchArmedUntil.store(0);
        Log::Write("TGTGROUP", "group switch expired with no target-mode list build -- nothing spoken");
        return;
    }
    // A non-target build cannot be the switched list; leave the arm for the one that is.
    if (!Targeting(ParentOf(ctrl))) return;
    g_switchArmedUntil.store(0);

    uint64_t kind = 0;
    SafeReadU64(msg, OFF_MSG_ARG0, &kind);
    const int textId = TitleIdForKind(kind);
    char m[128];
    if (textId == 0) {
        snprintf(m, sizeof(m), "list build kind=0x%llX is not a target group -- no title",
                 static_cast<unsigned long long>(kind));
        Log::Write("TGTGROUP", m);
        return;
    }
    std::wstring title = TextCapture::ResolveStringById(textId);   // game thread: the call is legal here
    if (title.empty()) {
        snprintf(m, sizeof(m), "title id 0x%X for kind 0x%llX resolved empty -- silent",
                 textId, static_cast<unsigned long long>(kind));
        Log::Write("TGTGROUP", m);
        return;
    }

    snprintf(m, sizeof(m), "group title kind=0x%llX id=0x%X:", static_cast<unsigned long long>(kind), textId);
    Log::WriteW("TGTGROUP", m, title);
    Speech::Output(title, /*interrupt=*/true);

    // Whatever the new list lands on queues behind the title instead of cutting it off. A Reserve row
    // is spoken by this reader; every other group's highlight is a field unit the nameplate path owns,
    // and it is re-armed so it speaks even when the switch landed on the unit it last named.
    if (kind == KIND_RESERVE)
        g_queueRowUntil.store(now + QUEUE_ROW_MS, std::memory_order_relaxed);
    else
        BattleTargetReader::ReannounceQueued();

    g_titleSpoken = title;
    g_titleCheckKind.store(kind, std::memory_order_relaxed);
}

void AfterCtrlMessage(void* ctrl, int msgId) {
    if (msgId != MSG_BUILD_LIST) return;
    const uint64_t kind = g_titleCheckKind.exchange(0, std::memory_order_relaxed);
    if (kind == 0) return;
    // LOG ONLY -- the falsifier for the kind->title table above. The game has now written its own title.
    const uint8_t* codec = static_cast<const uint8_t*>(PtrAt(ParentOf(ctrl), OFF_PARENT_TITLE));
    std::wstring written = codec ? GameText::Decode(codec, 64) : std::wstring();
    char m[96];
    snprintf(m, sizeof(m), "title check kind=0x%llX: game wrote", static_cast<unsigned long long>(kind));
    Log::WriteW("TGTGROUP", m, written + (written == g_titleSpoken ? L" -- matches" : L" -- MISMATCH with the spoken title"));
}

bool OnPanelFocus(void* panel, int index) {
    if (IngameMenuReader::BattleListDrawCallback(panel) != Hooks::ResolveRva(RVA_DRAW_RESERVE)) return false;
    STALL_SCOPE("TargetGroup::OnPanelFocus");
    // From here the panel is the Reserve list's by its own draw: claimed, even when it stays silent, so
    // the command reader never tries a command-name resolve on a charId.
    uint32_t listKind = 0, rowId = 0, rowFlags = 0;
    int count = 0;
    SafeReadU32(panel, OFF_LIST_KIND, &listKind);
    SafeReadInt(static_cast<char*>(panel) + OFF_ROW_COUNT, &count);
    char m[192];
    if (listKind != LISTKIND_RESERVE || index < 0 || count <= 0 || count > MAX_ROWS || index >= count ||
        !SafeReadU32(panel, OFF_ROW_ID + static_cast<uint32_t>(index) * ROW_STRIDE, &rowId) || rowId >= 0x28) {
        snprintf(m, sizeof(m), "reserve row NOT spoken: listKind=0x%X index=%d count=%d rowId=0x%X",
                 listKind, index, count, rowId);
        Log::Write("TGTGROUP", m);
        return true;
    }
    SafeReadU32(panel, OFF_ROW_FLAGS + static_cast<uint32_t>(index) * ROW_STRIDE, &rowFlags);

    std::wstring text = BattleState::CharacterName(static_cast<uint8_t>(rowId));
    if (text.empty()) {
        snprintf(m, sizeof(m), "reserve row NOT spoken: charId=%u has no name", rowId);
        Log::Write("TGTGROUP", m);
        return true;
    }
    int slot = -1;
    if (void* bc = ReserveBtlChr(rowId, &slot)) text += BattleTargetReader::AllyHpClause(bc);

    const uint64_t until = g_queueRowUntil.exchange(0, std::memory_order_relaxed);
    const bool queue = until != 0 && GetTickCount64() <= until;

    snprintf(m, sizeof(m), "reserve row %d/%d charId=%u slot=%d flags=0x%X%s:",
             index, count, rowId, slot, rowFlags, queue ? " (queued behind the title)" : "");
    Log::WriteW("TGTGROUP", m, text);
    if (queue) Speech::SpeakQueued(text);
    else       Speech::Output(text, /*interrupt=*/true);
    return true;
}

} // namespace TargetGroupReader
