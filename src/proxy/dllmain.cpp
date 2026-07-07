#include "proxy/dinput8_proxy.h"
#include "core/logger.h"
#include "core/hooks.h"
#include "speech/speech.h"
#include "input/input_tracker.h"
#include "ui/menu_observer.h"
#include "ui/text_capture.h"
#include "ui/menu_reader.h"
#include "ui/title_reader.h"
#include "ui/message_reader.h"
#include "navigation/navigation.h"

#include <Windows.h>
#include <Psapi.h>
#include <string>
#include <thread>
#include <atomic>

// DLL_PROCESS_ATTACH stages:
//   Stage A — minimal & fast (must not block the loader).
//     * load real System32\dinput8.dll so all exports work from frame 1
//     * spawn a background thread that does Stage B
//   Stage B — full mod init (background thread).
//     * start logger
//     * load Tolk (user-supplied)
//     * (later phases) initialize MinHook, validate RVAs from mod_config.ini,
//       install keyboard hook, register hotkeys, etc.

static HMODULE g_selfModule = nullptr;
static std::atomic<bool> g_modInitRan{false};

static std::string ResolveSelfDirectory() {
    char path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(g_selfModule, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return ".";
    for (DWORD i = len; i > 0; --i) {
        if (path[i - 1] == '\\' || path[i - 1] == '/') {
            path[i - 1] = '\0';
            return std::string(path);
        }
    }
    return ".";
}

// Stage B: full mod init. Runs on a detached background thread.
static void DeferredInitImpl() {
    // Brief pause so the loader can finish its own attach work before we
    // start touching the file system / loading more libraries.
    Sleep(100);

    std::string gameDir = ResolveSelfDirectory();
    Log::Init(gameDir);
    Log::Write("INIT", "Stage B starting (background thread)");

    bool tolkOk = Speech::Init();
    if (tolkOk) {
        // Startup announcement is diagnostic-only (not vocalized) by request.
        Log::Write("INIT", "Speech available — FFXII screen reader loaded.");
    } else {
        Log::Write("INIT", "Tolk unavailable — mod loaded silently. "
                           "User must place Tolk.dll + nvdaControllerClient64.dll in this folder.");
    }

    // Input timestamper (gates menu reader against animation-driven false
    // positives). Best-effort; if it fails the reader will still work but
    // will speak on every cursor-field jitter, including animation.
    InputTracker::Init();

    // Menu-reading pipeline. Order matters: hooks -> observer (installs
    // controller hooks) -> text_capture (installs wrapper hooks) -> reader
    // (subscribes to focus events; queries input_tracker on each event).
    if (Hooks::Init()) {
        MenuObserver::Init();
        TextCapture::Init();
        MenuReader::Init();
        // Title command menu (baked-sprite menu, separate from the in-game system).
        TitleReader::Init();
        // Dialogue + message-panel text reader (NPC dialogue, cutscene captions, item/
        // treasure/battle-system panels). Independent of the menu hooks above.
        MessageReader::Init();
        // Field navigation (Phase 4). M0: read-only leader/physics chain self-
        // diagnostic on the `\` key. Installs the map-load ctx-capture hook only —
        // no interpreter/action hooks (announce-only, non-interfering).
        Navigation::Init();
    } else {
        Log::Write("INIT", "MinHook init failed — menu reading disabled this session");
    }

    // Phase 2+ work plugs in here:
    //   - Config::Load(gameDir + "\\mod_config.ini")
    //   - Memory::ValidateRvas()  (byte-validate seeded RVAs; AOB self-heal mismatches)
    //   - Input::Init()           (WH_KEYBOARD_LL hook, hotkey state machine)
    //   - Per-menu specialized readers (Items, Equip, etc.)

    Log::Write("INIT", "Stage B complete");
}

static DWORD WINAPI DeferredInitThread(LPVOID) {
    bool expected = false;
    if (!g_modInitRan.compare_exchange_strong(expected, true)) return 0;

    __try {
        DeferredInitImpl();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Logger may not be alive yet; can't reliably write here.
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_selfModule = hinst;
            DisableThreadLibraryCalls(hinst);

            // Stage A: load System32\dinput8.dll so the game's DirectInput
            // exports resolve. Synchronous — this is fast (one LoadLibrary +
            // a handful of GetProcAddress) and must complete before the loader
            // returns control to the game.
            DInput8Proxy::Init();

            // Stage B: detached background thread.
            HANDLE h = CreateThread(nullptr, 0, DeferredInitThread, nullptr, 0, nullptr);
            if (h) CloseHandle(h);
            break;
        }
        case DLL_PROCESS_DETACH: {
            Navigation::Shutdown();
            MessageReader::Shutdown();
            TitleReader::Shutdown();
            MenuReader::Shutdown();
            TextCapture::Shutdown();
            MenuObserver::Shutdown();
            Hooks::Shutdown();
            InputTracker::Shutdown();
            Speech::Shutdown();
            Log::Shutdown();
            DInput8Proxy::Shutdown();
            break;
        }
        default:
            break;
    }
    return TRUE;
}
