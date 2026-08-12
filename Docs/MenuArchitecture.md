# FFXII Menu Architecture

This document captures the empirical menu architecture for `FFXII_TZA.exe` based on
static decompile analysis (Ghidra, 33,105 functions, image base `0x120000`). It is
narrower and more accurate than the older "Menu State Machine" section in
`GameArchitecture.md` and should be considered the source of truth for the
accessibility mod's menu hooks.

**Last updated:** 2026-05-11.

## Address Convention

- Project docs use **RVAs** (relative virtual addresses).
- Ghidra's decompile (`decompile_all.txt` and `output/decompile/*.c`) uses
  **absolute addresses** = RVA + `0x120000`. Globals appear as `DAT_<absolute>`
  (lowercase hex), e.g. RVA `0x1E5F5F0` → `DAT_01f7f5f0`.
- Cursor singletons documented as `<static_RVA> + <field_offset>` are **NOT** valid
  as a flat read. The static RVA is a POINTER variable in the data section; the
  field offset applies to the heap object after dereferencing the pointer.
  Correct read: `*((u64*)(base + static_RVA)) + field_offset`.

## Layer 1: Universal Render Pipeline (already mapped)

Every per-frame text/UI draw flows through:

```
FUN_0017f850 (RVA 0x5F850, "display : draw Font" per-frame entry)
  ├── FUN_001eb150("display : draw Font")     -- profiling marker
  ├── FUN_001b5fa0()                          -- font-scene singleton getter (28 callers)
  └── FUN_001e8580 (RVA 0xC8580, 3,572 B)     -- vertex dispatcher
        └── DynGeoFontTextInstance + siblings -- post-rasterization geometry submit
```

The 22-wrapper cluster (`FUN_0017f650`–`FUN_0017fd30`) are the typed
text/UI-state-set API: each wrapper calls `FUN_001b5fa0()` then a
`FUN_001e9XXX` worker. Wrappers cover: font property setters, struct
submissions, draw flushes, dimension queries, etc. **Source text passes through
these wrappers as parameters and is consumed before reaching the
`DynGeoFontTextInstance` layer** — by the time a text-instance object is alive,
its source string has been rasterized to vertex quads.

## Layer 2: Menu Registry (FFXII-specific)

> **RVA correction (2026-05-12):** earlier revisions of this section listed
> the registry's static RVA as `0x208EA60`. That fails the
> `absolute = RVA + 0x120000` convention stated above: actual sum is
> `0x208EA60 + 0x120000 = 0x21AEA60`, not the Ghidra-stated `0x228EA60`.
> A first-pass correction in this same session tried `0x108EA60`; a Frida
> probe at runtime read non-pointer garbage at `base + 0x108EA60`
> (= `0x11AEA60`), confirming that was also wrong. The **correct RVA is
> `0x216EA60`** (`0x228EA60 − 0x120000 = 0x216EA60`; high two hex digits
> shift `0x22 → 0x21` when subtracting `0x12_0000`, just like the doc's
> own `0x1F → 0x1E` example for `DAT_01f7f5f0`/RVA `0x1E5F5F0`). All four
> registry-related RVAs in this section and the critical-RVAs table have
> been corrected. The Ghidra symbol names (`DAT_0228ea60`, etc.) were
> always correct.

FFXII registers active menu instances in a small static table.

| Symbol | RVA | Description |
|---|---|---|
| `DAT_0228ea60` | `0x216EA60` | **Menu registry table**. Static array of 8-byte pointer slots indexed by menu-type byte. Slot N holds the pointer to the currently-active menu of type N, or 0 if none. |
| `DAT_0228ea68` | `0x216EA68` | Slot 1 of the registry (also referenced directly during type-1 close path). |
| `DAT_0228ea38` | `0x216EA38` | Holds the widget-instance pointer that, when matched, gets assigned **menu type 2**. |
| `DAT_0228ea50` | `0x216EA50` | Holds the widget-instance pointer that, when matched, gets assigned **menu type 4**. |
| `FUN_00241a50` | `0x121A50` (20 B) | **"Is menu of type N open?" query**. `bool FUN_00241a50(int menu_type)` returns `DAT_0228ea60[menu_type] != 0`. |
| `FUN_00241d40` | `0x121D40` (2,160 B) | **Menu state-machine controller / cursor renderer.** Receives `(menu_obj, opcode_ptr)`. Dispatched via function pointer (no direct C-level callers in decompile). Handles init (case 1), drawing (cursor + animation), and lifecycle. Registers `param_1` into `DAT_0228ea60[menu_obj->0x3c8]` on init (line 226771) and unregisters on close (line 227048). |

