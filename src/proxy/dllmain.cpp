#include "proxy/dinput8_proxy.h"
#include "core/logger.h"
#include "core/hooks.h"
#include "core/frame_probe.h"
#include "core/game_text.h"
#include "battle/battle_state.h"
#include "speech/speech.h"
#include "input/input_tracker.h"
#include "input/pad_hook.h"
#include "input/gamepad_sdl.h"
#include "ui/text_capture.h"
#include "ui/menu_reader.h"
#include "ui/title_reader.h"
#include "ui/message_reader.h"
#include "ui/dialogue_reader.h"
#include "ui/primer_reader.h"
#include "ui/mod_menu.h"
#include "ui/text_prompt.h"
#include "audio/audio_engine.h"
#include "navigation/navigation.h"
#include "battle/combat_events.h"

#include <Windows.h>
#include <Psapi.h>
#include <string>
#include <thread>
#include <atomic>

// ============================================================================
// tkMalloc fix: reserve memory below 2GB before the game starts.
// FFXII uses legacy PS2 code with 32-bit memory pointers. The game's custom
// allocator (tkMalloc) can't handle memory above 2GB. By reserving memory
// at 0x10000000 before the game's main function runs, we ensure the allocator
// has a valid region to use. This prevents crashes caused by driver/shader
// cache pushing allocations above the 2GB limit.
// ============================================================================

static constexpr uintptr_t TKMALLOC_PREFERRED_ADDRESS = 0x10000000;
static constexpr size_t TKMALLOC_RESERVE_SIZE = 0x12000000;  // 288 MB
static void* g_tkMallocReserved = nullptr;

static void* FindFreeRegionBelow2GB(size_t size) {
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    const uintptr_t granularity = sysInfo.dwAllocationGranularity;

    uintptr_t addr = 0x10000;  // skip low null-guard region
    const uintptr_t limit = 0x7FFFFFFF;

    while (addr < limit) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0)
            break;

        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t regionEnd = base + mbi.RegionSize;

        if (mbi.State == MEM_FREE) {
            uintptr_t aligned = (base + granularity - 1) & ~(granularity - 1);
            if (aligned >= base && regionEnd - aligned >= size && aligned < limit) {
                return reinterpret_cast<void*>(aligned);
            }
        }

        if (regionEnd == 0 || regionEnd <= addr) break;
        addr = regionEnd;
    }
    return nullptr;
}

