#pragma once

// Bridges the FUN_00247510 msg-0x8000 focus event to TextCapture lookups, then to
// Speech::Output. By design contains no option-name strings — speaks only
// the bytes captured from the game's text-wrapper layer.
//
// Surface identity and the focused-pane test live in ui/menu_state.h; config row
// values in ui/config_reader.h.
namespace MenuReader {

bool Init();
void Shutdown();

} // namespace MenuReader
