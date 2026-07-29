#include "navigation/item_scan.h"

#include "navigation/entity_classify.h"
#include "navigation/map_query.h"
#include "core/hooks.h"
#include "core/item_names.h"
#include "core/logger.h"
#include "core/mem_read.h"
#include "core/stall_probe.h"
#include "speech/phrasebook.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace ItemScan {

using EntityList::Category;
using MemRead::SafeReadU8;
using MemRead::SafeReadU32;
using MemRead::SafeReadS16;
using MemRead::SafeReadF32;

namespace {

// ---- the loot pool (abs = RVA + 0x120000) ------------------------------------------------------
//
// DAT_02ec0fa0: 10 records, stride 0x60. A record is live when +0x04 is non-zero; the value is the
// despawn state machine FUN_00319600 drives (1 = arcing in, 2-6 = blinking out, 7 = expiring), and
// every non-zero value means "there is something on the ground here".
//
// The 7-entry payload is 5 normal drop slots + 2 rare ones, matching the 7-iteration loops in both
// the roll (FUN_003180f0) and the award (FUN_00319920). (Docs/combat_system.md said 4; that was
// wrong and has been struck.)
constexpr uint32_t RVA_LOOT_POOL   = 0x2DA0FA0;
constexpr uint32_t LOOT_SLOTS      = 10;
constexpr uint32_t LOOT_STRIDE     = 0x60;
constexpr uint32_t LOOT_STATE      = 0x04;   // u32, 0 = free
constexpr uint32_t LOOT_ITEMS      = 0x20;   // 7 x { i16 itemId, i16 pad, i32 count }
constexpr uint32_t LOOT_ITEM_COUNT = 7;
constexpr uint32_t LOOT_ITEM_SIZE  = 8;

// DAT_022be7f0: the parallel position table, one 0x20-byte marker per pool slot. The engine's own
// getter (FUN_002fb310) reads exactly these two fields, and refuses the slot when the flag is clear.
constexpr uint32_t RVA_LOOT_MARKERS = 0x219E7F0;
constexpr uint32_t MARKER_STRIDE    = 0x20;
constexpr uint32_t MARKER_ALIVE     = 0x00;  // u8
constexpr uint32_t MARKER_POS       = 0x10;  // float x, y, z

// ---- the three events ---------------------------------------------------------------------------
// The pool is not in the handle table, so EntityList::OnFieldFrame's container-mask edge cannot see
// it change. These hooks ARE the event: without them a drop would only appear when the player
// happened to press rescan or flip category, which is exactly the design the project forbids.
constexpr uint32_t RVA_LOOT_SPAWN   = 0x1F9CA0;  // FUN_00319ca0(lootBuf, worldPos) -- loot hits the ground
constexpr uint32_t RVA_LOOT_TAKEN   = 0x1F9920;  // FUN_00319920(slot, picker)      -- party collected it
constexpr uint32_t RVA_LOOT_DROPPED = 0x1F97B0;  // FUN_003197b0(slot, picker)      -- discarded / expired

typedef void (*Pfn_Spawn)(void*, void*);
typedef void (*Pfn_Teardown)(void*, uint32_t);

Pfn_Spawn    s_origSpawn   = nullptr;
Pfn_Teardown s_origTaken   = nullptr;
Pfn_Teardown s_origDropped = nullptr;

std::atomic<bool> g_dirty{false};

// Labels are rendered on the GAME THREAD by the spawn hook and read by the scanner on whichever
// thread rebuilt the list, so the cache needs its own lock. It is deliberately tiny and never held
// across a game call.
std::mutex   g_labelMx;
std::wstring g_label[LOOT_SLOTS];

void* PoolSlot(uint32_t i) {
    void* base = Hooks::ResolveRva(RVA_LOOT_POOL);
    if (!base || i >= LOOT_SLOTS) return nullptr;
    return static_cast<char*>(base) + static_cast<size_t>(i) * LOOT_STRIDE;
}

void* MarkerSlot(uint32_t i) {
    void* base = Hooks::ResolveRva(RVA_LOOT_MARKERS);
    if (!base || i >= LOOT_SLOTS) return nullptr;
    return static_cast<char*>(base) + static_cast<size_t>(i) * MARKER_STRIDE;
}

bool SlotIsLive(uint32_t i) {
    uint32_t state = 0;
    return SafeReadU32(PoolSlot(i), LOOT_STATE, &state) && state != 0;
}

// GAME THREAD ONLY -- ItemNames::Resolve is a game call. One drop can hold up to seven distinct
// items, so name the first and count the rest rather than reading a list nobody asked for.
std::wstring RenderLabel(uint32_t slot) {
    void* rec = PoolSlot(slot);
    if (!rec) return std::wstring();

    std::wstring first;
    int extra = 0;
    for (uint32_t k = 0; k < LOOT_ITEM_COUNT; ++k) {
        int16_t itemId = -1;
        if (!SafeReadS16(rec, LOOT_ITEMS + k * LOOT_ITEM_SIZE, &itemId) || itemId == -1) continue;
        if (first.empty()) {
            first = ItemNames::Resolve(static_cast<uint16_t>(itemId));
            if (first.empty()) ++extra;          // unresolvable: still a thing on the ground
        } else {
            ++extra;
        }
    }
    if (first.empty()) return std::wstring();    // caller falls back to the numbered category word
    if (extra > 0) return first + L", +" + std::to_wstring(extra) + Phrase::Get(Phrase::Id::MoreSuffix);
    return first;
}

// Rebuild every slot, not just the one that just spawned. The spawn path also EVICTS the oldest
// drop when all ten are full, so a diff-the-new-slot approach would leave that eviction's stale
// label behind; a full pass is ten cheap reads and cannot drift.
void RefreshLabels() {
    std::wstring fresh[LOOT_SLOTS];
    for (uint32_t i = 0; i < LOOT_SLOTS; ++i)
        if (SlotIsLive(i)) fresh[i] = RenderLabel(i);

    std::lock_guard<std::mutex> lk(g_labelMx);
    for (uint32_t i = 0; i < LOOT_SLOTS; ++i) g_label[i] = fresh[i];
}

void HookedSpawn(void* lootBuf, void* worldPos) {
    STALL_SCOPE("ItemScan::HookedSpawn");
    // Let the game place the drop first: the pool record and its marker are both written inside.
    s_origSpawn(lootBuf, worldPos);
    RefreshLabels();
    g_dirty.store(true, std::memory_order_release);
}

// Both teardown paths only have to invalidate the list. The scanner keys on the slot's live flag,
// so a stale label on a freed slot is never read, and the next spawn rewrites it anyway.
void HookedTaken(void* slot, uint32_t picker) {
    STALL_SCOPE("ItemScan::HookedTaken");
    s_origTaken(slot, picker);
    g_dirty.store(true, std::memory_order_release);
}

void HookedDropped(void* slot, uint32_t picker) {
    STALL_SCOPE("ItemScan::HookedDropped");
    s_origDropped(slot, picker);
    g_dirty.store(true, std::memory_order_release);
}

} // namespace

