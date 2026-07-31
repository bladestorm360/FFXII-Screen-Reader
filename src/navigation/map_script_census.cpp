#include "navigation/map_script_census.h"
#include "navigation/map_script_internal.h"
#include "navigation/map_names.h"
#include "navigation/nav_rva.h"
#include "navigation/map_rva.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/logger.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace MapScript::Internal;

namespace {

// Output bounds. NO SILENT CAPS: whatever is dropped is counted and printed, because a census that
// truncates without saying so reads as "that is everything" when it is not.
constexpr int    kMaxCallLines  = 32;   // per container, setmapjumpgroup + mapjump lines
constexpr size_t kMaxNameChars  = 150;  // routine-name dump, wrapped at this width

// One recovered call. `kind` is which native it was; the operands are its inline literals.
struct Call {
    bool     isJump = false;   // false = setmapjumpgroup(K), true = mapjump(dest, entrance, flags)
    uint32_t routine = 0;
    uint32_t at = 0;           // offset within the routine's span
    uint16_t a = 0, b = 0, c = 0;
};

// Printable-ASCII form of a routine name (most are Shift-JIS, which would corrupt the log line).
std::string AsciiSafe(const std::string& s) {
    std::string o;
    for (char ch : s) o.push_back((ch >= 0x20 && ch < 0x7f) ? ch : '?');
    return o;
}

// The blob installed in script container `c`, or null. Container base = HANDLE_TABLE_BASE +
// c*HANDLE_TABLE_STRIDE (FUN_00263ff0's own arithmetic); the blob pointer is the container's first
// field, which is what `MapScript::Internal::BlobBase()` already reads for container 0.
void* ContainerBlob(int c) {
    void* base = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!base) return nullptr;
    return MemRead::PtrAt(base, c * NavRva::HANDLE_TABLE_STRIDE + NavRva::TBL_GUARD_OFF);
}

// The four map tables a MAP blob carries, so a container holding something else is visible as such
// rather than as a silent zero. Each header slot is a blob-relative offset to `[u32 count][records]`.
void LogBlobTables(int c, void* blob) {
    struct { const char* name; uint32_t hdr; } tbls[] = {
        { "+0x54 arrivals",  HDR_JUMP_TABLE               },
        { "+0x84 arr+edge",  HDR_ARRIVE_TABLE             },
        { "+0x70 fieldsign", NavRva::TBL_FIELDSIGN_OFF    },
        { "+0x8c destinfo",  NavRva::TBL_DEST_OFF         },
    };
    std::string line;
    for (const auto& t : tbls) {
        uint32_t off = 0, count = 0;
        char frag[64];
        if (!BlobU32(blob, t.hdr, &off) || off == 0 || off >= OFFSET_MAX) {
            snprintf(frag, sizeof(frag), "%s=absent ", t.name);
        } else if (BlobU32(blob, off, &count)) {
            snprintf(frag, sizeof(frag), "%s=%u ", t.name, count);
        } else {
            snprintf(frag, sizeof(frag), "%s=unreadable ", t.name);
        }
        line += frag;
    }
    char m[224];
    snprintf(m, sizeof(m), "  container %d: %s", c, line.c_str());
    Log::Write("NAV-DIAG", m);
}

