// Derived from ff12-tkmalloc (https://gitlab.com/ffgriever/ff12-tkmalloc, v0.2.2, `e3f69b8`) --
// its hook.cpp and memory.cpp. The original notice follows, as its licence requires.
//
//   BSD 2-Clause License
//
//   Copyright (c) 2026, ffgriever
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are met:
//
//   1. Redistributions of source code must retain the above copyright notice, this
//      list of conditions and the following disclaimer.
//
//   2. Redistributions in binary form must reproduce the above copyright notice,
//      this list of conditions and the following disclaimer in the documentation
//      and/or other materials provided with the distribution.
//
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
//   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
//   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
//   FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
//   DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
//   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//   CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
//   OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
//   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// WHAT IS FFGRIEVER'S, UNCHANGED: the call-site table and its expected bytes, the pool's address
// and size, the one pool allocation answered with null, and the mspace approach itself.
//
// WHAT CHANGED IN THE PORT, and why (S194):
//   * ONE profile, Steam. Every other RVA in this mod is the Steam build's, so it runs nowhere else.
//   * It cannot fail the DLL. ffgriever returns FALSE from DllMain; here that would kill speech.
//   * All 28 sites and the pool-ready mask are checked BEFORE the first byte is written, and a patch
//     that fails part-way is rolled back. The original verified first but could stop mid-table.
//   * A fallback block must END below 2 GB, not merely start there.
//   * The near-jump stubs are our own -- one page, one stub per wrapper -- instead of ffgriever's
//     trampoline library.
//   * Logging is Log::Write from Stage B, instead of simpleLog from inside DllMain.

#include "core/tkmalloc_fix.h"
#include "core/logger.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "dlmalloc/malloc.h"

