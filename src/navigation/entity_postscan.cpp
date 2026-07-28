#include "navigation/entity_scan.h"
#include "navigation/entity_classify.h"
#include "navigation/entity_list_internal.h"
#include "navigation/nav_rva.h"
#include "navigation/nav_mesh.h"
#include "navigation/nav_reach.h"
#include "navigation/nav_common.h"
#include "navigation/map_exits.h"
#include "navigation/map_names.h"
#include "navigation/entity_labels.h"
#include "navigation/exit_scan.h"
#include "core/mem_read.h"
#include "core/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace MemRead;

// POST-SCAN PASSES: everything that runs over the finished object list rather than finding objects.
// Split out of entity_scan.cpp in Session 77 when that file passed the project's 500-line ceiling.
//
// The two dedupe passes here hold OPPOSITE policies on the same fact, and the difference is the whole
// point: FFXII reuses display names constantly (109 npcdic ids all read "Rabanastran"), so two objects
// sharing a name is normal. NumberDuplicateLabels therefore NUMBERS them. TagDoorwaysAndDropSignTwins
// DELETES one -- and is allowed to do so only in the narrow case it was written for, a shop sign that
// repeats its own doorway. Getting that scope wrong deleted four NPCs, one of them story-critical.
namespace EntityScan {

using EntityList::Category;

uint16_t ObjectHandle(void* sceneObj) {
    uint16_t h = 0xFFFF;
    if (sceneObj) SafeReadU16(sceneObj, 0, &h);
    return h;
}

// One line per handle-table object, once per map (see the detail latch in BuildLocked). This is the dump
// that answers "what IS that thing" for an object the game gives no name to: the container slot says
// whether the engine even considers it interactable, `act`/`talk` are the payloads it would run, and the
// nearest named neighbour usually identifies it outright -- North End's anonymous `Interactables 2` sits
// at the same distance and elevation as The Clan Hall, which is what marked it as that doorway's twin.
void LogObjectDump(const std::vector<Entity>& out) {
    int shown = 0;
    for (const auto& e : out) {
        if (!e.sceneObj) continue;
        // Nearest OTHER object the game did name, so an anonymous entry is placed against a landmark.
        // (Not `near` -- windows.h still defines that as a legacy empty macro.)
        const Entity* landmark = nullptr;
        float nearD = 1e9f;
        for (const auto& o : out) {
            if (&o == &e || !o.gameNamed) continue;
            const float d = NavCommon::Distance2D(o.pos, e.pos);
            if (d < nearD) { nearD = d; landmark = &o; }
        }
        char lbl[48] = {};
        for (size_t k = 0; k < e.label.size() && k < 47; ++k)
            lbl[k] = (e.label[k] < 128) ? static_cast<char>(e.label[k]) : '?';
        char nb[64] = "-";
        if (landmark) {
            char n8[40] = {};
            for (size_t k = 0; k < landmark->label.size() && k < 39; ++k)
                n8[k] = (landmark->label[k] < 128) ? static_cast<char>(landmark->label[k]) : '?';
            snprintf(nb, sizeof(nb), "\"%s\" %.1fm", n8, nearD);
        }
        // `en` = the engine's own interactivity switch (+0x0E & 0x10), read live. It is in this dump
        // because Session 82 needed it and it was the one field the dump did not carry: nineteen bare
        // "NPC n" entries could not be told from real story NPCs without it, and the answer had to wait
        // for a `'` dump that was never taken on that map.
        uint8_t enByte = 0;
        MemRead::SafeReadU8(e.sceneObj, NavRva::SCENEOBJ_ENABLE_OFF, &enByte);
        char m[312];
        snprintf(m, sizeof(m),
                 "obj [%u:%u] kind=%u en=%d flags=%08X nameIdx=%d act=%u talk=%u door=%d named=%d avail=%d cat=%d pos=(%.2f,%.2f,%.2f) \"%s\" near=%s",
                 e.container, e.slot, e.kind, (enByte & NavRva::INTERACT_ENABLE_BIT) ? 1 : 0,
                 e.flags, e.nameIdx, e.actionId, e.talkId,
                 e.doorway ? 1 : 0, e.gameNamed ? 1 : 0, e.available ? 1 : 0,
                 static_cast<int>(e.category), e.pos.x, e.pos.y, e.pos.z, lbl, nb);
        Log::Write("NAV-DIAG", m);
        if (++shown >= 128) { Log::Write("NAV-DIAG", "obj dump: capped at 128"); break; }
    }
}

// SHADOW REGISTRATIONS -- an unnamed character standing at EXACTLY a named object's coordinates.
//
// Session 79's include-by-KIND widening admits characters the scan used to reject, and a few of them
// are not people at all but a second registration of somebody already listed. Nomad Village's [0:39]
// sits at (53.80, 0.00, 38.80) -- the identical coordinates of [0:38] "Nomad". Two bodies cannot
// occupy one point, so an unnamed character sharing a named object's exact position is bookkeeping.
//
// EXACT ONLY, and that is the whole design (kStackedDist = 5 cm). The same map has real, distinct
// NPCs 0.62 m and 1.22 m from a named neighbour -- and the woman this entire pass exists to surface
// stands 1.28 m from the Nomad Elder. Any radius loose enough to feel like "nearby" deletes her. This
// filter already erased a story-critical NPC once (Session 77) by matching on name with no proximity
// test at all, so it stays as tight as the arithmetic allows.
//
// Collection dedup, the AlreadyListed category: it removes a duplicate ENTRY, never a repeat
// announcement.
void DropShadowRegistrations(std::vector<Entity>& out) {
    for (size_t i = 0; i < out.size();) {
        const Entity& cur = out[i];
        if (cur.gameNamed || !cur.sceneObj || cur.category != EntityList::Category::NPC) {
            ++i;
            continue;
        }
        int anchor = -1;
        for (size_t j = 0; j < out.size(); ++j) {
            if (j == i || !out[j].gameNamed || !out[j].sceneObj)              continue;
            if (NavCommon::Distance2D(out[j].pos, cur.pos) > kStackedDist)    continue;
            if (std::fabs(out[j].pos.y - cur.pos.y)        > kStackedDist)    continue;
            anchor = static_cast<int>(j);
            break;
        }
        if (anchor < 0) { ++i; continue; }

        // ALWAYS logged. A deletion the player cannot see must never be one the log cannot show.
        char n8[64] = {};
        for (size_t k = 0; k < out[anchor].label.size() && k < 63; ++k)
            n8[k] = (out[anchor].label[k] < 128) ? static_cast<char>(out[anchor].label[k]) : '?';
        char m[240];
        snprintf(m, sizeof(m),
                 "shadow dropped: unnamed [%u:%u] nameIdx=%d at (%.2f,%.2f,%.2f) is stacked on "
                 "\"%s\" [%u:%u] -- duplicate registration",
                 cur.container, cur.slot, cur.nameIdx, cur.pos.x, cur.pos.y, cur.pos.z, n8,
                 out[anchor].container, out[anchor].slot);
        Log::Write("NAV-DIAG", m);
        NoteFiltered(cur.sceneObj);   // so the caller's grace window cannot carry it straight back in
        out.erase(out.begin() + static_cast<long long>(i));
    }
}

// STRUCK (this session) -- `DropUnplacedCharacters`, the placement + reachability filter.
//
// It dropped an unnamed `Category::NPC` that floated more than `kFloatingDrop` above its own floor,
// or that fell outside `NavReach`'s component. It was the FOURTH attempt at one complaint ("a lot of
// extraneous NPC1, NPC2 etc entries"), after three that guessed what those objects ARE -- party
// members, roster bodies, bodies the story had not switched on -- each refuted in play.
//
// Deleted for two reasons, and the second is the one that matters:
//
//  1. Its candidate set is now empty by construction. BuildLocked no longer admits an object the game
//     does not name, so there is no unnamed NPC left for it to judge.
//
//  2. IT ONLY EVER WORKED ON ONE MAP, and that was luck rather than scope. Every map carries three
//     unnamed bodies stacked on one authored coordinate. On Nomad Village that coordinate is 6.06 m
//     up, so the floating test caught them and the map came out clean. On Lowtown, Eastgate and
//     Garamsythe the identical three stand ON the floor and inside the reachable set -- so the very
//     same code looked straight at them and kept them. The code was never map-specific; its EFFECT
//     was, which is what a threshold read off a single map's dump will always be.
//
// The thresholds went with it (`kFloatingDrop`, `kNpcReachTol`, entity_scan.h). `NavReach` stays:
// exit_scan.cpp filters exits with it, where the question genuinely is about position.
//
// LESSON, and it is the fourth session in a row to pay for it: when a filter keeps needing new
// evidence to justify itself, check whether the thing it filters should be in the list at all.

// The category-word fallback -- "NPC", "Interactables", "Sign" -- for anything the game named.
//
// IT RUNS HERE, AFTER TagDoorwaysAndDropSignTwins, AND THAT IS THE POINT. These two lines used to sit
// inline in BuildLocked, and the "Sign" half was DEAD CODE from the day it was written: `doorway` is
// assigned by a pass over the FINISHED list, so on a freshly-built entity it is always false. The word
// the tester authorised specifically -- for the North End sign the game itself shows as "???" /
// "(You're not sure what this sign is for.)" -- could never once be spoken, and every unnamed doorway
// said "Interactables" instead. The logic is unchanged; only the point at which it runs.
void ApplyFallbackLabels(std::vector<Entity>& out) {
    for (auto& e : out) {
        if (!e.label.empty()) continue;
        // An unnamed object carrying a `+0x70` field-sign record IS a sign: the map script bound it
        // with `setfieldsignlocationjumpinfo`, which is what `doorway` records. Shop doorways carry
        // the same record but resolve a real name, so they never reach here.
        e.label = e.doorway ? std::wstring(L"Sign") : std::wstring(CategoryWord(e.category));
    }
}


// Mark every object the map script bound to a location jump, then drop the same-named twins that were
// NOT bound to one.
//
// WHAT THE TWINS ACTUALLY ARE (East End, `'` dump 2026-07-23): each shop appears twice, and the two
// objects are indistinguishable by every field the engine exposes -- same scene category (0x21), same
// kind (4), same enable bit, same flags (0x2134, ACTION + the on-screen name-draw bit), and BOTH sit
// inside the container's action span [19,35), the exact range FUN_0025b820 walks to decide what the
// player is near. They are 6-15 m apart, so no proximity radius separates them either. Both are genuine
// field signs: `nameIdx = -1` means the name comes from `sceneObj+0xf8`, the string the map script writes
// with `fieldsignmes` / `fieldsignmesbyid`.
//
// The one thing that DOES separate them is `setfieldsignlocationjumpinfo` -- the script native that binds
// a sign to a map transition. A bound sign has a `+0x70` record; an unbound one does not. Every doorway
// matched a record inside ~2 m; not one of its twins matched at all.
//
// So the twin is a sign that only repeats the doorway's name, and it earns no place in the list. A sign
// carrying ANY other text (a slogan, a notice) has a different label and survives untouched -- the test
// is exact string equality on the game's own words, so nothing is invented and nothing is paraphrased.
// This is collection dedup, the same category as AlreadyListed; it never suppresses a repeat announcement.
void TagDoorwaysAndDropSignTwins(std::vector<Entity>& out, bool logDetail) {
    const std::vector<MapExits::SignRec>& signs = CachedSigns();
    if (signs.empty()) return;                 // no field-sign table on this map -> nothing to judge with

    // Tag doorways -- but NEVER a person. A character who happens to stand within 2.5 m of a shop
    // sign is not a doorway, and tagging one made it an ANCHOR that then deleted every same-named
    // NPC on the map (Session 77: four "Nomad" NPCs, 17-30 m apart, one of them story-critical).
    for (auto& e : out) {
        if (!e.sceneObj) continue;             // fixed exits have no scene node
        if (e.category == EntityList::Category::NPC) continue;
        for (const auto& s : signs) {
            if (NavCommon::Distance2D(s.pos, e.pos) <= kSignObjectDist) { e.doorway = true; break; }
        }
    }

    for (size_t i = 0; i < out.size();) {
        const Entity& cur = out[i];
        if (!cur.gameNamed || !cur.sceneObj) { ++i; continue; }

        int         twin = -1;
        const char* why  = "";

        if (cur.category == EntityList::Category::NPC) {
            // PEOPLE ARE NEVER DOORWAY TWINS. The only duplicate worth removing is a double
            // REGISTRATION: two actors at literally the same coordinates. Anything else is two real
            // people the game gave one name -- which is normal, and is precisely what
            // NumberDuplicateLabels exists to disambiguate ("109 npcdic ids all read Rabanastran").
            //
            // `j < i` so the FIRST of a stacked pair survives; without it both would see each other
            // as a twin and both would be erased.
            for (size_t j = 0; j < i; ++j) {
                if (out[j].category != EntityList::Category::NPC) continue;
                if (!out[j].gameNamed || out[j].label != cur.label) continue;
                if (NavCommon::Distance2D(out[j].pos, cur.pos) > kStackedDist) continue;
                if (std::fabs(out[j].pos.y - cur.pos.y) > kStackedDist)        continue;
                twin = static_cast<int>(j);
                why  = "stacked duplicate registration";
                break;
            }
        } else {
            // INTERACTABLES: the case this filter was written for. A shop SIGN and the shop DOORWAY
            // are two interactables the game gave the same name, and only one carries the location
            // jump (`Entity::doorway`). Drop the one WITHOUT the transition point; keep the one with.
            if (cur.doorway) { ++i; continue; }
            for (size_t j = 0; j < out.size(); ++j) {
                if (j == i) continue;
                if (out[j].category == EntityList::Category::NPC) continue;
                if (out[j].doorway && out[j].gameNamed && out[j].label == cur.label) {
                    twin = static_cast<int>(j);
                    why  = "sign repeats a doorway";
                    break;
                }
            }
        }

        if (twin < 0) { ++i; continue; }

        // ALWAYS logged, not just on a new population high-water mark. This filter deleted a
        // story-critical NPC once and did it silently on every rescan after the first.
        char n8[64] = {};
        for (size_t k = 0; k < cur.label.size() && k < 63; ++k)
            n8[k] = (cur.label[k] < 128) ? static_cast<char>(cur.label[k]) : '?';
        char m[288];
        snprintf(m, sizeof(m),
                 "twin dropped (%s) \"%s\": [%u:%u] (%.2f,%.2f,%.2f) <- keeping [%u:%u] "
                 "(%.2f,%.2f,%.2f) %.2fm away",
                 why, n8, cur.container, cur.slot, cur.pos.x, cur.pos.y, cur.pos.z,
                 out[twin].container, out[twin].slot,
                 out[twin].pos.x, out[twin].pos.y, out[twin].pos.z,
                 NavCommon::Distance2D(cur.pos, out[twin].pos));
        Log::Write("NAV-DIAG", m);
        // THE SAME GRACE-WINDOW BUG SESSION 83 FIXED IN THE OTHER TWO PASSES, still live here.
        // RescanLocked carries an entity over when its scene object is missing from the fresh list,
        // and it cannot tell "the engine stopped reporting it" from "we just deleted it" -- a
        // filtered object is a LIVE engine object whose transform keeps reading, so `lastSeenMs`
        // keeps being refreshed and it never ages out. Without this the twin came straight back,
        // permanently, while this pass logged the deletion on every single rescan.
        NoteFiltered(cur.sceneObj);
        out.erase(out.begin() + static_cast<long long>(i));
    }
    (void)logDetail;   // drops are unconditional now; the flag remains for the caller's signature
}

// Append " 1", " 2", ... to labels that occur more than once, so fifteen identically-named townsfolk
// become addressable. Labels that occur once are untouched: nothing gains a number it does not need.
//
// THE NUMBER IS ASSIGNED ONCE AND KEPT (Session 65). It used to be an ordinal within whatever group the
// current scan happened to see, which meant it moved constantly: the handle table streams objects in
// and out (measured NPC=14 <-> 15 across 118 rescans on one map) and every cycle keypress rebuilds the
// list, so one flickering NPC renumbered everyone after it. The tester's "Rabanastran 7" kept becoming
// "Rabanastran 5". EntityLabels now hands out the lowest number not yet used under that name on that
// map and remembers it -- across rescans, streaming, reloads and sessions.
//
// This is NOT a fabricated label (see the no-invented-UI-text rule): the words are still the game's
// own string; only a counting suffix is added, in the same spirit as the step counts already spoken.
// The player's own name for an entity outranks everything the mod would otherwise say -- the game's
// string, the duplicate number, the category word. Applied after all of those are settled and BEFORE
// duplicate numbering, so a labelled entity leaves its old counting group entirely: name the gate guard
// and the remaining Rabanastrans keep the numbers they already had.
// IDEMPOTENT ON PURPOSE. This pass and the numbering below now run in RescanLocked over a list that
// can contain grace-window survivors -- entities carried over from the previous scan, which already
// went through both passes and are therefore already suffixed. Re-running must not append a second
// suffix or freeze a suffixed label as the identity words.
void ApplyPlayerLabels(std::vector<Entity>& out) {
    const int mapId = MapNames::CurrentMapId();
    EntityLabels::BeginScan();   // one record per live object for the duration of this pass
    for (auto& e : out) {
        if (!e.sceneObj) continue;                      // fixed exits are named from the map script
        // Freeze the identity words BEFORE anything appends a number to them. Every later lookup keys
        // on this, so `label` and `baseLabel` must not be allowed to diverge. Only ONCE: a carried
        // entity already has its `baseLabel`, and overwriting it with the suffixed `label` would make
        // the identity drift a number further every scan.
        if (e.baseLabel.empty()) e.baseLabel = e.label;
        // Strip whatever suffix a previous pass appended, so this pass starts from the game's words.
        e.label = e.baseLabel;
        std::wstring custom = EntityLabels::LabelFor(mapId, e.nameIdx, e.baseLabel, e.pos);
        if (custom.empty()) continue;
        e.label     = custom;
        e.gameNamed = true;   // it is a real name for list purposes: never a category-word fallback
    }
}

// NUMBERS ARE WORKED OUT HERE AND NOWHERE ELSE, fresh on every scan (Session 81).
//
// They used to come from the persistent store, so that a number "never moved". For stationary
// objects that held; for anything that ROAMS it was unbounded growth -- the store's anchor is never
// refreshed, so a moving object outran its own record every scan, minted a new one, and the
// free-number search counted every leaked record as taken. Six Cockatrices produced 39 records
// numbered 1..39 and the tester heard "Cockatrice 37".
//
// The key is `{container, slot}` -- the object's position in the game's OWN handle table, which is
// the pair the engine itself uses to name an interaction target. It is stable for exactly as long as
// the map is loaded, which is exactly as long as a number computed this way needs to hold. Numbers
// may differ after leaving and re-entering a map; for objects that share one npcdic id and roam, the
// game's data contains no identity that would survive that, and inventing one is what leaked.
void NumberDuplicateLabels(std::vector<Entity>& out, bool logDetail) {
    // The old loop relied on `label += " 2"` mutating the field it grouped on to stop a group being
    // re-scanned as fresh groups. Numbering from a reset `baseLabel` removes that side effect, so
    // membership is tracked explicitly.
    std::vector<bool> done(out.size(), false);
    for (size_t i = 0; i < out.size(); ++i) {
        if (done[i] || out[i].label.empty()) continue;
        std::vector<size_t> same;
        for (size_t j = i; j < out.size(); ++j)
            if (!done[j] && out[j].label == out[i].label) same.push_back(j);
        if (same.size() < 2) continue;   // unique label -> speak it as the game wrote it
        // {container, slot} first -- the game's own identity for a handle-table object. Entities from
        // the other three sources leave both at their sentinels (0xFF / 0xFFFF) and sort after, each
        // separated by its own stable discriminator: exits and ground drops encode a controller index
        // or pool slot in `nameIdx`, and combatants (all nameIdx -1) fall to the scene-object handle,
        // which entity_scan.cpp documents as stable while the map is loaded. The final index compare
        // makes the order total, never arbitrary: each scanner appends in its own pool order.
        std::sort(same.begin(), same.end(), [&out](size_t a, size_t b) {
            const Entity& x = out[a];
            const Entity& y = out[b];
            if (x.container != y.container) return x.container < y.container;
            if (x.slot      != y.slot)      return x.slot      < y.slot;
            if (x.nameIdx   != y.nameIdx)   return x.nameIdx   < y.nameIdx;
            const uint16_t hx = ObjectHandle(x.sceneObj), hy = ObjectHandle(y.sceneObj);
            if (hx != hy) return hx < hy;
            return a < b;
        });
        // OPEN BUG (reported in play): every shop appears twice. Numbering makes the twins
        // addressable but does NOT explain them, so dump each duplicate group ONCE PER MAP with the
        // fields that tell twins apart from genuinely distinct objects: pointer, slot, kind, scene
        // category, flags, position. Two entries at the SAME position with different pointers are a
        // shadow/duplicate registration; two at different positions are two real objects that share
        // the game's own name (which is normal -- 109 npcdic ids all read "Rabanastran").
        // Cross-reference the slot against the container's grp0/grp1 spans logged by the ` dump.
        if (logDetail) {
            char m[128];
            char n8[64] = {};
            for (size_t k = 0; k < out[i].label.size() && k < 63; ++k)
                n8[k] = (out[i].label[k] < 128) ? static_cast<char>(out[i].label[k]) : '?';
            snprintf(m, sizeof(m), "dup-label \"%s\" x%zu:", n8, same.size());
            Log::Write("NAV-DIAG", m);
            int seq = 0;
            for (size_t idx : same) {
                const Entity& e = out[idx];
                char l[240];
                snprintf(l, sizeof(l),
                         "    -> %d  obj=%p [%u:%u] handle=%u kind=%u nameIdx=%d flags=%08X avail=%d "
                         "pos=(%.2f,%.2f,%.2f)",
                         ++seq, e.sceneObj, e.container, e.slot, ObjectHandle(e.sceneObj), e.kind,
                         e.nameIdx, e.flags, e.available ? 1 : 0, e.pos.x, e.pos.y, e.pos.z);
                Log::Write("NAV-DIAG", l);
            }
        }
        // `baseLabel` is the un-suffixed words. ApplyPlayerLabels froze it, but a fixed exit never goes
        // through that pass, so fall back to the current label before any suffix is appended.
        for (size_t idx : same)
            if (out[idx].baseLabel.empty()) out[idx].baseLabel = out[idx].label;

        // A dense rank over the group, in the order settled above. The old code had to ask whether the
        // GAME's words or the PLAYER's had formed the group, because the store scoped its counters by
        // the game name and a player-formed group would draw from two counters and get "1" twice. One
        // counter per group cannot do that, so both branches collapsed into this and the distinction
        // is gone.
        int n = 0;
        for (size_t idx : same) {
            out[idx].label += L" " + std::to_wstring(++n);
            done[idx] = true;
        }
    }
}

// Rebuild the set from the scene-object HANDLE TABLE — the game's own registry of live
// interactive field objects (DAT_02098e10, 5 containers), populated at map load and
// walked every frame by FUN_0025b820 to decide what the player is near. We list every
// live talk/action object with a readable position: NPCs (FLAG_TALK) and action gimmicks
// like gates/doors/switches/treasure/crystals (FLAG_ACTION) — the latter are present from
// load, not spawned on approach. Positions come from the scene object's transform
// (sceneObj+0xB8), the same chain the leader uses. Caller holds g_mutex.

} // namespace EntityScan
