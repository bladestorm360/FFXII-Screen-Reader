#include "core/hooks.h"
#include "core/message_macro.h"
#include "core/stall_probe.h"
#include "runtime_stubs.h"

#include <atomic>
#include <thread>

namespace {
std::atomic<bool> pauseNext{false}, lookupStarted{false}, lookupReleased{false};
std::atomic<int> lookupCount{0};
}

namespace TestRuntime {
void PauseNextLookup() {
    lookupStarted.store(false);
    lookupReleased.store(false);
    pauseNext.store(true);
}
bool LookupStarted() { return lookupStarted.load(); }
void ReleaseLookup() { lookupReleased.store(true); }
int LookupCount() { return lookupCount.load(); }
}

// Exercise the actual decoder/logger without a game process, hook installs or
// the screen reader. Only external runtime services are stubbed.
namespace Hooks {
void* ResolveRva(uint32_t) {
    ++lookupCount;
    if (pauseNext.exchange(false)) {
        lookupStarted.store(true);
        while (!lookupReleased.load()) std::this_thread::yield();
    }
    return nullptr;
}
}
namespace MessageMacro {
bool ValueAt(int index, int32_t* out, int* slot) {
    if (slot) *slot = 0;
    if (index < 0 || index > 2) return false;
    *out = index * 7;
    return true;
}
}
namespace StallProbe {
Scope::Scope(const char* name) : m_name(name), m_t0(0) {}
Scope::~Scope() = default;
void AddLoggerSample(int64_t, int64_t) {}
void NoteThread(const char*) {}
int64_t Now() { return 0; }
}
