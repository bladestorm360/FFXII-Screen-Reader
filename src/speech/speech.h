#pragma once

#include <string>

namespace Speech {

// Initialize Tolk by loading Tolk.dll at runtime. The DLL is user-supplied —
// the user manually deploys Tolk.dll + nvdaControllerClient64.dll to the game's
// x64\ folder. If Tolk.dll is absent, this returns false; the mod continues
// silently (Speech::IsAvailable() returns false; all speak calls are no-ops).
bool Init();

// Speak text through the screen reader. If interrupt is true, cancels current
// speech first.
void Speak(const std::wstring& text, bool interrupt = true);

// Speak text without interrupting current speech (queued).
void SpeakQueued(const std::wstring& text);

// Output text to both speech and braille (always preferred — never use
// Tolk_Speak directly; braille displays depend on Output).
void Output(const std::wstring& text, bool interrupt = true);

// Cancel any current speech.
void Silence();

// Tolk loaded and a screen reader (or SAPI fallback) is available.
bool IsAvailable();

// Name of the detected screen reader (or empty if none).
std::wstring GetScreenReaderName();

// Mute toggle. Frida coexistence: Frida calls Tolk directly and is unaffected.
void SetEnabled(bool enabled);
bool IsEnabled();
void ToggleEnabled();  // flips + announces state (announcement bypasses mute)

// Output text bypassing mute state — for announcements that must always be
// heard/brailled (e.g. mute-toggle confirmations).
void Raw(const std::wstring& text, bool interrupt = true);

// Polled-monitor dedup helper: speak `text` only if it differs from `cached`,
// then update `cached`. Returns true if a new announcement was made.
// Use ONLY for unavoidably polled monitors that have no event-driven hook.
bool MaybeAnnounce(const std::wstring& text, std::wstring& cached, bool interrupt = true);

void Shutdown();

} // namespace Speech
