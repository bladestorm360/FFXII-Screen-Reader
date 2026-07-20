#include "navigation/map_script.h"
#include "navigation/map_query.h"
#include "navigation/nav_rva.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/logger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

// ---- Field-script blob layout (engine-wide; identical on every map) -------------------------
// The map-control blob starts with a header of u32 offsets, all relative to the blob base:
//   hdr+0x18 -> ROUTINE TABLE:  [u32 count][count records of 0x30 bytes]
//               record: {+0x00 nameOff (into the name pool), +0x08 codeOff (routine entry)}
//   hdr+0x4c -> NAME POOL: NUL-terminated names; a routine's name is  pool + nameOff.
//   hdr+0x54 -> the map-jump (door) position table, read separately by MapQuery.
//
// A routine's code SPAN is [codeOff, next-highest codeOff) — the record's other fields are label
// and variable sub-tables, not a byte length (two controller routines on one map had byte-identical
// sub-tables, because they are the same compiled template differing only in the destination literal).
constexpr uint32_t HDR_ROUTINE_TABLE = 0x18;
constexpr uint32_t HDR_NAME_POOL     = 0x4C;
// hdr+0x54 -> the map-jump (door) table: [u32 count][records of 0x20], each record four floats
// x/y/z/angle starting at record+0. These are DEPARTURE triggers; arrival positions live in a separate
// table at hdr+0x84 (`getmapdestposbyindex`), which is why entrance indices never matched door positions.
constexpr uint32_t HDR_JUMP_TABLE    = 0x54;
constexpr uint32_t JUMP_STRIDE       = 0x20;
constexpr uint32_t JUMP_COUNT_MAX    = 64;
constexpr uint32_t ROUTINE_STRIDE    = 0x30;
constexpr uint32_t REC_NAME_OFF      = 0x00;
constexpr uint32_t REC_CODE_OFF      = 0x08;
constexpr uint32_t MAX_ROUTINES      = 512;    // sanity bound; real maps run ~24-40

// The map toolchain's auto-generated name for a map-jump door controller.
constexpr char MJ_PREFIX[]    = "__MJ_CTRL";
constexpr size_t MJ_PREFIX_LEN = sizeof(MJ_PREFIX) - 1;

// `mapjump(dest, entrance, flags)` compiles to three push-immediates then the native call:
//   4f <destU16> 4f <entU16> 4f <flagsU16> 5d 8d 00
// 0x4F = push u16, 0x5D = CALLACTPOPA, native 0x8D = mapjump. flags==0 is a field door;
// flags==0x0A is the world-map teleport menu (a long run of them sits in every map) and is excluded.
constexpr uint8_t OP_PUSH_U16   = 0x4F;
constexpr uint8_t OP_CALLACTPOPA = 0x5D;
constexpr uint8_t NATIVE_MAPJUMP = 0x8D;
constexpr uint16_t MAPJUMP_FLAGS_FIELD_DOOR = 0;

// How much of the blob to snapshot. Comfortably covers header, code, routine table and name pool on
// the maps measured (largest structure seen ended below +0x6000).
constexpr size_t BLOB_MAX  = 0x18000;
constexpr size_t BLOB_PAGE = 0x1000;

inline uint32_t U32(const std::vector<uint8_t>& b, size_t off) {
    if (off + 4 > b.size()) return 0;
    return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}
inline uint16_t U16(const std::vector<uint8_t>& b, size_t off) {
    if (off + 2 > b.size()) return 0;
    return static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
}
inline float F32(const std::vector<uint8_t>& b, size_t off) {
    const uint32_t v = U32(b, off);
    float f = 0.0f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

// World position of `+0x54` door `slot`, straight out of the snapshot. False when the slot is absent.
bool JumpSlotPos(const std::vector<uint8_t>& b, int slot, FVec3& out) {
    if (slot < 0) return false;
    const uint32_t tbl = U32(b, HDR_JUMP_TABLE);
    if (tbl == 0 || tbl + 4 >= b.size()) return false;
    const uint32_t count = U32(b, tbl);
    if (count == 0 || count > JUMP_COUNT_MAX || static_cast<uint32_t>(slot) >= count) return false;
    const size_t rec = static_cast<size_t>(tbl) + 4 + static_cast<size_t>(slot) * JUMP_STRIDE;
    if (rec + 12 > b.size()) return false;
    out = FVec3{ F32(b, rec + 0), F32(b, rec + 4), F32(b, rec + 8) };
    return true;
}

// Snapshot the live blob into a local buffer, page by page, stopping at the first unmapped page so a
// short/torn map still parses whatever was readable.
bool SnapshotBlob(std::vector<uint8_t>& buf) {
    void* containerBase = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!containerBase) return false;
    void* mapData = MemRead::PtrAt(containerBase, NavRva::TBL_GUARD_OFF);
    if (!mapData) return false;

    buf.clear();
    buf.reserve(BLOB_MAX);
    uint8_t page[BLOB_PAGE];
    for (size_t off = 0; off < BLOB_MAX; off += BLOB_PAGE) {
        if (!MemRead::SafeReadBytes(static_cast<const char*>(mapData) + off, page, BLOB_PAGE)) break;
        buf.insert(buf.end(), page, page + BLOB_PAGE);
    }
    return buf.size() >= BLOB_PAGE;
}

