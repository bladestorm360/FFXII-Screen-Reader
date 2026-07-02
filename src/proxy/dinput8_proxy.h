#pragma once

// dinput8.dll proxy. Our DLL ships as `dinput8.dll` next to FFXII_TZA.exe.
// Windows' loader picks ours up before searching System32 (Windows DLL search
// order). We forward all dinput8 exports to the real `C:\Windows\System32\dinput8.dll`
// so the game's DirectInput initialization works identically to no-mod.
//
// Stage A (proxy bootstrap) loads System32's dinput8 and resolves its exports.
// Stage B (full mod init) is started from a background thread and handles
// Tolk, MinHook, RVA validation, hotkey hook, and screen-reader features.

namespace DInput8Proxy {

// Lazy-load System32\dinput8.dll and resolve its exported function pointers.
// Idempotent + thread-safe (uses CAS on a flag). Returns true on success.
bool Init();

// Free the loaded System32 DLL. Called at DLL_PROCESS_DETACH.
void Shutdown();

} // namespace DInput8Proxy
