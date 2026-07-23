#include "navigation/map_script.h"
#include "navigation/map_script_internal.h"
#include "navigation/map_query.h"
#include "navigation/map_names.h"
#include "navigation/nav_rva.h"
#include "navigation/map_rva.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/logger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The blob layout constants and the guarded blob readers live in map_script_internal.h, shared with
// map_script_diag.cpp so the reader and its capture dump can never drift apart.
using namespace MapScript::Internal;

namespace MapScript {
namespace Internal {

void* BlobBase() {
    void* containerBase = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!containerBase) return nullptr;
    return MemRead::PtrAt(containerBase, NavRva::TBL_GUARD_OFF);
}

bool BlobU32(void* blob, uint32_t off, uint32_t* out) {
    if (off >= OFFSET_MAX) return false;
    return MemRead::SafeReadU32(blob, off, out);
}

bool BlobBytes(void* blob, uint32_t off, void* dst, size_t n) {
    if (!blob || off >= OFFSET_MAX || n > OFFSET_MAX) return false;
    return MemRead::SafeReadBytes(static_cast<const char*>(blob) + off, dst, n);
}

std::string PoolName(void* blob, uint32_t poolOff, uint32_t nameOff) {
    if (nameOff >= OFFSET_MAX) return std::string();
    char buf[NAME_MAX + 1] = {};
    // Graduated: the last name in the pool can sit close enough to the end of its mapped page that a
    // full-width read faults, so fall back to shorter spans rather than losing the name entirely.
    // 16 bytes always covers "__MJ_CTRL" + index + NUL, which is all the controller match needs.
    for (size_t want : { NAME_MAX, size_t{32}, size_t{16} }) {
        if (BlobBytes(blob, poolOff + nameOff, buf, want)) { buf[want] = '\0'; break; }
    }
    return std::string(buf);   // stops at the first NUL; unterminated names truncate
}

int ParseCtrlIndex(const std::string& name) {
    if (name.size() <= MJ_PREFIX_LEN) return -1;
    if (std::strncmp(name.c_str(), MJ_PREFIX, MJ_PREFIX_LEN) != 0) return -1;
    int v = 0;
    for (size_t i = MJ_PREFIX_LEN; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return -1;
        v = v * 10 + (name[i] - '0');
        if (v > 9999) return -1;
    }
    return v;
}

} // namespace Internal
} // namespace MapScript

namespace {

// World position of `+0x54` door `slot`, read live. False when the slot is absent.
bool JumpSlotPos(void* blob, int slot, FVec3& out) {
    if (slot < 0) return false;
    uint32_t tbl = 0;
    if (!BlobU32(blob, HDR_JUMP_TABLE, &tbl) || tbl == 0 || tbl >= OFFSET_MAX) return false;
    uint32_t count = 0;
    if (!BlobU32(blob, tbl, &count)) return false;
    if (count == 0 || count > JUMP_COUNT_MAX || static_cast<uint32_t>(slot) >= count) return false;
    const uint32_t rec = tbl + 4 + static_cast<uint32_t>(slot) * JUMP_STRIDE;
    float xyz[3] = {};
    if (!BlobBytes(blob, rec, xyz, sizeof(xyz))) return false;
    out = FVec3{ xyz[0], xyz[1], xyz[2] };
    return true;
}

// One record of a position table: the four floats the engine reads (x/y/z/angle).
struct PosRec { float x, y, z, ang; };

// Read a whole position table (`hdrOff` = 0x54 or 0x84). False when absent/insane. Records start after
// the u32 count.
bool ReadPosTable(void* blob, uint32_t hdrOff, std::vector<PosRec>& out) {
    out.clear();
    uint32_t tblOff = 0;
    if (!BlobU32(blob, hdrOff, &tblOff) || tblOff == 0 || tblOff >= OFFSET_MAX) return false;
    uint32_t count = 0;
    if (!BlobU32(blob, tblOff, &count) || count == 0 || count > JUMP_COUNT_MAX) return false;
    out.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        float rec[4] = {};
        if (!BlobBytes(blob, tblOff + 4 + i * JUMP_STRIDE, rec, sizeof(rec))) { out.clear(); return false; }
        out[i] = PosRec{ rec[0], rec[1], rec[2], rec[3] };
    }
    return true;
}

inline bool SamePos(const PosRec& a, const PosRec& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.ang == b.ang;   // byte-identical copies
}