### Menu types observed

| Type byte | Selector | Notes |
|---|---|---|
| 1 (default) | "any widget not matching type 2 or 4" | Catch-all generic menu. Queried by `FUN_00241a50(1)` at lines 277404, 279062. |
| 2 | third qword of the query record equals `DAT_0228ea38` | Specific menu (widget identity TBD). Queried at lines 207000, 207125. |
| 4 | third qword of the query record equals `DAT_0228ea50` | Specific menu (widget identity TBD). Not directly queried in decompile. |

**Type 1 is queried in two places, type 2 in two places, type 4 never queried.**
Only three concrete types observed.

**Whether the title menu registers into this table is NOT YET CONFIRMED.** Earlier
in this session I claimed it does not, based on (a) not finding a title-specific
code path that sets `param_1 + 0x3c8`, and (b) `probe_menu_writers` seeing no
candidate writer fire on title arrow presses. **Both are weak evidence.** I never
traced what calls `FUN_00241d40` and with what query-record value — title might use
the default path (→ type 1 catch-all). The probe_menu_writers candidates were
the wrong set of functions (none of them was `FUN_00241d40`).

The architectural argument is the load-bearing one: **why would Square reinvent
the menu system for one screen?** It wouldn't. The `fsttl_*` script symbols
(`fsttl_newgamestart`, `fsttl_opencommand`, etc.) are most likely action
handlers — what happens when an option is selected — not the menu implementation
itself. The menu cursor / option list / rendering is almost certainly the same
C++ architecture as every other menu.

**Next session, first move:** read `DAT_0228ea60` at runtime while on the title
screen. Any non-null slot means the title menu uses this architecture and we
can read its focus state directly. Only if all slots are null on the title
screen does the .ebp/script-side investigation become necessary.

### How to read menu context at runtime

The accessibility mod can determine **which menu is currently open** by reading
the registry at static RVA `0x216EA60`. The registry is a `void* table[N]`
indexed by the menu-type byte at `menu_obj + 0x3c8`; each slot is 8 bytes,
so `table[N]` lives at byte offset `N * 8`. The `DAT_0228ea68` symbol
(byte +8) labeled "Slot 1, type-1 close path" confirms this — type 1 is at
+8, NOT at +0:

```c
// Pseudo-C; in Frida this is base.add(0x216EA60 + 8 * N).readPointer()
void* active_type1 = *(void**)(image_base + 0x216EA60 + 8 * 1);  // catch-all
void* active_type2 = *(void**)(image_base + 0x216EA60 + 8 * 2);
void* active_type4 = *(void**)(image_base + 0x216EA60 + 8 * 4);
// Non-null slots = menu open
```

A frame-tick poll of these 3 slots tells us the menu context.

## Layer 3: Cursor Sprite Renderer

The cursor draw call is **inside `FUN_00241d40`**, at line 226935 of
`decompile_all.txt`:

It calls `FUN_00243f70(target, x, y, 1)` — variant `1` is the cursor — with each coordinate summed
from three sources:

| axis | window fields | global |
|---|---|---|
| X | `+0x3B8` (`u16`, item X) + `+0x3BA` (`u16`, X offset adjustment) | `DAT_01E0C148` |
| Y | `+0x9E` (`i16`, item Y) + `+0x3C2` (`i8`, animation Y bob) | `DAT_01E0C14C` |

### Cursor sprite is the arrow icon

The 11 cursor-direction pairs configured by `FUN_001ca600` (`mCursorRightX/Y`,
`mCursorOpenX`, `m2DSideCursorCenterX/Y`, etc.) are stored at:

