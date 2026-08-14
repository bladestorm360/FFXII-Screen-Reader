#include "ui/battle_target_reader.h"
#include "ui/mod_menu.h"               // AutoDetailOn (whether the Libra detail volunteers itself)
#include "ui/text_capture.h"           // ResolveStringById -- THE message-id resolver, game thread only
#include "ui/ingame_menu_reader.h"     // BattleCommandActive -- log-only, for the `o` state line
#include "battle/battle_state.h"
#include "battle/battle_state_diag.h"
#include "core/game_text.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"
#include "speech/speech.h"
#include "speech/phrasebook.h"
#include "speech/phrase_format.h"
#include "core/logger.h"
#include "core/stall_probe.h"
#include "navigation/player_state.h"   // ReadSceneObjectPos (target world pos)

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

// Battle target-selection readout.
//
// Confirmed model (three decompile traces + Frida): target selection is a SEPARATE object on the
// battle-HUD context DAT_0209be80 (RVA 0x1F7BE80). The highlighted target's HANDLE is
//   P = *(void**)DAT_0209be80 ; handle = *(int)(P + 0x9FD8)     (nameplate/target-info manager)
// active while *(P + 0x10f78) != 0 (the single-target selector; absent during command navigation).
//
// The nameplate builder FUN_002bfd20 (0x19FD20) renders that target and, in the same call, resolves
// the handle to the real BtlChr and passes it to the vitals builder FUN_00329220 (0x209220). Rather
// than re-decode the handle (FUN_003588b0 was unreliable to call directly), we mark "this render is
// the current target" while FUN_002bfd20 runs, then capture the real BtlChr from the nested
// FUN_00329220 call -- the same object that resolves to the correct name + real HP via the actor
// pool (proven in the probe). Both hooks run on the render/game thread (synchronous, single-thread),
// so the flag needs no locking. Ally-vs-enemy uses the mod's confirmed faction test (scene-kind
// nibble), NOT a nameplate flag (panel+0x280 & 2 mis-classified allies).