// THE DOOR BINDING (Session 58, decoded from the `'` capture on East End + Muthru Bazaar).
//
// `+0x54` is the party ARRIVAL table. `+0x84` is the SAME arrivals, in the same order, with ONE extra
// "edge" record inserted per controller -- the transition TRIGGER, whose coordinate lies off the
// walkable mesh at the map boundary (East End: z=-78, x=-39, x=160, z=255 ...). The record immediately
// AFTER the i-th edge is controller i's arrival, i.e. the doorway it actually owns.
//
// This replaces the S46 "`__MJ_CTRL<N>` owns `+0x54` slot N+1" rule, which paired the arrival table
// with the exit routines -- two structurally independent things -- and only ever held on Nalbina's
// 2-3-loader maps. On East End it put Muthru (west) mid-map and Southern Plaza (south) at the far west,
// which is exactly the reported "Southern Plaza loads the Bazaar".
//
// Verified: East End 3/3 real exits land on the doorway their neighbour's atlas direction implies
// (North End north, Muthru west, Southern Plaza south) and Muthru Bazaar matches too; and it reproduces
// the tester's walked ground truth (arriving from the Bazaar you spawn on `+0x54[2]`, and the Bazaar
// exit resolves to that same point).
//
// The edge coordinate itself is NOT used as a route target: it is a trigger VOLUME's reference point,
// off-mesh and at a variable distance (Southern Plaza's is ~120 u past the arrival). Adjacency- and
// nearest-distance pairing of controller->edge both fail on at least one real exit. But its DIRECTION
// from the arrival is exact -- north to (125,-78), west to (-39,..), south to (..,255) on East End's
// three real exits -- so it is returned as well, and exit_scan.cpp's `EdgeTarget` marches that heading
// across the walkmap to put the route target on the seam itself.
//
// Fills `outPos` / `outEdge` indexed by controller ordinal (0..n-1, ascending ctrlIndex). Returns false
// when the tables do not have the expected shape -- callers then fall back to the old `+0x54[N+1]`, so a
// map that breaks the pattern degrades to previous behaviour instead of emitting garbage.
bool ResolveControllerArrivals(void* blob, size_t ctrlCount, std::vector<FVec3>& outPos,
                               std::vector<FVec3>& outEdge, std::vector<int>& outSlot, bool logDetail) {
    outPos.clear();
    outEdge.clear();
    outSlot.clear();
    if (ctrlCount == 0) return false;
    std::vector<PosRec> t54, t84;
    if (!ReadPosTable(blob, HDR_JUMP_TABLE, t54) || !ReadPosTable(blob, HDR_ARRIVE_TABLE, t84))
        return false;

    // An edge is a +0x84 record present in no +0x54 record.
    std::vector<size_t> edgeIdx;
    for (size_t j = 0; j < t84.size(); ++j) {
        bool isArrival = false;
        for (const auto& a : t54) if (SamePos(t84[j], a)) { isArrival = true; break; }
        if (!isArrival) edgeIdx.push_back(j);
    }
    if (edgeIdx.size() != ctrlCount) {
        if (logDetail) {
            char m[176];
            snprintf(m, sizeof(m),
                     "door binding: %zu edge records vs %zu controllers -- shape unexpected, using +0x54[N+1]",
                     edgeIdx.size(), ctrlCount);
            Log::Write("NAV-DIAG", m);
        }
        return false;
    }
    outPos.resize(ctrlCount);
    outEdge.resize(ctrlCount);
    outSlot.assign(ctrlCount, -1);
    for (size_t i = 0; i < ctrlCount; ++i) {
        const size_t e0 = edgeIdx[i];
        const size_t a  = e0 + 1;
        // The record after an edge must exist and must itself be an arrival, never another edge.
        if (a >= t84.size()) return false;
        for (size_t e : edgeIdx) if (e == a) return false;
        outPos[i]  = FVec3{ t84[a].x,  t84[a].y,  t84[a].z  };
        outEdge[i] = FVec3{ t84[e0].x, t84[e0].y, t84[e0].z };
        // Recover the `+0x54` index of this arrival. The two tables hold byte-identical copies, so an
        // exact match is exact -- and this index is the doorway's name in the game's own wiring, which is
        // what a neighbour map's `mapjump(us, slot)` refers to.
        for (size_t s = 0; s < t54.size(); ++s)
            if (SamePos(t84[a], t54[s])) { outSlot[i] = static_cast<int>(s); break; }
    }
    return true;
}

// One-line NAV-DIAG bail reason. Every failure path used to `return false` in silence, which is why
// "the exit reader produced nothing on this map" was indistinguishable from "this map has no exits"
// for two sessions -- the log had no trace of this file at all. Gated on logDetail (once per map).
bool Bail(bool logDetail, const char* why, uint32_t a, uint32_t b, uint32_t c) {
    if (logDetail) {
        char m[192];
        snprintf(m, sizeof(m),
                 "field-script exits: BAILED (%s) mapId=%d routineTable=+0x%X namePool=+0x%X count=%u",
                 why, MapNames::CurrentMapId(), a, b, c);
        Log::Write("NAV-DIAG", m);
    }
    return false;
}

