#include <atomic>
#include <Windows.h>

namespace {
bool loaded = false;
bool accepting = true;
bool lastInterrupt = false;
int outputs = 0;
}
#define EXPORT extern "C" __declspec(dllexport)
#ifndef OMIT_TOLK_LOAD
EXPORT void __cdecl Tolk_Load() { loaded = true; }
#endif
EXPORT void __cdecl Tolk_Unload() { loaded = false; }
EXPORT bool __cdecl Tolk_IsLoaded() { return loaded; }
EXPORT bool __cdecl Tolk_Output(const wchar_t*, bool interrupt) {
    ++outputs;
    lastInterrupt = interrupt;
    return accepting;
}
EXPORT bool __cdecl Tolk_Silence() { return true; }
EXPORT const wchar_t* __cdecl Tolk_DetectScreenReader() { return L"Test backend"; }
EXPORT int __cdecl TestOutputCount() { return outputs; }
EXPORT bool __cdecl TestLastInterrupt() { return lastInterrupt; }
EXPORT void __cdecl TestAccept(bool value) { accepting = value; }
