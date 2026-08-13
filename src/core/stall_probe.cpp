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

// RATE LIMIT. A probe that floods the log becomes the problem it is hunting: a mis-scoped per-frame
// hook fired ~30 STALL lines/second into a game-thread fprintf, which is exactly the class of defect
// under investigation. Each scope may report at most once per this interval; the aggregate dump
// still carries the full counts, so nothing is lost -- only the repetition.
constexpr uint64_t kStallLogIntervalMs = 1000;

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
    std::atomic<uint64_t>    lastLogMs{0};  // rate limit, per scope
    std::atomic<uint64_t>    suppressed{0}; // breaches swallowed since the last report
};
Counter g_counters[kMaxCounters];

// PER-THREAD BREADCRUMB: the last scope entered and the last one exited, per thread.
//
// When the game thread blocks somewhere we do NOT instrument, no scope reports -- the log simply
// goes silent, which is what happened at the party-menu freeze. The last mod code that ran before
// the silence is then the only evidence there is, and it was being thrown away. thread_local, so
// recording costs one store and needs no synchronisation.
struct Crumb { const char* entered = nullptr; const char* exited = nullptr; int64_t enteredAt = 0; };
thread_local Crumb t_crumb;

// Gap anchors get their OWN storage. The first version parked the previous-tick stamp in
// Counter::maxTicks, which DumpAndReset zeroes -- so every pane change reset the anchor and almost
// every gap went uncomputed (2 fired in an 8.5-minute session). An instrument the reporting path can
// silently disarm is worse than none.
constexpr int kMaxAnchors = 16;
struct Anchor { std::atomic<const char*> name{nullptr}; std::atomic<int64_t> lastTick{0}; };
Anchor g_anchors[kMaxAnchors];

Anchor* AnchorFor(const char* name) {
    for (int i = 0; i < kMaxAnchors; ++i) {
        const char* n = g_anchors[i].name.load(std::memory_order_acquire);
        if (n == name) return &g_anchors[i];
        if (n == nullptr) {
            const char* expected = nullptr;
            if (g_anchors[i].name.compare_exchange_strong(expected, name)) return &g_anchors[i];
            if (g_anchors[i].name.load(std::memory_order_acquire) == name) return &g_anchors[i];
        }
    }
    return nullptr;
}

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

// True at most once per kStallLogIntervalMs for this counter; counts what it swallows so the
// suppression is visible rather than silent.
bool ShouldLogStall(Counter* c) {
    const uint64_t now = GetTickCount64();
    const uint64_t last = c->lastLogMs.load(std::memory_order_relaxed);
    if (last != 0 && now - last < kStallLogIntervalMs) {
        c->suppressed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    c->lastLogMs.store(now, std::memory_order_relaxed);
    return true;
}

} // namespace

int64_t Now() {
    int64_t t = 0;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&t));
    return t;
}

Scope::Scope(const char* name) : m_name(name), m_t0(Now()) {
    t_crumb.entered = name;
    t_crumb.enteredAt = m_t0;
}