namespace {

// ffgriever's addresses live in the exe's PREFERRED-BASE space, which is the space our Ghidra project
// uses too -- so `0x22A1E1` below sits inside FUN_0022a0b0 in the decompile. RVA = address - 0x120000.
constexpr uintptr_t kPreferredBase = 0x120000;
constexpr uintptr_t Rva(uintptr_t ghidraAddr) { return ghidraAddr - kPreferredBase; }

constexpr uintptr_t kPoolWant = 0x10000000;
// The eleven startup pools come to ~257 MB (FUN_0022a0b0's `malloc` sizes); the rest is headroom for
// the pool FUN_0037b3d0 frees and re-creates at runtime, and the two small thread-object allocations.
constexpr size_t    kPoolSize = 0x12000000;
constexpr uintptr_t kTop32    = 0x7FFFFFFF;   // the last byte a 32-bit signed pointer can reach

constexpr uintptr_t kTkallocInit = 0x369D30;  // FUN_00369d30(pool, base, size, page size)
constexpr uintptr_t kPoolsReady  = 0x22D89B8; // DAT_022d89b8: bit N set once pool N is set up

enum Target : uint8_t { kMallocInt, kMallocSizet, kMallocNull, kFree, kTkInit, kTargetCount };
constexpr size_t kStubBytes = 16;

struct Site { const char* name; uintptr_t at; Target target; uint8_t original[5]; };

// ffgriever's `steamTkHooks`, verbatim -- names, addresses and bytes -- so it diffs line for line
// against upstream. Where each group lives, from our decompile:
//   malloc01..11, tkInit01..09   FUN_0022a0b0, the startup pool setup. malloc10's result is written
//                                to DAT_01fd4a20 and read NOWHERE in the exe (checked S194), which is
//                                why ffgriever answers it with null: 512 KB of low memory saved.
//   free01, malloc12, tkInit10   FUN_0037b3d0, which frees pool 11 and re-creates it at runtime.
//   mallocThread1/2              FUN_008636e0, the thread-object constructor.
//   tkInit11..13                 other pool (re)creators; 12 and 13 are tail JUMPS (E9), not calls.
const Site kSites[] = {
    {"malloc01", 0x22A1E1, kMallocInt,  {0xE8, 0x5A, 0x30, 0x14, 0x00}},
    {"malloc02", 0x22A1F2, kMallocInt,  {0xE8, 0x49, 0x30, 0x14, 0x00}},
    {"malloc03", 0x22A203, kMallocInt,  {0xE8, 0x38, 0x30, 0x14, 0x00}},
    {"malloc04", 0x22A214, kMallocInt,  {0xE8, 0x27, 0x30, 0x14, 0x00}},
    {"malloc05", 0x22A225, kMallocInt,  {0xE8, 0x16, 0x30, 0x14, 0x00}},
    {"malloc06", 0x22A236, kMallocInt,  {0xE8, 0x05, 0x30, 0x14, 0x00}},
    {"malloc07", 0x22A247, kMallocInt,  {0xE8, 0xF4, 0x2F, 0x14, 0x00}},
    {"malloc08", 0x22A258, kMallocInt,  {0xE8, 0xE3, 0x2F, 0x14, 0x00}},
    {"malloc09", 0x22A269, kMallocInt,  {0xE8, 0xD2, 0x2F, 0x14, 0x00}},
    {"malloc10", 0x22A27A, kMallocNull, {0xE8, 0xC1, 0x2F, 0x14, 0x00}},
    {"malloc11", 0x22A28B, kMallocInt,  {0xE8, 0xB0, 0x2F, 0x14, 0x00}},
    {"malloc12", 0x37B420, kMallocInt,  {0xE8, 0x1B, 0x1E, 0xFF, 0xFF}},

    {"mallocThread1", 0x863703, kMallocSizet, {0xE8, 0xB8, 0xD7, 0x9A, 0xFF}},
    {"mallocThread2", 0x863715, kMallocSizet, {0xE8, 0xA6, 0xD7, 0x9A, 0xFF}},

    {"free01", 0x37B419, kFree, {0xE8, 0x32, 0x1E, 0xFF, 0xFF}},

    {"tkInit01", 0x22A2AE, kTkInit, {0xE8, 0x7D, 0xFA, 0x13, 0x00}},
    {"tkInit02", 0x22A2CB, kTkInit, {0xE8, 0x60, 0xFA, 0x13, 0x00}},
    {"tkInit03", 0x22A2E7, kTkInit, {0xE8, 0x44, 0xFA, 0x13, 0x00}},
    {"tkInit04", 0x22A304, kTkInit, {0xE8, 0x27, 0xFA, 0x13, 0x00}},
    {"tkInit05", 0x22A321, kTkInit, {0xE8, 0x0A, 0xFA, 0x13, 0x00}},
    {"tkInit06", 0x22A33E, kTkInit, {0xE8, 0xED, 0xF9, 0x13, 0x00}},
    {"tkInit07", 0x22A35B, kTkInit, {0xE8, 0xD0, 0xF9, 0x13, 0x00}},
    {"tkInit08", 0x22A377, kTkInit, {0xE8, 0xB4, 0xF9, 0x13, 0x00}},
    {"tkInit09", 0x22A394, kTkInit, {0xE8, 0x97, 0xF9, 0x13, 0x00}},
    {"tkInit10", 0x37B43D, kTkInit, {0xE8, 0xEE, 0xE8, 0xFE, 0xFF}},
    {"tkInit11", 0x3E9AEF, kTkInit, {0xE8, 0x3C, 0x02, 0xF8, 0xFF}},

    {"tkInit12", 0x25AA21, kTkInit, {0xE9, 0x0A, 0xF3, 0x10, 0x00}},
    {"tkInit13", 0x25AAC1, kTkInit, {0xE9, 0x6A, 0xF2, 0x10, 0x00}},
};
constexpr int kSiteCount = static_cast<int>(sizeof(kSites) / sizeof(kSites[0]));

// ---- state: written once in DllMain, read by the wrappers and by LogReport -----------------------
enum class Verdict : uint8_t {
    NotRun, Installed, WrongBuild, TooLate, NoAddressSpace, CommitFailed, MspaceFailed, NoStubPage,
    PatchFailed
};
Verdict   g_verdict   = Verdict::NotRun;
int       g_badSite   = -1;       // WrongBuild / PatchFailed: which site
uint32_t  g_readyMask = 0;        // TooLate: what the game had already set up
DWORD     g_lastError = 0;
uintptr_t g_poolBase  = 0;
mspace    g_space     = nullptr;  // non-null exactly when the sites are patched

using TkInitFn = void (*)(int32_t, uintptr_t, uint64_t, uint64_t);
TkInitFn g_tkInit = nullptr;

// ---- what the game did with it, for the log -----------------------------------------------------
// The startup pools are set up long before the logger opens (Stage B waits 100 ms), so each set-up is
// RECORDED here and written by LogReport; later ones write themselves. `logged` makes that exactly
// once whichever side gets there first. LogReport opens the gate BEFORE it scans, so a record that
// lands mid-scan is still written by one side or the other.
struct PoolRec {
    std::atomic<bool> ready{false};
    std::atomic<bool> logged{false};
    int32_t   pool = 0;
    uintptr_t base = 0;
    uint32_t  size = 0;
};
constexpr int kMaxRecs = 32;
PoolRec g_recs[kMaxRecs];
std::atomic<int>      g_recCount{0};
std::atomic<bool>     g_reportOpen{false};
std::atomic<uint32_t> g_strays{0};   // allocations the pool could not serve below 2 GB

void LogPool(int i) {
    PoolRec& r = g_recs[i];
    if (!r.ready.load(std::memory_order_acquire) || r.logged.exchange(true)) return;
    const uintptr_t last = r.base + (r.size ? r.size - 1 : 0);
    char m[200];
    snprintf(m, sizeof(m), "game memory pool %d set up at 0x%llX..0x%llX (%u KB)%s", r.pool,
             (unsigned long long)r.base, (unsigned long long)last, r.size / 1024,
             last > kTop32 ? " -- ABOVE 2 GB: this pool is the crash the fix exists to stop" : "");
    Log::Write("MEM", m);
}

// A null, or an address past 2 GB, means dlmalloc ran out of the pool and went to the system for
// more -- which lands wherever the system likes, the exact failure this file exists to prevent.
void NoteAlloc(void* p, size_t n) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    if (n == 0 || (a != 0 && a + n - 1 <= kTop32)) return;
    g_strays.fetch_add(1, std::memory_order_relaxed);
    if (!g_reportOpen.load(std::memory_order_acquire)) return;
    char m[160];
    snprintf(m, sizeof(m), "game memory pool could not serve %llu bytes below 2 GB (got 0x%llX)",
             (unsigned long long)n, (unsigned long long)a);
    Log::Write("MEM", m);
}

