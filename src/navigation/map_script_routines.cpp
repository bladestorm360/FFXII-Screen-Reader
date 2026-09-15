#include "navigation/map_script_routines.h"
#include "navigation/map_script_internal.h"
#include "core/mem_read.h"

#include <algorithm>

using namespace MapScript::Internal;

namespace {

// Routine record `+0x10`: blob-relative offset of the routine's ENTRY TABLE.
constexpr uint32_t REC_ENTRY_TABLE_OFF = 0x10;

// `setmapidfloor(id, class, state)` compiles to three push-immediates and the native call, exactly
// like `mapjump`: `4f <id> 4f <class> 4f <state> 5d fe 00`. See map_script_routines.h for what it writes.
constexpr uint8_t  NATIVE_SETMAPIDFLOOR_LO = 0xFE;
constexpr uint16_t FLOOR_CLASS_LEADER      = 0;   // class 0 -> material bit 23
constexpr uint16_t FLOOR_STATE_OPEN        = 1;   // state 1 forces the refusal bit OFF
constexpr uint16_t MATERIAL_ID_COUNT       = 32;  // the material bank is entries 0x00-0x1F

std::string AsciiSafe(const std::string& s) {
    std::string o;
    for (char c : s) o.push_back((c >= 0x20 && c < 0x7f) ? c : '?');
    return o;
}

} // namespace

namespace MapScript {

bool ReadRoutineFacts(std::vector<RoutineFacts>& out) {
    out.clear();
    void* blob = BlobBase();
    if (!blob) return false;

    uint32_t rtOff = 0, poolOff = 0, count = 0;
    if (!BlobU32(blob, HDR_ROUTINE_TABLE, &rtOff) || rtOff == 0 || rtOff >= OFFSET_MAX) return false;
    if (!BlobU32(blob, HDR_NAME_POOL, &poolOff) || poolOff == 0 || poolOff >= OFFSET_MAX) return false;
    if (!BlobU32(blob, rtOff, &count) || count == 0 || count > MAX_ROUTINES) return false;

    std::vector<uint8_t> tbl(static_cast<size_t>(count) * ROUTINE_STRIDE);
    if (!BlobBytes(blob, rtOff + 4, tbl.data(), tbl.size())) return false;

    // A routine's code span ends at the next-highest entry offset -- the same rule ReadExitDests uses.
    std::vector<uint32_t> sorted;
    sorted.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        sorted.push_back(U32(tbl, static_cast<size_t>(i) * ROUTINE_STRIDE + REC_CODE_OFF));
    std::sort(sorted.begin(), sorted.end());

    out.resize(count);
    std::vector<uint8_t> code;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t rec = static_cast<size_t>(i) * ROUTINE_STRIDE;
        RoutineFacts& f = out[i];
        f.index = static_cast<int>(i);
        f.name  = AsciiSafe(PoolName(blob, poolOff, U32(tbl, rec + REC_NAME_OFF)));
        const uint32_t entryOff = U32(tbl, rec + REC_ENTRY_TABLE_OFF);
        if (entryOff != 0 && entryOff < OFFSET_MAX)
            f.entryTable = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(blob)) + entryOff;

        const uint32_t start = U32(tbl, rec + REC_CODE_OFF);
        auto nx = std::upper_bound(sorted.begin(), sorted.end(), start);
        const uint32_t end = (nx == sorted.end()) ? (start + static_cast<uint32_t>(CODE_SPAN_MAX)) : *nx;
        if (end <= start) continue;
        size_t span = std::min<size_t>(end - start, CODE_SPAN_MAX);
        code.assign(span, 0);
        while (span >= 0x40 && !BlobBytes(blob, start, code.data(), span)) {   // last routine: guessed span
            span /= 2;
            code.assign(span, 0);
        }
        if (span < 0x40) continue;
        f.codeRead = true;
        for (size_t o = 0; o + 12 <= code.size(); ++o) {
            if (code[o] != OP_PUSH_U16 || code[o + 3] != OP_PUSH_U16 || code[o + 6] != OP_PUSH_U16) continue;
            if (code[o + 9] != OP_CALLACTPOPA || code[o + 10] != NATIVE_SETMAPIDFLOOR_LO || code[o + 11] != 0)
                continue;
            const uint16_t id  = U16(code, o + 1);
            const uint16_t cls = U16(code, o + 4);
            const uint16_t st  = U16(code, o + 7);
            if (cls != FLOOR_CLASS_LEADER || st != FLOOR_STATE_OPEN || id >= MATERIAL_ID_COUNT) continue;
            f.opensFloorMask |= (1u << id);
        }
    }
    return true;
}

uint64_t ScriptFingerprint() {
    void* blob = BlobBase();
    if (!blob) return 0;
    uint32_t rtOff = 0, poolOff = 0, count = 0;
    if (!BlobU32(blob, HDR_ROUTINE_TABLE, &rtOff) || !BlobU32(blob, HDR_NAME_POOL, &poolOff)) return 0;
    if (rtOff == 0 || rtOff >= OFFSET_MAX || !BlobU32(blob, rtOff, &count)) return 0;
    return (static_cast<uint64_t>(rtOff) << 40) ^ (static_cast<uint64_t>(poolOff) << 12) ^ count;
}

int RoutineIndexOfObject(const std::vector<RoutineFacts>& facts, void* sceneObj) {
    if (!sceneObj) return -1;
    void* tbl = MemRead::PtrAt(sceneObj, 0x48);
    if (!tbl) return -1;
    const uint64_t addr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(tbl));
    for (const RoutineFacts& f : facts)
        if (f.entryTable != 0 && f.entryTable == addr) return f.index;
    return -1;
}

} // namespace MapScript
