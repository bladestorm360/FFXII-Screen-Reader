#include "navigation/map_exits.h"
#include "navigation/map_names.h"
#include "navigation/nav_rva.h"
#include "navigation/map_rva.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"
#include "core/logger.h"
#include "core/game_text.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

namespace MapExits {

// The mapData+0x54 table = the MAP-JUMP POINT table (getmapjumpposbyindex) — the intra-map "Mapjump"
// transitions that move the party between areas. Tester-confirmed by walking into them. See the header
// for why Session 43's "arrival/spawn, not exits" demotion was wrong (FUN_00353490 is
// getmapjumpanglebyindex, not a party-placement call).
//
// REMOVED (Session 55): DiagScanRecordForDest, which fed every u16 in a record to the area-name resolver
// and logged whatever came back. It produced only FALSE POSITIVES and actively misled: on East End it
// read a field-sign record's destIdx byte pair as area 768 and announced "Eruyt Village: Road of Verdant
// Praise", and elsewhere "Draklor Laboratory: Rm 6612 West" and "Trial Mode: Stage 90" out of offset
// bytes. A resolver that accepts any in-range id will always "find" a name in arbitrary bytes; that is
// fishing, not evidence. The destination fields are now known (see exit_diag.cpp) and are read directly.

// SEH-guarded bulk copy, isolated so callers can use C++ objects (a function with __try can't also unwind).
static bool SehReadBytes(const void* src, void* dst, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// DIAGNOSTIC (log-only): the field-door DESTINATION is a LITERAL in the map's loaded field script — the
// game runs `mapjump(destMapId, entrance, flags)` encoded as three push-immediates then the native call:
//   4f <destU16> 4f <entU16> 4f <flagsU16> 5d 8d 00   (0x4f = push u16; 0x5d = CALLACTPOPA; native 0x8d).
// Runtime-confirmed (hook_mapjump_trace): Lower Apartments 279 -> mapjump(280,1,0). This scans the loaded
// blob for that pattern and logs each mapjump's dest + resolved name + entrance + preceding bytecode (the
// dispatch that ties a source trigger to a mapjump) so the source-door<->dest pairing can be finalized.
void DiagScanScriptMapjumps() {
    void* containerBase = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!containerBase) return;
    void* mapData = MemRead::PtrAt(containerBase, NavRva::TBL_GUARD_OFF);   // *(u64*)(containerBase+0)
    if (!mapData) return;

    // Read the blob into a local buffer, page by page, stopping at the first unmapped page.
    // WIN_START is 0 (was 0x400): the routine table shows the map's FIRST routines live BELOW +0x400,
    // and the init/setup routine that BINDS each +0x54 trigger to its destination handler is down there.
    // Every prior dump started at +0x400, so we only ever captured the transition EXECUTION, never the
    // wiring that decides which door runs which handler — the one fact the door<->dest pairing needs.
    constexpr size_t WIN_START = 0x0, MAX = 0x18000, PAGE = 0x1000;
    std::vector<uint8_t> buf;
    buf.reserve(MAX);
    uint8_t page[PAGE];
    for (size_t off = WIN_START; off < WIN_START + MAX; off += PAGE) {
        if (!SehReadBytes(static_cast<const char*>(mapData) + off, page, PAGE)) break;
        buf.insert(buf.end(), page, page + PAGE);
    }
    const int here = MapNames::CurrentMapId();
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "==== field-script mapjump literals  (curMap=%d, scanned 0x%zX bytes) ====",
             here, buf.size());
    Log::Write("NAV-DIAG", hdr);

