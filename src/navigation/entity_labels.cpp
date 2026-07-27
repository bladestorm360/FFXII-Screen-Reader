#include "navigation/entity_labels.h"
#include "core/logger.h"

#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace EntityLabels {

namespace {

// Bump when the IDENTITY KEY changes. The store outlives the build that wrote it, and a key scheme
// change would silently attach one person's name to another; a mismatch discards the file instead.
//
// 1 -> 2 (Session 79): the key was `mapId . container . slot` and the slot is not stable across map
// loads. Discarding cost nothing when this shipped -- the store held 127 records and ZERO player
// labels, because F6 had never been bound to a key (see input_tracker.cpp), so there was no player
// work in it to lose.
//
// 2 -> 3 (Session 81): NUMBERS LEFT THE STORE. `NumberFor` allocated a new record whenever an
// ambiguous key failed the anchor test, with no cap, no eviction and no cleanup, so the file grew
// without bound and the assigned number could only climb: the v2 store held 39 records labelled
// "Cockatrice" numbered 1..39 for SIX real objects, and the tester heard "Cockatrice 37". Numbering
// is now worked out within each scan and never written down. Discarding again costs nothing -- the
// v2 file held zero player labels, and the discard path now logs the count so that is proven rather
// than assumed.
constexpr int kFormatVersion = 3;

// How near a live object must be to a record's stored anchor for them to be the same object -- used
// ONLY to break a tie between records that share `{mapId, baseLabel, nameIdx}`. Generous on purpose:
// it separates distinct NPCs (metres apart) rather than tracking one precisely, and a stationary NPC
// re-spawns within centimetres.
constexpr float kAnchorDist = 1.5f;

struct Rec {
    int          mapId    = 0;
    int16_t      nameIdx  = 0;
    std::wstring baseLabel;      // the game's own words for this object, before any " 2" suffix
    FVec3        anchor{};       // where this object stood when the record was created
    std::wstring label;          // the player's own words; empty = not named
    // Diagnostics only. STRUCK as identity in Session 79 -- see the header.
    uint8_t      container = 0xFF;
    uint16_t     slot      = 0xFFFF;
    // Which matching pass last handed this record out, so two live objects sharing a key cannot both
    // be given it. STILL LOAD-BEARING after numbering left this file: without it, two objects sharing
    // {mapId, baseLabel, nameIdx} and both inside kAnchorDist of one anchor would BOTH speak the
    // player's label -- one person's chosen name on a body they never named. Never persisted.
    uint32_t     claim    = 0;
};

std::vector<Rec> g_recs;
bool             g_loaded  = false;
uint32_t         g_scanSeq = 0;

float Dist3(const FVec3& a, const FVec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// %LOCALAPPDATA%, NOT the game folder -- the install is read-only to this mod and only
// build_and_deploy writes there. Empty when the variable is missing, which just disables persistence.
std::wstring StorePath(bool createDir) {
    wchar_t* base = nullptr;
    size_t   len  = 0;
    if (_wdupenv_s(&base, &len, L"LOCALAPPDATA") != 0 || !base) return std::wstring();
    std::wstring dir = std::wstring(base) + L"\\FFXII-Screen-Reader";
    free(base);
    if (createDir) CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\entity_labels.txt";
}

// Find the record describing this object, or null.
//
// `{mapId, baseLabel, nameIdx}` first. When that triple is UNIQUE on the map the match is exact and
// position never enters into it -- which is what keeps a wandering NPC with its own npcdic id stable
// wherever it has walked to. Only when the triple repeats (every anonymous object shares
// `{map, "NPC", -1}`) does the nearest anchor decide.
//
// The anchor is deliberately NOT refreshed on a match: rewriting it every rescan would mean a disk
// write on every scan, and would let a record drift across the map behind a roaming NPC.
Rec* Match(int mapId, int16_t nameIdx, const std::wstring& baseLabel, const FVec3& pos, bool claim) {
    Rec*  best       = nullptr;
    float bestD      = 0.0f;
    int   candidates = 0;
    for (auto& r : g_recs) {
        if (r.mapId != mapId || r.nameIdx != nameIdx || r.baseLabel != baseLabel) continue;
        if (r.claim == g_scanSeq && g_scanSeq != 0) continue;   // already given to another live object
        ++candidates;
        const float d = Dist3(r.anchor, pos);
        if (!best || d < bestD) { best = &r; bestD = d; }
    }
    if (!best) return nullptr;
    // Only an ambiguous key has to prove itself by position. One candidate is the answer by
    // elimination, however far it has wandered from where it was first seen.
    if (candidates > 1 && bestD > kAnchorDist) return nullptr;
    if (claim) best->claim = g_scanSeq;
    return best;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back((c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?');
    return s;
}

// `baseLabel` is delimited by " | " and " = " rather than tokenised, because game names contain
// spaces ("Nomad Elder") -- the old `%127s` read only the first word of one.
void Save() {
    const std::wstring path = StorePath(/*createDir=*/true);
    if (path.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8") != 0 || !f) return;
    fwprintf(f, L"version %d\n", kFormatVersion);
    fwprintf(f, L"# <mapId> <nameIdx> <x> <y> <z> <container> <slot> | <baseLabel> = <your label>\n");
    fwprintf(f, L"# Edit freely; reloaded on every area change. Clear a label by emptying it.\n");
    fwprintf(f, L"# container/slot are diagnostics only -- the identity is mapId + baseLabel + nameIdx,\n");
    fwprintf(f, L"# with the position breaking ties between objects that share all three.\n");
    fwprintf(f, L"# Only entries YOU named live here. Duplicate numbering is NOT stored -- it is worked\n");
    fwprintf(f, L"# out fresh on every scan, so it can neither drift nor accumulate in this file.\n");
    for (const auto& r : g_recs) {
        fwprintf(f, L"%d %d %.2f %.2f %.2f %u %u | %s = %s\n", r.mapId,
                 static_cast<int>(r.nameIdx), r.anchor.x, r.anchor.y, r.anchor.z,
                 static_cast<unsigned>(r.container), static_cast<unsigned>(r.slot),
                 r.baseLabel.empty() ? L"-" : r.baseLabel.c_str(), r.label.c_str());
    }
    fclose(f);
}

std::wstring Trim(const wchar_t* begin, const wchar_t* end) {
    while (begin < end && (*begin == L' ' || *begin == L'\t')) ++begin;
    while (end > begin && (end[-1] == L' ' || end[-1] == L'\t' || end[-1] == L'\n' ||
                           end[-1] == L'\r')) --end;
    return std::wstring(begin, end);
}

void Load() {
    g_recs.clear();
    const std::wstring path = StorePath(/*createDir=*/false);
    if (path.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") != 0 || !f) return;

    wchar_t line[512];
    int     ver = 0;
    if (!fgetws(line, 512, f) || swscanf_s(line, L"version %d", &ver) != 1 || ver != kFormatVersion) {
        fclose(f);
        char m[160];
        snprintf(m, sizeof(m), "entity labels: store is version %d, this build wants %d -- discarded "
                               "and rewritten empty",
                 ver, kFormatVersion);
        Log::Write("NAV-DIAG", m);
        // REWRITE, don't merely refuse. The old code left the stale file on disk, and `Reload()` runs
        // on EVERY area change, so this line repeated for the rest of the session -- and once numbers
        // stopped being persisted, nothing would ever have overwritten the file at all. Writing the
        // empty store at the new version makes the next Load silent and DELETES the leaked records
        // (the v2 file held 39 "Cockatrice" rows for six real animals). Save() does not call Load(),
        // so this is not re-entrant.
        Save();
        return;
    }
    while (fgetws(line, 512, f)) {
        if (line[0] == L'#' || line[0] == L'\n') continue;
        Rec r;
        int   mp = 0, ni = 0, ct = 0, sl = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (swscanf_s(line, L"%d %d %f %f %f %d %d", &mp, &ni, &x, &y, &z, &ct, &sl) != 7)
            continue;
        const wchar_t* bar = wcschr(line, L'|');
        if (!bar) continue;
        const wchar_t* eq = wcschr(bar, L'=');
        if (!eq) continue;
        r.mapId     = mp;
        r.nameIdx   = static_cast<int16_t>(ni);
        r.anchor    = FVec3{ x, y, z };
        r.container = static_cast<uint8_t>(ct);
        r.slot      = static_cast<uint16_t>(sl);
        r.baseLabel = Trim(bar + 1, eq);
        if (r.baseLabel == L"-") r.baseLabel.clear();
        r.label = Trim(eq + 1, eq + wcslen(eq));
        g_recs.push_back(r);
    }
    fclose(f);

    // Silent on an empty store: Reload() fires on every area change and an empty file has nothing
    // to say. CONSOLE OUTPUT BUDGET.
    if (g_recs.empty()) return;
    int named = 0;
    for (const auto& r : g_recs) if (!r.label.empty()) ++named;
    char m[128];
    snprintf(m, sizeof(m), "entity labels: loaded %zu entries (%d named by the player)",
             g_recs.size(), named);
    Log::Write("NAV-DIAG", m);
}

} // namespace

void Init() {
    if (g_loaded) return;
    g_loaded = true;
    Load();
}

void Reload() {
    g_loaded = true;
    Load();
}

void BeginScan() {
    Init();
    ++g_scanSeq;
    if (g_scanSeq == 0) ++g_scanSeq;   // 0 means "never claimed"; skip it on wrap
}

std::wstring LabelFor(int mapId, int16_t nameIdx, const std::wstring& baseLabel, const FVec3& pos) {
    Init();
    Rec* r = Match(mapId, nameIdx, baseLabel, pos, /*claim=*/true);
    if (!r || r->label.empty()) return std::wstring();
    return r->label;
}

void SetLabel(int mapId, int16_t nameIdx, const std::wstring& baseLabel, const FVec3& pos,
              uint8_t container, uint16_t slot, const std::wstring& label) {
    // Fresh pass: this is a one-off player action, not part of a scan, and the claims left over from
    // the last pass would otherwise hide the very record it needs to update.
    BeginScan();
    Rec* r = Match(mapId, nameIdx, baseLabel, pos, /*claim=*/false);
    if (!r) {
        Rec n;
        n.mapId     = mapId;
        n.nameIdx   = nameIdx;
        n.baseLabel = baseLabel;
        n.anchor    = pos;
        n.container = container;
        n.slot      = slot;
        n.label     = label;
        g_recs.push_back(n);
    } else {
        r->label     = label;
        r->container = container;   // refreshed as diagnostics; not identity
        r->slot      = slot;
    }
    char m[224];
    snprintf(m, sizeof(m), "entity labels: map %d \"%s\" nameIdx=%d at (%.2f,%.2f,%.2f) labelled \"%s\"",
             mapId, Narrow(baseLabel).c_str(), static_cast<int>(nameIdx), pos.x, pos.y, pos.z,
             Narrow(label).c_str());
    Log::Write("NAV-DIAG", m);
    Save();   // persist immediately -- a crash must not cost the player the naming they just did
}

// `NumberFor` LIVED HERE AND IS GONE (Session 81).
//
// It allocated a fresh record whenever `Match` failed, which for an object that ROAMS was every time
// it wandered more than kAnchorDist from an anchor that is deliberately never refreshed. Nothing
// capped, evicted or cleaned up, and the free-number search counted every leaked record as taken, so
// the number could only climb. The live v2 store held **39 records labelled "Cockatrice", numbered
// 1..39, for six real animals**, and the tester heard "Cockatrice 37".
//
// Duplicate numbering now happens in EntityScan::NumberDuplicateLabels, within one scan, keyed on the
// object's own slot in the game's handle table, and is never written down. A store cannot leak
// numbers it does not hold. What remains here is what this file was always for: the player's own words.

} // namespace EntityLabels
