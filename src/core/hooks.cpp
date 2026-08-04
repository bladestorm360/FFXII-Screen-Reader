#include "core/hooks.h"
#include "core/logger.h"

#include <Windows.h>
#include <MinHook/MinHook.h>
#include <mutex>
#include <unordered_map>

namespace {

std::mutex g_mutex;
std::unordered_map<uint32_t, void*> g_installed;  // rva -> target absolute addr
bool g_initialized = false;
uintptr_t g_imageBase = 0;

// Install census -- see Hooks::LogInstallCensus in the header for why. All three are written only
// under g_mutex from Install(), so they need no atomics.
int g_attempted = 0;
int g_succeeded = 0;
int g_failedAlloc = 0;   // MEMORY_ALLOC specifically: the trampoline pool ran dry
uint32_t g_firstFailedRva = 0;

uintptr_t ResolveImageBase() {
    HMODULE h = GetModuleHandleA(nullptr);
    return reinterpret_cast<uintptr_t>(h);
}

const char* MhStatusName(MH_STATUS s) {
    switch (s) {
        case MH_OK: return "OK";
        case MH_ERROR_ALREADY_INITIALIZED: return "ALREADY_INITIALIZED";
        case MH_ERROR_NOT_INITIALIZED: return "NOT_INITIALIZED";
        case MH_ERROR_ALREADY_CREATED: return "ALREADY_CREATED";
        case MH_ERROR_NOT_CREATED: return "NOT_CREATED";
        case MH_ERROR_ENABLED: return "ENABLED";
        case MH_ERROR_DISABLED: return "DISABLED";
        case MH_ERROR_NOT_EXECUTABLE: return "NOT_EXECUTABLE";
        case MH_ERROR_UNSUPPORTED_FUNCTION: return "UNSUPPORTED_FUNCTION";
        case MH_ERROR_MEMORY_ALLOC: return "MEMORY_ALLOC";
        case MH_ERROR_MEMORY_PROTECT: return "MEMORY_PROTECT";
        case MH_ERROR_MODULE_NOT_FOUND: return "MODULE_NOT_FOUND";
        case MH_ERROR_FUNCTION_NOT_FOUND: return "FUNCTION_NOT_FOUND";
        case MH_UNKNOWN: default: return "UNKNOWN";
    }
}

} // namespace

namespace Hooks {

bool Init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) {
        Log::Write("HOOKS", "Hooks::Init called twice — ignoring");
        return true;
    }

    g_imageBase = ResolveImageBase();
    if (!g_imageBase) {
        Log::Write("HOOKS", "GetModuleHandle(nullptr) returned 0 — cannot resolve RVAs");
        return false;
    }

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK) {
        char msg[128];
        snprintf(msg, sizeof(msg), "MH_Initialize failed: %s", MhStatusName(s));
        Log::Write("HOOKS", msg);
        return false;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Hooks initialized; image base = 0x%llx",
             (unsigned long long)g_imageBase);
    Log::Write("HOOKS", msg);
    g_initialized = true;
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_installed.clear();
    g_initialized = false;
    Log::Write("HOOKS", "Hooks shut down");
}

void* ResolveRva(uint32_t rva) {
    if (!g_imageBase) {
        g_imageBase = ResolveImageBase();
    }
    if (!g_imageBase) return nullptr;
    return reinterpret_cast<void*>(g_imageBase + rva);
}

bool Install(uint32_t rva, void* detour, void** original_out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) {
        Log::Write("HOOKS", "Install before Init — refusing");
        return false;
    }
    if (!detour) {
        Log::Write("HOOKS", "Install with null detour — refusing");
        return false;
    }

    if (g_installed.count(rva)) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Install: RVA 0x%X already hooked — skipping (defensive)", rva);
        Log::Write("HOOKS", msg);
        return false;
    }

    void* target = reinterpret_cast<void*>(g_imageBase + rva);
    ++g_attempted;

    MH_STATUS s = MH_CreateHook(target, detour, original_out);
    if (s != MH_OK) {
        if (!g_firstFailedRva) g_firstFailedRva = rva;
        if (s == MH_ERROR_MEMORY_ALLOC) ++g_failedAlloc;
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "MH_CreateHook failed at RVA 0x%X (abs 0x%llx): %s",
                 rva, (unsigned long long)reinterpret_cast<uintptr_t>(target),
                 MhStatusName(s));
        Log::Write("HOOKS", msg);
        return false;
    }

    s = MH_EnableHook(target);
    if (s != MH_OK) {
        if (!g_firstFailedRva) g_firstFailedRva = rva;
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "MH_EnableHook failed at RVA 0x%X: %s — removing", rva, MhStatusName(s));
        Log::Write("HOOKS", msg);
        MH_RemoveHook(target);
        return false;
    }

    ++g_succeeded;
    g_installed[rva] = target;
    char msg[160];
    snprintf(msg, sizeof(msg),
             "Installed hook RVA 0x%X -> abs 0x%llx (detour 0x%llx)",
             rva, (unsigned long long)reinterpret_cast<uintptr_t>(target),
             (unsigned long long)reinterpret_cast<uintptr_t>(detour));
    Log::Write("HOOKS", msg);
    return true;
}

void LogInstallCensus() {
    std::lock_guard<std::mutex> lock(g_mutex);
    const int failed = g_attempted - g_succeeded;
    char msg[256];
    if (failed == 0) {
        snprintf(msg, sizeof(msg), "install census: %d attempted, %d installed, 0 failed",
                 g_attempted, g_succeeded);
        Log::Write("HOOKS", msg);
        return;
    }
    snprintf(msg, sizeof(msg),
             "install census: %d attempted, %d installed, %d FAILED (%d of them MEMORY_ALLOC); "
             "first failure at RVA 0x%X",
             g_attempted, g_succeeded, failed, g_failedAlloc, g_firstFailedRva);
    Log::Write("HOOKS", msg);
    if (g_failedAlloc > 0) {
        // Naming the cause in the log, because the raw MinHook status does not: MEMORY_ALLOC here
        // means the trampoline pool could not place a block within +/-1GB of the target, NOT that
        // the machine is out of memory. See the [LOCAL] note in include/MinHook/buffer.c.
        Log::Write("HOOKS",
                   "MEMORY_ALLOC = MinHook could not place a trampoline block within +/-1GB of the "
                   "target. Every feature behind a failed hook is silently dead this session.");
    }
}

bool Uninstall(uint32_t rva) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_installed.find(rva);
    if (it == g_installed.end()) return true;
    void* target = it->second;
    MH_DisableHook(target);
    MH_RemoveHook(target);
    g_installed.erase(it);
    char msg[80];
    snprintf(msg, sizeof(msg), "Uninstalled hook RVA 0x%X", rva);
    Log::Write("HOOKS", msg);
    return true;
}

} // namespace Hooks
