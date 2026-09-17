#pragma once

#include <string>

// THE MOD'S OWN DIALOGS -- a real Win32 edit field and a real Yes/No box, for the two questions the
// mod has to ask the player in words rather than answer in speech.
//
// WHY THIS EXISTS AT ALL, AND WHY IT IS NOT A KEY-CAPTURE (Session 185).
// The mod is strictly read-only on the keyboard: it inspects the `const` DirectInput buffer the game
// polls and never swallows a key (CLAUDE.md's input rule, Docs/Controls.md). That is why `F6` used to
// name an entity from the CLIPBOARD -- there was no way to run a text field *inside* the game.
//
// A separate top-level window sidesteps that completely instead of bending it. Typing into a Win32
// EDIT control is ordinary window-message input; it never touches the DirectInput buffer, so the
// read-only rule is not weakened by one byte. The game's own device is simply unacquired while
// another window of the process holds focus, exactly as it is during an alt-tab.
//
// WHY A REAL DIALOG (class #32770) AND NOT A POPUP WINDOW WITH CHILDREN. A screen reader announces a
// dialog's title, its static text and its focused control the moment it opens; a plain popup gets the
// focused control alone and the player has to go looking for the rest. The template is built in memory
// because this project has no .rc file, which is the only reason the byte-packing below exists.
//
// THREADING: every prompt runs on a thread OF ITS OWN and the callback fires there.
//   * NOT the game thread -- a modal loop on it would freeze the game, which needs express
//     permission the mod does not have (memory: NEVER STALL THE GAME THREAD).
//   * NOT the InputTracker thread either. That thread is the mod's hotkey dispatcher; blocking it in
//     a modal loop would queue every other key behind the dialog and stall `Shutdown`'s WM_QUIT.
// One prompt at a time: `Busy()` is the interlock, and while it is true the keyboard feed is fed a
// zeroed buffer (input_tracker.cpp) so no mod hotkey can fire at the keys being typed.
namespace TextPrompt {

enum class Result { Ok, Cancel };

// Fired on the prompt's own thread once the dialog closes. `text` carries the typed string for
// `AskText` and is empty for `AskYesNo`, whose answer is the `Result`. The callback may take the
// entity-list mutex and read game memory -- it is an ordinary worker thread, the same footing the
// InputTracker thread has always had.
typedef void (*Callback)(Result result, const std::wstring& text, void* ctx);

// True while a prompt is on screen. The keyboard feed reads this every poll, so it is a relaxed
// atomic load and free to call per frame.
bool Busy();

// A one-line edit field with OK and Cancel. `initial` pre-fills it and is selected, so typing
// replaces. Returns false -- having done nothing and NOT called `cb` -- when a prompt is already up
// or the dialog could not be created.
bool AskText(const std::wstring& title, const std::wstring& label, const std::wstring& initial,
             Callback cb, void* ctx);

// A Yes/No box. `Result::Ok` is Yes. Same false-means-nothing-happened contract as `AskText`.
bool AskYesNo(const std::wstring& title, const std::wstring& question, Callback cb, void* ctx);

// Closes an open prompt and waits briefly for its thread, so DLL detach cannot race a live window.
void Shutdown();

} // namespace TextPrompt
