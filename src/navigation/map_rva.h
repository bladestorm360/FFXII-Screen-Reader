#pragma once

#include <cstdint>

#include "core/phyre_types.h"

// Map-data RVAs and struct offsets: the exit arrays, exit destinations, the planmapname name
// tables, the in-game map's connection database, and the master-data name resolvers.
//
// Split out of nav_rva.h, where they were more than half the file and a different domain from the
// walkmap/scene-object addresses that header exists for. These are what map_query.cpp,
// map_names.cpp and map_exits.cpp read.
//
// RVAs are relative to FFXII_TZA.exe's image base and resolve via Hooks::ResolveRva(). Ghidra
// decompile addresses are ABSOLUTE (= RVA + 0x120000), so every RVA here is `Ghidra ABS - 0x120000`.
// Verify any change by adding 0x120000 back and matching the decompile symbol.
//
// NOTE ON LENGTH: this header is mostly PROVENANCE COMMENTS -- which reading was struck, why, and
// what replaced it. That commentary is the reason past sessions stopped re-deriving these addresses
// and re-shipping the same wrong ones. It is not padding; do not trim it to hit a line target.
namespace NavRva {

// ---- Map-jump EXIT ARRAY layout (per handle-table container; backs getmapjumpposbyindex) -----
// Derived from FUN_00264b90 (~0.85, PENDING confirmation via the '-key exit dump):
//   containerBase = HANDLE_TABLE_BASE + c*HANDLE_TABLE_STRIDE
//   exitBase = containerBase + *(u32)(containerBase + TBL_EXIT_OFF) + *(u32)(PhyreTypes::MASTERDATA_RELOC_BASE)
//   count    = *(u32)exitBase ; exit i: float x/y/z/angle at word [i*8 + 1..4] (EXIT_REC_STRIDE bytes)
constexpr uint32_t TBL_EXIT_OFF       = 0x54;       // u32 rel-offset to the exit sub-table (primary)
constexpr uint32_t TBL_EXIT_OFF_ALT   = 0x84;       // alt exit sub-table (destination-side positions)
// MAPJUMP_RELOC_BASE -> PhyreTypes::MASTERDATA_RELOC_BASE (battle_state.cpp declared the same
// address as RVA_RELOC). It is the master-data reloc delta, not a map-jump-specific one.
constexpr uint32_t EXIT_REC_STRIDE    = 0x20;       // bytes per jump record (from +0x54 table)
constexpr uint32_t EXIT_COUNT_MAX     = 64;         // sanity clamp on the exit count
// ---- Exit DESTINATION AREA id (mapData+0x8c) via FUN_00264920 by JUMP INDEX --------------------------
// FUN_00264920(idx, buf): reads mapData+0x8c record idx (16B, tbl+4+idx*0x10) -> *(u16)(buf+4) = dest
// AREA id (record word[5] @ +0x0a; 0xffff/<0 = none). It is a MAP ID — resolve via MapQuery::
// ResolveFullAreaName (FUN_00377b60 for the sub-area + the FUN_00264f90/FUN_00377870 hop for the region).
// `idx` is a FIELD-SIGN (+0x70) record's +0x1d byte — that is the ONLY correct key, per FUN_002648f0.
// STRUCK: "the getter can be called DIRECTLY with the +0x54 jump index i ... index join is ~0.5 -> confirm
// by the walk-test". Never do this: it was a 0.5-confidence guess (below the 0.98 bar), and +0x54 and +0x8c
// are unrelated spaces. The +0x70 path below supersedes it and needs no join at all.
constexpr uint32_t DEST_DESC_BY_INDEX    = 0x144920; // FUN_00264920(idx, buf) -> *(u16)(buf+4) = dest area id
constexpr uint32_t TBL_DEST_OFF          = 0x8c;    // rel-offset to the dest-id table (hand-walk fallback)
constexpr uint32_t EXIT_DEST_REC_STRIDE  = 0x10;    // bytes per dest record (8x u16)
constexpr uint32_t EXIT_DEST_AREAID_OFF  = 0x0A;    // u16 word[5] = destination AREA id
constexpr uint16_t AREAID_NONE           = 0xFFFF;  // sentinel: exit has no named destination

// ---- planmapname: TWO tables, ONE offset array. Get these backwards and every name is region-only. ----
// STRUCK (was: "MAPAREA_NAME_BY_ID = 0x257870 / planmapname is indexed BY MAP ID, so FUN_00377870(mapId)
// is the SUB-AREA"). DISPROVEN — it made ResolveAreaName return "" on every map, so every exit and the
// map-entry announcement spoke the REGION alone ("Nalbina Fortress"). FUN_00377870's argument was never a
// map id.
//
// The blob DAT_02add0f8 ('PLMN') carries two offset tables sharing the u16 array at +0x10:
//   +0x04 = countA = 1312  -> table A = AREA names,   idx = MAP ID          -> FUN_00377b60(i): i < *(u32)(blob+4)
//   +0x08 = countB = 59    -> table B = REGION names, idx = REGION index    -> FUN_00377870(i): i < *(u32)(blob+8),
//                             read at array slot (countA + i) — i.e. rebased PAST table A
//   +0x0c = 1371 = countA + countB (combined array length; what parse_planmapname.py walks, which is why
//                  notes/planmapname_areas.csv holds BOTH tables concatenated: idx<1312 = areas,
//                  idx>=1312 = regions. 274="Aerial Gardens" (area); 1327="Nalbina Fortress" (region 15).)
// So FUN_00377870(279) fails its `279 < 59` bounds check and returns the EMPTY_STRING sentinel — exactly
// the sub="" seen live. PROVEN 4 ways: both decompiles; the header arithmetic; the game's own map resolver
// FUN_003c2320 (type-0 path, sole writer of out+0x10, = FUN_00377b60(rec+0x0c)) probe-logged returning
// Aerial Gardens/Inner Ward/Lower Apartments/Upper Apartments/The Highhall for 274/275/279/280/282; and the CSV.
constexpr uint32_t MAPAREA_NAME_BY_ID    = 0x257B60; // FUN_00377b60(mapId)    -> SUB-AREA name codec ptr (table A)
constexpr uint32_t MAPREGION_NAME_BY_IDX = 0x257870; // FUN_00377870(regionIdx)-> REGION name codec ptr (table B).
                                                     // NOT a map id — feed it MAP_NAME_INDEX_BY_ID's output.
// FUN_00264f90(mapId) -> planmapname REGION INDEX (map-master DAT_02099d88, record+6). The game's own
// region resolver (FUN_003145e0 / the HUD / FUN_003778b0) is FUN_00377870(FUN_00264f90(mapId)).
constexpr uint32_t MAP_NAME_INDEX_BY_ID  = 0x144F90;
constexpr int      MAP_ID_MAX            = 8191;      // sanity bound before the getter (map ids are small)

// RETIRED: EXIT_JUMP_DESTIDX_OFF (0x1d on a +0x54 JUMP record). The +0x54 records carry x/y/z/angle ONLY
// — FUN_00264b90 is the sole reader of that table in the whole binary and touches just word[i*8+1..4];
// bytes +0x10..+0x1f are read by NOTHING. The old chain read dead bytes and returned destIdx=0 / areaId
// 0xffff on every record of every map (confirmed in the live log). +0x1d is real, but only on the +0x70
// FIELD-SIGN records below — a different table.

// CORRECTION (this session): Session 43's claim that the +0x54 table is the party ARRIVAL/SPAWN table
// rather than the exits is WRONG, and is reverted. Its sole evidence was "FUN_00353490 places the party
// from it" — but FUN_00353490 is abs 0x353490 = RVA 0x233490 = GETMAPJUMPANGLEBYINDEX above, the script
// native `getmapjumpanglebyindex`; it returns a jump's angle and places nothing. mapData+0x54 is the
// MAP-JUMP POINT table, i.e. the intra-map "Mapjump" exits (Inner Ward -> Upper Apartments), as Sessions
// 39-42 had it and as the tester confirmed by walking into them.

// ---- Map EXIT DESTINATIONS = field-sign array at mapData+0x70 (FUN_00264ae0/002649b0/002648f0) --------
// This is the curated list the game draws as radar blips / 3D "→ <area>" arrows, and the ONLY source of
// exit destination names. Read via the game's own side-effect-free getters (they apply the ETB
// indirection + leader-visibility filter + story gate):
//   count = FUN_00264ac0(group); obj = FUN_002649b0(group, i) (null if not shown);
//   FUN_002648f0(obj, buf) -> buf{b0 usable, .., areaId u16 @+4}.
//
// STRUCTURE (FUN_00264ae0): +0x70 is a GROUP-OFFSET table, not an inline record array:
//   groupTable = blob + *(u32)(blob+0x70) + reloc = [u32 groupCount][u32 groupOff_0][u32 groupOff_1]...
//   group g sub-table = blob + groupOff_g = [u32 count][12B hdr][rec x 0x20]; rec_i = sub + 0x10 + i*0x20
//   FUN_002649b0 applies the story/visibility mask (rec+0x1c) only for group < 2.
//
// ABI (was a live bug): FUN_00264ac0 and FUN_00264ae0 take a GROUP index in ecx. Ghidra shows them as
// taking no args because FUN_00264ac0 never WRITES ecx — it passes its own incoming ecx through. The mod
// declared the count getter as `int(*)()` and called it with no argument, so it read whatever junk was in
// rcx, FUN_00264ae0's `param_1 < groupCount` bounds check failed, and the count came back 0 on EVERY map.
// "map-exits(+0x70): count=0" therefore never measured the data — it measured this bug. (The offline
// parser's matching "+0x70 is 0 in all 550 .mpk" is also not credible: it reports +0x8c = 0 too, yet the
// live log shows +0x8c populated with destCount=2, so it is reading the wrong blob base.)
constexpr uint32_t TBL_FIELDSIGN_OFF     = 0x70;      // u32 rel-offset to the field-sign GROUP table
constexpr uint32_t MAPEXIT_GROUP_MAX     = 4;         // groups to enumerate (sanity bound)
constexpr uint32_t MAPEXIT_COUNT         = 0x144AC0;  // FUN_00264ac0(group) -> int exit count in group
constexpr uint32_t MAPEXIT_OBJ_BY_INDEX  = 0x1449B0;  // FUN_002649b0(uint group, int i) -> record* / null
constexpr uint32_t MAPEXIT_DESTINFO      = 0x1448F0;  // FUN_002648f0(record*, buf*) fills buf (usable+areaId)
// FUN_00264ae0(group) -> the group's exit sub-table: [u32 count][0x0c hdr][rec x 0x20]. This is what both
// getters above call. We walk it directly because FUN_002649b0 additionally applies a RENDER gate for
// group<2 — it returns null unless the record's +0x1c per-member visibility bit is set for the member
// FUN_00377860() reports, i.e. unless the 3D "-> area" arrow is on screen this instant. Live-proven: the
// prologue draws no arrows, so all 3 of Nalbina Lower Apartments' records came back null. Exits must be
// listed because they EXIST, not because they're being drawn; the STORY gate we do want is reported
// separately by FUN_002648f0's buf[0]. Record math is FUN_002649b0's own: rec_i = base + 0x10 + i*0x20.
constexpr uint32_t MAPEXIT_TABLE_BY_GROUP = 0x144AE0; // FUN_00264ae0(group) -> group sub-table base / null
constexpr uint32_t MAPEXIT_TBL_HDR       = 0x10;      // bytes before record 0 (count u32 at +0x00)
constexpr uint32_t EXITREC_X_OFF     = 0x00;   // float world X
constexpr uint32_t EXITREC_Y_OFF     = 0x04;   // float world Y (height)
constexpr uint32_t EXITREC_Z_OFF     = 0x08;   // float world Z
constexpr uint32_t EXITREC_ENABLE_OFF = 0x0C;  // float; != 0 => drawn/shown
constexpr uint32_t EXITREC_VIS_OFF   = 0x1C;   // u8 per-member visibility mask
constexpr uint32_t EXITREC_DESTGRP_OFF = 0x1D; // u8 dest-area-group byte (used by FUN_002648f0)
constexpr uint32_t EXITBUF_USABLE_OFF = 0x00;  // buf: b0 != 0 => story gate satisfied (usable now)
constexpr uint32_t EXITBUF_AREAID_OFF = 0x04;  // buf: u16 destination area id (0xffff = none)

// ---- Minimap / naviicon MARKERS — RETIRED Session 44 -------------------------------------------------
// The naviicon flat array (DAT_02b45a80 / _DAT_02b45a70, stride 0x20) was surfaced in Session 43 on the
// HYPOTHESIS (only 0.6 conf on the subtype) that it carried save/gate-crystal/target/OBJECTIVE icons.
// Two follow-up decompile traces DISPROVED that: the array holds ONLY character/unit dots (party / ally /
// enemy / neutral), each a 1:1 duplicate of a live scene object the combatant + handle-table scans already
// list. There is NO objective/waypoint marker (zero setnaviicon/objective code in the whole decompile), NO
// crystal, NO treasure in it, and marker[+0x04] was the entity HANDLE, not a label id. Enumerating it added
// nothing and an Objective category had no source here. The consts (NAVIICON_*/MARK_*) and the enumerator
// are removed; keeping this note so the dead hypothesis is on record and not re-attempted.

// ---- Name resolution (the game's own master data, read memory-only) ----------
// Each field object stores its name key on its SCENE OBJECT (*(actor+0x10)) — read
// by the game's own resolver FUN_00263990(sceneObj): idx = *(s16)(sceneObj+0x102).
// If idx >= 0 the name is npcdic[idx]; if idx < 0 the name is a per-map custom
// string pointer at *(sceneObj+0xf8) (set by the map's fieldsignmes script). This
// replaces the old FUN_0035d380(1, def+4) call, which was the party/roster resolver
// fed a model index and always failed (-> the "Object" fallback).
constexpr uint32_t SCENEOBJ_NAME_IDX = 0x102;  // *(s16): >=0 npcdic index, <0 -> use +0xf8
constexpr uint32_t SCENEOBJ_NAME_STR = 0xf8;   // *(codec*): per-map custom string (when idx<0)
// npcdic.bin ("NPC0") is loaded once at boot into DAT_02b5e0d8 (resource cat 9/id
// 0x1f); the global holds the blob base pointer. Lookup (FUN_003eac10): slot = id*2
// (base name; odd slot = yomi/reading); name codec* = *(s32)(base + 0xc + slot*4).
constexpr uint32_t NPCDIC_BASE       = 0x2A3E0D8;  // DAT_02b5e0d8 (ptr to npcdic blob)
constexpr uint32_t NPCDIC_COUNT_OFF  = 0x08;       // *(int)(blob+8) = slot count (name+yomi pairs)
constexpr uint32_t NPCDIC_TABLE_OFF  = 0x0c;       // s32 offset table at blob+0xc, indexed by slot
constexpr uint32_t NPCDIC_NAME_MASK  = 0xffffbfff; // idx & this = npcdic id (game masks bit 14)
// AREA NAMES — the two paths are NOT interchangeable. See the planmapname table-A/B block above for the
// authoritative layout; the constants live there (MAPAREA_NAME_BY_ID / MAPREGION_NAME_BY_IDX).
//   SUB-AREA ("Lower Apartments") = FUN_00377b60(mapId)                 -> MAPAREA_NAME_BY_ID    = 0x257B60
//   REGION   ("Nalbina Fortress") = FUN_00377870(FUN_00264f90(mapId))   -> MAPREGION_NAME_BY_IDX = 0x257870
//
// STRUCK (was: "SUB-AREA = FUN_00377870(mapId) DIRECT ... planmapname's offset table is indexed BY MAP ID").
// DISPROVEN: FUN_00377870 bounds-checks against countB=59 (regions), so a map id like 279 always missed and
// returned the empty sentinel — that is why every exit AND the map-entry announcement spoke the region alone.
// The CSV's `mapid` column is the concatenated array index, not FUN_00377870's argument. The sub-area
// resolver is FUN_00377b60 (table A, bounds countA=1312), which this note had recorded as "NOT USED".
//
// FUN_003778b0(mapId) is the region pair above, and it TAKES a map id in RCX (Ghidra mislabels it (void)
// because it forwards RCX) — calling it argument-less returns the empty sentinel. Same register-passthrough
// trap as FUN_00264f90 and the FUN_00264ac0/FUN_00264ae0 group getters: assume Ghidra's "(void)" is a lie
// whenever the callee's first act is to use ECX/RCX.
constexpr uint32_t CURRENT_AREA_NAME = 0x2578B0;   // FUN_003778b0(mapId) = FUN_00377870(FUN_00264f90(mapId)) = REGION
constexpr uint32_t GETMAPID_FIELD    = 0x1F48F0;   // FUN_003148f0() -> gameState+0x1044 current map id (neg->0)
// DAT_01ceb638 = the shared empty-string sentinel ("") the name paths return on miss.
constexpr uint32_t EMPTY_STRING      = 0x1BCB638;

// ---- MAP-CONNECTION resolver: the in-game map's OWN exit -> destination database ---------------------
// This is the source the area/world map uses to label the surrounding LINKED areas (the DQ7R equivalent),
// and the game keeps it live for the field minimap from area entry onward — so we just CALL its getters.
// CONFIRMED live (probe, Nalbina subMap 9, 5 exits): record+0x0c = destination planmapname MAP ID ->
// ResolveMapName() gave "Aerial Gardens"(274) / "Inner Ward"(275) / "Lower Apartments"(279) /
// "Upper Apartments"(280) / "The Highhall"(282), matching planmapname_areas.csv exactly.
//   subMap = FUN_003bfdf0()            -> current sub-map index (does the map-id -> index mapping itself)
//   n      = FUN_003c0340(subMap)      -> exit count (adjacent first, then doors)
//   rec    = FUN_003bf430(subMap,i,&t) -> 0x18-byte record; t = 0 adjacent / 1 door / 3 invalid
constexpr uint32_t MAP_SUBMAP_INDEX  = 0x29FDF0;  // FUN_003bfdf0() -> current sub-map index
constexpr uint32_t MAP_CONN_COUNT    = 0x2A0340;  // FUN_003c0340(subMap) -> exit count
constexpr uint32_t MAP_CONN_RECORD   = 0x29F430;  // FUN_003bf430(subMap, i, &type) -> record*
constexpr uint32_t MAP_SUBMAP_NONE   = 0xFFFFFFFF; // FUN_003c0380 "not found" sentinel
constexpr uint32_t CONNREC_MAPX_OFF  = 0x00;      // s16 map-space X (minimap marker coords, NOT world)
constexpr uint32_t CONNREC_MAPY_OFF  = 0x02;      // s16 map-space Y
constexpr uint32_t CONNREC_DEST_OFF  = 0x0C;      // u16 DESTINATION planmapname map id  <-- the linkage
constexpr int      CONNREC_TYPE_DOOR = 1;         // type 1 = door/jump exit (0 = adjacent sub-map)

// ---- (STRUCK) Exit -> DESTINATION MAP id via the scene pool / field-sign site table ------------------
// Disproved at runtime: the scene-pool +0x714 / DAT_022c8060 site table are BATTLE data (BtlChr roster /
// target list), empty for exits; and the +0x8c dest table is indexed by a field-sign record's +0x1d byte,
// NOT the jump index (nothing pairs +0x8c[i] with +0x54[i]). Kept only so they are not re-attempted.
// 0.8-0.85, RUNTIME-CONFIRM before trusting. Each exit's scene object carries its destination map id at
// +0x714, set per region-change from the map-connection table (DAT_02ebf190). Also mirrored into the
// field-sign site record table. Same id space as gameState+0x1044 -> ResolveMapName(dest) names it.
constexpr uint32_t SCENE_POOL_ACCESSOR = 0x116820;  // FUN_00236820(i) -> pool object/handle for slot i
constexpr uint32_t SCENE_POOL_COUNT_FN = 0x116850;  // FUN_00236850() -> pool slot count
// SCENE_POOL_BASE / SCENE_POOL_COUNT were a second name for the SAME globals as the actor pool
// (0x1F6E688 / 0x1F6E6A0); both were unused. Use PhyreTypes::ACTOR_POOL_BASE / ACTOR_POOL_COUNT.
constexpr uint32_t SCENE_POOL_STRIDE   = 0xF50;     // bytes per pool object
constexpr uint32_t POOL_HANDLE_OBJ_OFF = 0x30;      // *(rec+0x30) -> object (FUN_00263e30), if accessor gives a handle
constexpr uint32_t POOL_OBJ_CONN_OFF   = 0x698;     // ptr -> map-connection entry (!=0 => exit gimmick)
constexpr uint32_t POOL_OBJ_DEST_OFF   = 0x714;     // u16 destination map id (0xffff none; 0x9f dynamic)
constexpr uint32_t POOL_OBJ_MODE_OFF   = 0x6B4;     // u8 mode (0 none / <9 raw dest / >=9 dynamic)
constexpr uint32_t POOL_OBJ_ARRIVAL_OFF = 0x710;    // u32 arrival jump-index on the destination map
constexpr uint32_t POOL_OBJ_POS_OFF    = 0xE0;      // field-actor world pos x/y/z (GameArchitecture.md:728)
constexpr uint32_t DYNAMIC_DEST_RESOLVE = 0x267090; // FUN_00387090(obj) -> dest map id for the 0x9f case
constexpr uint16_t DEST_DYNAMIC_SENTINEL = 0x9f;    // obj+0x714 == 0x9f => dynamic (party-dependent) dest
// Field-sign site record table (mirror; may be empty when no on-screen placards, like +0x70):
constexpr uint32_t SITE_TABLE_COUNT_FN = 0x23BB60;  // FUN_0035bb60() -> site count
constexpr uint32_t SITE_TABLE_HEADER   = 0x21A8060; // DAT_022c8060 (control block; +4 count, +0x20 recs)
constexpr uint32_t SITE_TABLE_COUNT    = 0x21A8064; // DAT_022c8064 (int site count)
constexpr uint32_t SITE_TABLE_RECS     = 0x21A8080; // DAT_022c8080 (record array base, stride 0xc0)
constexpr uint32_t SITE_REC_STRIDE     = 0xC0;      // bytes per site record
constexpr uint32_t SITE_REC_SITEID_OFF = 0x04;      // int site id (ties record to an exit)
constexpr uint32_t SITE_REC_DEST_OFF   = 0x5C;      // u16 destination map id (<0x4000 valid; 0xffff none)

} // namespace NavRva