    int found = 0;
    std::vector<size_t> field0;   // buffer offsets of flags=0 (field-door) mapjump calls -> dispatch dump range
    for (size_t i = 9; i + 3 <= buf.size(); ++i) {
        if (buf[i] != 0x5D || buf[i + 1] != 0x8D || buf[i + 2] != 0x00) continue;   // CALLACTPOPA mapjump(0x8d)
        const bool lit = (buf[i - 9] == 0x4F && buf[i - 6] == 0x4F && buf[i - 3] == 0x4F);
        const uint16_t dest  = static_cast<uint16_t>(buf[i - 8] | (buf[i - 7] << 8));
        const uint16_t ent   = static_cast<uint16_t>(buf[i - 5] | (buf[i - 4] << 8));
        const uint16_t flags = static_cast<uint16_t>(buf[i - 2] | (buf[i - 1] << 8));
        if (lit && flags == 0) field0.push_back(i);
        const uint32_t blobOff = static_cast<uint32_t>(WIN_START + i);
        const std::wstring name = lit ? MapNames::ResolveFullAreaName(dest) : std::wstring();
        char n8[80] = {};
        for (size_t k = 0; k < name.size() && k < 79; ++k) n8[k] = (name[k] < 128) ? static_cast<char>(name[k]) : '?';
        char m[192];
        snprintf(m, sizeof(m), "  mapjump @+0x%X lit=%d dest=%u (\"%s\") entrance=%u flags=%u%s",
                 blobOff, lit ? 1 : 0, dest, n8, ent, flags,
                 (lit && static_cast<int>(dest) == here) ? "  [self]" : "");
        Log::Write("NAV-DIAG", m);
        // ~0x30 bytes of bytecode preceding the first push — the dispatch that selects this call.
        const size_t cs = (i >= 9 + 0x30) ? i - 9 - 0x30 : 0;
        char ctx[3 * 0x30 + 24] = {};
        int p = snprintf(ctx, sizeof(ctx), "     pre[+0x%zX]: ", WIN_START + cs);
        for (size_t k = cs; k < i - 9 && p < static_cast<int>(sizeof(ctx)) - 3; ++k)
            p += snprintf(ctx + p, sizeof(ctx) - static_cast<size_t>(p), "%02x ", buf[k]);
        Log::Write("NAV-DIAG", ctx);
        if (++found >= 64) { Log::Write("NAV-DIAG", "  (cap 64)"); break; }
    }
    char s[64];
    snprintf(s, sizeof(s), "  mapjump literals found: %d", found);
    Log::Write("NAV-DIAG", s);

    (void)field0;
    // ROUTINE TABLE (blob header[0x18] -> array of (flag<<16 | codeOffset)): identifies each routine's entry
    // point, so the init/main routine that binds a +0x54 trigger to a destination routine can be located.
    // The previous parse reported only TWO entries and that was a BUG, not the data: the first u32 at
    // rtOff is an ENTRY COUNT (a live blob read showed 0x18 = 24 there, then 0x254, 0x00ff0000, ...),
    // so index 0 was really the count, and index 2 (0x00ff0000, low word 0) tripped the "coff == 0 ->
    // stop" guard and truncated the table. Entry layout is not yet pinned, so dump RAW u32s instead of
    // pretending to split flag/offset — the format gets settled offline against the hex dump below.
    uint32_t rtOff = 0;
    MemRead::SafeReadU32(mapData, 0x18, &rtOff);
    if (rtOff > 0x10 && rtOff < 0x18000) {
        uint32_t rtCount = 0;
        MemRead::SafeReadU32(mapData, rtOff, &rtCount);
        char rh[112];
        snprintf(rh, sizeof(rh), "==== routine table @+0x%X  count(word0)=%u  raw u32s ====", rtOff, rtCount);
        Log::Write("NAV-DIAG", rh);
        const int n = (rtCount > 0 && rtCount <= 256) ? static_cast<int>(rtCount) + 8 : 72;
        char rt[256]; int rp = 0;
        for (int i = 0; i < n; ++i) {
            uint32_t e = 0;
            if (!MemRead::SafeReadU32(mapData, rtOff + i * 4, &e)) break;
            rp += snprintf(rt + rp, sizeof(rt) - static_cast<size_t>(rp), "[%d]=%x ", i, e);
            if (rp > 200) { Log::Write("NAV-DIAG", rt); rp = 0; rt[0] = 0; }
        }
        if (rp) Log::Write("NAV-DIAG", rt);
    }
    // WHOLE blob as hex so the ENTIRE field script disassembles offline. Deliberately NOT capped at the
    // routine table / +0x3C00 any more: that old window cut off BOTH ends of what we need — the init
    // routines that wire trigger->handler sit below +0x400, and the per-exit "gateway" records (which
    // carry destMapId + entrance + a pointer into that door's handler) sit around +0x5E00..+0x8100.
    // 0x9000 covers init, all handlers, the routine table, and the gateway records in one pass.
    {
        size_t de = buf.size();
        if (de > 0x9000) de = 0x9000;
        char dh[80];
        snprintf(dh, sizeof(dh), "==== field-script code [+0x%zX .. +0x%zX] ====", WIN_START, WIN_START + de);
        Log::Write("NAV-DIAG", dh);
        for (size_t o = 0; o < de; o += 0x20) {
            char line[3 * 0x20 + 24] = {};
            int p = snprintf(line, sizeof(line), "  +0x%zX: ", WIN_START + o);
            for (size_t k = o; k < o + 0x20 && k < de && p < static_cast<int>(sizeof(line)) - 3; ++k)
                p += snprintf(line + p, sizeof(line) - static_cast<size_t>(p), "%02x ", buf[k]);
            Log::Write("NAV-DIAG", line);
        }
    }
}

