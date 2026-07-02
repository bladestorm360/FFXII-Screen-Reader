#include "ui/text_capture.h"
#include "core/hooks.h"
#include "core/logger.h"

#include <Windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>

namespace {

// Top text-entry candidate from MenuArchitecture.md. Wrapper cluster spans
// RVA 0x5F650-0x5FD30; this is the entry point most likely to carry a
// source-text pointer in its args. If Stream A G-A3 finds a different
// wrapper carries the strings, add it here.
constexpr uint32_t RVA_TEXT_ENTRY = 0x5FA10;

// Ring buffer cap. 256 events is plenty: a menu shows ~5-30 text strings
// per frame; we want a few seconds of history.
constexpr size_t RING_MAX = 256;

// Max chars to decode from a source-text pointer. Menu options are short;
// 256 is generous.
constexpr size_t MAX_DECODE_CHARS = 256;

std::mutex g_mutex;
std::deque<TextCapture::TextEvent> g_ring;
uint32_t g_frameCounter = 0;
bool g_initialized = false;

// FUN_0017fa10 signature is uncertain at design time. Per the decompile dig:
//   "callers use pattern FUN_0017fa10(param_1[0x17], 0x44, 1)"
// That looks like (font_state*, int_id, int_flag) — possibly a property set,
// not a source-text submit. The probe_text_wrappers.js Frida script (G-A3)
// confirms which wrapper actually carries the strings. Until then, this
// detour reads args[0..3] defensively and tries to decode each as a string
// pointer; whichever decodes is what gets captured.
typedef void (*Pfn_TextEntry)(void* a0, void* a1, void* a2, void* a3);
Pfn_TextEntry s_origTextEntry = nullptr;

// "Mostly printable" heuristic — avoids capturing garbage pointers that
// happen to dereference to high-entropy memory.
bool IsMostlyPrintable(const wchar_t* s, size_t maxChars) {
    if (!s) return false;
    size_t printable = 0;
    size_t total = 0;
    for (size_t i = 0; i < maxChars && s[i] != 0; i++) {
        total++;
        wchar_t c = s[i];
        if ((c >= 0x20 && c <= 0xFFFD) && c != 0xFFFE && c != 0xFFFF) {
            printable++;
        }
    }
    if (total == 0) return false;
    return (printable * 100) / total >= 80;
}

bool IsMostlyPrintableAscii(const char* s, size_t maxChars) {
    if (!s) return false;
    size_t printable = 0;
    size_t total = 0;
    for (size_t i = 0; i < maxChars && s[i] != 0; i++) {
        total++;
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c <= 0x7E) printable++;
        else if (c == '\t' || c == '\n' || c == '\r') printable++;
    }
    if (total == 0) return false;
    return (printable * 100) / total >= 80;
}

// Read up to maxChars wchars, stopping at null. Tolerates unreadable
// memory (SEH around the loop).
size_t SafeReadWide(const wchar_t* p, wchar_t* out, size_t maxChars) {
    if (!p || !out) return 0;
    size_t n = 0;
    __try {
        for (; n < maxChars; n++) {
            wchar_t c = p[n];
            out[n] = c;
            if (c == 0) break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (n < maxChars) out[n] = 0;
    else out[maxChars - 1] = 0;
    return n;
}

size_t SafeReadAscii(const char* p, char* out, size_t maxChars) {
    if (!p || !out) return 0;
    size_t n = 0;
    __try {
        for (; n < maxChars; n++) {
            char c = p[n];
            out[n] = c;
            if (c == 0) break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (n < maxChars) out[n] = 0;
    else out[maxChars - 1] = 0;
    return n;
}

// Try to decode the pointer as a string. Returns empty if it doesn't look
// like text. UTF-16 LE is the game-wide convention; ASCII is fallback.
std::wstring DecodePtr(void* p) {
    if (!p) return {};
    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
    // Filter obvious non-pointers: < 0x10000 is virtually always not a valid
    // text address.
    if (addr < 0x10000) return {};

    // UTF-16 LE attempt.
    {
        wchar_t buf[MAX_DECODE_CHARS];
        size_t n = SafeReadWide(reinterpret_cast<const wchar_t*>(p), buf, MAX_DECODE_CHARS);
        if (n > 0 && IsMostlyPrintable(buf, MAX_DECODE_CHARS)) {
            buf[MAX_DECODE_CHARS - 1] = 0;
            return std::wstring(buf);
        }
    }
    // ASCII attempt.
    {
        char buf[MAX_DECODE_CHARS];
        size_t n = SafeReadAscii(reinterpret_cast<const char*>(p), buf, MAX_DECODE_CHARS);
        if (n > 0 && IsMostlyPrintableAscii(buf, MAX_DECODE_CHARS)) {
            buf[MAX_DECODE_CHARS - 1] = 0;
            // Widen to wstring for uniform downstream handling.
            std::wstring w;
            w.reserve(n);
            for (size_t i = 0; i < n && buf[i] != 0; i++) {
                w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(buf[i])));
            }
            return w;
        }
    }
    return {};
}

void PushEvent(uint32_t callerRva, void* p) {
    std::wstring decoded = DecodePtr(p);
    if (decoded.empty()) return;

    TextCapture::TextEvent ev;
    ev.timestampMs = GetTickCount64();
    ev.frameId = g_frameCounter;
    ev.callerRva = callerRva;
    ev.text = std::move(decoded);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_ring.size() >= RING_MAX) g_ring.pop_front();
    g_ring.push_back(std::move(ev));
}

uint32_t GetCallerRva(void* returnAddr) {
    HMODULE h = GetModuleHandleA(nullptr);
    uintptr_t base = reinterpret_cast<uintptr_t>(h);
    uintptr_t ret = reinterpret_cast<uintptr_t>(returnAddr);
    if (ret <= base) return 0;
    return static_cast<uint32_t>(ret - base);
}

// Detour: call original first, then try to capture any string arg.
// Signature is conservative — we read args by frame pointer below since the
// actual count/types are uncertain. Win64 ABI: RCX/RDX/R8/R9 are first 4.
void __fastcall HookedTextEntry(void* a0, void* a1, void* a2, void* a3) {
    // Capture return address BEFORE calling original (which would overwrite).
    void* returnAddr = _ReturnAddress();

    // Call original first. If we crash in capture below, the game already
    // got its work done.
    if (s_origTextEntry) {
        s_origTextEntry(a0, a1, a2, a3);
    }

    g_frameCounter++;

    uint32_t callerRva = GetCallerRva(returnAddr);

    // Try every arg as a potential string pointer. Cheap — only writes
    // to the ring if a decode succeeds.
    PushEvent(callerRva, a0);
    PushEvent(callerRva, a1);
    PushEvent(callerRva, a2);
    PushEvent(callerRva, a3);
}

} // namespace

