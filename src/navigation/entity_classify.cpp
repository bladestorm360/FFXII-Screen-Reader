#include "navigation/entity_classify.h"
#include "navigation/nav_rva.h"
#include "navigation/map_rva.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"
#include "core/game_text.h"
#include "speech/phrasebook.h"

#include <Windows.h>

using namespace MemRead;

namespace EntityScan {

using EntityList::Category;

const wchar_t* CategoryWord(Category c) {
    using Phrase::Id;
    switch (c) {
        case Category::All:          return Phrase::Get(Id::CatAll);
        case Category::Exit:         return Phrase::Get(Id::CatExit);
        case Category::Door:         return Phrase::Get(Id::CatDoor);
        case Category::Shop:         return Phrase::Get(Id::CatShop);
        case Category::SaveCrystal:  return Phrase::Get(Id::CatSaveCrystal);
        case Category::GateCrystal:  return Phrase::Get(Id::CatGateCrystal);
        case Category::Treasure:     return Phrase::Get(Id::CatTreasure);
        case Category::NPC:          return Phrase::Get(Id::CatNPC);
        case Category::Object:       return Phrase::Get(Id::CatInteractables);
        case Category::Enemy:        return Phrase::Get(Id::CatEnemy);
        case Category::Items:        return Phrase::Get(Id::CatItems);
        default:                     return Phrase::Get(Id::CatInteractables);
    }
}

// How many objects this scan spoke the ODD npcdic slot -- i.e. a name the mod would NOT have
// said before Session 81. Paired with the `TalkNameKnown` tally on the same log line, the
// difference is exactly what this change newly reveals. Session 79's rule: a change that cannot
// be sized offline ships with its own counter.
int s_oddSlotWins = 0;

void ResetNameStats() { s_oddSlotWins = 0; }
int  OddSlotWins()    { return s_oddSlotWins; }

// Has the player been introduced to this character? Replicates FUN_0032a930 — a live
// per-name-id bitfield in the game's own state block, which the `settalknpcname` /
// `releasetalknpcname` script natives write and `istalknpcname` reads back.
//
// DEMOTED TO DIAGNOSTICS (Session 81). It used to select which npcdic slot the mod spoke; it no
// longer does, because the mod now prefers the personal name whenever the dictionary carries a
// distinct one. Its only caller is the `inclusion:` tally, where it answers "how many of the
// personal names we are speaking has the player actually been introduced to".
// Pure reads; TALK_NAME_STATE is a static array, never dereferenced.
bool TalkNameKnown(int id) {
    if (id < 0 || id >= NavRva::TALK_NAME_MAX_ID) return false;
    void* base = Hooks::ResolveRva(NavRva::TALK_NAME_STATE);
    if (!base) return false;
    uint8_t bits = 0;
    if (!SafeReadU8(base, NavRva::TALK_NAME_BITMAP + static_cast<uint32_t>(id >> 3), &bits))
        return false;
    return (bits & (1u << (id & 7))) != 0;
}

namespace {

// npcdic codec-string pointer for one SLOT — replicates FUN_003eac10 (the game's own npcdic
// lookup) memory-only. The blob is loaded once at boot; DAT_02b5e0d8 holds its base. The offset
// table stores relocated absolute pointers as s32, read here exactly as the game does; 0 /
// out-of-range -> null. `odd` picks slot id*2+1 over id*2.
const uint8_t* NpcdicSlot(int id, bool odd) {
    void* blob = PtrAt(Hooks::ResolveRva(NavRva::NPCDIC_BASE), 0);
    if (!blob) return nullptr;
    uint32_t count = 0;
    if (!SafeReadU32(blob, NavRva::NPCDIC_COUNT_OFF, &count)) return nullptr;
    const uint32_t slot = static_cast<uint32_t>(id) * 2 + (odd ? 1u : 0u);
    if (slot >= count) return nullptr;
    uint32_t entry = 0;
    if (!SafeReadU32(blob, NavRva::NPCDIC_TABLE_OFF + slot * 4, &entry) || entry == 0)
        return nullptr;
    // Sign-extend the s32 to a full pointer, as the game does (blob mapped low).
    return reinterpret_cast<const uint8_t*>(
        static_cast<intptr_t>(static_cast<int32_t>(entry)));
}

// Decode + validate ONE codec pointer. Empty for null, for the engine's EMPTY_STRING sentinel, and
// for a blob that does not decode to printable text. Both npcdic slots go through this, so the
// sentinel check now guards both -- it used to guard only the single chosen pointer.
std::wstring DecodeCodec(const uint8_t* codec) {
    if (!codec) return std::wstring();
    void* empty = Hooks::ResolveRva(NavRva::EMPTY_STRING);
    if (reinterpret_cast<void*>(const_cast<uint8_t*>(codec)) == empty) return std::wstring();
    std::wstring s = GameText::Decode(codec, 256);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
}

// The name the mod SPEAKS for an npcdic id. Slot = id*2 (generic) or id*2+1 (personal), chosen the
// way the ENGINE chooses it: `FUN_00263990` uses `id*2 + FUN_0032a930(id)`, so the personal name
// appears only once the story has introduced that character.
//
// **STRUCK (Session 82): "the odd slot wins whenever the dictionary carries a distinct one."** That
// shipped for one session and was wrong in play. It read "Dania" for someone the game still calls
// "Nomad", i.e. it told the player a name the game had deliberately not given them yet -- a spoiler,
// and a divergence from the label on screen. The tester had already confirmed the gated behaviour was
// correct ("was Nomad 2 before, then Dania once interacted with") and reverted it on sight.
//
// The lesson is about the ORDER of the two questions. Reading the odd slot was a genuine fix for the
// mod reading the WRONG slot for characters the player had met. Extending it to characters they had
// NOT met was a separate decision dressed up as the same one, and it traded a correctness fix for a
// behaviour change nobody had asked for. Ship the fix; leave the behaviour alone.
//
// POINTER equality is still tested before decoding when the bit is set: the same s32 offset is the
// same bytes, so it is exactly equivalent to comparing the decoded strings and it keeps the common
// case (894 of 1141 ids share both slots) at a single decode.
std::wstring NpcdicDisplayName(int id) {
    const uint8_t* even = NpcdicSlot(id, /*odd=*/false);
    if (TalkNameKnown(id)) {
        const uint8_t* odd = NpcdicSlot(id, /*odd=*/true);
        if (odd && odd != even) {
            std::wstring personal = DecodeCodec(odd);
            if (!personal.empty()) { ++s_oddSlotWins; return personal; }
        }
    }
    return DecodeCodec(even);
}

}  // namespace

// The game's own (current-locale) display name for a field object, read memory-only
// from its SCENE OBJECT exactly as FUN_00263990 does: a name index at +0x102 selects
// the global npcdic dictionary; a negative index means a per-map custom string at
// +0xf8 (set by the map's fieldsignmes script). Empty on failure -> caller falls back
// to a category word. No game-function call — pure reads, SEH-guarded via MemRead.
//
// The odd-slot preference lives HERE rather than in the scanner on purpose: `entity_diag.cpp`'s
// dump and `interact_target.cpp`'s "what am I about to press Enter on" announcement both call this,
// so the list and the interaction prompt can never disagree about a person's name.
std::wstring ResolveObjectName(void* sceneObj) {
    if (!sceneObj) return L"";
    int16_t idx = 0;
    if (!SafeReadS16(sceneObj, NavRva::SCENEOBJ_NAME_IDX, &idx)) return L"";
    if (idx < 0)   // per-map custom string, written by the map's fieldsignmes script
        return DecodeCodec(
            reinterpret_cast<const uint8_t*>(PtrAt(sceneObj, NavRva::SCENEOBJ_NAME_STR)));
    return NpcdicDisplayName(static_cast<int>(static_cast<uint32_t>(idx) &
                                             NavRva::NPCDIC_NAME_MASK));
}

// (The current-area name is resolved via MapNames::CurrentMapId + ResolveMapName/ResolveRegionName —
//  FUN_003778b0 takes a MAP ID it was being called without, which returned the empty sentinel. See
//  CurrentAreaName below.)

// True when an npcdic name key falls in the field gimmick-object band (433-469:
// treasure, urn, crystals, anchor). Used so a named gimmick is always listed even if
// its interaction flag is momentarily clear.
bool InGimmickBand(int16_t nameIdx) {
    if (nameIdx < 0) return false;
    int id = static_cast<int>(static_cast<uint32_t>(nameIdx) & NavRva::NPCDIC_NAME_MASK);
    return id >= 433 && id <= 469;
}

// Category from the npcdic id band, then the scene CHARACTER class, then the engine's object KIND.
// ORDER IS THE WHOLE DESIGN HERE -- see the regression note below before touching it.
//
// 1. Named gimmick sub-types come from the npcdic id (sceneObj+0x102 when >= 0): ids 433-469 are the
//    field gimmick-object band. Treasure and crystals keep their OWN categories -- they are never
//    folded into Interactables. The exact bands are READ OFF THE GAME'S OWN npcdic NAME TABLE
//    (`FFXII-Decompile\notes\npcdic_names.csv`, extracted from `PS2Data\...\npcdic.bin`), not inferred:
//
//      433        "Anchor"                                         -> Object
//      434        "Treasure"          468 "Urn"                    -> Treasure
//      435-459    "Rabanastre Crystal" .. "Ridorana Crystal"       -> GateCrystal  (the 25 REAL ones)
//      460-465    "(Crystal 26)" .. "(Crystal 31)"                 -> GateCrystal  (unused placeholders)
//      466        "Gate Crystal"       -- the generic label        -> GateCrystal
//      467        "Life Crystal"      469 "Save Crystal"           -> SaveCrystal
//
//    SESSION 92 FIX: 435-459 used to be lumped into SaveCrystal on the guess "area/life crystals", so
//    the tester's first gate crystal -- id 435, which the game itself calls "Rabanastre Crystal" --
//    was announced under Save Crystal and the Gate Crystal filter read 0. Every one of 435-465 is a
//    per-AREA TELEPORT crystal; the name table says so in the game's own words. `Life Crystal` (467)
//    stays with Save Crystal, which is where it already was: it is a restorative dungeon crystal
//    rather than a teleport, that grouping is untouched by this fix, and nothing has reported it
//    wrong -- but it is the one line here NOT confirmed against play, so treat it as inherited.
// 2. `isCharacter` (scene category sceneObj+0x03 & 0x1f in 5-7, the classes carrying a char
//    component) => a person/actor => NPC. **People win over every kind test.**
// 3. Only THEN the KIND nibble (sceneObj+0x0E & 0xF), and only for non-characters: kind 5 is the
//    engine's ACTION gimmick -- the gate / door / switch / lever / well you press Enter on.
// 4. A talk-flagged leftover stays NPC, exactly as it did before this classifier changed.
//
// REGRESSION, caught in play (Session 54): an earlier version of this function tested `kind == 5`
// FIRST, ahead of `isCharacter`, and **every NPC was reclassified as Interactables**. That is an
// empirical refutation of the tidy "kind 5 = gimmick, kind 1 = person" reading taken from
// FUN_002675c0 / FUN_0025bad0: field NPCs evidently do NOT carry kind 1, so a kind test placed ahead
// of the character test swallows them. The kind nibble is still the right way to spot a GATE among
// non-characters -- FUN_002675c0's ACTION branch literally returns `(obj+0x0E & 0xF) == 5` -- but it
// is NOT a person-vs-object oracle, and the character class is the reliable "is it a person" test.
// Do not reorder these without the `'` dump's `kind=` field showing what field NPCs actually are.
//
// STRUCK (still): `if (flags & FLAG_TALK) return NPC` as the FIRST test. FUN_0025b820 branches on
// TALK and ACTION independently on the same object, so a gate with a confirm prompt is a talk target
// too -- which filed every one of them under NPC. It survives at step 4 only as the pre-existing
// fallback for objects the character and kind tests do not claim, so nothing that used to classify
// correctly changes; the ONLY behaviour that moves is a non-character kind-5 object.
//
// The spoken LABEL is always the game's own text; this only drives the category FILTER.
Category ClassifyByNameKey(uint32_t flags, int16_t nameIdx, bool isCharacter, uint8_t kind) {
    if (nameIdx >= 0) {
        int id = static_cast<int>(static_cast<uint32_t>(nameIdx) & NavRva::NPCDIC_NAME_MASK);
        if (id == 434 || id == 468)                    return Category::Treasure;   // Treasure, Urn
        if (id >= 435 && id <= 466)                    return Category::GateCrystal;// per-area + generic
        if (id == 467 || id == 469)                    return Category::SaveCrystal;// Life, Save
        if (id >= 433 && id <= 469)                    return Category::Object;     // misc gimmick
    }
    if (isCharacter) return Category::NPC;   // person/actor -- dominates every kind test
    if (kind == NavRva::KIND_ACTION_GIMMICK) return Category::Object;  // gate/door/switch/lever/well
    if (flags & NavRva::FLAG_TALK)           return Category::NPC;     // pre-existing fallback
    return Category::Object;                                           // non-character prop/sign
}

// Memory-only replica of the engine's own FUN_002675c0 -- "can the player interact with this object
// right now". Same order, same fields; nothing is CALLED (this runs on the input thread). A false
// here is what the F5 filter calls story-gated: the object exists and is listed, the game just
// won't act on it yet.
//
// The payload id (+0xCC action / +0xDC talk) is 0xFFFF when the object inherits it from the map's
// own object record; the engine then reads that record. We treat inherit as VALID rather than chase
// the fallback -- listing a gate we cannot fully resolve beats hiding one.
bool IsInteractionAvailable(void* sceneObj, uint8_t kind, uint32_t flags) {
    if (!sceneObj) return true;                       // fixed exit: no scene object -> never filtered
    // Only the two kinds the engine's predicate can answer for are candidates. A decorative sign or
    // a prop is not "story-gated", it is simply not interactive -- reporting it as gated would pad
    // the F5 list with things no story flag will ever open.
    if (kind != NavRva::KIND_ACTION_GIMMICK && kind != NavRva::KIND_TALK_TARGET) return true;
    uint8_t enable = 0;
    if (!SafeReadU8(sceneObj, NavRva::SCENEOBJ_ENABLE_OFF, &enable)) return false;
    if ((enable & NavRva::INTERACT_ENABLE_BIT) == 0) return false;    // the story gate
    uint8_t ready = 0;
    if (!SafeReadU8(sceneObj, NavRva::SCENEOBJ_READY_OFF, &ready) ||
        (ready & NavRva::READY_MODEL_BIT) == 0) return false;         // model not loaded
    uint8_t typeByte = 0;
    if (!SafeReadU8(sceneObj, NavRva::SCENEOBJ_TYPE_BYTE, &typeByte) ||
        (typeByte & NavRva::SCENEOBJ_CLASS_MASK) != NavRva::SCENEOBJ_CLASS_INTERACT) return false;

    uint16_t id = 0;
    if (flags & NavRva::FLAG_ACTION) {
        if (!SafeReadU16(sceneObj, NavRva::SCENEOBJ_ACTION_ID, &id)) return false;
        return kind == NavRva::KIND_ACTION_GIMMICK;
    }
    if (flags & NavRva::FLAG_TALK) {
        if (!SafeReadU16(sceneObj, NavRva::SCENEOBJ_TALK_ID, &id)) return false;
        return kind == NavRva::KIND_TALK_TARGET;
    }
    return false;   // neither prompt offered right now
}

} // namespace EntityScan
