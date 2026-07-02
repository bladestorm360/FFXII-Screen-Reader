#pragma once

// Bridges MenuObserver focus-change events to TextCapture lookups, then to
// Speech::Output. By design contains no option-name strings — speaks only
// the bytes captured from the game's text-wrapper layer.
namespace MenuReader {

bool Init();
void Shutdown();

} // namespace MenuReader
