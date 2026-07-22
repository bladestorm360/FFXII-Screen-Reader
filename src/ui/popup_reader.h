#pragma once
#include <string>

// Turns an identified confirm-prompt into TEXT — its body prompt and its button labels. Surface
// IDENTITY for both prompt classes lives in ui/menu_state.h; this file only reads their content.
//
//   * FUN_00241d40 — the body is an inline codec at +0x1b0.
//   * FUN_002cdf20 — a generic Yes/No prompt that stores no text of its own: it hands the composed
//     message to a FUN_0057c480 surface kept at prompt+0xc0 (also menuCtx+0x328), whose 0x400-byte
//     codec buffer at +0x1B0 is the read-point message_reader.cpp derived. Reading that composed
//     result covers EVERY prompt variant — including the ones that format a parameter in via
//     FUN_002b4090, which a message-id mapping could never reproduce.
//
// Buttons are registered by the game as string ids 1000 (Yes) / 1001 (No), captured passively by
// TextCapture, so the labels are the game's own localized text.
namespace PopupReader {

// The prompt's body text; empty when `owner` is not a prompt or the text is unreadable.
std::wstring BodyText(void* owner);

// Label of the prompt's button at `index` (0 = Yes, anything else = No).
std::wstring ButtonText(int index);

} // namespace PopupReader
