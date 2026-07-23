#include "navigation/exit_diag.h"
#include "navigation/map_exits.h"
#include "navigation/map_script.h"
#include "navigation/map_names.h"
#include "navigation/nav_rva.h"
#include "navigation/map_rva.h"
#include "navigation/nav_common.h"
#include "navigation/nav_trace.h"
#include "navigation/map_query.h"
#include "navigation/entity_scan.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/logger.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ExitDiag {

namespace {

// A door trigger and its field sign are never at the same point -- measured 3.6 to 6.4 m apart on East
// End, because the sign marks the "-> area" arrow and the trigger is the volume you step into. 8 m pairs
// every measured case while staying well under the ~25 m gap to the next-nearest door.
constexpr float kSignMatchDist = 8.0f;

// ---- Walkmap polygon CLASSES (Session 63) -------------------------------------------------------
//
// The `mapctrl` native table names `setmapidfloor`, `setmapidwall` and **`setmapidmj`** -- "mj" being
// the map-jump family that `mapjump` and `setmapjumpgroup` belong to. That says the walkmap's own
// polygons carry an id, and that one class of them is the MAP-JUMP surface. If so the transition
// trigger is IN the walkmap, addressable on the first frame of any map, with no cross-map data and
// nothing learned by playing -- which is exactly what the exit reader is missing.
//
// We have never looked: `NavRva::WALK_POLY_STRIDE` is 0x20, `ReadCellFloor` reads bytes 0x00-0x11 and
// uses only the LOW 3 BITS of the flags word at +0x0C. The upper 29 bits and the whole 0x12-0x1F tail
// are unread.
//
// So: sweep the grid, group polys by their full flags word, and for each distinct value report how many
// there are and where a few of them sit. The decode is then a one-glance comparison against ground
// truth -- on Muthru Bazaar the transition to East End fires at about (56, 64.3), so whichever class
// has members there is the map-jump surface. Bounded and one-shot (the `'` key), never in normal play.
constexpr int kPolyClassMax    = 24;      // distinct flag values worth reporting
constexpr int kPolySamples     = 3;       // example positions per class
constexpr int kPolyReadBudget  = 200000;  // hard ceiling on primitive reads for the whole sweep

void LogWalkPolyClasses() {
    MapQuery::WalkGridInfo g;
    if (!MapQuery::GetGridInfo(g) || !g.valid) {
        Log::Write("NAV-DIAG", "walk poly classes: no walkmap grid");
        return;
    }

    struct Cls {
        uint32_t flags;
        int      count;
        int      nSample;
        float    sx[kPolySamples], sz[kPolySamples];
        uint8_t  tail[14];      // bytes 0x12..0x1F of the first poly seen in this class
        bool     haveTail;
    };
    Cls cls[kPolyClassMax] = {};
    int  nCls   = 0;
    int  reads  = 0;
    int  seenPolys = 0;

    char hdr[176];
    snprintf(hdr, sizeof(hdr), "==== walkmap poly classes: grid %dx%d cell %dx%d origin (%d,%d) ====",
             g.nCols, g.nRows, g.cellSizeX, g.cellSizeZ, g.originX, g.originZ);
    Log::Write("NAV-DIAG", hdr);

    for (int row = 0; row < g.nRows && reads < kPolyReadBudget; ++row) {
        for (int col = 0; col < g.nCols && reads < kPolyReadBudget; ++col) {
            const int cell = g.nCols * row + col;
            uint16_t start = 0, end = 0;
            if (!MemRead::SafeReadU16(g.csrTable, static_cast<uint32_t>(cell) * 2u, &start)) continue;
            if (!MemRead::SafeReadU16(g.csrTable, static_cast<uint32_t>(cell + 1) * 2u, &end)) continue;
            if (end < start) continue;

            float cx, cz;
            MapQuery::CellCenter(g, col, row, cx, cz);

            for (uint32_t k = start; k < end && (k - start) < 256u; ++k) {
                if (++reads >= kPolyReadBudget) break;
                uint16_t prim = 0;
                if (!MemRead::SafeReadU16(g.primList, k * 2u, &prim)) break;
                if (prim >= NavRva::WALK_PRIM_FLOOR_MAX) continue;   // wall / empty, not a floor poly
                const uint32_t pbase = static_cast<uint32_t>(prim) * NavRva::WALK_POLY_STRIDE;
                uint32_t flags = 0;
                if (!MemRead::SafeReadU32(g.polyArr, pbase + NavRva::WALK_POLY_FLAGS, &flags)) continue;
                ++seenPolys;

                int ci = -1;
                for (int c = 0; c < nCls; ++c) if (cls[c].flags == flags) { ci = c; break; }
                if (ci < 0) {
                    if (nCls >= kPolyClassMax) continue;             // reported below as truncation
                    ci = nCls++;
                    cls[ci].flags = flags;
                    // Keep the unread tail of the first member, so a per-poly id shows up as data.
                    uint8_t tail[14] = {};
                    cls[ci].haveTail = MemRead::SafeReadBytes(
                        static_cast<const char*>(g.polyArr) + pbase + 0x12, tail, sizeof(tail));
                    if (cls[ci].haveTail) memcpy(cls[ci].tail, tail, sizeof(tail));
                }
                ++cls[ci].count;
                if (cls[ci].nSample < kPolySamples) {
                    cls[ci].sx[cls[ci].nSample] = cx;
                    cls[ci].sz[cls[ci].nSample] = cz;
                    ++cls[ci].nSample;
                }
            }
        }
    }

    char m[288];
    snprintf(m, sizeof(m), "  swept %d floor prims into %d classes (budget %d, reads %d)%s",
             seenPolys, nCls, kPolyReadBudget, reads,
             (reads >= kPolyReadBudget) ? "  *** BUDGET EXHAUSTED, sweep is PARTIAL ***" : "");
    Log::Write("NAV-DIAG", m);

    for (int c = 0; c < nCls; ++c) {
        char samples[128] = {};
        int  p = 0;
        for (int i = 0; i < cls[c].nSample && p < static_cast<int>(sizeof(samples)) - 24; ++i)
            p += snprintf(samples + p, sizeof(samples) - static_cast<size_t>(p), "(%.0f,%.0f) ",
                          cls[c].sx[i], cls[c].sz[i]);
        char tail[48] = "n/a";
        if (cls[c].haveTail) {
            int q = 0;
            for (int i = 0; i < 14 && q < static_cast<int>(sizeof(tail)) - 3; ++i)
                q += snprintf(tail + q, sizeof(tail) - static_cast<size_t>(q), "%02x", cls[c].tail[i]);
        }
        snprintf(m, sizeof(m), "  flags=0x%08X type=%u count=%-6d at %s| tail(+0x12..0x1F)=%s",
                 cls[c].flags, cls[c].flags & NavRva::WALK_POLY_TYPE_MASK, cls[c].count, samples, tail);
        Log::Write("NAV-DIAG", m);
    }
    if (nCls >= kPolyClassMax)
        Log::Write("NAV-DIAG", "  *** class table full -- more distinct flag values exist than reported ***");
}

// ---- Walkability field around a doorway (Session 59) -------------------------------------------
// Every model of the transition trigger so far has been derived from the blob and refuted on the
// ground, most recently the claim that the paired `+0x84` record's bearing is the direction you cross
// (the tester was sent east into a wall). Before proposing a fifth model, look at the terrain: this
// prints the walkmap as an ASCII field centred on the arrival, so "which way out of here is even open"
// stops being a guess.
//
// '#' floor at the arrival's own height, '~' floor at a different tier (|dY| > kTierStep), '.' no
// floor at all, 'A' the arrival itself. Read the picture and the doorway is visible as the gap.
constexpr int   kFieldRadius = 10;     // cells each way -> 21x21
constexpr float kFieldPitch  = 1.0f;   // metres per cell
constexpr float kTierStep    = 1.5f;   // same climbable-step threshold the planner uses

void LogWalkField(const MapScript::ExitDest& d) {
    if (!MapQuery::HasWorld()) { Log::Write("NAV-DIAG", "walk field: no walkmap"); return; }

    float baseY = d.pos.y;
    MapQuery::GroundAt(d.pos.x, d.pos.z, baseY);   // the arrival's real floor, not the blob's nominal

    char hdr[208];
    snprintf(hdr, sizeof(hdr),
             "==== walk field __MJ_CTRL%03d dest=%u arrival=(%.1f,%.1f,%.1f) floorY=%.2f | %dm/cell, north is UP, west is LEFT ====",
             d.ctrlIndex, d.destMapId, d.pos.x, d.pos.y, d.pos.z, baseY, static_cast<int>(kFieldPitch));
    Log::Write("NAV-DIAG", hdr);

    // Rows run north (-Z) to south (+Z) so the printout reads like a map.
    for (int r = -kFieldRadius; r <= kFieldRadius; ++r) {
        char line[2 * kFieldRadius + 40] = {};
        int  p = snprintf(line, sizeof(line), "  z=%+6.1f ", d.pos.z + r * kFieldPitch);
        for (int c = -kFieldRadius; c <= kFieldRadius && p < static_cast<int>(sizeof(line)) - 2; ++c) {
            const float px = d.pos.x + c * kFieldPitch, pz = d.pos.z + r * kFieldPitch;
            float gy = 0.0f;
            char  ch;
            if (c == 0 && r == 0)                        ch = 'A';
            else if (!MapQuery::GroundAt(px, pz, gy))    ch = '.';
            else if (std::fabs(gy - baseY) > kTierStep)  ch = '~';
            else                                         ch = '#';
            line[p++] = ch;
        }
        line[p] = '\0';
        Log::Write("NAV-DIAG", line);
    }
    char foot[176];
    snprintf(foot, sizeof(foot), "  x runs %.1f (left) .. %.1f (right); '#'=walkable at arrival tier, '~'=other tier, '.'=no floor",
             d.pos.x - kFieldRadius * kFieldPitch, d.pos.x + kFieldRadius * kFieldPitch);
    Log::Write("NAV-DIAG", foot);
}

// ASCII-safe rendering of a resolved area name for the log (names are wide and may be non-Latin).
std::string Ascii(const std::wstring& s) {
    std::string o;
    for (size_t k = 0; k < s.size() && k < 63; ++k)
        o.push_back((s[k] > 0 && s[k] < 128) ? static_cast<char>(s[k]) : '?');
    return o;
}

// "<region>: <sub-area>" for a map id, ASCII, or "-" when nothing resolves.
std::string AreaText(int mapId) {
    if (mapId <= 0 || mapId == NavRva::AREAID_NONE) return "-";
    const std::string n = Ascii(MapNames::ResolveFullAreaName(mapId));
    return n.empty() ? "-" : n;
}

float Dist2D(const FVec3& a, const FVec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

// The loaded map-control blob, or null when no field script is live.
void* BlobBase() {
    void* containerBase = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!containerBase) return nullptr;
    return MemRead::PtrAt(containerBase, NavRva::TBL_GUARD_OFF);
}

// Index of the sign nearest `p` within kSignMatchDist, or -1.
int NearestSign(const std::vector<MapExits::SignRec>& signs, const FVec3& p, float& outDist) {
    int best = -1;
    float bestD = kSignMatchDist;
    for (size_t i = 0; i < signs.size(); ++i) {
        const float d = Dist2D(signs[i].pos, p);
        if (d < bestD) { bestD = d; best = static_cast<int>(i); }
    }
    if (best >= 0) outDist = bestD;
    return best;
}

// Pure getters over the MapRef record. POD in/out, SEH-guarded; nothing is written.
typedef unsigned short (__fastcall* Pfn_MapRefU16)(int mapId);
int CallMapRef(uint32_t rva, int mapId) {
    Pfn_MapRefU16 fn = reinterpret_cast<Pfn_MapRefU16>(Hooks::ResolveRva(rva));
    if (!fn) return -1;
    __try { return static_cast<int>(fn(mapId)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// ---- Section 1: which table covers which door -------------------------------------------------------
void LogCoverage(const std::vector<MapExits::ExitRec>& jumps,
                 const std::vector<MapScript::ExitDest>& ctrls,
                 const std::vector<MapExits::SignRec>& signs) {
    Log::Write("NAV-DIAG", "==== exit coverage: +0x54 slots x __MJ_CTRL x +0x70 field signs ====");
    int onlyCtrl = 0, onlySign = 0, both = 0, neither = 0;

    for (const auto& j : jumps) {
        // Current shipping rule: the controller whose recorded door POSITION matches this slot.
        const MapScript::ExitDest* c = nullptr;
        for (const auto& d : ctrls) {
            if (!d.posOk) continue;
            if (std::fabs(d.pos.x - j.pos.x) < 0.05f && std::fabs(d.pos.y - j.pos.y) < 0.05f &&
                std::fabs(d.pos.z - j.pos.z) < 0.05f) { c = &d; break; }
        }
        float sd = 0.0f;
        const int si = NearestSign(signs, j.pos, sd);

        if (c && si >= 0) ++both;
        else if (c)       ++onlyCtrl;
        else if (si >= 0) ++onlySign;
        else              ++neither;

        char cbuf[96] = "-";
        if (c) snprintf(cbuf, sizeof(cbuf), "CTRL%03d dest=%u \"%s\"",
                        c->ctrlIndex, c->destMapId, AreaText(c->destMapId).c_str());
        char sbuf[96] = "-";
        if (si >= 0) snprintf(sbuf, sizeof(sbuf), "g%d.%d destIdx=%u areaId=%u d=%.1fm",
                              signs[si].group, signs[si].index, signs[si].destIdx,
                              signs[si].areaId, sd);

        const char* flag = (c && si >= 0) ? ""
                         : c              ? "   <== CONTROLLER ONLY (no field sign)"
                         : (si >= 0)      ? "   <== FIELD SIGN ONLY (invisible to the exit reader)"
                                          : "   <== arrival point (neither table claims it)";
        char m[288];
        snprintf(m, sizeof(m), "  slot %-2d (%.1f,%.1f,%.1f)  ctrl=[%s]  sign=[%s]%s",
                 j.index, j.pos.x, j.pos.y, j.pos.z, cbuf, sbuf, flag);
        Log::Write("NAV-DIAG", m);
    }

    char sum[176];
    snprintf(sum, sizeof(sum),
             "  coverage: %zu jump slots, %zu controllers, %zu signs | both=%d ctrlOnly=%d signOnly=%d neither=%d",
             jumps.size(), ctrls.size(), signs.size(), both, onlyCtrl, onlySign, neither);
    Log::Write("NAV-DIAG", sum);

    // Signs with no jump slot near them are the interior doorways (press-Enter, not step-on). They are a
    // transition class the exit reader has never had a source for.
    for (size_t i = 0; i < signs.size(); ++i) {
        bool paired = false;
        for (const auto& j : jumps)
            if (Dist2D(signs[i].pos, j.pos) < kSignMatchDist) { paired = true; break; }
        if (paired) continue;
        char m[208];
        snprintf(m, sizeof(m),
                 "  sign g%d.%d (%.1f,%.1f,%.1f) destIdx=%u areaId=%u  <== no +0x54 slot (interior doorway)",
                 signs[i].group, signs[i].index, signs[i].pos.x, signs[i].pos.y, signs[i].pos.z,
                 signs[i].destIdx, signs[i].areaId);
        Log::Write("NAV-DIAG", m);
    }
}

// ---- Section 2: the two candidate pairings, side by side --------------------------------------------
// Rule A (shipping): __MJ_CTRL<N> owns +0x54 slot N+1.
// Rule B (candidate): __MJ_CTRL<N> owns the group-0 field sign with destIdx N+1, and thereby the +0x54
// slot nearest that sign. Rule B is on the table because rule A produces a geographically incoherent
// layout on East End -- it places Muthru Bazaar (west on the map screen) mid-map and Southern Plaza
// (south) at the far west, while rule B puts each at the edge its atlas position implies.
//
// ONE WALK-THROUGH DISCRIMINATES THEM: walk a door, read `announce: mapId=` and compare.
void LogPairings(const std::vector<MapExits::ExitRec>& jumps,
                 const std::vector<MapScript::ExitDest>& ctrls,
                 const std::vector<MapExits::SignRec>& signs) {
    Log::Write("NAV-DIAG", "==== exit pairing: rule A (+0x54 slot N+1) vs rule B (field-sign destIdx N+1) ====");
    for (const auto& d : ctrls) {
        // Rule A door: what the shipping code already resolved.
        char abuf[80] = "no slot";
        if (d.posOk) snprintf(abuf, sizeof(abuf), "slot %d (%.1f,%.1f,%.1f)", d.slot, d.pos.x, d.pos.y, d.pos.z);

        // Rule B door: group-0 sign carrying destIdx == ctrlIndex+1, then the nearest jump slot to it.
        char bbuf[112] = "no sign";
        for (const auto& s : signs) {
            if (s.group != 0 || s.destIdx != static_cast<uint8_t>(d.ctrlIndex + 1)) continue;
            int nearSlot = -1;
            float nearD = kSignMatchDist;
            for (const auto& j : jumps) {
                const float dd = Dist2D(j.pos, s.pos);
                if (dd < nearD) { nearD = dd; nearSlot = j.index; }
            }
            if (nearSlot >= 0)
                snprintf(bbuf, sizeof(bbuf), "slot %d via sign g0.%d (%.1f,%.1f,%.1f) d=%.1fm",
                         nearSlot, s.index, s.pos.x, s.pos.y, s.pos.z, nearD);
            else
                snprintf(bbuf, sizeof(bbuf), "sign g0.%d (%.1f,%.1f,%.1f) but no slot within %.0fm",
                         s.index, s.pos.x, s.pos.y, s.pos.z, kSignMatchDist);
            break;
        }

        char m[288];
        snprintf(m, sizeof(m), "  CTRL%03d -> dest=%u \"%s\" entrance=%u | ruleA: %s | ruleB: %s",
                 d.ctrlIndex, d.destMapId, AreaText(d.destMapId).c_str(), d.entrance, abuf, bbuf);
        Log::Write("NAV-DIAG", m);
    }
}

// ---- Section 3: the +0x8c destination records --------------------------------------------------------
// The game's own getter hands callers word[5], and that word reads 0xffff on every record measured. Words
// 2/3/4 are recorded offline as three story-progress variants. Resolving all four is what will show
// whether a live variant names the doors whose planmapname slot holds only the placeholder string.
void LogDestTable(const std::vector<MapExits::SignRec>& signs) {
    void* blob = BlobBase();
    if (!blob) { Log::Write("NAV-DIAG", "dest table (+0x8c): no map-control blob"); return; }

    uint32_t tblOff = 0;
    if (!MemRead::SafeReadU32(blob, NavRva::TBL_DEST_OFF, &tblOff) || tblOff == 0) {
        Log::Write("NAV-DIAG", "dest table (+0x8c): offset unreadable or zero");
        return;
    }
    char* tbl = static_cast<char*>(blob) + tblOff;
    uint32_t count = 0;
    MemRead::SafeReadU32(tbl, 0, &count);
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "==== dest table (+0x8c) off=0x%X count=%u  [word5 = what the game returns] ====",
             tblOff, count);
    Log::Write("NAV-DIAG", hdr);

    // Only the indices this map actually references, in ascending order and each once.
    std::vector<int> idx;
    for (const auto& s : signs) idx.push_back(static_cast<int>(s.destIdx));
    std::sort(idx.begin(), idx.end());
    idx.erase(std::unique(idx.begin(), idx.end()), idx.end());

    for (int di : idx) {
        if (di < 0 || (count > 0 && count < 0x1000 && static_cast<uint32_t>(di) >= count)) {
            char oob[96];
            snprintf(oob, sizeof(oob), "  dest[%d] out of range (count=%u)", di, count);
            Log::Write("NAV-DIAG", oob);
            continue;
        }
        uint16_t w[NavRva::DEST_REC_WORDS] = {};
        const uint32_t recOff = NavRva::DEST_TBL_HDR + static_cast<uint32_t>(di) * NavRva::DEST_REC_STRIDE;
        if (!MemRead::SafeReadBytes(tbl + recOff, w, sizeof(w))) {
            char bad[80];
            snprintf(bad, sizeof(bad), "  dest[%d] record unreadable", di);
            Log::Write("NAV-DIAG", bad);
            continue;
        }
        char raw[128];
        int p = snprintf(raw, sizeof(raw), "  dest[%d] words:", di);
        for (int k = 0; k < static_cast<int>(NavRva::DEST_REC_WORDS); ++k)
            p += snprintf(raw + p, sizeof(raw) - static_cast<size_t>(p), " [%d]=%u", k, w[k]);
        Log::Write("NAV-DIAG", raw);
        // Resolve the story-variant candidates and the word the game itself uses.
        for (int k : { 2, 3, 4, 5 }) {
            const std::string nm = AreaText(static_cast<int>(w[k]));
            if (nm == "-") continue;
            char line[176];
            snprintf(line, sizeof(line), "      word[%d] = %u -> \"%s\"%s",
                     k, w[k], nm.c_str(), (k == 5) ? "   (the word the game returns)" : "   (story variant)");
            Log::Write("NAV-DIAG", line);
        }
    }
}

// ---- Section 4: MapRef records for every destination this map names ----------------------------------
// A map whose planmapname slot holds the placeholder string may still be identifiable from its MapRef
// record. Logged for every destination id the map's own script jumps to.
void LogMapRef(const std::vector<MapScript::ExitDest>& ctrls) {
    if (ctrls.empty()) return;
    Log::Write("NAV-DIAG", "==== MapRef records for this map's jump destinations ====");
    std::vector<int> ids;
    for (const auto& d : ctrls) ids.push_back(static_cast<int>(d.destMapId));
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (int id : ids) {
        char m[224];
        snprintf(m, sizeof(m),
                 "  mapId=%d f0=%d f4=%d grp0=%d grp2=%d regionIdx=%d name=\"%s\"",
                 id,
                 CallMapRef(NavRva::MAPREF_FIELD0_BY_ID, id),
                 CallMapRef(NavRva::MAPREF_FIELD4_BY_ID, id),
                 CallMapRef(NavRva::MAPREF_GROUP0_BY_ID, id),
                 CallMapRef(NavRva::MAPREF_GROUP2_BY_ID, id),
                 CallMapRef(NavRva::MAP_NAME_INDEX_BY_ID, id),
                 AreaText(id).c_str());
        Log::Write("NAV-DIAG", m);
    }
}

} // namespace

void DumpCoverage() {
    char hdr[112];
    snprintf(hdr, sizeof(hdr), "==== EXIT DIAG: mapId=%d ====", MapNames::CurrentMapId());
    Log::Write("NAV-DIAG", hdr);

    std::vector<MapExits::ExitRec> jumps;
    MapExits::EnumerateMapJumps(nullptr, EntityScan::kExitMaxDist, jumps, /*logRaw=*/false);

    std::vector<MapScript::ExitDest> ctrls;
    MapScript::ReadExitDests(ctrls, /*logDetail=*/false);

    std::vector<MapExits::SignRec> signs;
    MapExits::EnumerateFieldSignRaw(signs);

    LogCoverage(jumps, ctrls, signs);
    LogPairings(jumps, ctrls, signs);
    LogDestTable(signs);
    LogMapRef(ctrls);
    // Session 57: the raw +0x54/+0x84 tables + every __MJ_CTRL routine's bytecode with its zone-test
    // annotated — the data to decode the true transition-tile position offline.
    MapScript::DumpCaptureDiag();
    // Session 59: where the player has actually been able to walk, and what the walkmap says around each
    // doorway. Between them these answer the question every blob-only model has got wrong so far --
    // which way out of this arrival is even open.
    NavTrace::DumpTrail();
    LogWalkPolyClasses();
    for (const auto& c : ctrls)
        if (c.posOk) LogWalkField(c);
    Log::Write("NAV-DIAG", "==== end EXIT DIAG ====");
}

} // namespace ExitDiag