// Scan ONE container. Returns the number of routines walked (0 = no blob / no readable table).
//
// The span walk repeats `ReadExitDests`'s, deliberately and with the blob-format constants shared
// from map_script_internal.h. The working reader produces every exit on every map and is not being
// refactored to serve a diagnostic; the constants are what keep the two describing one format.
int ScanContainer(int c, std::vector<int>& groupsArmed, int& totalRoutines, int& totalJumps) {
    void* blob = ContainerBlob(c);
    if (!blob) {
        char m[96];
        snprintf(m, sizeof(m), "  container %d: NO BLOB INSTALLED", c);
        Log::Write("NAV-DIAG", m);
        return 0;
    }

    uint32_t rtOff = 0, poolOff = 0, count = 0;
    const bool haveRt   = BlobU32(blob, HDR_ROUTINE_TABLE, &rtOff) && rtOff != 0 && rtOff < OFFSET_MAX;
    const bool havePool = BlobU32(blob, HDR_NAME_POOL, &poolOff) && poolOff != 0 && poolOff < OFFSET_MAX;
    if (haveRt) BlobU32(blob, rtOff, &count);

    // An absent header slot reads as 0 and prints as +0x0 -- which is the honest answer, and is what
    // tells a reader "this container's blob is not the map-blob shape" rather than hiding it.
    char hdr[224];
    snprintf(hdr, sizeof(hdr),
             "  container %d: blob=%p routineTable=+0x%X%s namePool=+0x%X%s routines=%u",
             c, blob, rtOff, haveRt ? "" : " (ABSENT)", poolOff, havePool ? "" : " (ABSENT)", count);
    Log::Write("NAV-DIAG", hdr);
    LogBlobTables(c, blob);

    if (!haveRt || !havePool || count == 0 || count > MAX_ROUTINES) {
        if (haveRt && count > MAX_ROUTINES) {
            char m[160];
            snprintf(m, sizeof(m),
                     "  container %d: routine count %u out of range -- not a routine table, nothing scanned",
                     c, count);
            Log::Write("NAV-DIAG", m);
        }
        return 0;
    }

    std::vector<uint8_t> tbl(static_cast<size_t>(count) * ROUTINE_STRIDE);
    if (!BlobBytes(blob, rtOff + 4, tbl.data(), tbl.size())) {
        char m[160];
        snprintf(m, sizeof(m), "  container %d: routine table UNREADABLE (%u entries) -- nothing scanned",
                 c, count);
        Log::Write("NAV-DIAG", m);
        return 0;
    }

    std::vector<uint32_t> codeOffs;
    codeOffs.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        codeOffs.push_back(U32(tbl, static_cast<size_t>(i) * ROUTINE_STRIDE + REC_CODE_OFF));
    std::vector<uint32_t> sorted = codeOffs;
    std::sort(sorted.begin(), sorted.end());

    std::vector<std::string> names(count);
    std::vector<Call>        calls;
    std::vector<uint8_t>     code;
    int spansRead = 0, spansEmpty = 0, spansUnreadable = 0;

    for (uint32_t i = 0; i < count; ++i) {
        names[i] = AsciiSafe(
            PoolName(blob, poolOff, U32(tbl, static_cast<size_t>(i) * ROUTINE_STRIDE + REC_NAME_OFF)));

        const uint32_t start = codeOffs[i];
        auto nx = std::upper_bound(sorted.begin(), sorted.end(), start);
        const uint32_t end = (nx == sorted.end()) ? (start + static_cast<uint32_t>(CODE_SPAN_MAX)) : *nx;
        if (end <= start) { ++spansEmpty; continue; }

        size_t span = std::min<size_t>(end - start, CODE_SPAN_MAX);
        code.assign(span, 0);
        while (span >= 0x40 && !BlobBytes(blob, start, code.data(), span)) {
            span /= 2;
            code.assign(span, 0);
        }
        if (span < 0x40) { ++spansUnreadable; continue; }
        ++spansRead;

        const std::vector<uint8_t>& b = code;

        // EVERY setmapjumpgroup(K), not just the first: a routine that arms two groups, or a span
        // that bled into its neighbour, is exactly what this census is here to make visible.
        for (size_t o = 0; o + 6 <= b.size(); ++o) {
            if (b[o] != OP_PUSH_U16 || b[o + 3] != OP_CALLACTPOPA) continue;
            if (static_cast<uint16_t>(b[o + 4] | (b[o + 5] << 8)) != NATIVE_SETMAPJUMPGROUP) continue;
            Call k;
            k.isJump = false;
            k.routine = i;
            k.at = static_cast<uint32_t>(o);
            k.a = U16(b, o + 1);
            calls.push_back(k);
            if (std::find(groupsArmed.begin(), groupsArmed.end(), static_cast<int>(k.a)) ==
                groupsArmed.end())
                groupsArmed.push_back(static_cast<int>(k.a));
        }

        for (size_t o = 0; o + 12 <= b.size(); ++o) {
            if (b[o] != OP_PUSH_U16 || b[o + 3] != OP_PUSH_U16 || b[o + 6] != OP_PUSH_U16) continue;
            if (b[o + 9] != OP_CALLACTPOPA || b[o + 10] != NATIVE_MAPJUMP || b[o + 11] != 0) continue;
            Call k;
            k.isJump = true;
            k.routine = i;
            k.at = static_cast<uint32_t>(o);
            k.a = U16(b, o + 1);   // dest map id
            k.b = U16(b, o + 4);   // entrance slot on the destination map
            k.c = U16(b, o + 7);   // presentation flags
            calls.push_back(k);
            ++totalJumps;
        }
    }
    totalRoutines += spansRead;

    char census[224];
    snprintf(census, sizeof(census),
             "  container %d: scanned %u routine(s) -- spans read=%d, empty=%d, unreadable=%d | "
             "found %zu map-jump call(s)",
             c, count, spansRead, spansEmpty, spansUnreadable, calls.size());
    Log::Write("NAV-DIAG", census);

    // The routine names. On container 0 `ReadExitDests` already prints them; anywhere else this is
    // the first time this project has ever seen them, and the name is what identifies the script.
    if (c != 0) {
        std::string line;
        for (uint32_t i = 0; i < count; ++i) {
            if (names[i].empty()) continue;
            line += "[" + std::to_string(i) + "]" + names[i] + " ";
            if (line.size() > kMaxNameChars) { Log::Write("NAV-DIAG", ("    " + line).c_str()); line.clear(); }
        }
        if (!line.empty()) Log::Write("NAV-DIAG", ("    " + line).c_str());
    }

    int printed = 0;
    for (const auto& k : calls) {
        if (printed >= kMaxCallLines) break;
        ++printed;
        const char* nm = (k.routine < names.size()) ? names[k.routine].c_str() : "?";
        char m[288];
        if (!k.isJump) {
            snprintf(m, sizeof(m), "    c%d routine[%u] \"%s\": setmapjumpgroup(%u) @+0x%X",
                     c, k.routine, nm, k.a, k.at);
        } else {
            const std::wstring dn = MapNames::ResolveFullAreaName(static_cast<int>(k.a));
            char n8[96] = {};
            for (size_t j = 0; j < dn.size() && j < 95; ++j)
                n8[j] = (dn[j] < 128) ? static_cast<char>(dn[j]) : '?';
            snprintf(m, sizeof(m),
                     "    c%d routine[%u] \"%s\": mapjump(dest=%u \"%s\", entrance=%u, flags=0x%X) @+0x%X",
                     c, k.routine, nm, k.a, n8, k.b, k.c, k.at);
        }
        Log::Write("NAV-DIAG", m);
    }
    if (static_cast<size_t>(printed) < calls.size()) {
        char m[160];
        snprintf(m, sizeof(m), "    c%d: %zu further call(s) NOT PRINTED (cap %d) -- they were found, "
                               "not missed", c, calls.size() - printed, kMaxCallLines);
        Log::Write("NAV-DIAG", m);
    }
    return static_cast<int>(count);
}

} // namespace