| UI manager singleton offset | Points to | Meaning |
|---|---|---|
| `+0x3b68` | `DAT_01e0c120` | Cursor offset entry 0 |
| `+0x3b70` | `DAT_01e0c124` | Cursor offset entry 1 |
| `+0x3b78` | `DAT_01e0c128` | Cursor offset entry 2 |
| `+0x3b80` | `DAT_01e0c12c` | Cursor offset entry 3 |
| `+0x3b88` | `DAT_01e0c130` | Cursor offset entry 4 |
| `+0x3b90` | `DAT_01e0c134` | Cursor offset entry 5 |
| `+0x3b98` | `DAT_01e0c138` | Cursor offset entry 6 |
| `+0x3ba0` | `DAT_01e0c13c` | Cursor offset entry 7 |
| `+0x3ba8` | `DAT_01e0c140` | Cursor offset entry 8 |
| `+0x3bb0` | `DAT_01e0c144` | Cursor offset entry 9 |
| `+0x3bb8` | `DAT_01e0c458` | 2DSideCursorCenterX |
| `+0x3bc0` | `DAT_01e0c45c` | 2DSideCursorCenterY |
| `+0x3fb8` | `DAT_01e0c148` | Used by `FUN_00241d40` for cursor-draw X offset |
| `+0x3fc0` | `DAT_01e0c14c` | Used by `FUN_00241d40` for cursor-draw Y offset |

The UI manager holds POINTERS to these static config addresses. Init function
`FUN_001af470` (RVA `0x8F470`, 3,179 B) populates the pointers; config-loader
`FUN_001ca600` (RVA `0xAA600`, 8,186 B) populates the VALUES from `UISize.json`.

**`FUN_00241d40` reads the offset values DIRECTLY from the static globals**
(`_DAT_01e0c148`, `_DAT_01e0c14c`) — Ghidra's underscore prefix convention for
direct static reads. It does NOT bounce through the UI manager pointers.

### Cursor animation

Lines 226916–226934 of `FUN_00241d40` implement a 2-state cursor bob animation:

| Field on menu object | Type | Role |
|---|---|---|
| `+0x3bc` | char (bool) | Cursor visible flag (drawn iff != 0) |
| `+0x3c0` | byte | Animation phase (0 = idle, 1 = active) |
| `+0x3c1` | char | Frame counter within phase |
| `+0x3c2` | char | Current Y offset (0 or 4 pixels — the visual "bob") |

Phase 0 holds Y offset at 0 for 21 frames, then switches to phase 1 (Y offset = 4)
for 5 frames, then returns to phase 0. Classic arrow-cursor pulse.

## Layer 4: Per-Menu Focus State

Each menu object (the `param_1` of `FUN_00241d40`) is a large struct (≥ `0x3D0` bytes)
holding menu-specific state. Key fields observed:

| Offset | Type | Role |
|---|---|---|
| `+0x9e` | short | **Y position of currently-focused item** (in pixels, relative to menu) |
| `+0x48` | pointer | (Probably a vtable or callback pointer) |
| `+0xc0` | uint | Visibility/active flag |
| `+0xc8` (200) | longlong | Sub-widget pointer (the actual widget being managed) |
| `+0xd0` | pointer | A string/widget pointer |
| `+0xe0` | sub-object | Embedded sub-object (font-renderer state?) |
| `+0x1b0` | byte[0x200] | Buffer (likely menu title or item-text scratch) |
| `+0x3b8` | ushort | **X position of currently-focused item** |
| `+0x3ba` | ushort | X position adjustment |
| `+0x3bc` | char | Cursor visible flag |
| `+0x3be` | char | Some flag |
| `+0x3bf` | char | Orientation (e.g., right-side cursor vs left-side) |
| `+0x3c0..+0x3c2` | bytes | Cursor animation state (see above) |
| `+0x3c3` | char | Flag |
| `+0x3c4` | uint | Bit-flags (visibility, special render modes) |
| `+0x3c8` | byte | **Menu type ID** (1/2/4) — registers into `DAT_0228ea60[+0x3c8]` |

**Important:** `+0x3b8` and `+0x9e` are the POSITIONS of the currently-focused
item, not the focus INDEX. The index→position translation happens earlier, before
`FUN_00241d40` is invoked with the draw-cursor case. To find the focus INDEX
specifically, we need to read the menu's sub-widget pointer at `+0xc8`/`+200` and
inspect ITS fields — that's where the per-item list and current-index typically
live in widget-tree architectures.

## What this means for the accessibility mod

### Menu context (which menu is open)

Read three pointer slots at runtime:

- `*(u64*)(base + 0x216EA60 + 8)` — currently-active type-1 menu (catch-all)
- `*(u64*)(base + 0x216EA60 + 16)` — currently-active type-2 menu
- `*(u64*)(base + 0x216EA60 + 32)` — currently-active type-4 menu

Non-null = that menu is open. The pointer is the menu object — `param_1` of
`FUN_00241d40`. **All known fields are documented above** so reading the menu's
state once we have the pointer is straightforward.

### Cursor focus position (where the cursor is rendered)

For an active menu's pointer `m`:

