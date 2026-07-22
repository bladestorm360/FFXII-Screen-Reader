#include "ui/popup_reader.h"
#include "ui/menu_state.h"
#include "ui/message_reader.h"
#include "ui/text_capture.h"
#include "core/game_text.h"
#include "core/mem_read.h"

#include <cstdint>

namespace {

constexpr uint32_t OFF_POPUP_BODY = 0x1B0;     // FUN_00241d40 -> inline codec body prompt

constexpr int BUTTON_ID_YES = 1000;            // the game's own registered button string ids
constexpr int BUTTON_ID_NO  = 1001;

std::wstring DecodeAt(void* base, uint32_t off, size_t cap) {
    if (!base) return std::wstring();
    std::wstring s = GameText::Decode(
        reinterpret_cast<const uint8_t*>(reinterpret_cast<char*>(base) + off), cap);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
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

std::wstring ButtonText(int index) {
    return TextCapture::StringById(index == 0 ? BUTTON_ID_YES : BUTTON_ID_NO);
}

} // namespace PopupReader
