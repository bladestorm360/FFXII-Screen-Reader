#include "navigation/entity_classify.h"
#include "navigation/nav_rva.h"
#include "navigation/map_rva.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/phyre_types.h"
#include "core/game_text.h"

#include <Windows.h>

using namespace MemRead;

namespace EntityScan {

using EntityList::Category;

const wchar_t* CategoryWord(Category c) {
    switch (c) {
        case Category::All:         return L"All";
        case Category::Exit:        return L"Exit";
        case Category::SaveCrystal:  return L"Save Crystal";
        case Category::GateCrystal:  return L"Gate Crystal";
        case Category::Treasure:    return L"Treasure";
        case Category::NPC:         return L"NPC";
        case Category::Object:      return L"Interactables";
        case Category::Enemy:       return L"Enemy";
        default:                    return L"Interactables";
    }
}

// npcdic codec-string pointer for a name index — replicates FUN_003eac10 (the game's
// own npcdic lookup) memory-only. The blob is loaded once at boot; DAT_02b5e0d8 holds
// its base. Even slot = display name (odd = yomi/reading). The offset table stores
// relocated absolute pointers as s32, read here exactly as the game does; 0 /
// out-of-range -> null.
const uint8_t* NpcdicName(int id) {
    void* blob = PtrAt(Hooks::ResolveRva(NavRva::NPCDIC_BASE), 0);
    if (!blob) return nullptr;
    uint32_t count = 0;
    if (!SafeReadU32(blob, NavRva::NPCDIC_COUNT_OFF, &count)) return nullptr;
    uint32_t slot = static_cast<uint32_t>(id) * 2;
    if (slot >= count) return nullptr;
    uint32_t entry = 0;
    if (!SafeReadU32(blob, NavRva::NPCDIC_TABLE_OFF + slot * 4, &entry) || entry == 0)
        return nullptr;
    // Sign-extend the s32 to a full pointer, as the game does (blob mapped low).
    return reinterpret_cast<const uint8_t*>(
        static_cast<intptr_t>(static_cast<int32_t>(entry)));
}

// The game's own (current-locale) display name for a field object, read memory-only
// from its SCENE OBJECT exactly as FUN_00263990 does: a name index at +0x102 selects
// the global npcdic dictionary; a negative index means a per-map custom string at
// +0xf8 (set by the map's fieldsignmes script). Empty on failure -> caller falls back
// to a category word. No game-function call — pure reads, SEH-guarded via MemRead.
std::wstring ResolveObjectName(void* sceneObj) {
    if (!sceneObj) return L"";
    int16_t idx = 0;
    if (!SafeReadS16(sceneObj, NavRva::SCENEOBJ_NAME_IDX, &idx)) return L"";
    const uint8_t* codec =
        (idx < 0) ? reinterpret_cast<const uint8_t*>(PtrAt(sceneObj, NavRva::SCENEOBJ_NAME_STR))
                  : NpcdicName(static_cast<int>(static_cast<uint32_t>(idx) & NavRva::NPCDIC_NAME_MASK));
    void* empty = Hooks::ResolveRva(NavRva::EMPTY_STRING);
    if (!codec || reinterpret_cast<void*>(const_cast<uint8_t*>(codec)) == empty) return L"";
    std::wstring s = GameText::Decode(codec, 256);
    return GameText::IsMostlyPrintable(s) ? s : std::wstring();
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
//    field gimmick-object band (434 Treasure, 468 Urn, 466 Gate Crystal, 469 Save Crystal,
//    435-459/467 area/life crystals). Treasure and crystals keep their OWN categories -- they are
//    never folded into Interactables.
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
        if (id == 434 || id == 468)                    return Category::Treasure;
        if (id == 466)                                 return Category::GateCrystal;
        if (id == 469 || id == 467 || (id >= 435 && id <= 459))
                                                       return Category::SaveCrystal;
        if (id >= 433 && id <= 469)                    return Category::Object;   // misc gimmick
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