void EnumerateMapJumps(const FVec3* playerPos, float maxDist, std::vector<ExitRec>& out, bool logRaw) {
    out.clear();
    using MemRead::PtrAt; using MemRead::SafeReadU16;
    using MemRead::SafeReadU32; using MemRead::SafeReadF32;

    // mapData = *(u64*)(containerBase+0); jump table offset at mapData+0x54, base = mapData + off +
    // reloc (~0). Precondition: *(u16)(mapData)>2.
    void* containerBase = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!containerBase) return;
    uint32_t reloc = 0;
    SafeReadU32(Hooks::ResolveRva(PhyreTypes::MASTERDATA_RELOC_BASE), 0, &reloc);   // _DAT_01f83530 (~0)

    void* mapData = PtrAt(containerBase, NavRva::TBL_GUARD_OFF);   // *(u64*)(containerBase+0)
    uint16_t tag = 0;
    uint32_t tableOff = 0;
    if (mapData) { SafeReadU16(mapData, 0, &tag); SafeReadU32(mapData, NavRva::TBL_EXIT_OFF, &tableOff); }
    const uint32_t exitOff = tableOff + reloc;
    char* exitBase = (mapData && tag > 2 && exitOff != 0)
                         ? static_cast<char*>(mapData) + exitOff : nullptr;
    uint32_t count = 0;
    if (exitBase) SafeReadU32(exitBase, 0, &count);
    const bool countOk = (count >= 1 && count <= NavRva::EXIT_COUNT_MAX);

    if (logRaw) {
        char m[192];
        snprintf(m, sizeof(m),
                 "map-jumps(+0x54) slot0: mapData=%p tag=%u tableOff=0x%X base=%p count=%u ok=%d",
                 mapData, tag, tableOff, static_cast<void*>(exitBase), count, countOk ? 1 : 0);
        Log::Write("NAV-DIAG", m);
    }
    if (!countOk) return;

    for (uint32_t i = 0; i < count; ++i) {
        // float x/y/z/angle at uint-word [i*8 + 1..4] -> byte i*0x20 + {4,8,0xC,0x10}.
        const uint32_t rec = i * 8u;
        float x = 0, y = 0, z = 0, ang = 0;
        bool ok = SafeReadF32(exitBase, (rec + 1) * 4, &x)
               && SafeReadF32(exitBase, (rec + 2) * 4, &y)
               && SafeReadF32(exitBase, (rec + 3) * 4, &z)
               && SafeReadF32(exitBase, (rec + 4) * 4, &ang);

        // Position-only. Destination NAMES come from the in-game map's own resolver (map-screen manager
        // DAT_02b457e0 / FUN_003c0340+FUN_003bf430+FUN_003c2320), pending probe confirmation — NOT from
        // this +0x54 table (its trailer is all-zero) nor the +0x8c table (indexed by a field-sign byte,
        // not the jump index — the 0.5 guess, reverted).
        const bool finite = std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(ang);
        const bool nonZero = !(x == 0.0f && y == 0.0f && z == 0.0f);
        float dist = -1.0f;
        bool nearOk = true;
        if (playerPos && ok && finite) {
            const float dx = x - playerPos->x, dz = z - playerPos->z;
            dist = std::sqrt(dx * dx + dz * dz);
            nearOk = dist <= maxDist;
        }
        // Root-cause de-double: the +0x54 table literally repeats records (runtime: count=3 with records
        // 0 and 1 byte-identical). Drop a record whose pos+angle exactly equals one already emitted — the
        // same physical exit, not a proximity dedup (equality is exact because the bytes are identical).
        bool dupe = false;
        for (const auto& p : out)
            if (p.pos.x == x && p.pos.y == y && p.pos.z == z && p.angle == ang) { dupe = true; break; }
        const bool pass = ok && finite && nonZero && nearOk && !dupe;
        if (logRaw) {
            char m[160];
            snprintf(m, sizeof(m), "  jump[%u] pos=(%.1f,%.1f,%.1f) ang=%.2f d=%.1f dupe=%d pass=%d",
                     i, x, y, z, ang, dist, dupe ? 1 : 0, pass ? 1 : 0);
            Log::Write("NAV-DIAG", m);
        }
        if (!pass) continue;
        ExitRec e;
        e.pos = FVec3{ x, y, z };
        e.angle = ang;
        e.index = static_cast<int>(i);
        out.push_back(e);
    }
}