// ---- the five wrappers the call sites are pointed at --------------------------------------------
// Reachable only once Install has patched the sites, and it patches only with g_space live -- so none
// of them needs a "not installed" branch, and a CRT fallback would be WRONG: a block from the CRT
// later handed to mspace_free is heap corruption.
void* MallocSizet(size_t n) noexcept {
    void* p = mspace_malloc(g_space, n);
    NoteAlloc(p, n);
    return p;
}
// The twelve pool sites pass an int, and ffgriever widens it exactly like this; kept identical.
void* MallocInt(int n) noexcept   { return MallocSizet(static_cast<size_t>(n)); }
void* MallocNull(int) noexcept    { return nullptr; }
void  Free(void* p) noexcept      { mspace_free(g_space, p); }

// A pass-through, there to RECORD. Recorded before forwarding so a crash inside set-up still leaves
// the line behind. `size` is an int in the game's signature, so only its low 32 bits mean anything.
void TkInit(int32_t pool, uintptr_t base, uint64_t size, uint64_t page) noexcept {
    const int n = g_recCount.fetch_add(1, std::memory_order_relaxed);
    if (n < kMaxRecs) {
        g_recs[n].pool = pool;
        g_recs[n].base = base;
        g_recs[n].size = static_cast<uint32_t>(size);
        g_recs[n].ready.store(true, std::memory_order_release);
        if (g_reportOpen.load(std::memory_order_acquire)) LogPool(n);
    }
    g_tkInit(pool, base, size, page);
}

// ---- memory helpers -----------------------------------------------------------------------------
// ffgriever's findFreeRegion, with one change: the block must END by `limit`, not merely start before
// it. A free region straddling 2 GB would otherwise hand back a pool whose top half is unusable.
uintptr_t FindFreeRegion(size_t size, uintptr_t start, uintptr_t limit) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t addr = start;
    while (addr < limit) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end  = base + mbi.RegionSize;          // one past the region
        if (mbi.State == MEM_FREE) {
            // VirtualQuery reports free space at page granularity; VirtualAlloc needs the allocation
            // granularity (64 KB) and would otherwise round DOWN into the neighbouring region.
            const uintptr_t aligned = (base + gran - 1) & ~(gran - 1);
            if (aligned >= base && aligned + size <= end && aligned + size - 1 <= limit) return aligned;
        }
        if (end <= addr) break;                                // overflow guard
        addr = end;
    }
    return 0;
}

