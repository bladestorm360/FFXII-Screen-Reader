#include "battle/combat_log.h"

#include "core/logger.h"
#include "speech/speech.h"

#include <Windows.h>
#include <mutex>
#include <vector>
#include <cstdio>

namespace CombatLog {
namespace {

constexpr size_t kCapacity = 100;

struct Entry {
    uint64_t     seq = 0;      // monotonic, never reused -- the cursor's identity anchor
    Kind         kind = Kind::System;
    std::wstring text;
};

std::mutex          g_mx;
std::vector<Entry>  g_ring;    // fixed capacity, oldest evicted first
uint64_t            g_nextSeq = 1;

// The cursor is a SEQ, not an index, so an eviction that passes it is detectable rather than
// silently shifting what the user is reading.
uint64_t g_cursorSeq = 0;      // 0 = "following the newest"
bool     g_following = true;

// Find the ring position of a seq; -1 if it has been evicted.
int IndexOfSeq(uint64_t seq) {
    for (size_t i = 0; i < g_ring.size(); ++i)
        if (g_ring[i].seq == seq) return static_cast<int>(i);
    return -1;
}

// Move the cursor and speak where it lands. Input thread.
// Speech happens OUTSIDE the lock -- Tolk can block, and the game thread appends under this mutex.
void Move(int delta, bool toEnd, bool toStart) {
    std::wstring say;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_ring.empty()) {
            say = L"Combat log empty";   // the one case that gets a spoken explanation:
        } else {                          // a silent key is indistinguishable from a broken one
            const int last = static_cast<int>(g_ring.size()) - 1;
            int idx;
            if (toStart)      idx = 0;
            else if (toEnd)   idx = last;
            else {
                // While following, the cursor is conceptually AT the newest entry, so the first
                // `,` must step back from there rather than re-speaking it.
                int cur = g_following ? last : IndexOfSeq(g_cursorSeq);
                if (cur < 0) cur = 0;     // our entry was evicted; clamp to the oldest survivor
                idx = cur + delta;
            }

            if (idx < 0) idx = 0;
            if (idx > last) idx = last;

            const Entry& e = g_ring[static_cast<size_t>(idx)];
            g_cursorSeq = e.seq;
            // Re-attach on the newest entry so new events resume auto-following.
            g_following = (idx == last);
            say = e.text;
        }
    }
    Speech::Output(say, /*interrupt=*/true);
}

} // namespace

void Init() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_ring.clear();
    g_ring.reserve(kCapacity);
    g_nextSeq = 1;
    g_cursorSeq = 0;
    g_following = true;
    Log::Write("COMBAT", "CombatLog initialized (100 entries, continuous across battles)");
}

void Shutdown() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_ring.clear();
}

void Append(Kind kind, const std::wstring& text, bool speakNow) {
    if (text.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_ring.size() >= kCapacity) g_ring.erase(g_ring.begin());
        Entry e;
        e.seq = g_nextSeq++;
        e.kind = kind;
        e.text = text;
        g_ring.push_back(std::move(e));
        // Auto-follow: while the cursor sits at the newest entry, a new event moves it along, so a
        // reader parked at the end is never stranded in the past.
        if (g_following) g_cursorSeq = g_ring.back().seq;
    }

    char utf8[512];
    Log::ToUtf8(text, utf8, sizeof(utf8));
    char line[576];
    snprintf(line, sizeof(line), "%s | %s",
             kind == Kind::GameMessage ? "GAME" : (kind == Kind::Damage ? "DMG " : "SYS "), utf8);
    Log::Write("COMBAT", line);

    if (speakNow) Speech::Output(text, /*interrupt=*/true);
}

void StepBack()    { Move(-1, false, false); }
void StepForward() { Move(+1, false, false); }
void JumpOldest()  { Move(0, false, true); }
void JumpNewest()  { Move(0, true, false); }

} // namespace CombatLog
