#include "navigation/map_script.h"
#include "navigation/map_script_internal.h"
#include "navigation/map_names.h"
#include "core/logger.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

using namespace MapScript::Internal;

namespace MapScript {

// ---- Map-jump capture (S57, rebuilt S63): the data the destination binding is still missing --------
//
// Dumps in one `'` press: both position tables RAW and COMPLETE (all 32 bytes per record -- we had only
// ever read the first 16), the blob header's offset words, and the bytecode of the `__MJ_CTRL`
// controllers AND the map's Director routines, with every native call NAMED.
//
// Natives are named from the archived `.dbg` symbol table for the `mapctrl` script module
// (`FFXII-Decompile/notes/dbg_symbols_mapctrl.csv`) via `dbgIndex = nativeId + 5140`. That mapping is
// ANCHORED on `mapjump` (native 0x8D, index 5281) and cross-validated 15/15 against a whole `__MJ_CTRL`
// routine, which decodes to a coherent fade-out-and-jump sequence. It is trustworthy in this band; the
// standing warning about `dbg_idx - 5140` concerns index math across the whole symbol file, where
// variables and source markers interleave, and still applies everywhere else.
//
// What the capture is FOR: `__MJ_CTRL` carries a correct `mapjump(dest,...)` literal but nothing that
// says WHICH trigger it owns, and every local guess at that binding has been refuted in play. The
// routine's first distinguishing call is `setmapjumpgroup(K)`, so the binding runs through the engine's
// map-jump GROUP system -- and the existence of `setmapidmj` / `setmapidfloor` / `setmapidwall` says the
// WALKMAP's own polygons carry ids of the same family. `ExitDiag::DumpWalkPolyClasses` captures that
// other half. Read-only and SEH-guarded throughout.
namespace {

// Native id -> name for the map-jump / touch / fade family these routines use. Only ids we have
// actually seen or specifically expect are listed; anything else prints as raw hex, which is the honest
// answer -- inventing a name is how "the zone test is native 0x202d" became a fact quoted in three
// documents while sitting outside the native table entirely.
struct NativeEntry { uint16_t id; const char* name; };
constexpr NativeEntry kNatives[] = {
    { 0x0000, "wait" },                      { 0x0005, "waitv" },
    { 0x000D, "ucon" },                      { 0x000E, "ucoff" },
    { 0x0026, "settouchwh" },                { 0x002A, "reqenable" },
    { 0x002B, "reqdisable" },                { 0x002E, "keywait" },
    { 0x0051, "touchradius" },               { 0x007B, "usemapid" },
    { 0x0083, "usecharhit" },                { 0x008D, "mapjump" },
    { 0x008F, "setmapjumppos" },             { 0x009A, "ucmove" },
    { 0x009B, "sysucoff" },                  { 0x009C, "sysucon" },
    { 0x00B6, "fadeout" },                   { 0x00B8, "fadein" },
    { 0x00B9, "fadesync" },                  { 0x00BA, "fadecolor" },
    { 0x00CF, "fadeprior" },                 { 0x00D0, "fadeout" },
    { 0x00D5, "mapjumpstatus" },             { 0x00FA, "resetmapidmjall" },
    { 0x00FB, "resetmapidmj" },              { 0x00FD, "setmapidmjground" },
    { 0x00FE, "setmapidfloor" },             { 0x00FF, "setmapidwall" },
    { 0x0100, "setmapidmj" },                { 0x0103, "setmapidmj" },
    { 0x0108, "resetmapidmjground" },        { 0x010B, "resetmapidmj" },
    { 0x011B, "startscene" },                { 0x011E, "setmapjumpgroup" },
    { 0x0158, "fadelayer" },                 { 0x0168, "ucsetpos" },
    { 0x020F, "setnochecktouchheightflag" }, { 0x0211, "getmapjumpposbyindex" },
    { 0x0212, "ucsetpos" },                  { 0x026D, "istouchuc" },
    { 0x026E, "settouchuconly" },            { 0x02C5, "effectkeepmapjump" },
    { 0x02EA, "getmapid" },                  { 0x0323, "getmapjumpanglebyindex" },
    { 0x03D7, "stopspotsound" },             { 0x03D8, "startspotsound" },
    { 0x03EF, "spotsoundtrans" },            { 0x040B, "clearmapjumpstatus" },
    { 0x0430, "setmapjumpgroupflag" },       { 0x0431, "releasemapjumpgroupflag" },
    { 0x0432, "ismapjumpgroupflag" },        { 0x0525, "istouchucsync" },
    { 0x0529, "istouchuc" },                 { 0x052A, "istouchucsync" },
    { 0x052B, "ucmove" },                    { 0x052C, "ucmovesync" },
    { 0x0542, "pausesestop" },               { 0x056D, "setposparty_mapjump" },
};

const char* NativeName(uint16_t id) {
    for (const auto& n : kNatives) if (n.id == id) return n.name;
    return nullptr;
}

// Dump one parallel position table (count + x/y/z/angle per record), raw and complete.
void DumpPosTable(void* blob, uint32_t hdrOff, const char* tag) {
    uint32_t tblOff = 0;
    if (!BlobU32(blob, hdrOff, &tblOff) || tblOff == 0 || tblOff >= OFFSET_MAX) {
        char m[96]; snprintf(m, sizeof(m), "pos-table %s: header offset unreadable (0x%X)", tag, tblOff);
        Log::Write("NAV-DIAG", m); return;
    }
    uint32_t count = 0;
    if (!BlobU32(blob, tblOff, &count) || count > JUMP_COUNT_MAX) {
        char m[112]; snprintf(m, sizeof(m), "pos-table %s: count out of range (off=0x%X count=%u)", tag, tblOff, count);
        Log::Write("NAV-DIAG", m); return;
    }
    char hdr[112];
    snprintf(hdr, sizeof(hdr), "==== pos-table %s off=0x%X count=%u (RAW, un-deduped) ====", tag, tblOff, count);
    Log::Write("NAV-DIAG", hdr);
    // THE WHOLE RECORD. The stride is 0x20 and every previous capture read only the first 16 bytes
    // (x/y/z/angle), so half of every record in both tables has never been looked at. A group id, a zone
    // extent or a flag word would live exactly there, and it is the cheapest place left to look.
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t raw[JUMP_STRIDE] = {};
        if (!BlobBytes(blob, tblOff + 4 + i * JUMP_STRIDE, raw, sizeof(raw))) {
            char m[64]; snprintf(m, sizeof(m), "  [%u] <unreadable>", i); Log::Write("NAV-DIAG", m); continue;
        }
        float f[4] = {};
        std::memcpy(f, raw, sizeof(f));
        char hex[3 * JUMP_STRIDE + 8] = {};
        int  p = 0;
        for (uint32_t k = 0; k < JUMP_STRIDE && p < static_cast<int>(sizeof(hex)) - 3; ++k)
            p += snprintf(hex + p, sizeof(hex) - static_cast<size_t>(p), "%02x ", raw[k]);
        char m[240];
        snprintf(m, sizeof(m), "  %s[%u] (%.2f,%.2f,%.2f) ang=%.3f | %s",
                 tag, i, f[0], f[1], f[2], f[3], hex);
        Log::Write("NAV-DIAG", m);
        // The unread tail as scalars too, so a small integer id cannot hide inside the hex.
        uint32_t u[4] = {};
        std::memcpy(u, raw + 16, sizeof(u));
        snprintf(m, sizeof(m), "        tail: u32 %u %u %u %u  (0x%08X 0x%08X 0x%08X 0x%08X)",
                 u[0], u[1], u[2], u[3], u[0], u[1], u[2], u[3]);
        Log::Write("NAV-DIAG", m);
    }
}

// Log every CALLACTPOPA (`5d <u16 native>`) in the routine's bytecode, with the three preceding
// push-u16 operands (mapjump/zone-test both take their args as pushed u16s). This is the annotated
// view the offline decode reads to bind a routine's zone test to a position.
void DumpRoutineCalls(const std::vector<uint8_t>& b, uint32_t codeOff) {
    for (size_t o = 0; o + 3 <= b.size(); ++o) {
        if (b[o] != OP_CALLACTPOPA) continue;
        const uint16_t native = static_cast<uint16_t>(b[o + 1] | (b[o + 2] << 8));
        // Recover up to three preceding `4f <u16>` push operands (immediately before the call).
        int op[3] = { -1, -1, -1 };
        size_t p = o;
        for (int k = 2; k >= 0; --k) {
            if (p >= 3 && b[p - 3] == OP_PUSH_U16) { op[k] = b[p - 2] | (b[p - 1] << 8); p -= 3; }
            else break;
        }
        const char* nm = NativeName(native);
        char m[208];
        snprintf(m, sizeof(m), "    @+0x%zX call 0x%04X %-26s args=[%d,%d,%d]",
                 codeOff + o, native, nm ? nm : "<unknown>", op[0], op[1], op[2]);
        Log::Write("NAV-DIAG", m);
    }
}

// True when a routine name contains "director" in any case. The map-jump DIRECTOR is the routine we
// have never dumped and the only place left in the script where a trigger could be bound to a
// controller: `__MJ_CTRL` performs the jump but contains no touch test at all (its 15 natives all
// decode, and none of them reads player position). Muthru's table holds `mogxiMapJumpDirector` and
// `Map_Director`; the prefix is the map's own codename, so match on the suffix, never on a map id.
bool LooksLikeDirector(const std::string& n) {
    static const char kNeedle[] = "director";
    if (n.size() < 8) return false;
    for (size_t i = 0; i + 8 <= n.size(); ++i) {
        size_t k = 0;
        for (; k < 8; ++k) {
            char c = n[i + k];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != kNeedle[k]) break;
        }
        if (k == 8) return true;
    }
    return false;
}