- Cursor X: `(*(u16*)(m + 0x3b8)) + (*(u16*)(m + 0x3ba)) + (*(u32*)(base + 0x1E0C148))`
- Cursor Y: `(*(s16*)(m + 0x9e)) + (*(s8*)(m + 0x3c2)) + (*(s32*)(base + 0x1E0C14C))`
- Cursor visible: `*(u8*)(m + 0x3bc) != 0`

### Cursor focus INDEX (which item, not which pixel)

**Not yet identified.** The index lives somewhere on the sub-widget at
`*(u64*)(m + 0xc8)`. Phase 2 of the plan locates this by hooking `FUN_00241d40`
and inspecting the sub-widget's fields when the cursor renders.

### Title menu

**STATUS: UNCONFIRMED.** Earlier in this session I claimed the title menu does
not use this architecture. That claim was unsupported — see "Menu types
observed" section above for the retraction. The architectural argument cuts
the other way: the game would not reinvent the menu system for one screen.

**Working hypothesis:** title menu IS in this architecture, likely as type 1
(catch-all). The `fsttl_*` script symbols (`fsttl_newgamestart`, `fsttl_opencommand`,
`fsttl_openconfig`, `set_title_flag`, `get_title_flag`) are most likely **action
callbacks** — what happens when an option is selected — not the menu
implementation itself.

**First test next session:** read `DAT_0228ea60` at runtime while on the title
screen.
- Non-null slot → title menu IS in this architecture; we can read focus state
  directly using the field layout above.
- All slots null → title menu actually is separate, and the `.ebp` interpreter
  hunt becomes necessary.

## Open issues (for Phase 2)

1. **Title menu architecture — confirm or refute** by reading `DAT_0228ea60`
   (RVA `0x216EA60`) at runtime on title screen. Cheapest test in the project:
   a 30-second probe. **Implemented** as
   `FFXII-Decompile/frida/probe_menu_registry.js`.
2. **Focus index on the sub-widget**: which offset of `*(m + 0xc8)` holds the
   integer index of the currently-focused item? Hook `FUN_00241d40` and inspect
   the sub-widget at draw time.
3. **DAT_0228ea38 and DAT_0228ea50 identities**: what specific widgets get
   types 2 and 4? Helps interpret registry contents.
4. **Connection between menu-item text and focus index**: how to map "focus
   index N" to "item text at index N" — likely requires walking the sub-widget's
   item list.
5. **.ebp interpreter** (fallback if 1 fails): if title menu turns out to be
   genuinely separate, locate the bytecode interpreter to read script-side
   state. Currently unlocated; Phase A canaries scored 0/20 (index-keyed
   dispatch).

## Critical RVAs referenced

| RVA | ABS | Symbol | Role |
|---|---|---|---|
| `0x5F850` | `0x17F850` | `FUN_0017f850` | Per-frame "display:draw Font" entry |
| `0x73D50` | `0x193D50` | `FUN_00193d50` | Multi-choice singleton getter (allocates 0x4a8) |
| `0x8F470` | `0x1AF470` | `FUN_001af470` | UI manager init (stores config pointers) |
| `0x95FA0` | `0x1B5FA0` | `FUN_001b5fa0` | Font-scene singleton getter |
| `0x9DAB0` | `0x1BDAB0` | `FUN_001bdab0` | Yes/No singleton getter |
| `0xAA600` | `0x1CA600` | `FUN_001ca600` | Config loader (writes values through UI manager pointers) |
| `0xAD0E0` | `0x1CD0E0` | `FUN_001cd0e0` | Save/Load + Yes/No state machine (11 KB) |
| `0xB1660` | `0x1D1660` | `FUN_001d1660` | Debug menu controller (13.7 KB) |
| `0xC8580` | `0x1E8580` | `FUN_001e8580` | Vertex dispatcher (3,572 B) |
| `0x121A50` | `0x241A50` | `FUN_00241a50` | **"Is menu type N open?" query** |
| `0x121D40` | `0x241D40` | `FUN_00241d40` | **Menu state-machine controller + cursor renderer** |
| `0x1E0C148` (static) | `0x1F2C148` | `DAT_01f2c148` | Cursor draw X offset (const config) |
| `0x1E0C14C` (static) | `0x1F2C14C` | `DAT_01f2c14c` | Cursor draw Y offset (const config) |
| `0x216EA60` (static) | `0x228EA60` | `DAT_0228ea60` | **Menu registry table** (8-byte slots by type) |
