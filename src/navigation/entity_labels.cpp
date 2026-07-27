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
constexpr int kFormatVersion = 2;

// How near a live object must be to a record's stored anchor for them to be the same object -- used
// ONLY to break a tie between records that share `{mapId, baseLabel, nameIdx}`. Generous on purpose:
// it separates distinct NPCs (metres apart) rather than tracking one precisely, and a stationary NPC
// re-spawns within centimetres.
constexpr float kAnchorDist = 1.5f;

struct Rec {
    int          mapId    = 0;
    int16_t      nameIdx  = 0;
    std::wstring baseLabel;      // the game name (or category word) the number was assigned under
    FVec3        anchor{};       // where this object stood when the record was created
    int          number   = 0;   // 0 = none assigned
    std::wstring label;          // the player's own words; empty = not named
    // Diagnostics only. STRUCK as identity in Session 79 -- see the header.
    uint8_t      container = 0xFF;
    uint16_t     slot      = 0xFFFF;
    // Which matching pass last handed this record out, so two live objects sharing a key cannot both
    // be given it (and therefore the same number). Never persisted.
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
    fwprintf(f, L"# <mapId> <nameIdx> <number> <x> <y> <z> <container> <slot> | <baseLabel> = <your label>\n");
    fwprintf(f, L"# Edit freely; reloaded on every area change. Clear a label by emptying it.\n");
    fwprintf(f, L"# container/slot are diagnostics only -- the identity is mapId + baseLabel + nameIdx,\n");
    fwprintf(f, L"# with the position breaking ties between objects that share all three.\n");
    for (const auto& r : g_recs) {
        fwprintf(f, L"%d %d %d %.2f %.2f %.2f %u %u | %s = %s\n", r.mapId,
                 static_cast<int>(r.nameIdx), r.number, r.anchor.x, r.anchor.y, r.anchor.z,
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
        char m[128];
        snprintf(m, sizeof(m), "entity labels: store is version %d, this build wants %d -- discarded",
                 ver, kFormatVersion);
        Log::Write("NAV-DIAG", m);
        return;
    }
    while (fgetws(line, 512, f)) {
        if (line[0] == L'#' || line[0] == L'\n') continue;
        Rec r;
        int   mp = 0, ni = 0, num = 0, ct = 0, sl = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (swscanf_s(line, L"%d %d %d %f %f %f %d %d", &mp, &ni, &num, &x, &y, &z, &ct, &sl) != 8)
            continue;
        const wchar_t* bar = wcschr(line, L'|');
        if (!bar) continue;
        const wchar_t* eq = wcschr(bar, L'=');
        if (!eq) continue;
        r.mapId     = mp;
        r.nameIdx   = static_cast<int16_t>(ni);
        r.number    = num;
        r.anchor    = FVec3{ x, y, z };
        r.container = static_cast<uint8_t>(ct);
        r.slot      = static_cast<uint16_t>(sl);
        r.baseLabel = Trim(bar + 1, eq);
        if (r.baseLabel == L"-") r.baseLabel.clear();
        r.label = Trim(eq + 1, eq + wcslen(eq));
        g_recs.push_back(r);
    }
    fclose(f);

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

int NumberFor(int mapId, int16_t nameIdx, const std::wstring& baseLabel, const FVec3& pos,
              uint8_t container, uint16_t slot) {
    Init();
    Rec* r = Match(mapId, nameIdx, baseLabel, pos, /*claim=*/true);
    if (r && r->number > 0) return r->number;

    // Lowest number not yet handed out under this name on this map. Small and speakable, and it never
    // moves once assigned -- which is the entire point.
    //
    // `&o == r` SKIPS THE RECORD BEING RENUMBERED. Without it a record already holding 2 saw its own 2
    // as taken and moved to 3, so numbers could only ever creep upward and a vacated one was never
    // reclaimed -- which is how map 243 ended up with Nomads 1, 3, 5, 6 and 7.
    int n = 1;
    for (bool taken = true; taken; ++n) {
        taken = false;
        for (const auto& o : g_recs) {
            if (&o == r) continue;
            if (o.mapId == mapId && o.number == n && o.baseLabel == baseLabel) { taken = true; break; }
        }
        if (!taken) break;
    }

    if (!r) {
        Rec nr;
        nr.mapId     = mapId;
        nr.nameIdx   = nameIdx;
        nr.baseLabel = baseLabel;
        nr.anchor    = pos;
        nr.number    = n;
        nr.container = container;
        nr.slot      = slot;
        nr.claim     = g_scanSeq;
        g_recs.push_back(nr);
    } else {
        r->number    = n;
        r->container = container;
        r->slot      = slot;
    }
    Save();
    return n;
}

} // namespace EntityLabels