// The blob header is a run of u32 offsets and we have only ever opened four of them (0x18 routine
// table, 0x4C name pool, 0x54 / 0x84 position tables). Whatever holds the trigger geometry has to be
// reachable from one of the others, so print them all with their first words -- a table announces
// itself by starting with a plausible count.
void DumpHeaderWords(void* blob) {
    Log::Write("NAV-DIAG", "==== blob header words (u32 at hdr+0x00 .. +0xC0) ====");
    for (uint32_t off = 0; off <= 0xC0; off += 4) {
        uint32_t v = 0;
        if (!BlobU32(blob, off, &v)) continue;
        const bool plausible = (v != 0 && v < OFFSET_MAX);
        uint32_t first = 0;
        char note[72] = "";
        if (plausible && BlobU32(blob, v, &first) && first > 0 && first < 4096)
            snprintf(note, sizeof(note), "  -> [0]=%u  (plausible count)", first);
        else if (plausible)
            snprintf(note, sizeof(note), "  -> [0]=%u", first);
        char m[144];
        snprintf(m, sizeof(m), "  hdr+0x%02X = 0x%08X%s%s", off, v,
                 (off == 0x18) ? "  (routine table)" :
                 (off == 0x4C) ? "  (name pool)" :
                 (off == 0x54) ? "  (+0x54 positions)" :
                 (off == 0x84) ? "  (+0x84 positions)" : "", note);
        Log::Write("NAV-DIAG", m);
    }
}

} // namespace

