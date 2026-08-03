#include "ui/popup_reader.h"
#include "ui/menu_state.h"
#include "ui/message_reader.h"
#include "ui/text_capture.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "speech/phrasebook.h"
#include "speech/speech.h"

#include <cstdint>
#include <cstdio>

namespace {

constexpr uint32_t OFF_POPUP_BODY = 0x1B0;     // FUN_00241d40 -> inline codec body prompt

constexpr int BUTTON_ID_YES = 1000;            // the game's own registered button string ids
constexpr int BUTTON_ID_NO  = 1001;

// ---- the NO-LIST prompt (Game Over, and every sibling that has no buttons) ---------------------
// A confirm window built by FUN_00241d40 comes in two shapes, and the game itself branches on the
// flag it copies from its creation packet's +0x2C into win+0x3C4:
//
//     FUN_00241d40 case 1, :88   if ((*(byte *)(param_1 + 0x3c4) & 1) == 0) { ...
//                                    *(longlong *)(param_1 + 200) = FUN_002d14e0(...);  // the LIST
//
// bit 0 CLEAR  -> it builds a button list at win+0xC8, the cursor lands on it, and FUN_00247510
//                 emits focus msg 0x8000. That is the path menu_reader already covers.
// bit 0 SET    -> NO list is built (win+0xC8 stays 0), so no 0x8000 is EVER emitted, OnFocus never
//                 runs, and BodyText is never consulted. The window is therefore completely silent
//                 today -- which is the whole reason Game Over says nothing while the Load Game
//                 menu reached from it works fine.
//
// The two shapes are disjoint by the game's own branch, so this can never double-speak with the
// focus path and needs no latch or dedup. It fixes every no-list prompt in the game, not just
// Game Over.
constexpr uint32_t RVA_CONFIRM_WND = 0x121D40;  // FUN_00241d40(win, msgPacket)
constexpr uint32_t OFF_POPUP_FLAGS = 0x3C4;     // win+0x3C4, bit 0 = no button list
constexpr uint32_t MSG_CONSTRUCT   = 1;         // *packet == 1 is case 1, where +0x1B0 is filled

// DAT_022c83e8 -- the game-over state bitfield. Bit 1 is party wipe / GAME OVER; these are the
// bodies of the game's own getters, so they are reads of a documented global, not inference:
//     FUN_0035c740  return DAT_022c83e8 >> 1 & 1;   party wipe  (GAME OVER)
//     FUN_0035c750  return DAT_022c83e8 >> 4 & 1;   leader down
//     FUN_0035c760  return DAT_022c83e8      & 1;   guest wipe
constexpr uint32_t RVA_GAMEOVER_STATE = 0x21A83E8;
constexpr uint32_t GO_BIT_PARTY_WIPE  = 0x02;

typedef uint64_t (*Pfn_ConfirmWnd)(void*, uint32_t*);
Pfn_ConfirmWnd s_origConfirmWnd = nullptr;

std::wstring DecodeAt(void* base, uint32_t off, size_t cap) {
    if (!base) return std::wstring();
    std::wstring s = GameText::Decode(
        reinterpret_cast<const uint8_t*>(reinterpret_cast<char*>(base) + off), cap);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// "GAME OVER" is baked texture art (PS2Data\...\Tm2_Menu\gameover_c.tm2), exactly like the title
// logo -- there is no codec text anywhere in the binary to read. So the announcement is gated on
// the game's own STATE rather than on the picture: only the party-wipe variant says it, and the
// leader-down and guest-wipe variants fall through to their own bodies with nothing added.
bool PartyWipedOut() {
    uint32_t state = 0;
    if (!MemRead::SafeReadU32(Hooks::ResolveRva(RVA_GAMEOVER_STATE), 0, &state)) return false;
    return (state & GO_BIT_PARTY_WIPE) != 0;
}

// FUN_00241d40(win, packet). Case 1 is construction, and it is the call that copies the body codec
// into win+0x1B0 (`FUN_00254f30(param_1 + 0x1b0, *puVar4, 0x200)`), so we must read AFTER it.
uint64_t HookedConfirmWnd(void* win, uint32_t* packet) {
    STALL_SCOPE("PopupReader::HookedConfirmWnd");
    const uint64_t ret = s_origConfirmWnd ? s_origConfirmWnd(win, packet) : 0;

    uint32_t msg = 0;
    if (!win || !packet || !MemRead::SafeReadU32(packet, 0, &msg) || msg != MSG_CONSTRUCT)
        return ret;

    uint32_t flags = 0;
    if (!MemRead::SafeReadU32(win, OFF_POPUP_FLAGS, &flags)) return ret;
    if ((flags & 1) == 0) return ret;               // has a list -> menu_reader's focus path owns it

    const bool wiped = PartyWipedOut();
    if (wiped) Speech::Output(Phrase::Get(Phrase::Id::GameOver), /*interrupt=*/true);

    // SpeakBody logs the text it speaks. This line records the STRUCTURE, which is the self-check
    // standing in for a confirmation probe: `spoke=0` means the +0x1B0 read is wrong for this
    // variant and we stayed silent rather than guessing.
    const bool spoke = PopupReader::SpeakBody(win);
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "nolist flags=0x%X wipe=%d spoke=%d", flags, wiped ? 1 : 0, spoke ? 1 : 0);
    Log::Write("POPUP", hdr);
    return ret;
}

} // namespace

namespace PopupReader {

std::wstring BodyText(void* owner) {
    if (MenuState::IsConfirmWindow(owner)) return DecodeAt(owner, OFF_POPUP_BODY, 0x200);
    // FUN_002cdf20 stores no text of its own, and its surface buffer is only readable at the
    // surface's case-1 birth — long before this focus. MessageReader captures it there; consume it.
    if (MenuState::IsChoicePopup(owner)) return MessageReader::TakeConfirmPrompt();
    return std::wstring();
}

bool SpeakBody(void* owner) {
    const std::wstring body = BodyText(owner);
    if (body.empty()) return false;
    Log::WriteW("READER", "  body: ", body);
    Speech::Output(body, /*interrupt=*/true);
    return true;
}

std::wstring ButtonText(int index) {
    return TextCapture::StringById(index == 0 ? BUTTON_ID_YES : BUTTON_ID_NO);
}

void Init() {
    if (Hooks::InstallTyped(RVA_CONFIRM_WND, &HookedConfirmWnd, &s_origConfirmWnd))
        Log::Write("POPUP", "no-list prompt hook installed (FUN_00241d40)");
    else
        Log::Write("POPUP", "FAILED to install FUN_00241d40 hook -- no-list prompts stay silent");
}

void Shutdown() {
    Hooks::Uninstall(RVA_CONFIRM_WND);
    s_origConfirmWnd = nullptr;
}

} // namespace PopupReader
