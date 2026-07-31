#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The field-script blob's on-disk layout and the guarded readers over it — shared between the exit
// reader (`map_script.cpp`) and the offline capture dump (`map_script_diag.cpp`). Split out when
// map_script.cpp passed the project's 500-line ceiling: the two units parse the SAME structures, and
// duplicating the offsets is exactly how a reader and its diagnostic drift apart and stop describing
// the same thing.
//
// Not a public interface — `map_script.h` is. Nothing outside those two translation units includes this.
namespace MapScript {
namespace Internal {

// ---- Field-script blob layout (engine-wide; identical on every map) ---------------------------
// The map-control blob starts with a header of u32 offsets, all relative to the blob base:
//   hdr+0x18 -> ROUTINE TABLE:  [u32 count][count records of 0x30 bytes]
//               record: {+0x00 nameOff (into the name pool), +0x08 codeOff (routine entry)}
//   hdr+0x4c -> NAME POOL: NUL-terminated names; a routine's name is  pool + nameOff.
//
// A routine's code SPAN is [codeOff, next-highest codeOff) — the record's other fields are label
// and variable sub-tables, not a byte length (two controller routines on one map had byte-identical
// sub-tables, because they are the same compiled template differing only in the destination literal).
constexpr uint32_t HDR_ROUTINE_TABLE = 0x18;
constexpr uint32_t HDR_NAME_POOL     = 0x4C;

// hdr+0x54 and hdr+0x84 -> TWO parallel position tables, same format ([u32 count][records of 0x20],
// four floats x/y/z/angle per record); `FUN_00264b90` reads one or the other by a flag.
//
// SETTLED (Session 58, from the `'` capture — the S46-era "+0x54 = departure triggers" note was wrong):
//   +0x54 = the party ARRIVAL table. Party-spawn reads it to place you when you enter a map.
//   +0x84 = the same arrivals, in order, with ONE extra "edge" record inserted per map-jump controller.
//           An edge is the transition TRIGGER's reference point, off the walkable mesh at the map
//           boundary; the record right after the i-th edge is controller i's arrival.
constexpr uint32_t HDR_JUMP_TABLE    = 0x54;   // arrivals
constexpr uint32_t HDR_ARRIVE_TABLE  = 0x84;   // arrivals + one edge record per controller
constexpr uint32_t JUMP_STRIDE       = 0x20;
constexpr uint32_t JUMP_COUNT_MAX    = 64;
constexpr uint32_t ROUTINE_STRIDE    = 0x30;
constexpr uint32_t REC_NAME_OFF      = 0x00;
constexpr uint32_t REC_CODE_OFF      = 0x08;
constexpr uint32_t MAX_ROUTINES      = 512;    // sanity bound; real maps run ~24-40

// Sanity ceiling on any blob-relative offset. NOT a read window -- every read below is SEH-guarded and
// addresses the live blob directly, so this only rejects an offset so large it must be garbage.
//
// WHY THERE IS NO WINDOW ANY MORE: this reader used to snapshot a fixed 0x18000 (96 KB) prefix of the
// blob and parse the copy, bounds-checking the header offsets against the SNAPSHOT. On any map whose
// routine table or name pool sits past 96 KB, those checks failed and ReadExitDests returned false --
// silently, before its first log line -- so every door on the map was reported as "no controller
// (arrival point)" and the Exit category was empty. That is what killed exits across all of Rabanastre:
// East End, Muthru Bazaar and The Sandsea all produced ZERO log output, while Migelo's Sundries (a small
// shop interior) already had its routine table at +0xE350. The window was never a property of the
// format, only of the reader.
constexpr uint32_t OFFSET_MAX        = 0x400000;
constexpr size_t   NAME_MAX          = 64;     // longest routine name we bother to read
constexpr size_t   CODE_SPAN_MAX     = 0x8000; // per-controller code scan cap (templates are ~KB)

// The map toolchain's auto-generated name for a map-jump door controller.
constexpr char MJ_PREFIX[]     = "__MJ_CTRL";
constexpr size_t MJ_PREFIX_LEN = sizeof(MJ_PREFIX) - 1;

// `mapjump(dest, entrance, flags)` compiles to three push-immediates then the native call:
//   4f <destU16> 4f <entU16> 4f <flagsU16> 5d 8d 00
// 0x4F = push u16, 0x5D = CALLACTPOPA, native 0x8D = mapjump. flags==0 is a field door;
// flags==0x0A is the world-map teleport menu (a long run of them sits in every map) and is excluded.
constexpr uint8_t  OP_PUSH_U16     = 0x4F;
constexpr uint8_t  OP_CALLACTPOPA  = 0x5D;
constexpr uint8_t  NATIVE_MAPJUMP  = 0x8D;
constexpr uint16_t MAPJUMP_FLAGS_FIELD_DOOR = 0;
// The one flags value that is never a walk-to transition. Every map's Director routine holds a long
// run of these -- the world-map teleport MENU (S64) -- and admitting them would fill the exit list
// with places you cannot walk to.
//
// The rest of the value space is NOT a kind. `mapjump` = FUN_00355350 calls
// FUN_00314440(dest, entrance, flags, 1) -> FUN_003145e0, where `flags & 1` selects the no-fade path
// and `(flags >> 1) & 1` feeds FUN_002efa70: a PRESENTATION bitfield. `== 0` is therefore a filter on
// how a jump looks, not on what it is, which is why the controller path keeps it (play-confirmed on
// every map that lists exits today) and the group-claiming path does not need it.
constexpr uint16_t MAPJUMP_FLAGS_WORLDMAP_MENU = 0x0A;

// `setmapjumpgroup(K)` — native 0x011E, compiled as `4f <K:u16> 5d 1e 01`. It is the FIRST
// distinguishing call in every `__MJ_CTRL` routine, and K is the routine's MAP-JUMP GROUP: the same
// number the walkmap tags its trigger polygons with (`NavRva::WALK_POLY_MJ_*`). That is the whole
// destination binding -- the routine that owns group K owns the surface tagged K, and its own
// `mapjump` literal says where K goes. Session 58 read this call as a meaningless authoring-order id
// and threw away the answer; the name came from the archived `mapctrl` .dbg symbol table.
constexpr uint16_t NATIVE_SETMAPJUMPGROUP = 0x011E;

// ---- Direct, SEH-guarded reads against the LIVE blob ------------------------------------------
// The blob is game-owned memory that can be torn down under us, so every access goes through
// MemRead; a fault fails that one read instead of the whole parse. Nothing is copied wholesale.

// Little-endian scalars out of an already-copied buffer; 0 when the span runs off the end.
inline uint32_t U32(const std::vector<uint8_t>& b, size_t off) {
    if (off + 4 > b.size()) return 0;
    return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}
inline uint16_t U16(const std::vector<uint8_t>& b, size_t off) {
    if (off + 2 > b.size()) return 0;
    return static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
}

// The loaded map-control blob (mapData), or null when no field script is loaded.
void* BlobBase();

// A blob-relative u32, with the sanity ceiling applied. False on fault or an absurd offset.
bool BlobU32(void* blob, uint32_t off, uint32_t* out);

// Copy `n` bytes at blob+off into `dst`. False on fault or an out-of-range span.
bool BlobBytes(void* blob, uint32_t off, void* dst, size_t n);

// Read a NUL-terminated ASCII name out of the pool, live and bounded. Empty on fault or overrun.
// Names may be Shift-JIS (most routines are named in Japanese) — we only ever compare the ASCII
// `__MJ_CTRL` form, so raw bytes are fine and no transcoding is needed.
std::string PoolName(void* blob, uint32_t poolOff, uint32_t nameOff);

// `__MJ_CTRL012` -> 12. Returns -1 when the name is not a controller.
int ParseCtrlIndex(const std::string& name);

} // namespace Internal
} // namespace MapScript