// Read a NUL-terminated ASCII name out of the pool. Bounded; returns empty on overrun. Names may be
// Shift-JIS (most routines are named in Japanese) — we only ever compare the ASCII `__MJ_CTRL` form,
// so raw bytes are fine and no transcoding is needed.
std::string PoolName(const std::vector<uint8_t>& b, uint32_t poolOff, uint32_t nameOff) {
    const size_t start = static_cast<size_t>(poolOff) + nameOff;
    if (start >= b.size()) return std::string();
    std::string s;
    for (size_t i = start; i < b.size() && s.size() < 64; ++i) {
        if (b[i] == 0) return s;
        s.push_back(static_cast<char>(b[i]));
    }
    return std::string();   // unterminated -> treat as garbage
}

// `__MJ_CTRL012` -> 12. Returns -1 when the name is not a controller.
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

} // namespace

namespace MapScript {

bool ReadExitDests(std::vector<ExitDest>& out, bool logDetail) {
    out.clear();

    std::vector<uint8_t> b;
    if (!SnapshotBlob(b)) return false;

    const uint32_t rtOff   = U32(b, HDR_ROUTINE_TABLE);
    const uint32_t poolOff = U32(b, HDR_NAME_POOL);
    if (rtOff == 0 || rtOff + 4 >= b.size() || poolOff == 0 || poolOff >= b.size()) return false;

    // Word 0 of the routine table is an ENTRY COUNT, not a record. (Reading it as a record is what
    // made an earlier pass report "2 routines" on a 24-routine map.)
    const uint32_t count = U32(b, rtOff);
    if (count == 0 || count > MAX_ROUTINES) return false;

    // Collect every routine's code offset first: a routine's span ends at the next-highest entry,
    // and the table is not required to be sorted.
    std::vector<uint32_t> codeOffs;
    codeOffs.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t rec = static_cast<size_t>(rtOff) + 4 + static_cast<size_t>(i) * ROUTINE_STRIDE;
        if (rec + ROUTINE_STRIDE > b.size()) break;
        codeOffs.push_back(U32(b, rec + REC_CODE_OFF));
    }
    if (codeOffs.empty()) return false;
    std::vector<uint32_t> sorted = codeOffs;
    std::sort(sorted.begin(), sorted.end());

    if (logDetail) {
        char m[160];
        snprintf(m, sizeof(m),
                 "==== field-script exits: routineTable=+0x%X count=%u namePool=+0x%X blob=0x%zX ====",
                 rtOff, count, poolOff, b.size());
        Log::Write("NAV-DIAG", m);
    }

    for (uint32_t i = 0; i < codeOffs.size(); ++i) {
        const size_t rec = static_cast<size_t>(rtOff) + 4 + static_cast<size_t>(i) * ROUTINE_STRIDE;
        const std::string name = PoolName(b, poolOff, U32(b, rec + REC_NAME_OFF));
        const int idx = ParseCtrlIndex(name);
        if (idx < 0) continue;

        const uint32_t code = codeOffs[i];
        auto nx = std::upper_bound(sorted.begin(), sorted.end(), code);
        const size_t spanEnd = (nx == sorted.end()) ? b.size() : std::min<size_t>(*nx, b.size());
        if (code >= spanEnd) continue;

        // First field-door mapjump inside the routine is its destination. (Templates emit the same
        // call twice — e.g. a faded and an unfaded path — with identical operands, so first wins.)
        for (size_t o = code; o + 12 <= spanEnd; ++o) {
            if (b[o] != OP_PUSH_U16 || b[o + 3] != OP_PUSH_U16 || b[o + 6] != OP_PUSH_U16) continue;
            if (b[o + 9] != OP_CALLACTPOPA || b[o + 10] != NATIVE_MAPJUMP || b[o + 11] != 0) continue;
            if (U16(b, o + 7) != MAPJUMP_FLAGS_FIELD_DOOR) continue;

            ExitDest d;
            d.ctrlIndex = idx;
            d.slot      = idx + 1;          // DOOR RULE: __MJ_CTRL<N> owns +0x54 slot N+1 (slot 0 = arrival)
            d.posOk     = JumpSlotPos(b, d.slot, d.pos);
            d.destMapId = U16(b, o + 1);
            d.entrance  = U16(b, o + 4);
            d.codeOff   = code;
            d.destName  = MapQuery::ResolveFullAreaName(d.destMapId);
            out.push_back(d);
            break;
        }
    }

    std::sort(out.begin(), out.end(),
              [](const ExitDest& a, const ExitDest& c) { return a.ctrlIndex < c.ctrlIndex; });

    if (logDetail) {
        for (const auto& d : out) {
            char n8[96] = {};
            for (size_t k = 0; k < d.destName.size() && k < 95; ++k)
                n8[k] = (d.destName[k] < 128) ? static_cast<char>(d.destName[k]) : '?';
            char m[256];
            snprintf(m, sizeof(m),
                     "  __MJ_CTRL%03d -> door +0x54[%d] (%.1f,%.1f,%.1f)%s dest=%u (\"%s\") entrance=%u",
                     d.ctrlIndex, d.slot, d.pos.x, d.pos.y, d.pos.z, d.posOk ? "" : " <NO SLOT>",
                     d.destMapId, n8, d.entrance);
            Log::Write("NAV-DIAG", m);
        }
        char m[96];
        snprintf(m, sizeof(m), "  map-jump controllers found: %zu", out.size());
        Log::Write("NAV-DIAG", m);
    }
    return true;
}

} // namespace MapScript