namespace TextCapture {

bool Init() {
    if (g_initialized) {
        Log::Write("TEXT", "TextCapture::Init called twice — ignoring");
        return true;
    }

    bool ok = Hooks::InstallTyped(RVA_TEXT_ENTRY, &HookedTextEntry, &s_origTextEntry);
    if (!ok) {
        Log::Write("TEXT", "Failed to install text-entry hook at RVA 0x5FA10 — "
                           "TextCapture remains dormant. MenuReader will not have text.");
        return false;
    }
    g_initialized = true;
    Log::Write("TEXT", "TextCapture installed (FUN_0017fa10 @ RVA 0x5FA10). "
                       "If Frida G-A3 identifies a different wrapper as the text "
                       "carrier, add its RVA here.");
    return true;
}

void Shutdown() {
    if (!g_initialized) return;
    Hooks::Uninstall(RVA_TEXT_ENTRY);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_ring.clear();
    }
    g_initialized = false;
    Log::Write("TEXT", "TextCapture shut down");
}

std::vector<TextEvent> RecentEvents(void* /*menuObj*/, uint64_t sinceTimestampMs,
                                    size_t maxEvents) {
    // Phase B keeps text events menu-agnostic — we haven't wired up the
    // wrapper -> menu_obj attribution yet. menuObj is accepted for API
    // stability; will be used once we know which wrapper arg carries the
    // owning menu pointer (Stream A G-A3 informs this).
    std::vector<TextEvent> out;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_ring.empty()) return out;

    // Walk newest-to-oldest; collect events at-or-after sinceTimestamp, up to
    // maxEvents; then reverse to chronological.
    out.reserve((std::min)(g_ring.size(), maxEvents));
    for (auto it = g_ring.rbegin(); it != g_ring.rend(); ++it) {
        if (it->timestampMs < sinceTimestampMs) break;
        out.push_back(*it);
        if (out.size() >= maxEvents) break;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

void DumpRingToLog(const char* reason) {
    std::lock_guard<std::mutex> lock(g_mutex);
    char header[160];
    snprintf(header, sizeof(header), "Ring dump (%s); %zu events",
             reason ? reason : "no-reason", g_ring.size());
    Log::Write("TEXT", header);
    int idx = 0;
    for (const auto& ev : g_ring) {
        // Convert wide to UTF-8 lossy for the log.
        char utf8[512] = {};
        if (!ev.text.empty()) {
            WideCharToMultiByte(CP_UTF8, 0, ev.text.c_str(), (int)ev.text.size(),
                                utf8, sizeof(utf8) - 1, nullptr, nullptr);
        }
        char line[768];
        snprintf(line, sizeof(line),
                 "  [%d] t=%llums frame=%u caller=0x%X text=\"%s\"",
                 idx++, (unsigned long long)ev.timestampMs, ev.frameId,
                 ev.callerRva, utf8);
        Log::Write("TEXT", line);
    }
}

} // namespace TextCapture
