#include "navigation/entity_labels.h"
#include "core/logger.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace EntityLabels {

namespace {

// Bump when the IDENTITY KEY changes. The store outlives the build that wrote it, and a key scheme
// change would silently attach one person's name to another; a mismatch discards the file instead.
constexpr int kFormatVersion = 1;

struct Rec {
    int          mapId;
    uint8_t      container;
    uint16_t     slot;
    int16_t      nameIdx;      // validation: a reused slot shows up as a mismatch, not a wrong name
    int          number;       // 0 = none assigned
    std::wstring baseLabel;    // the game name the number was assigned under (so counters stay per-name)
    std::wstring label;        // the player's own words; empty = not named
};

std::vector<Rec> g_recs;
bool             g_loaded = false;

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

Rec* Find(int mapId, uint8_t container, uint16_t slot) {
    for (auto& r : g_recs)
        if (r.mapId == mapId && r.container == container && r.slot == slot) return &r;
    return nullptr;
}

// A record whose stored npcdic id no longer matches the live object is describing somebody else: the
// slot was reused. Report it once and treat the entry as absent -- a wrong name is worse than none.
bool Stale(const Rec& r, int16_t nameIdx) {
    if (r.nameIdx == nameIdx) return false;
    char m[176];
    snprintf(m, sizeof(m),
             "entity labels: map %d [%u:%u] stored nameIdx=%d but live object is %d -- slot reused, entry ignored",
             r.mapId, r.container, r.slot, r.nameIdx, nameIdx);
    Log::Write("NAV-DIAG", m);
    return true;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back((c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?');
    return s;
}

void Save() {
    const std::wstring path = StorePath(/*createDir=*/true);
    if (path.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8") != 0 || !f) return;
    fwprintf(f, L"version %d\n", kFormatVersion);
    fwprintf(f, L"# <mapId> <container> <slot> <nameIdx> <number> <baseLabel> = <your label>\n");
    fwprintf(f, L"# Edit freely; reloaded on every area change. Clear a label by emptying it.\n");
    for (const auto& r : g_recs) {
        fwprintf(f, L"%d %u %u %d %d %s = %s\n", r.mapId, static_cast<unsigned>(r.container),
                 static_cast<unsigned>(r.slot), static_cast<int>(r.nameIdx), r.number,
                 r.baseLabel.empty() ? L"-" : r.baseLabel.c_str(), r.label.c_str());
    }
    fclose(f);
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
        int mp = 0, ct = 0, sl = 0, ni = 0, num = 0;
        wchar_t base[128] = {};
        // The label runs to end-of-line (spaces and all), so it is separated by " = " and read after
        // the fixed fields rather than tokenised.
        if (swscanf_s(line, L"%d %d %d %d %d %127s", &mp, &ct, &sl, &ni, &num,
                      base, static_cast<unsigned>(_countof(base))) != 6) continue;
        std::wstring label;
        if (const wchar_t* eq = wcschr(line, L'=')) {
            const wchar_t* p = eq + 1;
            while (*p == L' ') ++p;
            label = p;
            while (!label.empty() && (label.back() == L'\n' || label.back() == L'\r' ||
                                      label.back() == L' ')) label.pop_back();
        }
        std::wstring bl = base;
        if (bl == L"-") bl.clear();
        g_recs.push_back(Rec{ mp, static_cast<uint8_t>(ct), static_cast<uint16_t>(sl),
                              static_cast<int16_t>(ni), num, bl, label });
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

std::wstring LabelFor(int mapId, uint8_t container, uint16_t slot, int16_t nameIdx) {
    Init();
    Rec* r = Find(mapId, container, slot);
    if (!r || r->label.empty() || Stale(*r, nameIdx)) return std::wstring();
    return r->label;
}

void SetLabel(int mapId, uint8_t container, uint16_t slot, int16_t nameIdx,
              const std::wstring& label) {
    Init();
    Rec* r = Find(mapId, container, slot);
    if (!r) {
        g_recs.push_back(Rec{ mapId, container, slot, nameIdx, 0, std::wstring(), label });
        r = &g_recs.back();
    } else {
        r->nameIdx = nameIdx;   // the player just pointed at it, so this IS the object that key means
        r->label   = label;
    }
    char m[192];
    snprintf(m, sizeof(m), "entity labels: map %d [%u:%u] labelled \"%s\"",
             mapId, container, slot, Narrow(label).c_str());
    Log::Write("NAV-DIAG", m);
    Save();   // persist immediately -- a crash must not cost the player the naming they just did
}

int NumberFor(int mapId, uint8_t container, uint16_t slot, int16_t nameIdx,
              const std::wstring& baseLabel) {
    Init();
    Rec* r = Find(mapId, container, slot);
    if (r && r->number > 0 && !Stale(*r, nameIdx) && r->baseLabel == baseLabel) return r->number;

    // Lowest number not yet handed out under this name on this map. Small and speakable, and it never
    // moves once assigned -- which is the entire point.
    int n = 1;
    for (bool taken = true; taken; ++n) {
        taken = false;
        for (const auto& o : g_recs)
            if (o.mapId == mapId && o.number == n && o.baseLabel == baseLabel) { taken = true; break; }
        if (!taken) break;
    }

    if (!r) {
        g_recs.push_back(Rec{ mapId, container, slot, nameIdx, n, baseLabel, std::wstring() });
    } else {
        r->nameIdx   = nameIdx;
        r->number    = n;
        r->baseLabel = baseLabel;
    }
    Save();
    return n;
}

} // namespace EntityLabels
