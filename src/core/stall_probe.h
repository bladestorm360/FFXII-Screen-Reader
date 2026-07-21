#pragma once

#include <cstdint>

// Game-thread stall probe: how long does OUR code hold the game up, and where?
//
// WHY THIS EXISTS. The mod must be invisible to the game's timing -- if the modded game is slower
// than vanilla, that is our bug (see the standing rule in CLAUDE.md). The ordinary log cannot answer
// "where did the time go": Log::Write stamps with GetTickCount64, whose granularity is ~15.6 ms, so
// a 400 ms stall and a 0 ms one can land on the same timestamp. This measures with
// QueryPerformanceCounter instead.
//
// It answers three questions the field-menu stall needed and the log could not give:
//   1. Which hook consumed the time (self time, EXCLUDING the s_orig* trampoline).
//   2. Was it one slow call or ten thousand cheap ones (max vs total).
//   3. Was it BLOCKING on one of our own mutexes -- i.e. our lock sitting in the game's hot path.
//
// COST: one QPC pair (~25 ns) per scope. Orders of magnitude below anything it can detect, so it
// stays compiled in: a stall that only reproduces in a real play session is exactly the kind we
// cannot afford to need a special build for.
namespace StallProbe {

// Time our own work inside a hook. Scope it around OUR code only -- never around the call to the
// original function, or we measure the game's work and blame ourselves for it.
//
//   void HookedThing(...) {
//       if (s_orig) s_orig(...);        // NOT measured -- the game's own work
//       STALL_SCOPE("HookedThing");     // from here down is ours
//       ...
//   }
struct Scope {
    explicit Scope(const char* name);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    const char* m_name;
    int64_t     m_t0;
};

// Record time spent BLOCKED acquiring a mutex, separately from time holding it. Use for any lock
// the game's own paint/update path can contend for.
//
//   { StallProbe::TimedLock lk(g_mutex, "textcapture.g_mutex"); ... }
//
// Templated on the mutex type so it works with std::mutex without dragging <mutex> into this header.
template <typename M>
class TimedLock {
public:
    TimedLock(M& m, const char* name) : m_m(m) { const int64_t t = Now(); m_m.lock(); AddWait(name, t); }
    ~TimedLock() { m_m.unlock(); }
    TimedLock(const TimedLock&) = delete;
    TimedLock& operator=(const TimedLock&) = delete;
private:
    M& m_m;
};

// Raw QPC tick count, and the wait-accumulator TimedLock feeds. Public only so the template above
// can reach them; call Scope/TimedLock rather than these.
int64_t Now();
void    AddWait(const char* name, int64_t startTicks);

// Note which thread a hook ran on. Logs once per unique (name, thread) pair, so a hook that fires
// on two threads is immediately visible -- that is what turns a shared mutex into a cross-thread
// stall on the game's own path. O(unique pairs), never per call.
void NoteThread(const char* name);

// FRAME HEARTBEAT. Call from a hook the game runs every frame (the DirectInput poll, which keeps
// running while the game's own menus are open). Measures wall time since the previous call and,
// when a frame takes longer than `gapWarnMs`, logs the gap AND dumps that window's per-hook
// breakdown immediately -- so a stall is captured together with what was running during it.
//
// This is the measurement that distinguishes "one of our hooks is slow" from "the frame is long and
// our hooks are barely in it". The second case still means WE caused it (vanilla is instant) -- it
// just means the cost is somewhere we are not yet timing, e.g. the painter callback swap or a write
// into a game structure -- and it tells us to widen coverage, never to blame the game.
//
// Spurious fires after alt-tab, a load screen or a breakpoint are expected; judge by whether the
// gap coincides with the reported symptom.
void FrameTick(double gapWarnMs);

// Same, for an anchor OTHER than the frame heartbeat -- pass a distinct name so several hooks can
// each watch their own cadence. Whichever thread keeps running during a stall will catch it.
void GapTick(const char* anchor, double gapWarnMs);

// MENU-OPEN BRACKET. MarkMenuEntry() stamps the moment we announce a newly-entered menu;
// NoteFirstPaint() is called from the paint path and, on the first paint after a mark, logs the
// split: how long we spent inside our own hook, and how long the game then took before it drew
// anything. The field-menu freeze needs exactly that split -- 2235 ms of silence with no scope
// reporting is otherwise unattributable. Repeat calls before the next mark are ignored.
// Logger self-measurement. Log::Write cannot use Scope/TimedLock -- StallProbe reports THROUGH
// Log::Write, so any logging from inside the logger recurses. These accumulate into a counter that
// only the aggregate dump reads, and emit nothing themselves.
void AddLoggerSample(int64_t waitTicks, int64_t writeTicks);

void MarkMenuEntry(const char* what);
void MarkMenuEntryDone();   // call as our hook returns

// RAII form: stamps "our hook returned" on scope exit, whatever path it takes.
struct MarkMenuEntryDoneGuard { ~MarkMenuEntryDoneGuard() { MarkMenuEntryDone(); } };
void NoteFirstPaint();

// Log every counter with calls > 0, then zero them. `reason` brackets the window, e.g. "menu entry".
// Output is O(unique hooks) (~30 lines), within the console/log budget.
//
// `minTotalMs` keeps the probe from becoming the thing it is hunting: below that total the counters
// are reset SILENTLY. This is called from a game-thread hook on every pane change, and writing ~30
// log lines there would add game-thread work to the exact path we are measuring. Quiet transitions
// cost nothing; only a window that actually burned time reports.
void DumpAndReset(const char* reason, double minTotalMs = 0.0);

} // namespace StallProbe

// Convenience: unique local name, so two scopes can share a function.
#define STALL_PASTE2(a, b) a##b
#define STALL_PASTE(a, b)  STALL_PASTE2(a, b)
#define STALL_SCOPE(lit)                              \
    StallProbe::NoteThread(lit);                      \
    StallProbe::Scope STALL_PASTE(_stall_, __LINE__)(lit)
