#pragma once

#include <string>
#include <cstdint>

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

// How many utterances have actually reached the screen reader. Monotonic, never reset. Lets a caller
// ask "has anything been spoken since X?" -- used by AUTO DETAIL so a volunteered description can
// never get ahead of the row line it belongs behind, whichever reader spoke that line and however it
// was deferred. Reading it changes nothing; it gates only the mod's own volunteered extra.
uint64_t UtteranceCount();

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

// NOTE: there is deliberately no MaybeAnnounce()/"speak only if changed" helper here. Speech is
// never deduplicated — see the no-dedup rule in CLAUDE.md. The one legitimate exception (a
// change-check guarding a per-frame game function) is written inline at the call site with a
// comment naming that function, so it stays visible and reviewable instead of hiding behind a
// convenience helper.

void Shutdown();

} // namespace Speech