namespace MapScript {

void LogContainerCensus() {
    // ONE PRINTING PER MAP, but the latch is only taken once a container actually held a readable
    // blob. The first scans of a new map land while the script blob is still streaming in, and
    // latching there would spend the one printing on "NO BLOB INSTALLED" x5 and never retry -- the
    // same shape as the sign-table diagnostic that once printed the table and not one object line.
    static int s_loggedMap = -1;
    const int  mapId = MapNames::CurrentMapId();
    if (mapId == s_loggedMap) return;

    char head[224];
    snprintf(head, sizeof(head),
             "==== SCRIPT CONTAINER CENSUS (map %d): %u containers at HANDLE_TABLE_BASE, stride 0x%X, "
             "blob at +0x%X ====",
             mapId, NavRva::HANDLE_TABLE_CONTAINERS, NavRva::HANDLE_TABLE_STRIDE, NavRva::TBL_GUARD_OFF);
    Log::Write("NAV-DIAG", head);

    std::vector<int> groupsArmed;
    int withBlob = 0, totalRoutines = 0, totalJumps = 0;
    for (unsigned c = 0; c < NavRva::HANDLE_TABLE_CONTAINERS; ++c)
        if (ScanContainer(static_cast<int>(c), groupsArmed, totalRoutines, totalJumps) > 0) ++withBlob;

    // THE LINE THAT SETTLES IT. Pair this against the swept surfaces printed by exit_scan: a group
    // that a seam carries and this set does not name is a transition bound OUTSIDE every loaded
    // script container, and the search moves to the field-sign table / the event blob's own loader.
    std::sort(groupsArmed.begin(), groupsArmed.end());
    std::string gs;
    for (int g : groupsArmed) gs += std::to_string(g) + " ";
    if (gs.empty()) gs = "(none)";
    char sum[256];
    snprintf(sum, sizeof(sum),
             "  CENSUS: %d/%u container(s) hold a blob | %d routine span(s) read | %d mapjump call(s) | "
             "MAP-JUMP GROUPS ARMED ANYWHERE: %s",
             withBlob, NavRva::HANDLE_TABLE_CONTAINERS, totalRoutines, totalJumps, gs.c_str());
    Log::Write("NAV-DIAG", sum);

    if (withBlob > 0) s_loggedMap = mapId;
}

} // namespace MapScript
