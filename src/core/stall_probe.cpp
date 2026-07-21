#include "core/stall_probe.h"
#include "core/logger.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>

namespace StallProbe {
namespace {

// A single slow invocation at or above this is logged the moment it happens, named. Well below a
// 16 ms frame, well above anything our hooks should ever legitimately cost.
constexpr double kStallMs = 2.0;

constexpr int kMaxCounters = 64;
constexpr int kMaxThreadPairs = 96;

// Keyed by the string LITERAL's pointer, not its contents -- every call site passes the same
// literal, so identity comparison is correct and costs one compare. No allocation, no map, nothing
// that could itself become the stall we are hunting.
struct Counter {
    std::atomic<const char*> name{nullptr};
    std::atomic<uint64_t>    calls{0};
    std::atomic<uint64_t>    ticks{0};      // total, self time
    std::atomic<uint64_t>    maxTicks{0};
    std::atomic<uint64_t>    waitTicks{0};  // total blocked acquiring a lock
    std::atomic<uint64_t>    waitCalls{0};
};
Counter g_counters[kMaxCounters];

// (hook literal, thread id) pairs already reported, so NoteThread is O(unique) not O(calls).
struct ThreadPair { std::atomic<const char*> name{nullptr}; std::atomic<uint32_t> tid{0}; };
ThreadPair g_pairs[kMaxThreadPairs];

int64_t g_freq = 0;

double MsOf(uint64_t ticks) {
    if (g_freq == 0) QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&g_freq));
    return g_freq ? (static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_freq)) : 0.0;
}

// Find or claim the slot for `name`. Returns nullptr only if the table is full, in which case the
// probe silently stops recording that hook rather than growing or blocking.
Counter* SlotFor(const char* name) {
    for (int i = 0; i < kMaxCounters; ++i) {
        const char* n = g_counters[i].name.load(std::memory_order_acquire);
        if (n == name) return &g_counters[i];
        if (n == nullptr) {
            const char* expected = nullptr;
            if (g_counters[i].name.compare_exchange_strong(expected, name)) return &g_counters[i];
            if (g_counters[i].name.load(std::memory_order_acquire) == name) return &g_counters[i];
        }
    }
    return nullptr;
}

} // namespace

int64_t Now() {
    int64_t t = 0;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&t));
    return t;
}

Scope::Scope(const char* name) : m_name(name), m_t0(Now()) {}

Scope::~Scope() {
    const uint64_t d = static_cast<uint64_t>(Now() - m_t0);
    Counter* c = SlotFor(m_name);
    if (!c) return;
    c->calls.fetch_add(1, std::memory_order_relaxed);
    c->ticks.fetch_add(d, std::memory_order_relaxed);
    uint64_t prevMax = c->maxTicks.load(std::memory_order_relaxed);
    while (d > prevMax && !c->maxTicks.compare_exchange_weak(prevMax, d)) {}

    const double ms = MsOf(d);
    if (ms >= kStallMs) {
        char line[160];
        snprintf(line, sizeof(line), "STALL %s %.1fms (single call)", m_name, ms);
        Log::Write("PERF", line);
    }
}

void AddWait(const char* name, int64_t startTicks) {
    const uint64_t d = static_cast<uint64_t>(Now() - startTicks);
    Counter* c = SlotFor(name);
    if (!c) return;
    c->waitCalls.fetch_add(1, std::memory_order_relaxed);
    c->waitTicks.fetch_add(d, std::memory_order_relaxed);
    const double ms = MsOf(d);
    if (ms >= kStallMs) {
        char line[160];
        snprintf(line, sizeof(line), "STALL %s %.1fms BLOCKED on lock", name, ms);
        Log::Write("PERF", line);
    }
}

void NoteThread(const char* name) {
    const uint32_t tid = GetCurrentThreadId();
    for (int i = 0; i < kMaxThreadPairs; ++i) {
        const char* n = g_pairs[i].name.load(std::memory_order_acquire);
        if (n == name && g_pairs[i].tid.load(std::memory_order_relaxed) == tid) return;  // seen
        if (n == nullptr) {
            const char* expected = nullptr;
            if (g_pairs[i].name.compare_exchange_strong(expected, name)) {
                g_pairs[i].tid.store(tid, std::memory_order_relaxed);
                char line[160];
                snprintf(line, sizeof(line), "thread: %s runs on tid=%lu",
                         name, static_cast<unsigned long>(tid));
                Log::Write("PERF", line);
                return;
            }
            --i;   // lost the race for this slot; re-examine it
        }
    }
}

void FrameTick(double gapWarnMs) {
    static int64_t s_last = 0;
    const int64_t now = Now();
    if (s_last != 0) {
        const double gap = MsOf(static_cast<uint64_t>(now - s_last));
        if (gap >= gapWarnMs) {
            char line[160];
            snprintf(line, sizeof(line), "FRAME GAP %.1fms -- breakdown of that window follows", gap);
            Log::Write("PERF", line);
            DumpAndReset("frame gap", 0.0);
        }
    }
    s_last = now;
}

void DumpAndReset(const char* reason, double minTotalMs) {
    // Cheap pre-pass: is this window even worth reporting? Reading is enough -- the reset happens
    // below either way, so a quiet window is zeroed without touching the log.
    if (minTotalMs > 0.0) {
        uint64_t total = 0;
        for (int i = 0; i < kMaxCounters; ++i) {
            if (!g_counters[i].name.load(std::memory_order_acquire)) continue;
            total += g_counters[i].ticks.load(std::memory_order_relaxed);
            total += g_counters[i].waitTicks.load(std::memory_order_relaxed);
        }
        if (MsOf(total) < minTotalMs) {
            for (int i = 0; i < kMaxCounters; ++i) {
                if (!g_counters[i].name.load(std::memory_order_acquire)) continue;
                g_counters[i].calls.store(0, std::memory_order_relaxed);
                g_counters[i].ticks.store(0, std::memory_order_relaxed);
                g_counters[i].maxTicks.store(0, std::memory_order_relaxed);
                g_counters[i].waitTicks.store(0, std::memory_order_relaxed);
                g_counters[i].waitCalls.store(0, std::memory_order_relaxed);
            }
            return;
        }
    }
    bool any = false;
    for (int i = 0; i < kMaxCounters; ++i) {
        Counter& c = g_counters[i];
        const char* n = c.name.load(std::memory_order_acquire);
        if (!n) continue;
        const uint64_t calls = c.calls.exchange(0, std::memory_order_relaxed);
        const uint64_t ticks = c.ticks.exchange(0, std::memory_order_relaxed);
        const uint64_t mx    = c.maxTicks.exchange(0, std::memory_order_relaxed);
        const uint64_t wt    = c.waitTicks.exchange(0, std::memory_order_relaxed);
        const uint64_t wc    = c.waitCalls.exchange(0, std::memory_order_relaxed);
        if (calls == 0 && wc == 0) continue;
        if (!any) {
            char hdr[128];
            snprintf(hdr, sizeof(hdr), "==== since last %s ====", reason ? reason : "dump");
            Log::Write("PERF", hdr);
            any = true;
        }
        char line[224];
        snprintf(line, sizeof(line),
                 "  %-28s calls=%-7llu self=%8.2fms max=%7.2fms  lockwait=%7.2fms/%llu",
                 n, (unsigned long long)calls, MsOf(ticks), MsOf(mx), MsOf(wt),
                 (unsigned long long)wc);
        Log::Write("PERF", line);
    }
}

} // namespace StallProbe
