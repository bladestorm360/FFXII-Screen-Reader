#include "core/frame_probe.h"

#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"

#include <atomic>
#include <cstdio>

namespace FrameProbe {

namespace {

constexpr const char* kTag = "PERF";

// ---- the engine's frame-pacing cluster (Docs\combat_system.md §7.5) ----------------------------
// All read-only. Confidences are the ones recorded with the originals; the two the probe's own
// conclusions rest on (RENDER_FRAMES and SPEED_MUL) are the two it prints raw so they can be
// challenged from the log rather than believed.
constexpr uint32_t RVA_RENDER_FRAMES = 0x1EB4A80;  // u32, +1 per render frame, runs while paused
constexpr uint32_t RVA_SIM_ACCUM     = 0x1F44AC0;  // f32, gains ac8*ac4 per call, drained by 1.0
constexpr uint32_t RVA_SPEED_MUL     = 0x1F44AC4;  // f32 "ac4", the multiplier in force this frame
constexpr uint32_t RVA_BASE_DELTA    = 0x1F44AC8;  // f32 "ac8", per-frame delta in SIM TICKS
constexpr uint32_t RVA_SPEED_INDEX   = 0x1EB4A98;  // i32, {0,1,2} — the game's own 1/2/3 keys
constexpr uint32_t RVA_SPEED_TABLE   = 0x7E8BA8;   // 3 floats in .rdata, indexed by SPEED_INDEX

constexpr uint64_t kReportMs = 10000;

// Input thread writes the window bounds; the game thread only ever increments its own counters.
std::atomic<uint64_t> g_fieldTicks{0};
std::atomic<uint64_t> g_textWalks{0};

uint64_t g_windowStartMs     = 0;
uint32_t g_windowStartFrames = 0;
uint64_t g_windowStartTicks  = 0;
uint64_t g_windowStartWalks  = 0;
bool     g_windowValid       = false;

// A global's address is its own base here — these are absolute statics, not fields of an object,
// so the offset is 0 and MemRead's null-base guard is the only precondition.
bool ReadU32(uint32_t rva, uint32_t* out) {
    return MemRead::SafeReadU32(Hooks::ResolveRva(rva), 0, out);
}
bool ReadF32(uint32_t rva, float* out) {
    return MemRead::SafeReadF32(Hooks::ResolveRva(rva), 0, out);
}

} // namespace

void Init() {
    // The multiplier table is the one value in the cluster still below the confidence bar — {1,2,4}
    // was a 0.85 reading of code that indexes .rdata, and .rdata contents are not recoverable
    // offline. Three guarded loads settle it, and printing them raw is the measurement.
    float t0 = 0.0f, t1 = 0.0f, t2 = 0.0f;
    const bool ok = ReadF32(RVA_SPEED_TABLE + 0, &t0) &&
                    ReadF32(RVA_SPEED_TABLE + 4, &t1) &&
                    ReadF32(RVA_SPEED_TABLE + 8, &t2);
    char m[160];
    if (ok) snprintf(m, sizeof(m), "frame probe: speed multiplier table = {%.3f, %.3f, %.3f}", t0, t1, t2);
    else    snprintf(m, sizeof(m), "frame probe: speed multiplier table UNREADABLE at RVA 0x%X", RVA_SPEED_TABLE);
    Log::Write(kTag, m);
}

void OnFieldFrame() {
    g_fieldTicks.fetch_add(1, std::memory_order_relaxed);
}

void OnTextWalk() {
    g_textWalks.fetch_add(1, std::memory_order_relaxed);
}

void OnInputPoll() {
    const uint64_t now = GetTickCount64();

    uint32_t frames = 0;
    if (!ReadU32(RVA_RENDER_FRAMES, &frames)) return;   // pre-boot / unmapped: say nothing

    const uint64_t ticks = g_fieldTicks.load(std::memory_order_relaxed);
    const uint64_t walks = g_textWalks.load(std::memory_order_relaxed);

    if (!g_windowValid) {
        g_windowStartMs = now; g_windowStartFrames = frames;
        g_windowStartTicks = ticks; g_windowStartWalks = walks;
        g_windowValid = true;
        return;
    }
    if (now - g_windowStartMs < kReportMs) return;

    const uint64_t elapsedMs = now - g_windowStartMs;
    // u32 subtraction wraps correctly, which is the whole reason the counter is read as u32.
    const uint32_t dFrames = frames - g_windowStartFrames;
    const uint64_t dTicks  = ticks - g_windowStartTicks;
    const uint64_t dWalks  = walks - g_windowStartWalks;

    float accum = 0.0f, mul = 0.0f, base = 0.0f;
    uint32_t idx = 0;
    ReadF32(RVA_SIM_ACCUM, &accum);
    ReadF32(RVA_SPEED_MUL, &mul);
    ReadF32(RVA_BASE_DELTA, &base);
    ReadU32(RVA_SPEED_INDEX, &idx);

    const double secs      = static_cast<double>(elapsedMs) / 1000.0;
    const double renderFps = secs > 0.0 ? dFrames / secs : 0.0;
    const double fieldFps  = secs > 0.0 ? dTicks  / secs : 0.0;
    const double walkRate  = secs > 0.0 ? dWalks  / secs : 0.0;
    // The ratio, spelled out, so nobody has to divide two numbers in a log to see the answer. 0 when
    // no dialogue box is up, which is most of the time -- that is not a reading, it is silence.
    const double walkPerFrame = dFrames ? static_cast<double>(dWalks) / dFrames : 0.0;

    // ONE line. renderFps is the game's own frame counter, so it is true even while the field tick
    // is stopped (menus, loads) — which is exactly when fieldFps legitimately reads lower. The pair
    // is the measurement: they should track during field play, and BOTH should be unmoved by a
    // change of speed index while ac4 changes underneath them.
    //
    // textWalk is the SECOND measurement and the one the tester's report turns on -- see the header.
    // If walk/frame tracks ac4 rather than staying at 1, the walk runs inside the sim loop.
    char m[288];
    snprintf(m, sizeof(m),
             "frame pacing: render %.1f fps (%u frames), field tick %.1f/s (%llu) over %llu ms | "
             "textWalk %.1f/s (%llu, %.2f/frame) | speedIdx=%u ac4=%.3f ac8=%.3f accum=%.3f",
             renderFps, dFrames, fieldFps, (unsigned long long)dTicks,
             (unsigned long long)elapsedMs, walkRate, (unsigned long long)dWalks, walkPerFrame,
             idx, mul, base, accum);
    Log::Write(kTag, m);

    g_windowStartMs = now; g_windowStartFrames = frames;
    g_windowStartTicks = ticks; g_windowStartWalks = walks;
}

} // namespace FrameProbe