// ================================================================================================

bool Init() {
    bool ok = Hooks::InstallTyped(RVA_LOOT_SPAWN,   &HookedSpawn,   &s_origSpawn);
    ok     &= Hooks::InstallTyped(RVA_LOOT_TAKEN,   &HookedTaken,   &s_origTaken);
    ok     &= Hooks::InstallTyped(RVA_LOOT_DROPPED, &HookedDropped, &s_origDropped);
    Log::Write("NAV", ok
        ? "ItemScan: installed (FUN_00319ca0 spawn, FUN_00319920 collected, FUN_003197b0 discarded)"
        : "ItemScan: a loot hook FAILED to install -- the Items category will stay empty");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_LOOT_DROPPED);
    Hooks::Uninstall(RVA_LOOT_TAKEN);
    Hooks::Uninstall(RVA_LOOT_SPAWN);
    std::lock_guard<std::mutex> lk(g_labelMx);
    for (auto& s : g_label) s.clear();
}

bool TakeDirty() { return g_dirty.exchange(false, std::memory_order_acq_rel); }

void ScanDrops(std::vector<EntityScan::Entity>& out) {
    int found = 0;
    for (uint32_t i = 0; i < LOOT_SLOTS; ++i) {
        if (!SlotIsLive(i)) continue;

        // Position lives in the marker table, not the record. Its own flag is the engine's validity
        // test (FUN_002fb310 returns 0 without it), so a record whose marker is not up yet is simply
        // not placed and must not be listed at a garbage coordinate.
        void* mk = MarkerSlot(i);
        uint8_t alive = 0;
        if (!SafeReadU8(mk, MARKER_ALIVE, &alive) || alive == 0) continue;

        FVec3 pos;
        if (!SafeReadF32(mk, MARKER_POS + 0, &pos.x) ||
            !SafeReadF32(mk, MARKER_POS + 4, &pos.y) ||
            !SafeReadF32(mk, MARKER_POS + 8, &pos.z)) continue;
        // Same unplaced-slot guard the handle-table walk uses: an exact origin is a slot that was
        // allocated but never positioned, not a drop lying at the centre of the world.
        if (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f) continue;

        EntityScan::Entity e;
        e.sceneObj = nullptr;
        e.fixed    = true;         // a drop does not move once it has landed -- no +0xB8 to refresh
        e.flags    = 0;
        e.kind     = 0;
        // Cursor identity for an entry with no scene node. The -(2000 + slot) band cannot collide
        // with exit_scan.cpp's -(1000 + ctrlIndex), so focus stays locked on the drop you are
        // walking to even as other slots come and go.
        e.nameIdx  = static_cast<int16_t>(-(2000 + static_cast<int>(i)));
        e.pos      = pos;
        e.category = Category::Items;
        e.available = true;        // nothing story-gates a drop; F5 must never hide one

        // Stand it on the floor, exactly as the exit scan does: the planner measures elevation
        // against this point, and the marker sits where the arc ended rather than on the walkmap.
        float floorY = 0.0f;
        if (MapQuery::HasWorld() && MapQuery::GroundAt(e.pos.x, e.pos.z, floorY)) e.pos.y = floorY;

        {
            std::lock_guard<std::mutex> lk(g_labelMx);
            e.label = g_label[i];
        }
        // No name resolved -> let the shared numbering turn it into "Items 1", "Items 2". Never
        // invent one.
        e.gameNamed = !e.label.empty();
        if (e.label.empty()) e.label = EntityScan::CategoryWord(Category::Items);

        out.push_back(e);
        ++found;
    }

    if (found > 0) {
        char m[96];
        snprintf(m, sizeof(m), "loot: %d ground drop(s) listed", found);
        Log::Write("NAV-DIAG", m);
    }
}

} // namespace ItemScan
