#include "ui/airship_reader.h"

#include "core/game_text.h"
#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "speech/speech.h"

#include <Windows.h>
#include <cstdio>
#include <string>
#include <vector>

namespace AirshipReader {
namespace {

// obj[0] IS this function pointer, which is why the unclaimed-pane census reported the class as
// RVA 0x4328C0: the class identity and the handler address are one address. Hooking this RVA
// therefore hooks exactly this pane and nothing else in the game.
constexpr uint32_t RVA_PANE_HANDLER = 0x4328C0;

constexpr uint32_t PANE_NODE_HEAD = 0x118;
constexpr uint32_t PANE_HOVER     = 0x9F40;

constexpr uint32_t NODE_ID    = 0x39;
constexpr uint32_t NODE_NAME  = 0x48;    // packed codec text; +0x88 mirrors it
constexpr uint32_t NODE_FLAGS = 0x54;
constexpr uint32_t NODE_NEXT  = 0x140;

constexpr int MSG_UPDATE = 2;      // the pane's per-frame tick
constexpr int MSG_INPUT  = 0xA;    // input; 0xB shares the branch

constexpr int  MAX_NODES     = 64;   // a walk guard, not a belief about the count
constexpr size_t NAME_MAX_BYTES = 128;

// ARITY -- four parameters, though the decompile shows two, and this is deliberate.
//
// Ghidra infers a parameter list from what a body USES, not from the ABI. In this very call graph
// the base handler decompiles as `FUN_005c5230(void)` and is called with two arguments one line
// later, so those signatures cannot be trusted. Session 129 crashed the game in a shop by believing
// one: a 4-arg detour on a 6-arg function, and the trampoline read two registers of garbage.
//
// On x64, declaring MORE parameters than the callee takes is the safe direction -- the extras ride
// in R8/R9 and a callee that wants two never looks at them. Declaring FEWER is the crash.
typedef uint64_t(*Pfn_PaneHandler)(void* pane, int* packet, uint64_t a3, uint64_t a4);
Pfn_PaneHandler s_origHandler = nullptr;

void* g_censusPane   = nullptr;   // the pane instance already censused
void* g_lastHover    = nullptr;
bool  g_namesTrusted = false;

std::wstring NameOf(void* node) {
    if (!node) return std::wstring();
    void* p = nullptr;
    if (!MemRead::SafeReadPtr(reinterpret_cast<uint8_t*>(node) + NODE_NAME, &p) || !p) {
        return std::wstring();
    }
    std::wstring t = GameText::Decode(reinterpret_cast<const uint8_t*>(p), NAME_MAX_BYTES);
    if (t.empty() || !GameText::IsMostlyPrintable(t)) return std::wstring();
    return t;
}

// Walk the graph once per pane instance and decide whether +0x48 has earned any speech.
//
// THE TEST IS DISTINCTNESS, because that is the thing a single dumped record could not settle. A
// shared placeholder decodes to one printable string repeated 34 times and looks exactly like a real
// name field until you compare records. Requiring two different names is what separates them.
void RunCensus(void* pane) {
    void* head = nullptr;
    MemRead::SafeReadPtr(reinterpret_cast<uint8_t*>(pane) + PANE_NODE_HEAD, &head);

    std::vector<std::wstring> names;
    int  count = 0, named = 0;
    void* node = head;
    while (node && count < MAX_NODES) {
        uint8_t id = 0, flags = 0;
        if (!MemRead::SafeReadU8(node, NODE_ID, &id)) break;
        MemRead::SafeReadU8(node, NODE_FLAGS, &flags);

        const std::wstring name = NameOf(node);
        if (!name.empty()) {
            ++named;
            names.push_back(name);
            char pre[96];
            snprintf(pre, sizeof(pre), "  node id=0x%02X flags=0x%02X name= ", id, flags);
            Log::WriteW("AIRSHIP", pre, name);
        }
        ++count;

        void* next = nullptr;
        if (!MemRead::SafeReadPtr(reinterpret_cast<uint8_t*>(node) + NODE_NEXT, &next)) break;
        if (next == node) break;               // a self-link would spin forever
        node = next;
    }

    bool distinct = false;
    for (size_t i = 1; i < names.size() && !distinct; ++i) {
        if (names[i] != names[0]) distinct = true;
    }
    g_namesTrusted = distinct;

    char m[224];
    snprintf(m, sizeof(m),
             "CENSUS pane=%p nodes=%d named=%d distinct=%s -> destinations %s",
             pane, count, named, distinct ? "yes" : "NO",
             distinct ? "will be spoken"
                      : "STAY SILENT (node+0x48 is not a per-node name on this surface)");
    Log::Write("AIRSHIP", m);
}

void SpeakHover(void* node) {
    const std::wstring name = NameOf(node);
    if (name.empty()) return;                  // an unnamed node is a waypoint: say nothing
    Speech::Output(name);
    Log::WriteW("AIRSHIP", "  speak= ", name);
}

uint64_t HookedPaneHandler(void* pane, int* packet, uint64_t a3, uint64_t a4) {
    int category = -1;
    if (pane && packet) MemRead::SafeReadInt(packet, &category);

    if (pane && (category == MSG_UPDATE || category == MSG_INPUT || category == MSG_INPUT + 1)) {
        if (g_censusPane != pane) {
            g_censusPane = pane;
            g_lastHover  = nullptr;
            RunCensus(pane);
        }

        void* hover = nullptr;
        if (MemRead::SafeReadPtr(reinterpret_cast<uint8_t*>(pane) + PANE_HOVER, &hover) &&
            hover != g_lastHover) {
            // CHANGE-CHECK GUARDING A PER-FRAME FUNCTION: category 2 is this pane's own tick
            // (`FUN_005528c0`), so without this every frame would re-speak the marker under the
            // cursor. That is the sanctioned exception to the no-dedup rule -- it detects the
            // cursor MOVING, it does not suppress a repeat. Re-entering a marker still speaks,
            // because leaving and returning changes the pointer both ways.
            g_lastHover = hover;
            if (g_namesTrusted) SpeakHover(hover);
        }
    }

    return s_origHandler ? s_origHandler(pane, packet, a3, a4) : 0;
}

}  // namespace

bool Init() {
    const bool ok = Hooks::InstallTyped(RVA_PANE_HANDLER, &HookedPaneHandler, &s_origHandler);
    Log::Write("AIRSHIP", ok
        ? "AirshipReader: destination map hooked (FUN_005528c0, RVA 0x4328C0)"
        : "AirshipReader: FUN_005528c0 hook FAILED -- the destination map stays silent");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_PANE_HANDLER);
    g_censusPane   = nullptr;
    g_lastHover    = nullptr;
    g_namesTrusted = false;
}

}  // namespace AirshipReader