static bool ReserveTkMallocMemory() {
    if (g_tkMallocReserved) return true;

    // Try preferred address first
    void* ptr = VirtualAlloc(
        reinterpret_cast<LPVOID>(TKMALLOC_PREFERRED_ADDRESS),
        TKMALLOC_RESERVE_SIZE,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE);

    if (!ptr) {
        // Preferred address occupied — scan for a free region below 2GB
        void* region = FindFreeRegionBelow2GB(TKMALLOC_RESERVE_SIZE);
        if (!region) return false;

        ptr = VirtualAlloc(region, TKMALLOC_RESERVE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }

    if (ptr) {
        g_tkMallocReserved = ptr;
        // Touch every page to fully commit the region
        volatile char* p = static_cast<volatile char*>(ptr);
        for (size_t i = 0; i < TKMALLOC_RESERVE_SIZE; i += 4096)
            p[i] = 0;
        p[TKMALLOC_RESERVE_SIZE - 1] = 0;
    }

    return ptr != nullptr;
}

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

    // The mod's own settings menu (F8) and the combat-verbosity toggle (F4). Deliberately OUTSIDE
    // the Hooks::Init() block below: it installs no hooks, only input callbacks and a settings file,
    // so it must keep working on a session where MinHook fails and the player needs to hear why.
    ModMenu::Init();

    // THE PAD, in two halves. AFTER ModMenu::Init, because the router asks it whether the Controller
    // setting is on before it reads or writes anything. Outside the Hooks block below for the same
    // reason ModMenu and AudioEngine are: neither half installs a MinHook hook, so both must keep
    // working on a session where MinHook fails.
    //
    // GamepadSDL is the READER and it is the one that makes controller support work for controllers
    // that are not Xbox pads -- SDL3 normalizes a DualSense, a DualShock 4, a Switch Pro pad and a
    // generic stick into one button layout. PadHook is now only the XInput half of SUPPRESSION; it
    // patches one IAT cell and no longer reads anything.
    GamepadSDL::Init();
    PadHook::Init();

    // SDL3 audio for the navigation beacon. Outside the Hooks block for the same reason — it
    // installs no hooks. A failure here is not fatal: AudioEngine::Available() goes false and the
    // beacon silently does nothing, exactly as speech no-ops when Tolk is missing.
    AudioEngine::Init();

    // Menu-reading pipeline. Order matters: hooks -> text_capture (installs
    // wrapper hooks) -> reader (subscribes to focus events; queries
    // input_tracker on each event).
    if (Hooks::Init()) {
        // Frame-pacing report. Installs no hook — it is ticked from the field-frame and input-poll
        // hooks that already exist. Must follow Hooks::Init so RVAs resolve. See frame_probe.h.
        FrameProbe::Init();
        // The codec decoder turns the eight inline ELEMENT sprites into words, but core/ must not
        // depend on battle/, so it asks a resolver for the name. Register it before anything can
        // decode: without it every element icon is silently dropped, which is what made an
        // accessory read "Half Damage: " and then stop.
        GameText::SetElementSpriteResolver(&BattleState::ElementName);
        TextCapture::Init();
        MenuReader::Init();
        // Title command menu (baked-sprite menu, separate from the in-game system).
        TitleReader::Init();
        // Message-panel text reader (obtained-item toast, menu system messages). Independent of
        // the menu hooks above.
        MessageReader::Init();
        // Field dialogue: NPC conversations, cutscene captions and tutorial banners, paginated off
        // the game's own page cursor so every input device turns the page. Must follow
        // MessageReader::Init — it feeds that module's shared `t` re-read store.
        DialogueReader::Init();
        // Clan Primer body text (Bestiary / Hunts / Traveller's Tips / Sky Pirate's Den). The entry
        // LIST rows already spoke through the universal painter; this is the BODY, which has no
        // cursor and so had nothing to announce it. Also registers the arrow-key page walk.
        PrimerReader::Init();
        InputTracker::SetPrimerNavCallback(&PrimerReader::OnMenuNavKey);
        // Field navigation (Phase 4). M0: read-only leader/physics chain self-
        // diagnostic on the `\` key. Installs the map-load ctx-capture hook only —
        // no interpreter/action hooks (announce-only, non-interfering).
        Navigation::Init();
        // Combat log: the game's own battle sentences (Tier 1) plus synthesized
        // damage lines (Tier 2). Installs three hooks — the codec-sprintf, the result applier and
        // the reward/death batch.
        //
        // IT IS LAST, AND THAT MATTERS. When MinHook's trampoline pool ran dry on a tester's
        // machine, these three were the ones that fell off the end. The census below is what makes
        // that visible; do not move this call without reading Hooks::LogInstallCensus first.
        if (!CombatEvents::Init())
            Log::Write("INIT", "combat log is DEAD this session — see the hook census below");

        // Every hook the mod installs has now been attempted. One line saying how many took.
        Hooks::LogInstallCensus();
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

// `reserved` IS NOT SPARE — it is the difference between a clean exit and a hung process.
//
// On DLL_PROCESS_DETACH Windows sets it NON-NULL when THE PROCESS IS TERMINATING and NULL only when
// someone called FreeLibrary on us. In the terminating case the documented contract is to do NOTHING:
// the loader lock is held, every other thread has already been killed without releasing whatever it
// held, and the OS is about to reclaim all memory, handles, threads and DLLs anyway.
//
// WE IGNORED IT, AND IT LEFT A PHANTOM PROCESS ON EVERY EXIT (Session 96, reported by the tester as
// starting "a couple sessions ago" — which dates it to Session 92, when SDL3 arrived). Three separate
// illegal calls were reachable from here:
//
//   1. AudioEngine::Shutdown -> SDL_QuitSubSystem(SDL_INIT_AUDIO). SDL's audio device runs on its own
//      worker thread, and quitting the subsystem SIGNALS AND JOINS it. That thread needs the loader
//      lock to finish exiting; we are holding the loader lock waiting for it. Textbook deadlock, and
//      the newest of the three — which is exactly why the symptom appeared when it did.
//   2. Speech::Shutdown -> FreeLibrary(Tolk.dll). Calling FreeLibrary from DllMain is explicitly
//      forbidden: it re-enters the loader we are already inside.
//   3. DInput8Proxy::Shutdown -> FreeLibrary(the real dinput8). Same violation.
//
// (Hooks::Shutdown is a fourth hazard: MinHook's MH_Uninitialize suspends and resumes threads, which
// is also unsafe under the loader lock.)
//
// So: on process termination, return immediately. The cleanup below is kept for the FreeLibrary case,
// which for a proxy DLL the game loads at startup and never unloads is effectively unreachable — but
// it is the one case where the calls are legal, so it stays rather than being deleted.
//
// Nothing is lost by skipping it. The log is written and flushed per line as the session plays, not
// assembled and dumped at exit, so the file on disk is already complete.
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_selfModule = hinst;
            DisableThreadLibraryCalls(hinst);

            // Stage A: load System32\dinput8.dll so the game's DirectInput
            // exports resolve. Synchronous — this is fast (one LoadLibrary +
            // a handful of GetProcAddress) and must complete before the loader
            // returns control to the game.
            DInput8Proxy::Init();

            // tkMalloc fix: reserve memory below 2GB BEFORE the game starts.
            // FFXII's custom allocator uses 32-bit pointers and crashes when
            // memory is allocated above 2GB. This must happen synchronously
            // in DLL_PROCESS_ATTACH, before the game's main function runs.
            ReserveTkMallocMemory();

            // Stage B: detached background thread.
            HANDLE h = CreateThread(nullptr, 0, DeferredInitThread, nullptr, 0, nullptr);
            if (h) CloseHandle(h);
            break;
        }
        case DLL_PROCESS_DETACH: {
            // THE PROCESS IS GOING AWAY -- do nothing. See the note above DllMain.
            if (reserved != nullptr) break;

            // Release tkMalloc reserved memory
            if (g_tkMallocReserved) {
                VirtualFree(g_tkMallocReserved, 0, MEM_RELEASE);
                g_tkMallocReserved = nullptr;
            }
            CombatEvents::Shutdown();
            // Before Navigation: the beacon lives under it and must stop pinging before the audio
            // device closes.
            AudioEngine::Shutdown();
            // Before InputTracker and Speech go: a prompt still on screen would call back into
            // both of them after they had been torn down.
            TextPrompt::Shutdown();
            ModMenu::Shutdown();
            Navigation::Shutdown();
            DialogueReader::Shutdown();
            InputTracker::SetPrimerNavCallback(nullptr);
            PrimerReader::Shutdown();
            MessageReader::Shutdown();
            TitleReader::Shutdown();
            MenuReader::Shutdown();
            TextCapture::Shutdown();
            Hooks::Shutdown();
            PadHook::Shutdown();
            GamepadSDL::Shutdown();
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