// Printable-ASCII form of a routine name for the log. Most routines are named in Japanese
// (Shift-JIS), so raw bytes would corrupt the log line; the `__MJ_CTRL` names we care about are
// pure ASCII and survive intact.
std::string AsciiSafe(const std::string& s) {
    std::string o;
    for (char c : s) o.push_back((c >= 0x20 && c < 0x7f) ? c : '?');
    return o;
}

} // namespace

namespace MapScript {

bool ReadExitDests(std::vector<ExitDest>& out, bool logDetail) {
    out.clear();

    void* blob = BlobBase();
    if (!blob) return Bail(logDetail, "no map-control blob", 0, 0, 0);

    uint32_t rtOff = 0, poolOff = 0;
    if (!BlobU32(blob, HDR_ROUTINE_TABLE, &rtOff) || rtOff == 0 || rtOff >= OFFSET_MAX)
        return Bail(logDetail, "routine-table offset unreadable/out of range", rtOff, poolOff, 0);
    if (!BlobU32(blob, HDR_NAME_POOL, &poolOff) || poolOff == 0 || poolOff >= OFFSET_MAX)
        return Bail(logDetail, "name-pool offset unreadable/out of range", rtOff, poolOff, 0);

    // Word 0 of the routine table is an ENTRY COUNT, not a record. (Reading it as a record is what
    // made an earlier pass report "2 routines" on a 24-routine map.)
    uint32_t count = 0;
    if (!BlobU32(blob, rtOff, &count)) return Bail(logDetail, "routine count unreadable", rtOff, poolOff, 0);
    if (count == 0 || count > MAX_ROUTINES)
        return Bail(logDetail, "routine count out of range", rtOff, poolOff, count);

    // Pull the whole routine table in ONE guarded copy, then parse the copy. This is the only bulk
    // read left: ~0x30 bytes per routine, so a 40-routine map costs ~2 KB instead of the 96 KB the
    // old snapshot copied on EVERY rescan.
    std::vector<uint8_t> tbl(static_cast<size_t>(count) * ROUTINE_STRIDE);
    if (!BlobBytes(blob, rtOff + 4, tbl.data(), tbl.size()))
        return Bail(logDetail, "routine table unreadable", rtOff, poolOff, count);

    // Collect every routine's code offset first: a routine's span ends at the next-highest entry,
    // and the table is not required to be sorted.
    std::vector<uint32_t> codeOffs;
    codeOffs.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        codeOffs.push_back(U32(tbl, static_cast<size_t>(i) * ROUTINE_STRIDE + REC_CODE_OFF));
    std::vector<uint32_t> sorted = codeOffs;
    std::sort(sorted.begin(), sorted.end());

    if (logDetail) {
        char m[160];
        snprintf(m, sizeof(m),
                 "==== field-script exits: routineTable=+0x%X count=%u namePool=+0x%X (live reads) ====",
                 rtOff, count, poolOff);
        Log::Write("NAV-DIAG", m);
    }

    std::vector<uint8_t> code;   // per-controller code span, reused
    std::vector<std::string> names(count);
    for (uint32_t i = 0; i < count; ++i) {
        names[i] = PoolName(blob, poolOff, U32(tbl, static_cast<size_t>(i) * ROUTINE_STRIDE + REC_NAME_OFF));
        const int idx = ParseCtrlIndex(names[i]);
        if (idx < 0) continue;

        const uint32_t start = codeOffs[i];
        auto nx = std::upper_bound(sorted.begin(), sorted.end(), start);
        const uint32_t end = (nx == sorted.end()) ? (start + static_cast<uint32_t>(CODE_SPAN_MAX)) : *nx;
        if (end <= start) continue;
        size_t span = std::min<size_t>(end - start, CODE_SPAN_MAX);
        code.assign(span, 0);
        // The LAST routine has no successor to bound it, so its span is a guess; shrink until the
        // read lands inside mapped memory rather than dropping the routine.
        while (span >= 0x40 && !BlobBytes(blob, start, code.data(), span)) {
            span /= 2;
            code.assign(span, 0);
        }
        if (span < 0x40) continue;
        const std::vector<uint8_t>& b = code;

        // The routine's MAP-JUMP GROUP: `setmapjumpgroup(K)`, compiled as `4f <K:u16> 5d 1e 01`. It is
        // the first distinguishing call in every controller, and K is what the walkmap tags this
        // transition's floor polygons with -- so it is what turns "this routine jumps to X" into
        // "the surface you walk on to reach X is HERE". Take the first match; a controller calls it once.
        int group = -1;
        for (size_t o = 0; o + 6 <= b.size(); ++o) {
            if (b[o] != OP_PUSH_U16 || b[o + 3] != OP_CALLACTPOPA) continue;
            if (static_cast<uint16_t>(b[o + 4] | (b[o + 5] << 8)) != NATIVE_SETMAPJUMPGROUP) continue;
            group = static_cast<int>(U16(b, o + 1));
            break;
        }

        // First field-door mapjump inside the routine is its destination. (Templates emit the same
        // call twice — e.g. a faded and an unfaded path — with identical operands, so first wins.)
        for (size_t o = 0; o + 12 <= b.size(); ++o) {
            if (b[o] != OP_PUSH_U16 || b[o + 3] != OP_PUSH_U16 || b[o + 6] != OP_PUSH_U16) continue;
            if (b[o + 9] != OP_CALLACTPOPA || b[o + 10] != NATIVE_MAPJUMP || b[o + 11] != 0) continue;
            if (U16(b, o + 7) != MAPJUMP_FLAGS_FIELD_DOOR) continue;

            ExitDest d;
            d.ctrlIndex = idx;
            d.slot      = idx + 1;          // authoring-order id; diagnostics only
            d.group     = group;            // the walkmap tag that locates this transition
            d.destMapId = U16(b, o + 1);
            d.entrance  = U16(b, o + 4);    // arrival slot on the DESTINATION map, not a local index
            d.codeOff   = start;
            d.destName  = MapNames::ResolveFullAreaName(d.destMapId);
            out.push_back(d);
            break;
        }
    }

    std::sort(out.begin(), out.end(),
              [](const ExitDest& a, const ExitDest& c) { return a.ctrlIndex < c.ctrlIndex; });

    // POSITION: bind each controller to the doorway it actually owns, via the `+0x84` edge-pairing.
    // Falls back to the old `+0x54[ctrlIndex+1]` when the tables don't have the expected shape, so a map
    // that breaks the pattern behaves exactly as it did before rather than emitting a wrong position.
    std::vector<FVec3> arrivals, edges;
    std::vector<int>   slots;
    const bool bound = ResolveControllerArrivals(blob, out.size(), arrivals, edges, slots, logDetail);
    for (size_t i = 0; i < out.size(); ++i) {
        if (bound) {
            out[i].pos         = arrivals[i];
            out[i].posOk       = true;
            out[i].edge        = edges[i];
            out[i].edgeOk      = true;
            out[i].arrivalSlot = slots[i];
        } else {
            out[i].posOk = JumpSlotPos(blob, out[i].slot, out[i].pos);
        }
    }
    if (logDetail) {
        char m[128];
        snprintf(m, sizeof(m), "door binding: %s (%zu controllers)",
                 bound ? "+0x84 edge-pairing" : "FALLBACK +0x54[N+1]", out.size());
        Log::Write("NAV-DIAG", m);
    }

    // Dump the routine names on EVERY map, not only when zero controllers were found. That old gate is
    // why we have never once seen the names of a map that HAS controllers -- East End has 93 routines
    // and all we ever learned was that six of them start with `__MJ_CTRL`. The map is known to contain
    // at least one transition no controller owns (a `+0x70` field sign with no `__MJ_CTRL`), so a
    // SECOND naming convention is the leading explanation, and it can only show up here.
    // (The on-disk .mpk cannot answer this offline: `__MJ_CTRL` appears as plaintext in none of the 20
    // extracted map archives, not even the Nalbina ones where it is proven present at runtime, so the
    // name pool is packed on disk.)
    if (logDetail) {
        Log::Write("NAV-DIAG", "  routine names follow (looking for transition routines other than __MJ_CTRL)");
        std::string line;
        for (uint32_t i = 0; i < count; ++i) {
            if (names[i].empty()) continue;
            line += "[" + std::to_string(i) + "]" + AsciiSafe(names[i]) + " ";
            if (line.size() > 160) { Log::Write("NAV-DIAG", ("  " + line).c_str()); line.clear(); }
        }
        if (!line.empty()) Log::Write("NAV-DIAG", ("  " + line).c_str());
    }

    if (logDetail) {
        for (const auto& d : out) {
            char n8[96] = {};
            for (size_t k = 0; k < d.destName.size() && k < 95; ++k)
                n8[k] = (d.destName[k] < 128) ? static_cast<char>(d.destName[k]) : '?';
            char m[288];
            snprintf(m, sizeof(m),
                     "  __MJ_CTRL%03d group=%d dest=%u (\"%s\") entrance=%u | arrival (%.1f,%.1f,%.1f)%s",
                     d.ctrlIndex, d.group, d.destMapId, n8, d.entrance,
                     d.pos.x, d.pos.y, d.pos.z, d.posOk ? "" : " <NO POS>");
            Log::Write("NAV-DIAG", m);
        }
        char m[96];
        snprintf(m, sizeof(m), "  map-jump controllers found: %zu", out.size());
        Log::Write("NAV-DIAG", m);
    }
    return true;
}

} // namespace MapScript
