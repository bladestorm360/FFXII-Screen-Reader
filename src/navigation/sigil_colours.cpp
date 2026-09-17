#include "navigation/sigil_colours.h"
#include "navigation/door_binding.h"
#include "navigation/map_names.h"
#include "speech/phrasebook.h"
#include "core/logger.h"

#include <cstdio>
#include <cstring>

namespace SigilColours {
namespace {

// The routine-name prefix of every Spire Ravel Way Stone (rbl_n01: s_warp_1_*, 2_*, 6_2; rbl_n02: 3_*, 4_*,
// 5_*, 6_1, ii_*, iii_*). No other shipped script uses it.
constexpr char kWayStonePrefix[] = "s_warp_";

// The Sigil of Sacrifice glow block, by colour. See sigil_colours.h for how each block was pinned.
struct Band { int lo, hi; Phrase::Id colour; const char* name; };
constexpr Band kBands[] = {
    { 0x2F, 0x32, Phrase::Id::SigilWhite,  "White"  },   // Altar of Steel    (bit 1)
    { 0x33, 0x34, Phrase::Id::SigilYellow, "Yellow" },   // Altar of Wealth   (bit 8)
    { 0x35, 0x38, Phrase::Id::SigilPink,   "Pink"   },   // Altar of Knowledge (bit 4)
    { 0x39, 0x3C, Phrase::Id::SigilPurple, "Purple" },   // Altar of Magicks  (bit 2)
};

const Band* BandFor(int effect) {
    for (const Band& b : kBands)
        if (effect >= b.lo && effect <= b.hi) return &b;
    return nullptr;
}

} // namespace

void Apply(std::vector<EntityScan::Entity>& out) {
    // Log each colour ONCE per scene object per map: the scan runs on every rescan, and a line per rescan
    // per sigil would bury the log (log-only volume control, not speech).
    static int                 s_logMap = -1;
    static std::vector<void*>  s_logged;
    const int mapId = MapNames::CurrentMapId();
    if (mapId != s_logMap) { s_logMap = mapId; s_logged.clear(); }

    for (auto& e : out) {
        if (!e.sceneObj || !e.gameNamed || e.label.empty()) continue;
        const MapScript::RoutineFacts* f = DoorBinding::RoutineOf(e.sceneObj);
        if (!f || f->bgEffect < 0) continue;
        if (std::strncmp(f->name.c_str(), kWayStonePrefix, sizeof(kWayStonePrefix) - 1) != 0) continue;
        const Band* b = BandFor(f->bgEffect);
        if (!b) continue;                                  // a Black / Green / Red sigil or a plain Way Stone

        e.label += L", ";
        e.label += Phrase::Get(b->colour);

        bool seen = false;
        for (void* o : s_logged) if (o == e.sceneObj) { seen = true; break; }
        if (!seen && s_logged.size() < 64) {
            s_logged.push_back(e.sceneObj);
            char m[200];
            snprintf(m, sizeof(m),
                     "sigil-colour: [%u:%u] routine[%d] \"%s\" glow effect 0x%02X -> %s at (%.1f,%.1f,%.1f) map %d",
                     e.container, e.slot, f->index, f->name.c_str(), f->bgEffect, b->name,
                     e.pos.x, e.pos.y, e.pos.z, mapId);
            Log::Write("NAV-DIAG", m);
        }
    }
}

} // namespace SigilColours
