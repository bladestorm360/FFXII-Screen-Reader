#include "ui/airship_diag.h"

#include "core/hooks.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "ui/text_capture.h"

#include <Windows.h>
#include <cstdio>

namespace AirshipDiag {
namespace {

// The pane's own message handler. obj[0] IS this function pointer, which is why the census line in
// menu_reader.cpp reports the class as RVA 0x4328C0 -- the class identity and the handler address
// are the same address, so hooking this RVA hooks exactly this pane and nothing else.
constexpr uint32_t RVA_PANE_HANDLER = 0x4328C0;

// Pane fields (see the header for provenance; all read-only, all SEH-guarded).
constexpr uint32_t PANE_NODE_HEAD = 0x118;
constexpr uint32_t PANE_SELECTED  = 0x120;
constexpr uint32_t PANE_HOVER     = 0x9F40;

// Node fields.
constexpr uint32_t NODE_ID     = 0x39;
constexpr uint32_t NODE_X      = 0x3C;
constexpr uint32_t NODE_Y      = 0x3E;
constexpr uint32_t NODE_FLAGS  = 0x54;
constexpr uint32_t NODE_RENDER = 0x130;
constexpr uint32_t NODE_NEXT   = 0x140;

// Message categories on the packet's first word, from the handler's own switch.
constexpr int MSG_UPDATE = 2;     // per-frame tick
constexpr int MSG_INPUT  = 0xA;   // input; 0xB is the same branch

// How much of a node to dump. The next pointer sits at +0x140, so 0x160 covers the whole record
// including it -- the point is to find the name field, and a partial dump is how you miss it.
constexpr uint32_t NODE_DUMP_BYTES = 0x160;

constexpr int MAX_NODES      = 64;   // a walk guard, not a belief about the count
constexpr int MAX_HOVER_LINE = 40;   // budget: a walked cursor must not flood the log

// ARITY, and why this is four parameters rather than the two the decompile shows.
//
// Ghidra infers a parameter list from what a body USES, not from the ABI. The base handler this pane
// falls through to decompiles as `FUN_005c5230(void)` and is called with two arguments one line
// later -- proof, in this very call graph, that those signatures cannot be trusted. Session 129
// crashed the game in a shop by taking that inference at face value: a 4-arg detour on a 6-arg
// function, and the trampoline call read two registers of garbage.
//
// Declaring MORE parameters than the callee takes is the safe direction on x64: the extra arguments
// ride in R8/R9, and a callee that wants only two simply never looks at them. Declaring FEWER is the
// crash. So take four, forward four, assume two.
typedef uint64_t(*Pfn_PaneHandler)(void* pane, int* packet, uint64_t a3, uint64_t a4);
Pfn_PaneHandler s_origHandler = nullptr;

void*  g_censusPane  = nullptr;   // the pane instance the census has already run for
void*  g_lastHover   = nullptr;
int    g_hoverLines  = 0;

// One node, one line. Returns false when the record cannot be read at all.
bool LogNode(int index, void* node) {
    uint8_t  id = 0, flags = 0;
    int16_t  x = 0, y = 0;
    uint32_t render = 0;
    const bool okId = MemRead::SafeReadU8(node, NODE_ID, &id);
    MemRead::SafeReadU8(node, NODE_FLAGS, &flags);
    MemRead::SafeReadS16(node, NODE_X, &x);
    MemRead::SafeReadS16(node, NODE_Y, &y);
    MemRead::SafeReadU32(node, NODE_RENDER, &render);
    if (!okId) return false;

    char m[192];
    snprintf(m, sizeof(m),
             "  node[%02d] %p id=0x%02X flags=0x%02X xy=(%d,%d) render=0x%08X",
             index, node, id, flags, static_cast<int>(x), static_cast<int>(y), render);
    Log::Write("AIRSHIP", m);
    return true;
}

// The whole record for ONE node, 16 bytes to a line. This is the line that is expected to answer the
// open question: a codec pointer or a text id on the node is what a destination's NAME must come
// from, and neither is anywhere in the handler.
void DumpNodeRecord(void* node) {
    uint8_t buf[NODE_DUMP_BYTES];
    if (!MemRead::SafeReadBytes(node, buf, sizeof(buf))) {
        Log::Write("AIRSHIP", "  node record unreadable -- skipping the dump");
        return;
    }
    char m[160];
    snprintf(m, sizeof(m), "  ---- node %p, first 0x%X bytes ----", node, NODE_DUMP_BYTES);
    Log::Write("AIRSHIP", m);
    for (uint32_t off = 0; off < NODE_DUMP_BYTES; off += 16) {
        char line[128];
        int n = snprintf(line, sizeof(line), "  +%03X ", off);
        for (int i = 0; i < 16; ++i) {
            n += snprintf(line + n, sizeof(line) - n, "%02X", buf[off + i]);
            if ((i & 3) == 3) n += snprintf(line + n, sizeof(line) - n, " ");
        }
        Log::Write("AIRSHIP", line);
    }
}

// Fires once per pane instance, on the first message that reaches a pane we have not censused.
void RunCensus(void* pane) {
    char m[192];
    void* head = nullptr;
    void* hover = nullptr;
    void* sel = nullptr;
    MemRead::SafeReadPtr(reinterpret_cast<uint8_t*>(pane) + PANE_NODE_HEAD, &head);
    MemRead::SafeReadPtr(reinterpret_cast<uint8_t*>(pane) + PANE_HOVER, &hover);
    MemRead::SafeReadPtr(reinterpret_cast<uint8_t*>(pane) + PANE_SELECTED, &sel);

    snprintf(m, sizeof(m), "CENSUS pane=%p head=%p hover=%p selected=%p", pane, head, hover, sel);
    Log::Write("AIRSHIP", m);

    int count = 0;
    void* node = head;
    void* first = head;
    while (node && count < MAX_NODES) {
        if (!LogNode(count, node)) break;
        ++count;
        void* next = nullptr;
        if (!MemRead::SafeReadPtr(reinterpret_cast<uint8_t*>(node) + NODE_NEXT, &next)) break;
        if (next == node) break;              // a self-link would spin forever
        node = next;
    }
    snprintf(m, sizeof(m), "CENSUS walked %d node(s)%s", count,
             count >= MAX_NODES ? " (hit the walk guard -- there may be more)" : "");
    Log::Write("AIRSHIP", m);

    if (first) DumpNodeRecord(first);

    // Built in Session 112 and never once called. If the destination labels are drawn through any of
    // the resolvers TextCapture already hooks, they are sitting in this ring right now.
    TextCapture::DumpRingToLog("airship destination pane");
}

uint64_t HookedPaneHandler(void* pane, int* packet, uint64_t a3, uint64_t a4) {
    int category = -1;
    if (pane && packet) MemRead::SafeReadInt(packet, &category);

    if (pane && (category == MSG_UPDATE || category == MSG_INPUT || category == MSG_INPUT + 1)) {
        if (g_censusPane != pane) {
            g_censusPane = pane;
            g_lastHover  = nullptr;
            g_hoverLines = 0;
            RunCensus(pane);
        }
        // CHANGE-CHECK, and it guards a PER-FRAME function: this handler's category 2 is the pane's
        // own tick, so without it every frame would log. This is the sanctioned exception -- the
        // no-dedup rule is about SPEECH, and this file speaks nothing.
        void* hover = nullptr;
        if (MemRead::SafeReadPtr(reinterpret_cast<uint8_t*>(pane) + PANE_HOVER, &hover) &&
            hover != g_lastHover && g_hoverLines < MAX_HOVER_LINE) {
            g_lastHover = hover;
            ++g_hoverLines;
            uint8_t id = 0, flags = 0;
            int16_t x = 0, y = 0;
            if (hover) {
                MemRead::SafeReadU8(hover, NODE_ID, &id);
                MemRead::SafeReadU8(hover, NODE_FLAGS, &flags);
                MemRead::SafeReadS16(hover, NODE_X, &x);
                MemRead::SafeReadS16(hover, NODE_Y, &y);
            }
            char m[192];
            snprintf(m, sizeof(m), "hover -> %p id=0x%02X flags=0x%02X xy=(%d,%d) cat=%d",
                     hover, id, flags, static_cast<int>(x), static_cast<int>(y), category);
            Log::Write("AIRSHIP", m);
        }
    }

    return s_origHandler ? s_origHandler(pane, packet, a3, a4) : 0;
}

}  // namespace

bool Init() {
    const bool ok = Hooks::InstallTyped(RVA_PANE_HANDLER, &HookedPaneHandler, &s_origHandler);
    Log::Write("AIRSHIP", ok
        ? "AirshipDiag: pane handler hooked (FUN_005528c0, RVA 0x4328C0) -- LOG ONLY, speaks nothing"
        : "AirshipDiag: FUN_005528c0 hook FAILED -- the destination pane stays undiagnosed");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_PANE_HANDLER);
    g_censusPane = nullptr;
    g_lastHover  = nullptr;
    g_hoverLines = 0;
}

}  // namespace AirshipDiag