bool ReadBytes(uintptr_t at, void* out, size_t n) {
    __try { memcpy(out, reinterpret_cast<const void*>(at), n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool WriteRel32(uintptr_t site, int32_t rel) {
    void* op = reinterpret_cast<void*>(site + 1);
    DWORD old = 0;
    if (!VirtualProtect(op, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(op, &rel, 4);
    VirtualProtect(op, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 5);
    return true;
}

// One page of `jmp [rip+0]; dq target` stubs, placed where a rel32 from the exe can reach it -- our
// wrappers live in this DLL, gigabytes away. Searched from the bottom of the window up, so on the
// usual 0x120000 exe it lands in the free space below the image, well clear of the pool.
uint8_t* MakeStubPage(uintptr_t exe, const void* const (&targets)[kTargetCount]) {
    constexpr uintptr_t kReach = 0x7F000000;  // under 2 GB, less the ~8 MB the sites span
    const uintptr_t lo = exe > kReach + 0x10000 ? exe - kReach : 0x10000;
    const uintptr_t at = FindFreeRegion(0x1000, lo, exe + kReach);
    if (!at) return nullptr;
    auto* page = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(at), 0x1000,
                                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!page) return nullptr;
    for (int t = 0; t < kTargetCount; ++t) {
        uint8_t* s = page + t * kStubBytes;
        s[0] = 0xFF; s[1] = 0x25; s[2] = s[3] = s[4] = s[5] = 0x00;   // jmp qword ptr [rip+0]
        memcpy(s + 6, &targets[t], 8);
        s[14] = s[15] = 0xCC;
    }
    DWORD old = 0;
    VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    return page;
}

void Abandon(Verdict why, void* pool, uint8_t* stubs) {
    g_lastError = GetLastError();
    if (g_space) { destroy_mspace(g_space); g_space = nullptr; }
    if (pool)    VirtualFree(pool, 0, MEM_RELEASE);
    if (stubs)   VirtualFree(stubs, 0, MEM_RELEASE);
    g_verdict = why;
}

} // namespace

namespace TkmallocFix {

void Install() {
    if (g_verdict != Verdict::NotRun) return;
    const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    // 1. Every site must be the Steam build's, byte for byte, before anything is touched.
    for (int i = 0; i < kSiteCount; ++i) {
        uint8_t now[5] = {};
        if (!ReadBytes(exe + Rva(kSites[i].at), now, 5) || memcmp(now, kSites[i].original, 5) != 0) {
            g_badSite = i;
            g_verdict = Verdict::WrongBuild;
            return;
        }
    }
    // 2. The game must not have set up a single pool yet (charter point 2).
    if (!ReadBytes(exe + Rva(kPoolsReady), &g_readyMask, 4) || g_readyMask != 0) {
        g_verdict = Verdict::TooLate;
        return;
    }
    // 3. The block: ffgriever's address first, then the lowest free fit that ends below 2 GB.
    void* pool = VirtualAlloc(reinterpret_cast<void*>(kPoolWant), kPoolSize, MEM_RESERVE, PAGE_READWRITE);
    if (!pool) {
        if (const uintptr_t at = FindFreeRegion(kPoolSize, 0x10000, kTop32))
            pool = VirtualAlloc(reinterpret_cast<void*>(at), kPoolSize, MEM_RESERVE, PAGE_READWRITE);
    }
    if (!pool) { Abandon(Verdict::NoAddressSpace, nullptr, nullptr); return; }
    if (!VirtualAlloc(pool, kPoolSize, MEM_COMMIT, PAGE_READWRITE)) {
        Abandon(Verdict::CommitFailed, pool, nullptr);
        return;
    }
    g_space = create_mspace_with_base(pool, kPoolSize, 0);
    if (!g_space) { Abandon(Verdict::MspaceFailed, pool, nullptr); return; }

    // 4. The stubs, and a reach check for every site BEFORE the first write.
    const void* const targets[kTargetCount] = {
        reinterpret_cast<const void*>(&MallocInt),  reinterpret_cast<const void*>(&MallocSizet),
        reinterpret_cast<const void*>(&MallocNull), reinterpret_cast<const void*>(&Free),
        reinterpret_cast<const void*>(&TkInit),
    };
    uint8_t* stubs = MakeStubPage(exe, targets);
    if (!stubs) { Abandon(Verdict::NoStubPage, pool, nullptr); return; }
    int32_t rel[kSiteCount] = {};
    for (int i = 0; i < kSiteCount; ++i) {
        const int64_t site = static_cast<int64_t>(exe + Rva(kSites[i].at));
        const int64_t stub = reinterpret_cast<int64_t>(stubs + kSites[i].target * kStubBytes);
        const int64_t d    = stub - (site + 5);
        if (d < INT32_MIN || d > INT32_MAX) { Abandon(Verdict::NoStubPage, pool, stubs); return; }
        rel[i] = static_cast<int32_t>(d);
    }

    // 5. Publish what the wrappers read, then patch. A failure part-way puts back every site already
    //    written -- safe, because the game has not run a single instruction yet.
    g_poolBase = reinterpret_cast<uintptr_t>(pool);
    g_tkInit   = reinterpret_cast<TkInitFn>(exe + Rva(kTkallocInit));
    for (int i = 0; i < kSiteCount; ++i) {
        if (WriteRel32(exe + Rva(kSites[i].at), rel[i])) continue;
        const DWORD err = GetLastError();   // before the rollback's own VirtualProtects overwrite it
        for (int j = 0; j < i; ++j) {
            int32_t orig = 0;
            memcpy(&orig, kSites[j].original + 1, 4);
            WriteRel32(exe + Rva(kSites[j].at), orig);
        }
        g_badSite = i;
        Abandon(Verdict::PatchFailed, pool, stubs);
        g_lastError = err;
        return;
    }
    g_verdict = Verdict::Installed;
}

void LogReport() {
    char m[320];
    const char* site = g_badSite >= 0 ? kSites[g_badSite].name : "?";
    switch (g_verdict) {
        case Verdict::Installed:
            snprintf(m, sizeof(m), "tkMalloc fix INSTALLED (ffgriever's FF12 tkMalloc Fix, built in): "
                     "the game's memory pools come from 0x%llX..0x%llX, below 2 GB; %d call sites "
                     "redirected", (unsigned long long)g_poolBase,
                     (unsigned long long)(g_poolBase + kPoolSize - 1), kSiteCount);
            break;
        case Verdict::WrongBuild:
            snprintf(m, sizeof(m), "tkMalloc fix NOT installed: %s is not the Steam build's bytes "
                     "(another exe version, or another mod patched it). Nothing was written; the "
                     "game allocates exactly as unmodded", site);
            break;
        case Verdict::TooLate:
            snprintf(m, sizeof(m), "tkMalloc fix NOT installed: the game had already set up memory "
                     "pools (mask 0x%X) before this DLL loaded. Redirecting now would corrupt them, "
                     "so nothing was written", g_readyMask);
            break;
        case Verdict::NoAddressSpace:
            snprintf(m, sizeof(m), "tkMalloc fix NOT installed: no free 288 MB block below 2 GB "
                     "(error %lu). This is the condition the fix exists for -- the game may crash",
                     g_lastError);
            break;
        case Verdict::CommitFailed:
        case Verdict::MspaceFailed:
        case Verdict::NoStubPage:
        case Verdict::PatchFailed:
            snprintf(m, sizeof(m), "tkMalloc fix NOT installed: %s failed (site %s, error %lu). "
                     "Everything was rolled back; the game allocates exactly as unmodded",
                     g_verdict == Verdict::CommitFailed ? "committing the pool" :
                     g_verdict == Verdict::MspaceFailed ? "creating the mspace" :
                     g_verdict == Verdict::NoStubPage   ? "placing the jump stubs" : "patching",
                     site, g_lastError);
            break;
        default:
            snprintf(m, sizeof(m), "tkMalloc fix never ran -- DllMain did not call Install");
            break;
    }
    Log::Write("MEM", m);

    // Open the gate FIRST, then scan: see PoolRec.
    g_reportOpen.store(true, std::memory_order_release);
    int n = g_recCount.load(std::memory_order_acquire);
    if (n > kMaxRecs) n = kMaxRecs;
    for (int i = 0; i < n; ++i) LogPool(i);
    if (const uint32_t s = g_strays.load(std::memory_order_relaxed)) {
        snprintf(m, sizeof(m), "%u game allocation(s) before the log opened could not be served "
                 "below 2 GB", s);
        Log::Write("MEM", m);
    }
}

} // namespace TkmallocFix
