#include "speech/speech.h"
#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <thread>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
}
int main() {
#ifdef OMIT_TOLK_LOAD
    Check(!Speech::Init(), "missing required export refuses initialization");
    Check(GetModuleHandleW(L"Tolk.dll") == nullptr, "failed initialization releases DLL");
    const auto count = Speech::UtteranceCount();
    Speech::Output(L"after failed initialization");
    Speech::SpeakQueued(L"after failed initialization");
    Speech::Raw(L"after failed initialization");
    Speech::Silence();
    Check(!Speech::IsAvailable(), "failed backend is unavailable");
    Check(Speech::GetScreenReaderName().empty(), "failed backend has no live name pointer");
    Check(Speech::UtteranceCount() == count, "failed backend cannot count output");
    Speech::Shutdown();
    Speech::Shutdown();
#else
    Check(Speech::Init(), "initialize fake backend");
    HMODULE backend = GetModuleHandleW(L"Tolk.dll");
    if (!backend) return 1;
    const auto outputs = reinterpret_cast<int(__cdecl*)()>(GetProcAddress(backend, "TestOutputCount"));
    const auto interrupt = reinterpret_cast<bool(__cdecl*)()>(GetProcAddress(backend, "TestLastInterrupt"));
    const auto accept = reinterpret_cast<void(__cdecl*)(bool)>(GetProcAddress(backend, "TestAccept"));
    if (!outputs || !interrupt || !accept) return 1;
    Check(Speech::Init(), "repeated initialization is idempotent");
    Check(Speech::IsAvailable(), "initialized fake backend is available");
    Speech::SetEnabled(true);
    const auto count = Speech::UtteranceCount();
    Speech::Speak(L"same valid event");
    Check(interrupt(), "Speak preserves interrupt policy");
    Speech::SpeakQueued(L"same valid event");
    Check(!interrupt(), "SpeakQueued preserves queue policy");
    Speech::Output(L"same valid event", false);
    Speech::Raw(L"same valid event", true);
    Check(outputs() == 4 && Speech::UtteranceCount() == count + 4,
          "every speech API counts acceptance; identical events remain distinct");
    Speech::SetEnabled(false);
    Speech::Speak(L"muted"); Speech::SpeakQueued(L"muted"); Speech::Output(L"muted");
    Check(outputs() == 4, "normal APIs honor mute");
    Speech::Raw(L"mute acknowledgement");
    Check(outputs() == 5 && Speech::UtteranceCount() == count + 5, "Raw retains mute bypass");
    Speech::SetEnabled(true);
    accept(false);
    Speech::Output(L"rejected");
    Check(Speech::UtteranceCount() == count + 5, "rejected output is not counted as accepted");
    accept(true);

    std::atomic<bool> started{false};
    std::thread caller([&] {
        started.store(true);
        for (int i = 0; i < 500; ++i) {
            Speech::Output(L"lifecycle race");
            Speech::IsAvailable();
            Speech::GetScreenReaderName();
            Speech::Silence();
        }
    });
    while (!started.load()) std::this_thread::yield();
    Speech::Shutdown();
    caller.join();
    Speech::Shutdown();
    Check(GetModuleHandleW(L"Tolk.dll") == nullptr, "idempotent initialization does not leak a DLL reference");
    Check(!Speech::IsAvailable(), "shutdown backend is unavailable");
    Check(Speech::GetScreenReaderName().empty(), "shutdown does not retain export pointers");
    const auto stoppedCount = Speech::UtteranceCount();
    Speech::Output(L"after shutdown"); Speech::Raw(L"after shutdown"); Speech::Silence();
    Check(Speech::UtteranceCount() == stoppedCount, "shutdown output cannot reach unloaded exports");
#endif
    if (failures) return 1;
    std::puts("Speech regression tests passed.");
    return 0;
}