void DumpCaptureDiag() {
    void* blob = BlobBase();
    if (!blob) { Log::Write("NAV-DIAG", "capture: no map-control blob"); return; }

    char hdr[96];
    snprintf(hdr, sizeof(hdr), "==== MAP-JUMP CAPTURE mapId=%d ====", MapNames::CurrentMapId());
    Log::Write("NAV-DIAG", hdr);

    // 1) Every header offset word, then both position tables complete (all 32 bytes per record).
    DumpHeaderWords(blob);
    DumpPosTable(blob, HDR_JUMP_TABLE,   "+0x54");
    DumpPosTable(blob, HDR_ARRIVE_TABLE, "+0x84");

    // 2) Each __MJ_CTRL routine's bytecode + annotated native calls. Same table parse as ReadExitDests.
    uint32_t rtOff = 0, poolOff = 0;
    if (!BlobU32(blob, HDR_ROUTINE_TABLE, &rtOff) || rtOff == 0 || rtOff >= OFFSET_MAX ||
        !BlobU32(blob, HDR_NAME_POOL, &poolOff) || poolOff == 0 || poolOff >= OFFSET_MAX) {
        Log::Write("NAV-DIAG", "capture: routine table / name pool offset unreadable"); return;
    }
    uint32_t count = 0;
    if (!BlobU32(blob, rtOff, &count) || count == 0 || count > MAX_ROUTINES) {
        Log::Write("NAV-DIAG", "capture: routine count out of range"); return;
    }
    std::vector<uint8_t> tbl(static_cast<size_t>(count) * ROUTINE_STRIDE);
    if (!BlobBytes(blob, rtOff + 4, tbl.data(), tbl.size())) {
        Log::Write("NAV-DIAG", "capture: routine table unreadable"); return;
    }
    std::vector<uint32_t> codeOffs(count);
    for (uint32_t i = 0; i < count; ++i)
        codeOffs[i] = U32(tbl, static_cast<size_t>(i) * ROUTINE_STRIDE + REC_CODE_OFF);
    std::vector<uint32_t> sorted = codeOffs;
    std::sort(sorted.begin(), sorted.end());

    // A one-line index of EVERY routine first: name, code offset, span. Cheap, and it means a routine
    // we have not thought to dump can never stay invisible again.
    Log::Write("NAV-DIAG", "==== routine index (all routines: name @codeOff span) ====");
    for (uint32_t i = 0; i < count; ++i) {
        const std::string nm =
            PoolName(blob, poolOff, U32(tbl, static_cast<size_t>(i) * ROUTINE_STRIDE + REC_NAME_OFF));
        std::string safe;
        for (char c : nm) safe.push_back((c >= 0x20 && c < 0x7f) ? c : '?');
        auto nx2 = std::upper_bound(sorted.begin(), sorted.end(), codeOffs[i]);
        const uint32_t sp = (nx2 == sorted.end()) ? 0u : (*nx2 - codeOffs[i]);
        char m[176];
        snprintf(m, sizeof(m), "  [%u] %-40s @+0x%X span=%u", i, safe.c_str(), codeOffs[i], sp);
        Log::Write("NAV-DIAG", m);
    }

    std::vector<uint8_t> code;
    for (uint32_t i = 0; i < count; ++i) {
        const std::string name =
            PoolName(blob, poolOff, U32(tbl, static_cast<size_t>(i) * ROUTINE_STRIDE + REC_NAME_OFF));
        const int  idx      = ParseCtrlIndex(name);
        const bool director = LooksLikeDirector(name);
        if (idx < 0 && !director) continue;

        const uint32_t start = codeOffs[i];
        auto nx = std::upper_bound(sorted.begin(), sorted.end(), start);
        const uint32_t end = (nx == sorted.end()) ? (start + 0x400u) : *nx;
        // A Director is long -- it polls every trigger on the map -- so give it the full span cap
        // rather than the 0x400 a controller needs. Truncating it is how the answer stays hidden.
        const size_t cap = director ? CODE_SPAN_MAX : size_t{0x400};
        size_t span = std::min<size_t>((end > start) ? end - start : cap, cap);
        code.assign(span, 0);
        while (span >= 0x20 && !BlobBytes(blob, start, code.data(), span)) { span /= 2; code.assign(span, 0); }
        if (span < 0x20) continue;

        std::string safe;
        for (char c : name) safe.push_back((c >= 0x20 && c < 0x7f) ? c : '?');
        char rh[160];
        snprintf(rh, sizeof(rh), "  %s @codeOff=+0x%X span=%zu bytes:", safe.c_str(), start, span);
        Log::Write("NAV-DIAG", rh);
        DumpRoutineCalls(code, start);
        if (director) continue;   // named calls are what a Director is read for; skip its raw hex
        // Full hex so nothing is lost to offline decode (32 bytes/line).
        for (size_t o = 0; o < span; o += 0x20) {
            char line[3 * 0x20 + 24] = {};
            int p = snprintf(line, sizeof(line), "    +0x%zX: ", start + o);
            for (size_t k = o; k < o + 0x20 && k < span && p < static_cast<int>(sizeof(line)) - 3; ++k)
                p += snprintf(line + p, sizeof(line) - static_cast<size_t>(p), "%02x ", code[k]);
            Log::Write("NAV-DIAG", line);
        }
    }
    Log::Write("NAV-DIAG", "==== end MAP-JUMP CAPTURE ====");
}

} // namespace MapScript
