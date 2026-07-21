#pragma once
#include <string>

namespace Log {

void Init(const std::string& gameDir);
void Shutdown();
void Write(const char* category, const char* message);

// Log game-supplied text. Emits `<prefix>"<text>"` under `category`, converting the wide
// string to UTF-8 once here rather than at each call site.
//
// This is THE place wide text becomes log bytes. Three files had grown byte-identical private
// `LogLine` helpers (menu / in-game menu / message readers) with their own buffers and their own
// truncation limits; a fourth would have followed. If you need a variant, add an overload here.
void WriteW(const char* category, const char* prefix, const std::wstring& text);

// Same, with an object pointer between the prefix and the text: `<prefix> owner=<p> "<text>"`.
// The menu readers use it to tie a spoken row to the surface that owns it.
void WriteW(const char* category, const char* prefix, const void* owner, const std::wstring& text);

// Wide -> UTF-8 into a caller buffer, always NUL-terminated, truncating rather than failing.
// For log lines that interleave the text with other fields ("handle=0x%x %s \"%s\"") and so do not
// fit either WriteW shape — use this + snprintf + Write rather than another private
// WideCharToMultiByte with its own cap arithmetic. Do NOT add a WriteW overload per format.
void ToUtf8(const std::wstring& text, char* out, size_t cap);

const std::string& GetGameDir();

uint64_t GetStartTick();

} // namespace Log