namespace {

using MemRead::PtrAt;
using MemRead::SafeReadU8;
using MemRead::SafeReadU32;
using MemRead::SafeReadF32;

// ---- offline-derived RVAs / offsets (abs = RVA + 0x120000) --------------------------------------
constexpr uint32_t RVA_NAMEPLATE = 0x19FD20;  // FUN_002bfd20(panel, flag) -- target/nameplate render
constexpr uint32_t RVA_SNAPSHOT  = 0x209220;  // FUN_00329220(BtlChr, ...) -- vitals builder (nested)
constexpr uint32_t RVA_BSTATE    = 0x1F7BE80;  // DAT_0209be80 (ptr) -> P (battle-HUD context)

constexpr uint32_t OFF_TARGETID  = 0x9FD8;    // *(int)(P+0x9FD8) = highlighted target handle
constexpr uint32_t OFF_GATE      = 0x10F78;   // *(P+0x10f78) != 0 = target selection active
constexpr uint32_t OFF_LIBRA_FLAGS = 0x10F68; // *(u32)(P+0x10f68) -- HUD flag word; see LibraActive()
constexpr uint32_t LIBRA_BIT     = 0x2;       // bit 1 = "enemy vitals are numbers" = Libra is up
constexpr uint32_t OFF_PANEL_ID  = 0x288;     // *(u32)(panel+0x288) = the unit this nameplate draws

// Actor pool, BtlChr and scene-kind layout: core/phyre_types.h owns them (this file used to keep
// its own copy that "mirrored" nav_rva.h -- two mirrors of the same offsets is how they drift).
using namespace PhyreTypes;
constexpr uint32_t SCENEOBJ_KIND = PhyreTypes::SCENEOBJ_KIND_OFF;   // local spelling kept for the reads below

typedef void  (*Pfn_Nameplate)(void*, int);
typedef void* (*Pfn_Snapshot)(void*, int, void*, int);

Pfn_Nameplate s_origNameplate = nullptr;
Pfn_Snapshot  s_origSnapshot  = nullptr;

// Set by FUN_002bfd20 while it renders the CURRENT target; consumed by the nested FUN_00329220.
bool     g_wantTargetBc = false;
int32_t  g_targetHandle = 0;
// PERMITTED per-frame change-check (the no-dedup rule's one exception; see CLAUDE.md).
// GUARDS: FUN_002bfd20 (nameplate render) -> FUN_00329220 (vitals build), both called EVERY frame
// a target nameplate is on screen. Without this the reader would speak the target once per frame.
// It is keyed on the target HANDLE, not on the spoken text, and HookedNameplate resets it to 0 the
// moment selection ends — so leaving targeting and coming back re-announces.
int32_t  g_lastHandle   = 0;

// Target cache for the `p`-key route. Written on the render/game thread whenever the target
// nameplate redraws (HookedSnapshot — EVENT-driven, NOT per-frame); read on the input thread
// (GetLockedTarget). The `bc` pointer + `handle` are the handle->BtlChr bridge; `p` re-resolves a
// FRESH position from `bc` at press time and gates on the LIVE DAT_0209be80 state (not cache age,
// since the redraw is sparse). `bc`=nullptr => never captured. `pos` is a last-resort fallback.
struct TargetCache {
    void*        bc     = nullptr;   // target BtlChr (matches actor+0x698); for live pos re-resolve
    FVec3        pos;                 // last resolved pos (fallback if the live re-resolve fails)
    std::wstring label;
    int32_t      handle = 0;
    uint64_t     tickMs = 0;   // GetTickCount64() at capture (diagnostic only)
};
std::mutex  g_cacheMx;
TargetCache g_cache;

// Forget the selected target. Until now the ONLY thing that ever cleared this was Shutdown(), so a
// captured target outlived its own death, its deselection, and the whole battle.
void ClearCache() {
    std::lock_guard<std::mutex> lk(g_cacheMx);
    g_cache = TargetCache{};
}

void* Pstate() { return PtrAt(Hooks::ResolveRva(RVA_BSTATE), 0); }

// IS THE TARGET CURSOR ACTUALLY UP? The game's own bit, the same one the browse path in
// `ResolveTarget` already trusts and the nameplate hook disarms on.
//
// This is a SEPARATE question from "is there a target", and conflating the two is what broke the
// describe key -- see the note in `SpeakTargetDetail`. A commitment outlives the aiming UI: it is
// still there while the player is in the battle menu picking a spell, which is exactly when they
// want the description bar, not a monster's vitals.
bool TargetSelectActive() {
    void* P = Pstate();
    return P && PtrAt(P, OFF_GATE) != nullptr;
}

// ---- LIBRA -------------------------------------------------------------------------------------
// Is the party's Libra up right now? This is the game's OWN answer, mirrored into the battle-HUD
// context once per frame, so reading it costs one guarded u32 and calls nothing:
//
//   FUN_0030c300 (0x1EC300)  walks the 9 party slots, skips the KO'd, returns 1 when any LIVING
//                            member carries bit 30 of (BtlChr+0x64 | BtlChr+0x3C)      <- Libra
//   FUN_0028e290:58-62       mirrors that into bit 1 of *(u32*)(P + 0x10F68), every frame
//   FUN_00290100 (0x170100)  the getter the game itself uses: *(u32*)(P + 0x10F68) >> 1 & 1
//
// What the game does with it is exactly the branch this file already has: FUN_002bfd20:139-141 sets
// its show-numbers flag when the target is a party member OR this bit is up, and FUN_002c0400 draws
// the HP as DIGITS when that flag is set and blanks them (0xFFFFFFFF) when it is not. So "Libra up"
// and "the enemy's HP is a number rather than a bar" are the same fact, and mirroring it here is
// reading the game's display, not bypassing it.
//
// This replaces the "there is no pre-Libra HP-visible flag to read" note that stood from Session 32:
// the old hunt looked for a bit on the ENEMY's BtlChr (it tried status bit 0x10000, which is the
// forced-max-display bit and reads 0 on an un-Libra'd enemy). Libra is a bit on a PARTY member.
bool LibraActive() {
    void* P = Pstate();
    uint32_t flags = 0;
    if (!P || !MemRead::SafeReadU32(P, OFF_LIBRA_FLAGS, &flags)) return false;
    return (flags & LIBRA_BIT) != 0;
}

// THE FOUR AFFINITY LABELS, and why they are cached rather than resolved on demand.
//
// They are the GAME's words -- the same four message ids the equipment detail panels head their rows
// with (FUN_00293310 / FUN_00293fe0 / FUN_00294b50 each emit all four in this order), and 0x2331 is
// additionally what FUN_00295d90 puts at the head of the row the target panel draws. So they must be
// read, not invented. But the one sanctioned resolver -- TextCapture::ResolveStringById -- is GAME
// THREAD ONLY, and `o` runs on the input thread. Adding a private ResolveMsgCodec here is exactly
// what text_capture.h asks callers not to do; it already exists because two files had each grown one.
//
// So they are resolved once from HookedSnapshot, which runs on the game thread every time a target
// nameplate renders -- necessarily before `o` can mean anything, because there has to be a target.
// The strings are locale-fixed and never change, so one resolve per session is all they need.
// If one is somehow still empty, the caller speaks that clause's element names bare rather than
// inventing a label for them.
//
// ORDER HERE IS THE SPOKEN ORDER, and it is deliberately not the equipment panels': weak first
// because it is what the player is nearly always asking for, then the three that were never spoken
// before. Each clause is omitted entirely when its mask is zero -- see LibraDetail.
enum class Affinity { Weak = 0, Absorb, Half, Immune, COUNT };
constexpr int kAffinityMsg[static_cast<int>(Affinity::COUNT)] = {
    0x2331,   // "Weak: "
    0x232F,   // "Absorb: "
    0x2330,   // "Half Damage: "
    0x232E,   // "Immune: "
};
std::mutex   g_affinityLabelMx;
std::wstring g_affinityLabel[static_cast<int>(Affinity::COUNT)];

std::wstring AffinityLabel(Affinity a) {
    std::lock_guard<std::mutex> lk(g_affinityLabelMx);
    return g_affinityLabel[static_cast<int>(a)];
}

// GAME THREAD ONLY. Resolves whichever of the four are still missing; a partial result is fine
// because each is retried independently on the next render.
void CacheAffinityLabels() {
    for (int i = 0; i < static_cast<int>(Affinity::COUNT); ++i) {
        {
            std::lock_guard<std::mutex> lk(g_affinityLabelMx);
            if (!g_affinityLabel[i].empty()) continue;
        }
        std::wstring s = TextCapture::ResolveStringById(kAffinityMsg[i]);
        if (s.empty()) continue;                 // try again on the next render
        std::lock_guard<std::mutex> lk(g_affinityLabelMx);
        g_affinityLabel[i] = s;
    }
}

// IS THIS UNIT LIBRA-PROOF? Marks, bosses and rare game show "????" instead of vitals even with
// Libra up, and that is a real per-target flag rather than missing data: FUN_002bfd20 tests
//     if ((*(u8*)(panel + 0x111) & 2) != 0) { iVar19 = 1; iVar18 = 0; }
// which zeroes the weakness row's alpha AND drops the HP back to blanked digits. `panel+0x111` is
// snapshot +0x51, and the snapshot's +0x4C..+0x5B is `bc[0x68+i] | bc[0x78+i]` -- so this is
// extended status bit 41 on the target.
//
// The mod honours it. Reading a weakness the screen is deliberately withholding would be the mod
// out-revealing the game, which is a different failure from a missing readout and a worse one.
bool LibraSuppressed(void* bc) {
    uint8_t a = 0, b = 0;
    if (!bc) return false;
    MemRead::SafeReadU8(bc, BC_EXT_A + BC_EXT_LIBRAPROOF_BYTE, &a);
    MemRead::SafeReadU8(bc, BC_EXT_B + BC_EXT_LIBRAPROOF_BYTE, &b);
    return ((a | b) & BC_EXT_LIBRAPROOF_BIT) != 0;
}

// The Libra readout. Everything here is data the game itself reveals once Libra is up, and every
// value is already sitting in the vitals snapshot FUN_00329220 builds for the nameplate -- this
// reads it straight off the BtlChr instead, which is the same numbers by the same offsets.
//
// THE WEAKNESS CLAUSE, and the mistake that nearly lost it (Session 147). This function first
// shipped saying weaknesses "are NOT in the game's data anywhere the mod can reach". That was wrong,
// and the two searches behind it were both true and both beside the point: there is no
// Weak/Absorb/Half/Immune QUARTET on the enemy (that shape belongs to the EQUIPMENT record, and its
// only consumers are the equip-preview scratch globals), and there is no affinity field on the enemy
// record at actor+0xE68. The mask is ONE BYTE on the BtlChr -- `bc+0x40` -- and it was already
// arriving in the snapshot this very file's hook receives, at +0x89.
//
// The game's own chain, which is what settles both the data and the gate:
//     FUN_00329220   snapshot+0x89 <- bc+0x40
//     FUN_002bfd20   FUN_00295d90(0x80, panel+0x200, *(u8*)(panel+0x149))
//     FUN_00295d90   emits message 0x2331 ("Weak: ") then one 0x4B27+bit element string per set bit
//     ...and that row's alpha is ramped by FUN_00290100() -- the same Libra flag LibraActive() reads.
// So the row is exactly Libra-gated, and mirroring the screen means weaknesses and nothing else:
// Absorb / Half / Immune (0x232F / 0x2330 / 0x232E) appear only on the equipment detail panels.
//
// The NAMES are the game's own via BattleState::ElementNames -- same bit order, already shipped --
// and so is the label. Nothing here is a mod-authored word.
std::wstring LibraDetail(void* bc) {
    if (!bc) return std::wstring();
    std::wstring out;
    auto add = [&](const std::wstring& s) {
        if (s.empty()) return;
        if (!out.empty()) out += L", ";
        out += s;
    };

    uint8_t level = 0;
    if (MemRead::SafeReadU8(bc, BC_LEVEL, &level) && level > 0)
        add(Phrase::Get(Phrase::Id::LevelPrefix) + std::to_wstring(level));

    // ~~MP~~ REMOVED (S160, user instruction): "enemies don't use MP, neither is it shown on libra."
    // The clause read the i16 pair behind the game's own MP-gauge guard, which meant it was correct
    // and still wrong to speak -- a number the enemy does not spend and the game's own Libra never
    // puts on screen is noise in the one readout the player is querying under time pressure. The
    // BC_CURMP / BC_MAXMP / BC_MP_GUARD_A / BC_MP_GUARD_B offsets stay in phyre_types.h; the ALLY
    // readouts still use them, and this was only ever the enemy path.

    // Statuses, as the game's own words, across the WHOLE status space.
    //
    // This used to read only the two u32 words (+0x3C, +0x64) -- 32 distinct bits, because both
    // index the same 32 statuses. The rest of an enemy's statuses live in the two 16-byte EXTENDED
    // masks at +0x68/+0x78, ~128 further bits that index the SAME master table (FUN_00385570 walks
    // those bits and looks each one up for its timer slot), and none of them was ever spoken.
    //
    // The two spaces are OR'd into ONE mask and walked ONCE, which is what makes a bit that exists
    // in both impossible to say twice. Anything the table does not cover, or that carries the
    // suppress marker, drops out inside StatusName -- so widening the read cannot invent a word.
    // A clean enemy adds nothing: silence, not "no statuses".
    uint8_t status[16] = {};
    uint8_t extA[16] = {}, extB[16] = {};
    const bool haveA = MemRead::SafeReadBytes(
        reinterpret_cast<char*>(bc) + BC_EXT_A, extA, sizeof(extA));
    const bool haveB = MemRead::SafeReadBytes(
        reinterpret_cast<char*>(bc) + BC_EXT_B, extB, sizeof(extB));
    for (size_t i = 0; i < sizeof(status); ++i)
        status[i] = static_cast<uint8_t>((haveA ? extA[i] : 0) | (haveB ? extB[i] : 0));

    uint32_t sa = 0, sb = 0;
    MemRead::SafeReadU32(bc, BC_STATUS_A, &sa);
    MemRead::SafeReadU32(bc, BC_STATUS_B, &sb);
    const uint32_t base = sa | sb;
    for (int i = 0; i < 4; ++i)
        status[i] = static_cast<uint8_t>(status[i] | ((base >> (i * 8)) & 0xFF));

    add(BattleState::StatusNamesMask(status, sizeof(status)));

    // THE ELEMENTAL AFFINITIES, all four, last -- they are the longest clauses and the ones the
    // player is most often waiting for the rest to get out of the way of.
    //
    // Absorb, Half and Immune were added in S160 on the user's instruction ("it should show absorb
    // as well"). They are NOT on the game's own target panel, which draws the Weak row alone -- but
    // they are on the enemy, they are the game's own data with the game's own labels, and an absorb
    // is the single most consequential thing to not know about the monster you are about to hit.
    // Byte meanings are measured from the damage path's own resolver, not inferred from the
    // equipment record's layout: see the block over BC_WEAK_MASK in phyre_types.h.
    //
    // Each clause is omitted entirely when its mask is zero -- a non-elemental enemy draws no icons,
    // so "no weaknesses" would be a sentence the screen never shows. All four sit behind the same
    // LibraSuppressed gate the weakness row already honoured: a mark the game is deliberately
    // hiding behind "????" must not have its affinities read out by the mod either.
    if (!LibraSuppressed(bc)) {
        struct Clause { uint32_t off; Affinity label; };
        static constexpr Clause kClauses[] = {
            { BC_WEAK_MASK,   Affinity::Weak   },
            { BC_ABSORB_MASK, Affinity::Absorb },
            { BC_HALF_MASK,   Affinity::Half   },
            { BC_IMMUNE_MASK, Affinity::Immune },
        };
        for (const Clause& c : kClauses) {
            uint8_t mask = 0;
            if (!MemRead::SafeReadU8(bc, c.off, &mask) || mask == 0) continue;
            const std::wstring names = BattleState::ElementNames(mask);
            if (!names.empty()) add(AffinityLabel(c.label) + names);
        }
    }

    return out;
}

// The HP clause, in ONE place. Allies always show numbers; an enemy shows a percentage until Libra
// is up and then shows numbers, mirroring the gauge-vs-digits swap the game makes on the same
// condition. Both callers (the highlight announce and the `;` key) used to carry their own copy of
// this branch, which is how they would have drifted apart.
std::wstring HpClause(void* bc, bool ally) {
    uint32_t curHPu = 0, maxHPu = 0;
    MemRead::SafeReadU32(bc, BC_CURHP, &curHPu);
    MemRead::SafeReadU32(bc, BC_MAXHP, &maxHPu);
    const int32_t rawCur = static_cast<int32_t>(curHPu), rawMax = static_cast<int32_t>(maxHPu);
    if (rawMax <= 0) return std::wstring();
    // SPOKEN NUMBERS GO THROUGH THE GAME'S CLAMP; the PERCENTAGE DOES NOT.
    //
    // The clamp is a display rule (FUN_002fef30: party side caps at 9999, everything else at 1e9), so
    // it belongs on the digits the player hears. It must NOT touch the ratio below: clamping a
    // bubbled ally's current to 9999 while its max stayed 7319 would compute 137%, and clamping both
    // would flatten a real difference to a flat 100%. A fraction wants the raw pair.
    //
    // For an ENEMY the clamp is a no-op by construction (cap 1e9), which is the point of taking the
    // game's own selector -- a boss over 9999 HP still reports its real number under Libra.
    if (ally || LibraActive())
        return std::wstring(L", ") + Phrase::Get(Phrase::Id::HPPrefix)
             + std::to_wstring(BattleState::DisplayHp(bc, rawCur)) + L"/"
             + std::to_wstring(BattleState::DisplayHp(bc, rawMax));
    return L", " + PhraseFormat::Percent(Phrase::Id::HPPrefix, rawCur, rawMax);
}

inline bool NonZero(const FVec3& p) { return !(p.x == 0.0f && p.y == 0.0f && p.z == 0.0f); }

// Baked confirmation of the target position sources (for the diagnostic log).
struct PosDiag {
    bool  sceneOk = false;  FVec3 scenePos;   // sceneObj+0xB8 transform node
    bool  actorOk = false;  FVec3 actorPos;   // actor+0xE0/E4/E8 cached pos (GameArchitecture.md:728)
    int   source  = 0;      // 0 none, 1 scene node, 2 actor cache
};

// Target world position with a fallback: the scene node (sceneObj+0xB8) first — the same chain
// entity_list uses; if it fails or reads (0,0,0), as it does for a battle target during attack-menu
// selection, fall back to the actor's own cached world position (actor+0xE0/E4/E8). Returns havePos
// (a non-zero position from either source). Fills `d` for the confirmation log when non-null.
bool ResolveActorPos(void* actor, void* sceneObj, FVec3& out, PosDiag* d) {
    FVec3 sp;
    const bool sceneOk = PlayerState::ReadSceneObjectPos(sceneObj, sp) && NonZero(sp);
    FVec3 ap;
    const bool ax = SafeReadF32(actor, ACTOR_POS_X, &ap.x);
    const bool ay = SafeReadF32(actor, ACTOR_POS_Y, &ap.y);
    const bool az = SafeReadF32(actor, ACTOR_POS_Z, &ap.z);
    const bool actorOk = ax && ay && az && NonZero(ap);
    if (d) { d->sceneOk = sceneOk; d->scenePos = sp; d->actorOk = actorOk; d->actorPos = ap; }
    if (sceneOk) { out = sp; if (d) d->source = 1; return true; }
    if (actorOk) { out = ap; if (d) d->source = 2; return true; }
    if (d) d->source = 0;
    return false;
}

// Combatant DISPLAY name (name + instance letter) + faction (scene-kind nibble) from the actor pool,
// matching the BtlChr via *(actor+0x698)==bc. Returns the name (empty if not found / unprintable);
// *ally set from kind. The one pool scan is what earns this helper its existence -- it answers name,
// faction, position and dead-ness together -- but the NAMING inside it is BattleState's, not its own.
// When posOut is non-null, also reads the unit's live world position (scene node, else actor cache)
// and sets *havePosOut; fills *diagOut with both sources for the confirmation log.
// *deadOut is set when the unit's scene-kind says it has been removed/killed (KIND_DEAD), so callers
// can drop a target that died — an empty return means "not in the pool at all" (also not targetable).
std::wstring NameForBtlChr(void* bc, bool* ally, FVec3* posOut = nullptr, bool* havePosOut = nullptr,
                           PosDiag* diagOut = nullptr, bool* deadOut = nullptr) {
    *ally = false;
    if (havePosOut) *havePosOut = false;
    if (deadOut) *deadOut = false;
    if (!bc) return std::wstring();
    void* base = PtrAt(Hooks::ResolveRva(ACTOR_POOL_BASE), 0);
    if (!base) return std::wstring();
    uint32_t count = 0;
    SafeReadU32(Hooks::ResolveRva(ACTOR_POOL_COUNT), 0, &count);
    if (count == 0) return std::wstring();
    if (count > 64) count = 64;
    for (uint32_t i = 0; i < count; ++i) {
        void* actor = reinterpret_cast<char*>(base) + static_cast<size_t>(i) * ACTOR_STRIDE;
        if (PtrAt(actor, ACTOR_DEF_PTR) != bc) continue;
        // ONE naming path for enemies. BattleState::DisplayNameForActor is NameForActor (actor+0x18,
        // printable-gated) PLUS the instance letter from InstanceIndex -- the same helper `;` and `p`
        // already call. This loop used to decode actor+0x18 inline, which is exactly why the
        // targeting-menu highlight said "Steeling" while `;` said "Steeling B" for the same unit:
        // two name paths, and only one of them ever grew the letter. Do not re-inline it.
        // A lone enemy still has no letter -- InstanceIndex returns 0 and that is the correct answer,
        // not a miss.
        std::wstring nm = BattleState::DisplayNameForActor(actor);
        if (nm.empty()) return std::wstring();
        void* sceneObj = PtrAt(actor, ACTOR_SCENEOBJ);
        uint8_t kind = 0xFF, def5 = 0xFF;
        SafeReadU8(sceneObj, SCENEOBJ_KIND, &kind);
        SafeReadU8(bc, DEF_KIND_BYTE, &def5);
        // ally = the player-controlled leader (self-target: def+5==0, but its scene-kind is NOT 3)
        // OR a party-side unit (guests / AI party, kind==3). Mirrors the proven nav classifier
        // (entity_list.cpp) — KIND_ALLY alone misses the solo-Reks leader, so a curative on
        // yourself read as a percentage instead of a number.
        *ally = (def5 == PLAYER_DEF_KIND) || ((kind & KIND_MASK) == KIND_ALLY);
        if (deadOut) *deadOut = ((kind & KIND_MASK) == KIND_DEAD);
        if (posOut) {
            bool hp = ResolveActorPos(actor, sceneObj, *posOut, diagOut);
            if (havePosOut) *havePosOut = hp;
        }
        return nm;
    }
    return std::wstring();
}

// Speak the current target. Enemy = HP percentage (mirrors the gauge; no MP/numbers pre-Libra),
// ally = HP numbers. Real HP off the BtlChr (bc+0x48/0x24). Silent if the name can't resolve.
// name/ally are pre-resolved by the caller (single actor-pool lookup shared with the p-key cache).
void AnnounceTargetBc(void* bc, const std::wstring& name, bool ally) {
    if (name.empty()) return;

    std::wstring text = name + HpClause(bc, ally);

    // AUTODETAIL: volunteer the full Libra readout behind the short line, never instead of it, and
    // never as a second Speech::Output -- one utterance, so nothing can race it. OFF (the default)
    // leaves this line byte-identical to what it has always been. There is deliberately NO
    // "Libra not active" here: that answers the `o` KEY, and on a per-highlight line it would nag
    // on every cursor move, which is exactly the filler the standing rule forbids.
    if (ModMenu::AutoDetailOn() && !ally && LibraActive()) {
        const std::wstring detail = LibraDetail(bc);
        if (!detail.empty()) text += L". " + detail;
    }

    char utf8[256];
    Log::ToUtf8(text, utf8, sizeof(utf8));
    char line[320];
    snprintf(line, sizeof(line), "handle=0x%x %s \"%s\"", g_targetHandle, ally ? "ally" : "enemy", utf8);
    Log::Write("TARGET", line);

    Speech::Output(text, /*interrupt=*/true);
}

// FUN_002bfd20 render. If this call is drawing the CURRENT target (panel+0x288 == P+0x9FD8) while
// target selection is active, flag it so the nested FUN_00329220 grabs the real BtlChr.
void HookedNameplate(void* panel, int flag) {
    STALL_SCOPE("BattleTarget::HookedNameplate");
    g_wantTargetBc = false;
    void* P = Pstate();
    if (P) {
        if (!PtrAt(P, OFF_GATE)) {
            g_lastHandle = 0;   // not selecting -> disarm the per-frame guard (re-entry re-announces)
            ClearCache();       // ...and drop the target itself, not just the dedup key
        } else {
            uint32_t th = 0, ph = 0;
            SafeReadU32(P, OFF_TARGETID, &th);
            SafeReadU32(panel, OFF_PANEL_ID, &ph);
            if (th != 0 && ph == th) {
                g_wantTargetBc = true;
                g_targetHandle = static_cast<int32_t>(th);
            }
        }
    }

    if (s_origNameplate) s_origNameplate(panel, flag);   // calls FUN_00329220 with the target BtlChr
    g_wantTargetBc = false;
}

// FUN_00329220 vitals build. When invoked for the flagged target, `bc` is the real target BtlChr.
// Resolve name+faction+world-pos once: refresh the p-key cache EVERY render while a target is live
// (so `p` always has the current target), but announce only on a target CHANGE — the permitted
// per-frame guard documented on g_lastHandle above.
void* HookedSnapshot(void* bc, int p2, void* outBuf, int p4) {
    void* r = s_origSnapshot ? s_origSnapshot(bc, p2, outBuf, p4) : nullptr;
    STALL_SCOPE("BattleTarget::HookedSnapshot");
    if (g_wantTargetBc && bc) {
        g_wantTargetBc = false;                 // take only the first (the target) per render
        CacheAffinityLabels();                  // game thread: the one place `o`'s labels can be read
        bool ally = false; FVec3 pos; bool havePos = false; PosDiag pd;
        std::wstring name = NameForBtlChr(bc, &ally, &pos, &havePos, &pd);
        if (!name.empty()) {
            // Always cache bc+handle+label (the handle->BtlChr bridge); pos when we have it. `p`
            // re-resolves a fresh pos from bc and gates on the live DAT_0209be80 state, so it no
            // longer depends on how often this (event-driven) render refreshes the cache.
            {
                std::lock_guard<std::mutex> lk(g_cacheMx);
                g_cache.bc     = bc;
                g_cache.label  = name;
                g_cache.handle = g_targetHandle;
                g_cache.tickMs = GetTickCount64();
                if (havePos) g_cache.pos = pos;
            }
            if (g_targetHandle != g_lastHandle) {
                g_lastHandle = g_targetHandle;
                // Baked confirmation: which position source populated the p-key cache (scene node
                // vs actor+0xE0 fallback). If both are 0/absent, havePos is false and `p` = No target.
                char pm[192];
                snprintf(pm, sizeof(pm),
                         "pos: sceneOk=%d scene=(%.1f,%.1f,%.1f) actorOk=%d actor=(%.1f,%.1f,%.1f) src=%d havePos=%d",
                         pd.sceneOk ? 1 : 0, pd.scenePos.x, pd.scenePos.y, pd.scenePos.z,
                         pd.actorOk ? 1 : 0, pd.actorPos.x, pd.actorPos.y, pd.actorPos.z,
                         pd.source, havePos ? 1 : 0);
                Log::Write("TARGET", pm);
                AnnounceTargetBc(bc, name, ally);
            }
        }
    }
    return r;
}

} // namespace