// ---- The in-game map's OWN exit -> destination resolver (the DQ7R equivalent) ------------------------
// We CALL the game's getters and let the GAME do the lookup on its own live data — it resolves this on
// area entry and keeps it live to draw the field minimap, so there is no "is it loaded?" branch: each
// call is POD-only + SEH-guarded, and a fault mid-load simply yields nothing this pass.
namespace {
typedef int   (__fastcall* Pfn_SubMapIdx)();
typedef int   (__fastcall* Pfn_ConnCount)(unsigned int subMap);
typedef void* (__fastcall* Pfn_ConnRec)(unsigned int subMap, unsigned int i, int* typeOut);

static int CallSubMapIndex() {   // FUN_003bfdf0() — current sub-map index (does map-id -> index itself)
    Pfn_SubMapIdx fn = reinterpret_cast<Pfn_SubMapIdx>(Hooks::ResolveRva(NavRva::MAP_SUBMAP_INDEX));
    if (!fn) return -1;
    __try { return fn(); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static int CallConnCount(unsigned int subMap) {          // FUN_003c0340(subMap) -> exit count
    Pfn_ConnCount fn = reinterpret_cast<Pfn_ConnCount>(Hooks::ResolveRva(NavRva::MAP_CONN_COUNT));
    if (!fn) return 0;
    __try { return fn(subMap); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void* CallConnRec(unsigned int subMap, unsigned int i, int* typeOut) {  // FUN_003bf430 -> record*
    Pfn_ConnRec fn = reinterpret_cast<Pfn_ConnRec>(Hooks::ResolveRva(NavRva::MAP_CONN_RECORD));
    if (!fn) return nullptr;
    __try { return fn(subMap, i, typeOut); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
} // namespace

void EnumerateMapConnections(std::vector<ExitRec>& out, bool logRaw) {
    out.clear();
    const int subMap = CallSubMapIndex();
    if (subMap < 0 || static_cast<uint32_t>(subMap) == NavRva::MAP_SUBMAP_NONE) {
        if (logRaw) Log::Write("NAV-DIAG", "map-conn: no sub-map index (not on a mapped field area)");
        return;
    }
    const int n = CallConnCount(static_cast<unsigned int>(subMap));
    if (logRaw) {
        char m[80];
        snprintf(m, sizeof(m), "map-conn: subMap=%d count=%d", subMap, n);
        Log::Write("NAV-DIAG", m);
    }
    if (n <= 0 || n > 64) return;

    const int here = MapNames::CurrentMapId();   // the record pointing at the area we're standing in isn't an exit
    for (int i = 0; i < n; ++i) {
        int type = 3;                  // 3 = invalid (the getter's own sentinel)
        void* rec = CallConnRec(static_cast<unsigned int>(subMap), static_cast<unsigned int>(i), &type);
        if (!rec || type == 3) continue;

        uint16_t dest = 0; int16_t mx = 0, my = 0;
        MemRead::SafeReadU16(rec, NavRva::CONNREC_DEST_OFF, &dest);
        MemRead::SafeReadS16(rec, NavRva::CONNREC_MAPX_OFF, &mx);
        MemRead::SafeReadS16(rec, NavRva::CONNREC_MAPY_OFF, &my);
        // dest = destination planmapname MAP ID (PROVEN live). "<region>: <sub-area>".
        std::wstring name = MapNames::ResolveFullAreaName(static_cast<int>(dest));

        if (logRaw) {
            // Log BOTH halves so the sub-area/region split stays verifiable from the log alone (getting
            // them backwards is what made every exit announce as "Nalbina Fortress").
            const std::wstring sub = MapNames::ResolveAreaName(static_cast<int>(dest));
            const std::wstring reg = MapNames::ResolveRegionName(static_cast<int>(dest));
            char s8[48] = {}, r8[48] = {};
            for (size_t k = 0; k < sub.size() && k < 47; ++k) s8[k] = (sub[k] < 128) ? static_cast<char>(sub[k]) : '?';
            for (size_t k = 0; k < reg.size() && k < 47; ++k) r8[k] = (reg[k] < 128) ? static_cast<char>(reg[k]) : '?';
            char m[208];
            snprintf(m, sizeof(m), "  conn[%d] type=%d atlasPos=(%d,%d) dest=%u sub=\"%s\" region=\"%s\"",
                     i, type, mx, my, dest, s8, r8);
            Log::Write("NAV-DIAG", m);
        }
        if (name.empty()) continue;                                   // unresolvable -> stay silent
        if (here > 0 && static_cast<int>(dest) == here) continue;     // that's the area we're in

        ExitRec e;
        e.pos = FVec3{ static_cast<float>(mx), 0.0f, static_cast<float>(my) };  // MAP-SPACE, not world
        e.index = i;
        e.areaId = dest;
        e.destName = name;
        e.usable = (type != NavRva::CONNREC_TYPE_DOOR);
        out.push_back(e);
    }
}

// (EnumerateExitDestinations / the +0x8c-by-jump-index dest resolver were REMOVED: that table is indexed
//  by a field-sign record's +0x1d byte, NOT the jump index — no code pairs +0x8c[i] with +0x54[i] (0.5,
//  wrong). The +0x70 field-sign enumerator below is that key, taken from the game's own getters.)

// ---- Map EXITS = the field-sign array at mapData+0x70, via the game's own getters --------------------
// The ONLY structure carrying a world position AND a destination on ONE record, so it is the only source
// that can speak "Exit, <region>: <sub-area>, <steps> <bearing>" without joining unrelated tables.
//
// ABI — the bug that hid this table for four sessions: FUN_00264ac0 / FUN_00264ae0 take a GROUP index in
// ECX. Ghidra prints them "(void)" because FUN_00264ac0 never WRITES ecx, it forwards its own incoming
// ecx to FUN_00264ae0. Declaring the count getter as int(*)() passed junk in rcx, FUN_00264ae0's
// `param_1 < groupCount` bounds check failed, and the count came back 0 on EVERY map — which is what
// "+0x70 is empty on interiors" actually measured. These typedefs take the group explicitly.
namespace {
typedef int   (__fastcall* Pfn_ExitCount)(unsigned int group);          // FUN_00264ac0(group)
typedef void* (__fastcall* Pfn_ExitTable)(unsigned int group);          // FUN_00264ae0(group)
typedef void* (__fastcall* Pfn_ExitObj)(unsigned int group, int i);     // FUN_002649b0(group, i)
typedef int   (__fastcall* Pfn_ExitDestInfo)(void* rec, void* buf);     // FUN_002648f0(rec, buf)

static int CallExitCount(unsigned int group) {
    Pfn_ExitCount fn = reinterpret_cast<Pfn_ExitCount>(Hooks::ResolveRva(NavRva::MAPEXIT_COUNT));
    if (!fn) return 0;
    __try { return fn(group); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void* CallExitTable(unsigned int group) {        // group sub-table base ([0]=count), null if none
    Pfn_ExitTable fn = reinterpret_cast<Pfn_ExitTable>(Hooks::ResolveRva(NavRva::MAPEXIT_TABLE_BY_GROUP));
    if (!fn) return nullptr;
    __try { return fn(group); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static void* CallExitObj(unsigned int group, int i) {   // null = not DRAWN right now (render gate) — diag only
    Pfn_ExitObj fn = reinterpret_cast<Pfn_ExitObj>(Hooks::ResolveRva(NavRva::MAPEXIT_OBJ_BY_INDEX));
    if (!fn) return nullptr;
    __try { return fn(group, i); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
static bool CallExitDestInfo(void* rec, void* buf) {    // buf: b0 usable, u16 areaId @+4
    Pfn_ExitDestInfo fn = reinterpret_cast<Pfn_ExitDestInfo>(Hooks::ResolveRva(NavRva::MAPEXIT_DESTINFO));
    if (!fn) return false;
    __try { return fn(rec, buf) != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

// The RAW walk of the +0x70 field-sign table: every record in every group, unfiltered and unlogged.
// This is the game's own list of "places this map connects to", and it is the ONLY table that covers
// both step-on district doors and press-Enter interior doorways in one place. Session 55 measured 13
// records in group 0 and 12 in group 3 on East End — which strikes the old "+0x70 is empty on every
// map" verdict outright (see the header note; the emptiness was a calling-convention bug).
//
// Nothing is dropped here, deliberately: the destination resolution below fails on many maps
// (`areaId` comes back 0xffff), and a walker that hid those records also hid the fact that we cannot
// resolve them. Callers filter; the diagnostic reports.
void EnumerateFieldSignRaw(std::vector<SignRec>& out) {
    out.clear();
    for (unsigned int g = 0; g < NavRva::MAPEXIT_GROUP_MAX; ++g) {
        const int n = CallExitCount(g);
        void* tbl = CallExitTable(g);
        if (!tbl || n <= 0 || n > static_cast<int>(NavRva::EXIT_COUNT_MAX)) continue;

        for (int i = 0; i < n; ++i) {
            // Walk the record ourselves (FUN_002649b0's own arithmetic) so the RENDER gate can't hide an
            // exit that exists: for group<2 that getter returns null unless the "-> area" arrow is being
            // drawn this instant. We keep its answer only as a diagnostic ("shown").
            void* rec = static_cast<char*>(tbl) + NavRva::MAPEXIT_TBL_HDR +
                        static_cast<size_t>(i) * NavRva::EXIT_REC_STRIDE;

            float x = 0.0f, y = 0.0f, z = 0.0f;
            if (!MemRead::SafeReadF32(rec, NavRva::EXITREC_X_OFF, &x) ||
                !MemRead::SafeReadF32(rec, NavRva::EXITREC_Y_OFF, &y) ||
                !MemRead::SafeReadF32(rec, NavRva::EXITREC_Z_OFF, &z)) continue;

            // The game fills the destination: rec+0x1d -> FUN_00264870 -> +0x8c record = map id.
            uint8_t buf[16] = {};
            const bool gotDest = CallExitDestInfo(rec, buf);
            uint16_t areaId = NavRva::AREAID_NONE;
            memcpy(&areaId, buf + NavRva::EXITBUF_AREAID_OFF, sizeof(areaId));

            SignRec s;
            s.pos     = FVec3{ x, y, z };
            s.group   = static_cast<int>(g);
            s.index   = i;
            s.areaId  = gotDest ? areaId : NavRva::AREAID_NONE;
            s.usable  = (buf[NavRva::EXITBUF_USABLE_OFF] != 0);
            s.shown   = (CallExitObj(g, i) != nullptr);
            MemRead::SafeReadU8(rec, NavRva::EXITREC_DESTGRP_OFF, &s.destIdx);
            out.push_back(s);
        }
    }
}

void EnumerateFieldSignExits(std::vector<ExitRec>& out, bool logRaw) {
    out.clear();
    const int here = MapNames::CurrentMapId();

    std::vector<SignRec> raw;
    EnumerateFieldSignRaw(raw);

    for (const auto& s : raw) {
        std::wstring name = MapNames::ResolveFullAreaName(static_cast<int>(s.areaId));

        if (logRaw) {
            // Log EVERY record and why it drops — a silent `continue` is what hid all 3 exits once.
            char n8[64] = {};
            for (size_t k = 0; k < name.size() && k < 63; ++k)
                n8[k] = (name[k] < 128) ? static_cast<char>(name[k]) : '?';
            char m[224];
            snprintf(m, sizeof(m),
                     "  sign[g%d.%d] world=(%.1f,%.1f,%.1f) destIdx=%u areaId=%u usable=%d shown=%d name=\"%s\"",
                     s.group, s.index, s.pos.x, s.pos.y, s.pos.z, s.destIdx, s.areaId,
                     s.usable ? 1 : 0, s.shown ? 1 : 0, n8);
            Log::Write("NAV-DIAG", m);
        }

        if (s.areaId == NavRva::AREAID_NONE || name.empty()) continue;  // no honest name -> stay silent
        if (here > 0 && static_cast<int>(s.areaId) == here) continue;   // leads back into this same area

        ExitRec e;
        e.pos      = s.pos;                       // WORLD — bearing/steps are honest for these
        e.index    = s.group * 100 + s.index;
        e.areaId   = s.areaId;
        e.destName = name;
        e.usable   = s.usable;
        out.push_back(e);
    }
}

// RETIRED (this session): the mapData+0x70 field-sign array (EnumerateExits + LogExitGroupTableRaw +
// the FUN_00264ac0/002649b0/002648f0 getters) is confirmed EMPTY on every map by two testers, so it
// never produced a destination name. Exit destination names now come from the game's own resolver
// applied to the +0x54 record's trailer inside EnumerateMapJumps above (see that function). The dead
// +0x70 walker and its getters are removed; NavRva keeps the struck offsets on record.

// (EnumerateMarkers removed in Session 44 — the naviicon array DAT_02b45a80 holds only character/unit dots
//  (party/ally/enemy) that duplicate the combatant scan, with no objective/crystal/label source. See
//  nav_rva.h for the retired NAVIICON_*/MARK_* note.)
} // namespace MapExits