Scope::~Scope() {
    t_crumb.exited = m_name;
    const uint64_t d = static_cast<uint64_t>(Now() - m_t0);
    Counter* c = SlotFor(m_name);
    if (!c) return;
    c->calls.fetch_add(1, std::memory_order_relaxed);
    c->ticks.fetch_add(d, std::memory_order_relaxed);
    uint64_t prevMax = c->maxTicks.load(std::memory_order_relaxed);
    while (d > prevMax && !c->maxTicks.compare_exchange_weak(prevMax, d)) {}

    const double ms = MsOf(d);
    if (ms >= kStallMs && ShouldLogStall(c)) {
        const uint64_t sup = c->suppressed.exchange(0, std::memory_order_relaxed);
        char line[176];
        if (sup) snprintf(line, sizeof(line), "STALL %s %.1fms (single call, +%llu more suppressed)",
                          m_name, ms, (unsigned long long)sup);
        else     snprintf(line, sizeof(line), "STALL %s %.1fms (single call)", m_name, ms);
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
    if (ms >= kStallMs && ShouldLogStall(c)) {
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

// What was this thread doing immediately before the gap? With no scope spanning the block, this is
// the strongest evidence available.
void LogCrumb(const char* anchor, double gapMs) {
    char line[256];
    const int64_t sinceEnter = t_crumb.enteredAt ? (Now() - t_crumb.enteredAt) : 0;
    snprintf(line, sizeof(line),
             "GAP %.1fms on anchor %s (tid=%lu) -- last scope entered=%s exited=%s, %.1fms ago",
             gapMs, anchor, static_cast<unsigned long>(GetCurrentThreadId()),
             t_crumb.entered ? t_crumb.entered : "(none)",
             t_crumb.exited  ? t_crumb.exited  : "(none)",
             MsOf(static_cast<uint64_t>(sinceEnter)));
    Log::Write("PERF", line);
}

void FrameTick(double gapWarnMs) {
    static int64_t s_last = 0;
    const int64_t now = Now();
    if (s_last != 0) {
        const double gap = MsOf(static_cast<uint64_t>(now - s_last));
        if (gap >= gapWarnMs) {
            LogCrumb("input-poll", gap);
            DumpAndReset("frame gap", 0.0);
        }
    }
    s_last = now;
}

void GapTick(const char* anchor, double gapWarnMs) {
    Anchor* a = AnchorFor(anchor);
    if (!a) return;
    const int64_t now = Now();
    const int64_t prev = a->lastTick.exchange(now, std::memory_order_relaxed);
    if (prev != 0) {
        const double gap = MsOf(static_cast<uint64_t>(now - prev));
        if (gap >= gapWarnMs) {
            LogCrumb(anchor, gap);
            DumpAndReset("gap", 0.0);
        }
    }
}

namespace {
std::atomic<int64_t>     g_menuMarkAt{0};      // QPC at the announce
std::atomic<int64_t>     g_menuMarkDone{0};    // QPC when our hook returned
std::atomic<const char*> g_menuMarkWhat{nullptr};
std::atomic<uint32_t>    g_paintsSinceMark{0};
} // namespace

// Fed by Log::Write. Deliberately silent: no STALL line, no LogCrumb, nothing that would re-enter
// the logger. Surfaces only through DumpAndReset, which is called from outside the logger.
namespace {
std::atomic<uint64_t> g_logWait{0}, g_logWrite{0}, g_logCalls{0}, g_logMaxWait{0};
} // namespace

void AddLoggerSample(int64_t waitTicks, int64_t writeTicks) {
    g_logCalls.fetch_add(1, std::memory_order_relaxed);
    g_logWait.fetch_add(static_cast<uint64_t>(waitTicks), std::memory_order_relaxed);
    g_logWrite.fetch_add(static_cast<uint64_t>(writeTicks), std::memory_order_relaxed);
    uint64_t prev = g_logMaxWait.load(std::memory_order_relaxed);
    const uint64_t w = static_cast<uint64_t>(waitTicks);
    while (w > prev && !g_logMaxWait.compare_exchange_weak(prev, w)) {}
}

void MarkMenuEntry(const char* what) {
    g_menuMarkWhat.store(what, std::memory_order_relaxed);
    g_paintsSinceMark.store(0, std::memory_order_relaxed);
    g_menuMarkDone.store(0, std::memory_order_relaxed);
    g_menuMarkAt.store(Now(), std::memory_order_release);
}

void MarkMenuEntryDone() { g_menuMarkDone.store(Now(), std::memory_order_relaxed); }

void NoteFirstPaint() {
    const int64_t at = g_menuMarkAt.load(std::memory_order_acquire);
    if (at == 0) return;
    g_paintsSinceMark.fetch_add(1, std::memory_order_relaxed);
    // Only the FIRST paint after a mark reports; clear the mark so this is one line per menu open.
    if (!g_menuMarkAt.compare_exchange_strong(const_cast<int64_t&>(at), 0)) return;
    const int64_t done = g_menuMarkDone.load(std::memory_order_relaxed);
    const int64_t now  = Now();
    const char* what = g_menuMarkWhat.load(std::memory_order_relaxed);
    char line[224];
    snprintf(line, sizeof(line),
             "menu-open %s: in-our-hook %.1fms, hook-return -> first paint %.1fms (total %.1fms)",
             what ? what : "?",
             done ? MsOf(static_cast<uint64_t>(done - at)) : 0.0,
             done ? MsOf(static_cast<uint64_t>(now - done)) : MsOf(static_cast<uint64_t>(now - at)),
             MsOf(static_cast<uint64_t>(now - at)));
    Log::Write("PERF", line);
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
    // Logger first: if the game thread is blocked on g_logMutex behind the input thread's disk
    // flush, this is the only line that can show it.
    const uint64_t lc = g_logCalls.exchange(0, std::memory_order_relaxed);
    if (lc) {
        const uint64_t lw = g_logWait.exchange(0, std::memory_order_relaxed);
        const uint64_t lx = g_logWrite.exchange(0, std::memory_order_relaxed);
        const uint64_t lm = g_logMaxWait.exchange(0, std::memory_order_relaxed);
        char l[192];
        snprintf(l, sizeof(l),
                 "  %-28s calls=%-7llu lockwait=%7.2fms max=%7.2fms  write=%7.2fms",
                 "Log::Write", (unsigned long long)lc, MsOf(lw), MsOf(lm), MsOf(lx));
        Log::Write("PERF", l);
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