namespace BattleTargetReader {

bool Init() {
    bool ok = Hooks::InstallTyped(RVA_NAMEPLATE, &HookedNameplate, &s_origNameplate);
    ok     &= Hooks::InstallTyped(RVA_SNAPSHOT,  &HookedSnapshot,  &s_origSnapshot);
    Log::Write("TARGET", ok ? "BattleTargetReader: target readout installed (FUN_002bfd20 + FUN_00329220, DAT_0209be80+0x9FD8)"
                            : "BattleTargetReader: a target hook FAILED to install");
    return ok;
}

void Shutdown() {
    Hooks::Uninstall(RVA_SNAPSHOT);
    Hooks::Uninstall(RVA_NAMEPLATE);
    std::lock_guard<std::mutex> lk(g_cacheMx);
    g_cache = TargetCache{};   // drop any stale target
}

struct ResolvedTarget {
    void*        actor    = nullptr;
    void*        bc       = nullptr;
    std::wstring name;                  // display name, instance letter included
    bool         ally     = false;
    bool         browsing = false;      // true = the cursor, NOT a commitment
    bool         acting   = false;      // committed and mid-action (vs queued)
    uint16_t     actionId = 0xFFFF;
    FVec3        pos;
    bool         havePos  = false;
};

// Resolve THE TARGET, committed first. Shared by `p` (route) and `;` (status).
//
// ===== WHY THIS CHANGED (Session 49) =====
// The old implementation resolved `*(P + 0x9FD8)`, which is written ONLY by browse handlers. A live
// capture settled it: scrolling the cursor across two enemies moved that field on every step while
// the character's actual commitment sat on a THIRD enemy the cursor never visited. So the previous
// readout could name a unit the character was not acting on at all.
//
// The real commitment lives on the ACTOR, and BattleState::CommittedTargetOf applies the corrected
// precedence: ACTIVE (+0x710/+0x714) only when it holds a real ability id and a non-null target --
// actor+0x714 can carry an AI/behaviour opcode from a 0x4000+ band with target 0 -- otherwise the
// QUEUED pair (+0xBB8/+0xBA0) gated on flag bit 0x4000, which must be tested because those fields
// retain stale values after it clears.
//
// The browse cursor is kept as an explicitly-labelled SECOND choice: while the select UI is open,
// what you are hovering is genuinely useful, it just is not "the target".
//
// No cache is needed any more. It existed because FUN_003588b0 was unreliable to call; handle ->
// actor is now a direct scan of actor+0x08, which is the actor's own handle (assigned outright in
// FUN_00322080).
bool ResolveTarget(ResolvedTarget& out) {
    out = ResolvedTarget{};

    void* actor = nullptr;

    // 1. the committed target
    void* leader = BattleState::LeaderActor();
    if (leader) {
        const BattleState::Committed c = BattleState::CommittedTargetOf(leader);
        if (c.valid) {
            actor = BattleState::ActorForHandle(c.targetHandle);
            if (actor) { out.acting = c.active; out.actionId = c.actionId; }
            else {
                // THE BLIND SPOT. A commitment that resolves but whose handle does not map to a
                // live actor falls through to the browse cursor below and is logged as "BROWSING"
                // -- indistinguishable from "there was no commitment". That ambiguity is why the
                // log appeared to show commitment almost never firing; it may in fact be firing
                // and this lookup failing. Never let these two look the same again.
                char m[160];
                snprintf(m, sizeof(m),
                         "commit: VALID but ActorForHandle(0x%X) FAILED -- falling back to browse",
                         static_cast<unsigned>(c.targetHandle));
                Log::Write("TARGET", m);
            }
        }
    }

    // 2. else the browse cursor, but ONLY while the select UI is genuinely open
    if (!actor) {
        void* P = Pstate();
        if (P && MemRead::PtrAt(P, OFF_GATE) != nullptr) {
            uint32_t h = 0;
            if (MemRead::SafeReadU32(P, OFF_TARGETID, &h) && h != 0) {
                actor = BattleState::ActorForHandle(static_cast<int32_t>(h));
                if (actor) out.browsing = true;
            }
        }
    }
    if (!actor) return false;

    // LIVENESS — a handle alone is not enough. When a target dies the game leaves the selection
    // state alone for a moment and the actor flips to scene-kind 5 (dead/removed) or leaves the
    // pool. Without this, `p` used to route to the corpse's last known spot.
    void* bc = BattleState::BtlChrForActor(actor);
    void* sceneObj = MemRead::PtrAt(actor, ACTOR_SCENEOBJ);
    uint8_t kind = 0xFF;
    MemRead::SafeReadU8(sceneObj, SCENEOBJ_KIND, &kind);
    if ((kind & KIND_MASK) == KIND_DEAD) return false;

    uint32_t curHP = 0;
    if (bc && MemRead::SafeReadU32(bc, BC_CURHP, &curHP) && curHP == 0) return false;

    out.name = BattleState::DisplayNameForActor(actor);   // includes the instance letter
    if (out.name.empty()) return false;

    const BattleState::Faction f = BattleState::FactionOf(actor);
    out.ally = (f == BattleState::Faction::Party || f == BattleState::Faction::Guest ||
                f == BattleState::Faction::Ally);

    out.actor = actor;
    out.bc = bc;
    out.havePos = ResolveActorPos(actor, sceneObj, out.pos, nullptr);

    char lg[224];
    char utf8[128];
    Log::ToUtf8(out.name, utf8, sizeof(utf8));
    snprintf(lg, sizeof(lg), "ResolveTarget: \"%s\" %s%s hp=%u havePos=%d",
             utf8, out.browsing ? "BROWSING" : (out.acting ? "committed/acting" : "committed/queued"),
             out.ally ? " ally" : " enemy", curHP, out.havePos ? 1 : 0);
    Log::Write("TARGET", lg);
    return true;
}

// Input-thread accessor for `p` (route to the target). Shares ResolveTarget with `;`, so the two
// keys agree on which unit they mean -- which was NOT true between 2026-07-20 and 2026-07-21, when
// `;` rejected the browsed target that `p` happily routed to. The comment claimed agreement the
// whole time; it is true again now.
bool GetLockedTarget(FVec3& posOut, std::wstring& labelOut) {
    ResolvedTarget t;
    if (!ResolveTarget(t) || !t.havePos) return false;
    // AN ALLY IS ROUTABLE ONLY WHILE YOU ARE ACTUALLY AIMING AT ONE. The user's case for keeping it:
    // *"too far away for a heal, track them with p until close enough"* -- so a LIVE ally selection
    // is a legitimate destination and a blanket refusal (which this was, briefly) throws that away.
    // What must not survive is a STALE one: the game's commitment record does not clear when the
    // action that aimed at a party member finishes, so a leftover would otherwise keep routing to
    // the leader's own party for the rest of the fight.
    if (t.ally && !t.browsing && !t.acting) {
        Log::Write("TARGET", "p: SILENT -- stale ALLY commitment (not browsing, not acting)");
        return false;
    }
    posOut = t.pos;
    labelOut = t.name;
    return true;
}

// Input-thread accessor for `;` (speak the target's status): the unit the player is fighting.
//
// REPORTS WHATEVER ResolveTarget RESOLVED -- the commitment when there is one, the live select-UI
// target otherwise. It does NOT reject a "browsed" target.
//
// WHY (regression, fixed 2026-07-21). This used to be `if (!ResolveTarget(t) || t.browsing)`, added
// 2026-07-20 in eeffde5 on top of the 3903dbc rewrite that replaced `ResolveLiveTarget` with the
// commitment path. Before that rewrite the key resolved the live select-UI target (gate P+0x10F78
// open, handle from P+0x9FD8) and worked on every press -- the 07-20 12:11 log shows
// `ResolveLiveTarget: gate=1 handle=0x20000f ... hp=65 match=1` succeeding repeatedly. The
// `t.browsing` clause discarded exactly that state, so with an enemy targeted and Attack confirmed
// the key went silent: commitment resolution rejects the ACTIVE branch (the action-table row lookup
// returns null for Attack, id 0x96 -- see DiagnoseCommitment), the QUEUED bit is clear mid-swing,
// and the live target it fell back to was then thrown away.
//
// OUT OF BATTLE IT IS STILL SILENT, structurally and with no "am I in battle" flag: ResolveTarget
// returns false on its own out of combat, because there is no commitment AND the browse branch
// requires the select-UI gate to be open. The release-0.1 requirement is preserved by that, not by
// the clause removed here. Do NOT restore the old "No target" speech -- silence on
// nothing-to-report is a standing rule.
// Returns TRUE only when it actually spoke. `;` is shared with the field-side interact-target
// readout (InteractTarget::SpeakCurrent), which runs only when this had nothing -- so the caller
// needs to know the difference between "spoke" and "stayed silent". It is NOT an "in battle" flag:
// a battle with no committed or browsed target also returns false, and the field reader is silent
// there too, so the combined key stays silent exactly where it always did.
bool SpeakTargetStatus() {
    ResolvedTarget t;
    if (!ResolveTarget(t)) {
        // Say WHY, every link of it. A confirmed attack that reports "no commitment" is a bug in the
        // BtlWork -> leader -> actor-pool -> active/queued chain, and without this the whole chain
        // fails as one silent boolean with nothing to grep.
        BattleState::DiagnoseCommitment();
        Log::Write("TARGET", "; SILENT: no target (no commitment and no open select UI)");
        return false;
    }

    // `;` NEVER REPORTS AN ALLY. User instruction, twice over: *"if not actively targeting an enemy,
    // it should be no-op"*, then, after a build that still spoke on a BROWSED ally, *"; is still
    // pinging an active target even when p says no target, and it's usually an ally."*
    //
    // The log says why a browsing test was not enough: `ResolveTarget: "Basch" BROWSING ally hp=14638
    // havePos=1`, press after press. The select-UI browse state sits on a party member and stays
    // there, so "browsing" is not the fleeting aim it sounds like -- it is the resting state. And
    // because `;` returns TRUE when it speaks, every one of those presses consumed the key and the
    // FIELD interact readout it falls through to (InteractTarget::SpeakCurrent) became unreachable.
    // That is the actual harm: not a wrong word, a whole branch of the key silently deleted.
    //
    // So ally-ness is the test here, matching `o` (SpeakTargetDetail), which has refused allies from
    // the day it was written. `p` keeps the softer rule on purpose -- routing to a party member you
    // are aiming a heal at is useful, reading their HP over the interact prompt is not.
    if (t.ally) {
        Log::Write("TARGET", "; SILENT: target is an ALLY -- `;` answers for enemies only, "
                             "falling through to the interact-target readout");
        return false;
    }

    std::wstring text = t.name + HpClause(t.bc, t.ally);
    // Mod-emitted qualifier, and ONLY for a real commitment that has not started executing. A
    // browsed target reaches here now, and it is neither acting nor queued -- calling it "queued"
    // would be a fabricated state. It gets no suffix, matching what this key said when it worked.
    if (!t.browsing && !t.acting) text += std::wstring(L", ") + Phrase::Get(Phrase::Id::Queued);

    Speech::Output(text, /*interrupt=*/true);
    return true;
}

// The `o` key, in battle. Returns TRUE only when it spoke, so MenuReader::DescribeHotkey can fall
// through to the help text everywhere this has nothing to say -- the same first-refusal shape `;`
// already uses with InteractTarget::SpeakCurrent.
//
// Not aiming, or not aiming at an enemy => SILENT and false. `o` keeps every meaning it already had;
// a player pressing it on a menu row is not asking about a monster.
//
// ~~"No enemy target => SILENT and false"~~ was the whole gate until 2026-08-12 and it is **STRUCK**:
// it tested whether a target EXISTS, not whether the player is aiming at one, and a commitment
// outlives the aiming UI. See the first gate in the body.
//
// Enemy targeted but Libra down => "Libra not active." This is a USER-INSTRUCTED exception to the
// never-speak-filler rule (granted 2026-08-10) and it is not filler: `o` is a direct question, and
// silence here is indistinguishable from a broken mod. It fires on the KEY only -- the
// per-highlight autodetail path in AnnounceTargetBc deliberately never says it.
bool SpeakTargetDetail() {
    // ---- FIRST GATE: THE CURSOR MUST ACTUALLY BE UP (reported 2026-08-12) --------------------
    //
    // The header above claimed this was "structurally silent everywhere else (no committed/browsed
    // enemy target => false)". **That was wrong, and it made ability descriptions unreadable.**
    // `ResolveTarget` resolves the COMMITTED target first and that path has no UI gate at all -- only
    // its browse fallback is gated. A commitment outlives the aiming step, so standing in the battle
    // menu choosing a spell, with an enemy still committed from the last action, this returned true,
    // spoke Libra, and shadowed the description bar on every press. The fall-through was unreachable
    // exactly where it was needed most.
    //
    // `o` is the description key. It only becomes the Libra key while the player is genuinely aiming,
    // which is the one moment "tell me about this monster" is the question being asked.
    //
    // ---- THE STATE LINE: what the two candidate discriminators actually read, per state ----------
    //
    // S156 STRUCK the name of OFF_GATE on the claim that `P+0x10F78` "was true while the battle
    // command menu was open" -- but every archived log runs a build that predates the gate which
    // would have tested it, so the claim has NO MEASUREMENT BEHIND IT, and this file has gone on
    // naming and trusting the offset regardless. Rather than settle that with a third inferred gate,
    // this prints the raw facts and lets one play session close it: the gate pointer, the highlighted
    // target handle, and whether the battle command panel is live.
    //
    // Keyed on the state TUPLE, not the press, so it emits once per distinct combination and a
    // hundred presses in one state cost one line. Not throttled and not capped -- a rate-limited
    // line is not a measurement (L-04), and this line exists to be one.
    //
    // DELETE THIS once the log has named the state for both the command list and the target cursor.
    // It is written to be thrown away.
    {
        void*    P      = Pstate();
        void*    gate   = P ? PtrAt(P, OFF_GATE) : nullptr;
        uint32_t handle = 0;
        if (P) MemRead::SafeReadU32(P, OFF_TARGETID, &handle);
        const bool bcmd = IngameMenuReader::BattleCommandActive();

        const int state = (gate ? 4 : 0) | (handle ? 2 : 0) | (bcmd ? 1 : 0);
        static int s_lastState = -1;
        if (state != s_lastState) {
            s_lastState = state;
            char m[192];
            snprintf(m, sizeof(m),
                     "o: state gate=%s handle=0x%X bcmdLive=%d  (gate is P+0x10F78; "
                     "does it read up in the COMMAND LIST?)",
                     gate ? "up" : "down", static_cast<unsigned>(handle), bcmd ? 1 : 0);
            Log::Write("TARGET", m);
        }
    }

    if (!TargetSelectActive()) return false;

    ResolvedTarget t;
    if (!ResolveTarget(t) || t.ally || !t.bc) return false;

    // RESIDUAL, LOGGED RATHER THAN GUESSED AT. With the cursor up, `ResolveTarget` still answers
    // with the COMMITTED target when there is one, not the unit under the cursor -- so aiming at a
    // second enemy could report the first. That order is deliberate and load-bearing for `p` and `;`
    // (the 2026-07-21 regression note on this file), so it is not being changed on a hunch. This line
    // says whether the two ever actually disagree; if a play log shows it, the fix is a cursor-first
    // resolution for this key only, and it will have evidence behind it.
    if (!t.browsing) {
        uint32_t cursorHandle = 0;
        void* P = Pstate();
        if (P && MemRead::SafeReadU32(P, OFF_TARGETID, &cursorHandle) && cursorHandle != 0) {
            void* cursorActor = BattleState::ActorForHandle(static_cast<int32_t>(cursorHandle));
            if (cursorActor && cursorActor != t.actor) {
                char m[192];
                char nm[96];
                Log::ToUtf8(BattleState::DisplayNameForActor(cursorActor), nm, sizeof(nm));
                snprintf(m, sizeof(m),
                         "o: DISAGREEMENT -- committed target spoken, but the cursor is on \"%s\"",
                         nm);
                Log::Write("TARGET", m);
            }
        }
    }

    if (!LibraActive()) {
        Log::Write("TARGET", "o: Libra not active");
        Speech::Output(Phrase::Get(Phrase::Id::LibraNotActive), /*interrupt=*/true);
        return true;
    }

    // Baked falsifier for the whole Libra model, printed once per session. Bit 30 of
    // (BtlChr+0x64 | +0x3C) is Libra by derivation, not by measurement -- if this ever names a
    // different status, LibraActive() is reading the wrong thing and the feature must be pulled.
    static bool s_loggedBit = false;
    if (!s_loggedBit) {
        s_loggedBit = true;
        char b[128];
        char nm[64];
        Log::ToUtf8(BattleState::StatusName(30, /*includeSuppressed=*/true), nm, sizeof(nm));
        snprintf(b, sizeof(b), "libra bit30 = \"%s\" (must be the game's own Libra)", nm);
        Log::Write("TARGET", b);
    }

    std::wstring text = t.name + HpClause(t.bc, /*ally=*/false);
    const std::wstring detail = LibraDetail(t.bc);
    if (!detail.empty()) text += L". " + detail;

    char utf8[320];
    Log::ToUtf8(text, utf8, sizeof(utf8));
    Log::Write("TARGET", (std::string("o: ") + utf8).c_str());
    Speech::Output(text, /*interrupt=*/true);
    return true;
}

} // namespace BattleTargetReader
