# FFXII Combat System — RE findings and implementation plan

**Status:** RESEARCH COMPLETE, NOTHING IMPLEMENTED. Written 2026-07-20 (Session 48 research pass).

> # ⛔ READ THIS FIRST — Session 49 struck 11 claims in this document
>
> A second offline sweep (five parallel passes over the 33,128-function corpus **and over the
> shipped master data**, which turned out to be parseable) re-mined every sub-0.98 claim here.
> Most of the document survived. **These did not.** Each is struck inline below as well; this is
> the index. Full reasoning: `~/.claude/plans/find-the-document-labelled-hazy-summit.md` and
> Session 49 in `sessions_001_current.md`.
>
> | § | struck claim | replacement | conf |
> |---|---|---|---|
> | §5.4, §9.1.3b | "`result+0x04 == 9` is the AI preview — filter it" | 9 is the **default** seed. Preview = `result+0x00 & 0x20`, emitted by `FUN_00308b90`, which **never calls the applier** ⇒ **no filter needed** | 0.99 |
> | §9.1.3d Q5.1 | tick filter `attacker == 0` | filter **`actionId == 0xFFFF`**; `FUN_00310db0` makes two real calls with `attacker == 0` | 0.99 |
> | §3.4, §11 P-E | leader = `DAT_0209a1f0[3]` | **that array is 4 sceneObj\* in roster order.** Leader = `*(u8*)(W + 0x5AA4)` (`FUN_00327150`). **P-E cancelled** | 0.99 |
> | §4.2 | ability name id at `row+0x00` | that is a **description** id (`Attack` and every `Reserve` row share `4000`). Name = **`row+0x34`** → the `word.bin` pool | 0.99 |
> | §6.2 | "foes list accepts groups 0 and 1; a Neutral is a legal attack target" | **backwards.** Emit gate is `(g & ~2) == 0` ⇒ foes = group **0 only**; Neutrals are in **neither** list | 0.96 |
> | §6.3 | aggression `f = *(u32*)(actor+0x6A0)`, radius `+0x6A4` | **`+0x6A0` is a POINTER** — same bug class as §1.1. Use `*(u32*)(*(u64*)(actor+0x6A0))`, radius at `aiData+0x04` | 0.97 |
> | §7.1 | in-battle = `actor+0xEA4 != 0` | that mask is "**who has an action aimed at me**", not hostility — an out-of-combat Cure sets it. Use `*(u32*)(actor+4) & 0x100000` | 0.90 |
> | §7.2 | `FUN_00313b30` = battle start/end | fires **~2× per action, per combatant** | 0.96 |
> | §7.2 | game over = `FUN_0035c7b0` | **no caller ever passes `0x17`.** Real edge fn is **`FUN_0035c8a0`** | 0.95 |
> | §7.2 | `FUN_00312280` "gives EXP/LP/gil/loot" | **none are arguments.** gil → hook `FUN_00469e80`; loot → `FUN_003180f0`; EXP/LP → before/after diff | 0.95 |
> | §7.2 | the loot buffer is "**4 slots**, stride 8" | **7 slots** — 5 normal + 2 rare. Both `FUN_003180f0` and `FUN_00319920` loop 7 times. Struck S72 | 0.99 |
> | §9.1.4 | `0x0D`–`0x0F` are all "the spam tier, log-only" | **`0x0D` "begins casting" is REALTIME** since S72; only `0x0E`/`0x0F` are the spam tier | — |
> | §9.4, §10 | Tier-1 hook `FUN_0028e110` | **it never receives the message id.** Hook **`FUN_00536410`** (RVA `0x416410`): id at `RCX+4 & 0x7FFF`, finished string into `RDX`, one frame | 0.97 |
>
> **Also materially enlarged:** §1.7's codec fix is far bigger than the `0x29` rule — the complete
> escape table is now derived and the old fallback is wrong on **75 of 102** messages. See §1.7.
>
> **Confirmed and raised:** §5.4's `+0x2c` bits (all 19 mapped, 0.99) · §5.5's no-critical-hit
> negative (0.85 → 0.95, and FFXII's real **Combo** is readable at `result+0x18 & 2`) · §5.5.1's
> status table (**all 32 names decoded offline**, so §6.3's "bit 5 = Confuse" goes 0.85 → 0.99) ·
> §8.2.3's guest slot (**reachable now**: `list3[3]`, guest charId at `W+0x5AD4`) · §8.1's fix
> (byte-offset arithmetic verified firsthand; the roster gate is literally `slot < 9`).
**Scope:** combat messaging, committed-vs-browsed target, damage/heal processing, hostility/Neutral,
the combat log, `;` target status, `4`/`5`/`6` party vitals.
**Method:** offline decompile only (`..\FFXII-Decompile\output\decompile\*.c`, 33,105 functions) plus
the community RE archive. **No Ghidra run, no Frida run, no game-directory access.**

---

## 0. How to read this document

**Addresses.** Ghidra decompile uses ABSOLUTE addresses. Image base is `0x120000`, so
**RVA = abs − 0x120000**. Both are given for everything. Mod hooks resolve
`GetModuleHandle(nullptr) + RVA`.

**Confidence.** Per `CLAUDE.md` and `memory/feedback_re_confidence_bar.md`, every conclusion carries an
explicit 0.00–1.00 confidence. **Only ≥0.98 may be built on.** Everything below 0.98 in this document is
tagged **HYPOTHESIS** and is accompanied by the probe that would settle it. Nothing sub-0.98 may be
shipped, and no amount of "it's probably fine" promotes it — only a live Frida confirmation does.

**Order of work.** Per `CLAUDE.md` FRIDA-FIRST: every behavioural claim below is confirmed by a Frida
probe **before** a line of C++ is written, and the user runs the probes — Claude never executes Ghidra
or Frida. §11 is the probe plan and it is a hard gate on §10.

---

## 1. Corrections to existing project documentation

Per the house rule "when you disprove something, STRIKE it, do not merely add a newer entry", these must
be applied to `GameArchitecture.md` and `debug.md` when this work lands.

### 1.1 STRIKE — `party_status.cpp` reads the wrong base (this is a shipped bug)

`src/battle/party_status.cpp:58` treats `DAT_02ebf190` (RVA `0x2D9F190`) as the BtlWork struct.
**It is a POINTER.** Confidence **0.99**, three independent write sites:

| site | abs / RVA | evidence |
|---|---|---|
| `FUN_00238a80` | `0x238A80` / `0x118A80` | `DAT_02ebf190 = FUN_002ef640() + 0x4e68c;` — assigned |
| `FUN_00233c10` | `0x233C10` / `0x113C10` | `if (DAT_02ebf190 == (int *)0x0) { … }`, then `if (*DAT_02ebf190 != 0x5071901)` |
| `FUN_002370c0` | `0x2370C0` / `0x1170C0` | `*DAT_02ebf190 = 0x5071901;` — writes a magic through it |

and the two readers use it as a value, not an address:
`FUN_00320ab0` → `return DAT_02ebf190 + 8 + bcIdx * 0x1c8;`,
`FUN_0031b9f0` → `return *(u8*)(DAT_02ebf190 + 0x5a7e + slot*2);`.

**Consequence:** every party-slot read lands at `imageBase + 0x2D9F190 + 0x5a7e`, unrelated memory ⇒
`bcIdx >= 0x28` ⇒ `BtlChrForSlot` returns null ⇒ `SpeakSlot` logs and returns **silently**. Combined
with `[PARTY]` not being in the logger's flush list (`logger.cpp:173` flushes only ERROR/INIT/
HOOK_HEALTH), the diagnostic line was lost on exit too. That is the complete explanation of
"`4`/`5`/`6` do nothing and produce zero `[PARTY]` lines".

**This means `Docs/Controls.md:88-108` is wrong.** It attributes the failure to the input path and lists
`g_extraDown[]` array growth as "the sharpest lead". The input path is fine — `input_tracker.cpp:255-257`
and `nav_commands.cpp:229-231` are both correct. Strike that whole block. Same class of bug as the
Session-40 `mapData` missing dereference (`memory/feedback_verify_rva_arithmetic.md`).

### 1.2 STRIKE — the community RVA CSV's blanket address-convention claim

`notes/community_rvas.csv`'s header asserts *"All RVAs in this CSV are image-base-relative."*
**That is false for at least the InsurgCT-sourced entries.** Confidence **0.99**:

CSV entry `0x0030F5A0 ; SetCurrentMpAndMistChargesMax` — at **abs** `0x30F5A0` (RVA `0x1EF5A0`) the body
is unambiguous:

```c
if (bc+0x6c < 0 || bc+0x7c < 0) mp = 0; else mp = *(i16*)(bc + 0x28);   // maxMP
if (mp != *(i16*)(bc + 0x4c)) {                                          // curMP
    FUN_00300ce0(bc, mp - *(i16*)(bc + 0x4c), 0x217);                    // refill MP
    if (*(i16*)(bc + 6) < mp) FUN_00300bc0(bc, mp - *(i16*)(bc + 6), 0x217);  // refill mist
}
```
Sets current MP and mist charges to max — exactly what the label says. So the CSV value is an
**absolute** address.

**But do NOT generalise this into "the whole CSV is absolute".** `seed_from_drummer_ix.java` applied
*every* CSV value as an absolute address, so the names in `decompile_index.csv` reflect the seeder's
choice and are **not** evidence of any source's convention. Concretely, the ELF-sourced
`0x019AEE0 ; OpenPhyreFunc` got its label at abs `0x19AEE0`, where the body is a **CryptoAPI hash
routine**, not a Phyre file-open — so that label is at best unverified.

**Operational rule (this is `memory/feedback_validate_community_rvas.md`, now with a concrete
counter-example): never consume a community address without semantically validating the decompiled body
against its label.** The convention is per-source and unproven; the body is the only authority.

### 1.3 STRIKE — DrummerIX's chain / gil / steal direct pointers are unusable

`ChainLevelPtr 0x219BE18`, `ChainCountPtr 0x219BE1C`, `GilPtr 0x1F8B468`, `StealPtr1..3 0x29C6CEC..EE`
have **zero references under either interpretation** (as-RVA and as-abs). Confidence 0.95–0.97 stale.
Behaviour-derived replacements are in §8.3 (chain) and §8.4 (gil). This does not impugn DrummerIX's
**AOB** patterns, which are build-independent and did resolve correctly (§5.1).

### 1.4 CORRECT, not strike — `FUN_002b58b0`

Recorded as "the codec decoder". Its 62-byte body is actually a **variant selector**: if the blob starts
`00 00`, skip 2 bytes then skip *n* NUL-terminated strings; otherwise identity. Its return is still raw
FFXII-codec bytes that `GameText::Decode` then decodes. Confidence 0.98. The registry line should read
"variant-**selected** by `FUN_002b58b0`, decoded by the codec". No behavioural change to shipped code.

### 1.5 FILL IN — `GameArchitecture.md` "Damage / Heal / Status Event Funnel"

That section is entirely `TBD` and its stated plan ("Frida-watch HP writes and walk callers") is now
**obsolete** — the writer set is closed and small (§5.2). Replace it with §5.

### 1.6 SUPERSEDE — the stored combat-log design

`memory/project_combat_log_design.md` and `CLAUDE.md`'s "Combat log specifics" specify **F4 to open,
Esc to close, 50 entries, a modal `WH_KEYBOARD_LL` intercept, and pausing the game**. The design in §9
replaces all of it: **no modal overlay, no pause, 100 entries, `,`/`.` navigation.** Record the
supersession in both places rather than leaving two designs standing.

**And a harder strike in the same note:** it asserts *"FFXII is not narrative — combat is animations +
floating damage numbers, no textual 'Vaan attacks Wolf for 12 damage' strings exist in the game.
Confirmed by `damage_candidates.txt` returning 3 hits"*, and builds a 12-locale synthesized-template
phrasebook on that premise. **That is wrong** (§4.1, §4.5): FFXII composes full localized combat
sentences from `battle_message.bin` format strings and keeps its own scrollable battle log. The
string-grep failed because the text is codec-encoded master data, not plain ASCII in the binary — the
same reason the "numeric IDs, not strings" note in `GameArchitecture.md` reached the right observation
but the wrong conclusion. **The synthesized-template design must not be built.**

### 1.6a STRIKE — two RVAs in this document were wrong by `0x60000`

Inherited from a research pass and **corrected 2026-07-20** against `decompile_index.csv`, which is the
authority (its `rva_hex`/`abs_hex` columns differ by exactly `0x120000`):

| function | abs | WRONG RVA | **correct RVA** |
|---|---|---|---|
| `FUN_0030ab40` relation test | `0x30AB40` | ~~`0x18AB40`~~ | **`0x1EAB40`** |
| `FUN_0030bd00` target filter | `0x30BD00` | ~~`0x18BD00`~~ | **`0x1EBD00`** |

Neither was shipped. Spot-checked as correct: `0x1ED160`, `0x1F12F0`, `0x143BE0`, `0x1D8E90`,
`0x2D9F138`, `0x2030E0`, `0x267090`, `0x119170`.

**This is the third arithmetic slip found in this document** (after `DAT_02ebb560` and `DAT_02ebf018`).
`memory/feedback_verify_rva_arithmetic.md` exists for exactly this reason. **Before using any address
in §12, add `0x120000` back and confirm the function exists at that abs in `decompile_index.csv`.**

### 1.7 FIX — `GameText::Decode` eats punctuation after escape selector `0x29`

**This is a live defect affecting ALL mod text, not just combat.** Confidence **0.98**.

`game_text.cpp`'s `EscapeParamCount` returns `-1` for selector `0x29`, so it falls back to "consume every
following byte `>= 0x80`". Punctuation is `>= 0x80`, so the character immediately after a `0x29` escape
is silently eaten. Live proof from the decoded message table: `{0}'s HP is partially restored.` came out
as `{0}s HP is partially restored.` — raw bytes `0f 29 80 80 | ac | 4c …`, where `0xac` is the
apostrophe and the fallback swallowed it.

**The real rule, from the game's own interpreter.** `FUN_002ac5f0` case `0x29` calls **`FUN_003ffbc0`**
(abs `0x3FFBC0` / RVA `0x2DFBC0`), which is variable-length:

```
b = the byte immediately after the selector;   n = b & 7
n == 0  ->  2 param bytes   (a colour: FUN_0039ba00(p2 & 0x7f) -> RGBA, byte-swapped)
n >  0  ->  1 + n param bytes  (packed 7-bit run: ((b >> 3) >> i) << 7 | (byte & 0x7f))
```

Call convention confirmed at the call site — it loads a byte through the pointer and switches on it, then
advances the caller's cursor by the return value, so the pointer is at the **selector** and a decoder returning 3 means
selector + 2 params. That also resolves 13 further selectors as 2-param, all returning 3:

| decoder | returns | params | selectors |
|---|---|---|---|
| `FUN_003ffab0` | 3 | 2 | `0x20 0x23 0x24 0x25 0x28 0x2b 0x2c 0x2f 0x35 0x36 0x37 0x39 0x3c 0x3e 0x3f 0x56` |
| `FUN_003ffc60` / `003ffda0` / `003ffee0` / `003ffb90` | 3 | 2 | `0x2e` / `0x33` / `0x22` / `0x6d` |
| `FUN_003fff10` | 4 | 3 | `0x31` (the "%s" substitution slot) |
| `FUN_003ffbc0` | variable | see above | `0x29` |
| `FUN_003ffd70` / `FUN_003ffb00` | variable | **not yet decoded** | `0x2a` / `0x38`, `0x6f` |

Applying just the `0x29` rule plus the 2-param additions dropped unmapped bytes across all 102 battle
messages from pervasive to **two**. This partially closes the Session-45 "icon family still broken" item
(`memory/project_exits_items_status_session45.md`) — `0x2a`, `0x38` and `0x6f` remain undecoded, and
`0x40`–`0x6b` were never in `FUN_002ac5f0`'s switch at all (they fall to the shared icon handler
`FUN_002aeb20`).

**Ship this in Phase 1** — it is a pure decoder change, testable offline against the decoded table, and
it improves every string the mod already speaks.

> ### ⛔ ENLARGED (S49): the `0x29` rule is only a quarter of the fix
>
> The **complete** escape table is now derived from `FUN_002ac5f0` and lives in
> **`..\FFXII-Decompile\tools\ffxii_codec.py`** — the reference implementation `game_text.cpp`
> should be ported from, carrying a regression that re-parses all 102 messages. **Current result:
> ZERO structurally-unexplained bytes** (only `0xC7` ×2 remains, as an unmapped *glyph*).
>
> Measured against the mod's decoder as it stands today: **it is wrong on 75 of the 102 messages**
> — verified this session by reimplementing `game_text.cpp` in Python and diffing. The damage is
> not limited to apostrophes: sentence-final `.` and `!` vanish too, because messages end with a
> `0x29` colour escape and the high-bit fallback eats whatever follows it.
>
> | issue beyond `0x29` | rule | conf |
> |---|---|---|
> | three **variable-length** escapes | `0x29`: `n=b1&7`, `n==0 ? 2 : 1+n` · `0x2a` (ruby): `2 + (b2 & 0x7f)` · `0x38`/`0x6f`: `(b1&7)==0 ? 2 : 3` | 0.98–0.99 |
> | **icon family `0x40`–`0x6b`** | `FUN_002aeb20` returns 2 ⇒ **1** param byte, not a high-bit run. `0x56` is its own 2-param case; `0x5f` is a hole | 0.99 |
> | **`0x10`–`0x1f` are 2-byte extended glyphs** | `FUN_002ac2f0:25-32` — the mod drops the lead byte, then renders the trailing byte as a **stray letter** | 0.99 |
> | **`0x0b`/`0x0c`/`0x0d` terminate**; `0x08` is 3 bytes; `0x0e` variable | `FUN_002ac5f0:355-358` returns 0 and every caller stops | 0.98–0.99 |
> | **`0x01`/`0x06`/`0x07` are SPACES, not no-ops** | `FUN_002ac5f0:115-118` lists `case '\x01': case '\x04': case '\x06': case '\a':` as **one fall-through group**, and `0x04` is the known space. The mod maps only `0x04`. **Found live 2026-07-20:** the game separates an enemy name from its instance letter with `0x06` (`31 3a 4d 06 21` = `"Rat" SPACE "B"`), so the mod rendered `"Dire RatB"` where the game draws `"Dire Rat B"`. `0x05` is **excluded** — it has its own case and the `0x0a` handler scans for it as a structural delimiter | 0.99 |
>
> And the cause of a long-standing visible defect: **`0x2d` is the NUMERIC substitution slot**
> (index `b1 & 0x7f`), which is why the level-up line decoded as `"{0} is now level !"` with its
> number missing. `0x31` is the string slot; `0x6c`+`0x6d`/`0x6e` are the item-name/quantity pair.
>
> Not confirmable from code, by construction: the **font atlas is the character map**
> (`FUN_002ac2f0:75` computes the glyph slot as `byte - 0x20`), so the letter/digit/punctuation
> tables are empirical. A–Z/a–z and `! ? , . '` are corroborated at 0.99 by this corpus; the digits
> `0x85`–`0x8e` and the rarer punctuation are **not exercised by it** and keep their prior status.

---

## 2. The battle object model

Three records per combatant. Getting these straight is what the rest of the document rests on.

```
actor      stride 0xF50   pool  *(void**)(base + 0x1F6E688), count *(i32*)(base + 0x1F6E6A0)
             +0x00  u64  flag word   (bit 0x10 = live/present, bit 0x4000 = player command queued)
             +0x08  i32  the actor's OWN object handle
             +0x10  u64  sceneObj
             +0x18  u64  name codec
             +0x58  u8   authored side byte (non-PC)
             +0xE0/E4/E8 f32  cached world position
             +0x698 u64  BtlChr
             +0x6A0 u64  aiData        (dword 0 = aggression bits)
             +0x6B4 u8   action phase  (0 = idle)
             +0x708 u64  action definition record
             +0x710 i32  ACTIVE action's target handle
             +0x714 u16  ACTIVE action id   (0xFFFF = none)
             +0x740 i32  previous target handle
             +0x744 i16  previous action id
             +0x760/764 i32  pre-action wait accumulator / max
             +0x768 i32  CT current        +0x76C i32  CT max
             +0x77C i32  resolved target count   +0x780 + i*0x10  target sceneObj
             +0xBA0 i16  QUEUED action id       +0xBB8 i32  QUEUED target handle
             +0xBC4 i32  live target-list count +0xBC8 + i*0x10  target sceneObj
             +0xDD4 u32[0x20]  per-attacker grudge deadline
             +0xE60 u64  stat record   +0xE68 u64  behaviour record
             +0xEA4 u32  engaged-by bitmask   +0xEA8 u8 attacker count   +0xEA9 u8 own pool index

BtlChr     stride 0x1C8   array  W + 0x08, 0x28 entries        (W = the BtlWork pointer below)
             +0x04 u8 charId   +0x05 u8 kind (0 = player-roster character)
             +0x06 i16 mist points          +0x24 i32 maxHP     +0x28 i16 maxMP
             +0x37 u8 licensed mist bars    +0x3c u32 statusA   +0x48 i32 curHP
             +0x4c i16 curMP                +0x4e u8 mist bars  +0x64 u32 statusB
             +0x6c/+0x7c i8 MP-enable guards (sign bit set = disabled)
             +0x18C u32 EXP   +0x190 u32 LP   +0x1C2 u8 level

sceneObj     +0x00 u16 slot   +0x0E u8 (low nibble = scene-kind, high bits = state)
             +0x0F i8 side    +0x15 u8 (low nibble = list)  +0x16 u16 generation
             +0x1C u32 interaction flags   +0x30 u64 actor back-pointer
             +0xB8 u64 transform node (world position)
```

**BtlWork** `W = *(void**)(base + 0x2D9F190)`. Validate with the engine's own magic
`*(u32*)(W + 0x00) == 0x5071901`. Roster lists: `W + 0x5A48`/`0x5A5A`/`0x5A6C`/**`0x5A7E`**/`0x5A90`,
each 9 × u16, stride `0x12`. List 3 (`+0x5A7E`) holds **BtlChr indices** (`>= 0x28` = empty) and is the
unmasked master party — keep using it. Note list 1 (`+0x5A5A`) holds **charIds**, not indices; do not
confuse them. Confidence 0.99 (§1.1 evidence).

**Handle ↔ object, without calling the game.** `FUN_003588b0` is known-unreliable to call and is not
needed. The encoder `FUN_003589b0` (abs `0x3589B0` / RVA `0x2389B0`) is:
```
handle = ((*(u16*)(so+0x16) & 0x7FF) << 4 | (*(u8*)(so+0x15) & 0xF)) << 16 | *(u16*)(so + 0x00)
```
and **`*(i32*)(actor + 0x08)` is the actor's own handle** (confidence 0.98), so handle→actor is a scan of
the existing actor pool comparing `+0x08`. `*(u64*)(sceneObj + 0x30)` is the actor back-pointer
(confidence 0.99, `FUN_00263e30` is literally `return *(u64*)(p+0x30);`).

---

## 3. Target selection — browsed vs committed

### 3.1 The bug, precisely

`*(P + 0x9FD8)` (where `P = *(void**)(base + 0x1F7BE80)`) is written **only by browse handlers**, and
there are **two** of them:

| writer | abs / RVA | what it is |
|---|---|---|
| `FUN_0027b880` | `0x27B880` / `0x15B880` | battle-menu highlight-changed handler |
| `FUN_003c9420` | `0x3C9420` / `0x2A9420` | **free-target cursor** move (a second, independent browse UI) |

Both funnel `FUN_0028fe40` (RVA `0x16FE40`) → `FUN_002be1a0` (RVA `0x19E1A0`) → `FUN_002be300`
(RVA `0x19E300`), whose body is `*(int*)(obj + 0x578) = handle`, and `0x8FA0 + 0xAC0 + 0x578 = 0x9FD8` ✓.
The complete caller set of `FUN_0028fe40` is five functions and **none of them commits anything**.
Confidence **0.99**.

So the mod's current model can only ever report the cursor. Browsing re-announces every hovered unit and
leaves the last-hovered one as "the target" — exactly the reported symptom.

### 3.2 Where the committed target actually lives

On the **actor**, in two stages:

| stage | gate | target handle | action id |
|---|---|---|---|
| **queued** (confirmed, waiting for ATB) | `*(u64*)actor & 0x4000` | `*(i32*)(actor + 0xBB8)` | `*(i16*)(actor + 0xBA0)` |
| **active** (charging → executing) | `*(u16*)(actor+0x714) != 0xFFFF` and `*(u8*)(actor+0x6B4) != 0` | `*(i32*)(actor + 0x710)` | `*(u16*)(actor + 0x714)` |
| last finished | — | `*(i32*)(actor + 0x740)` | `*(i16*)(actor + 0x744)` |

Confidence **0.98**, resting on a closed producer→consumer→clear triangle:
`FUN_00311cf0` writes `0xBA0`/`0xBB8`/bit `0x4000` → `FUN_00305ab0` (RVA `0x1E5AB0`) reads exactly those
three → `FUN_0030f760` (RVA `0x1EF760`) writes `0x708`/`0x710`/`0x714` → `FUN_00304850` (RVA `0x1E4850`)
consumes them → `FUN_003105d0` (RVA `0x1F05D0`) clears them (`0x714 = 0xFFFF`).

Two independent research passes converged on `actor+0x714` / `+0x744` from different directions (the
action-name trace and the commit trace), which is part of why this sits at 0.98 rather than 0.9.

### 3.3 The commit hook

**`FUN_00311cf0` — abs `0x311CF0` / RVA `0x1F1CF0`**, signature
`(actor*, i32 targetHandle, u8 subId, i16 cmdId, i32 allEffect)`.

Its complete caller set is the player's battle-command UI: the container `FUN_002778c0` (RVA `0x1578C0`)
message `0x8001` = DECIDE → `FUN_00311cd0` (RVA `0x1F1CD0`) → `FUN_00311b20` (RVA `0x1F1B20`), plus the
Attack shortcut `FUN_0027aa70` and the auto-attack toggle `FUN_003138d0` (RVA `0x1F38D0`).
**Gambits and enemy AI never reach it** — they enter at `FUN_00305cc0` → `FUN_0030f760` directly.
So it fires once per player confirm and never for AI. Confidence **0.98**.

**Must post-verify after calling the original.** `FUN_00311cf0` silently no-ops when the actor is
Stop/Sleep/Confuse-class (`(statusB|statusA) & 0x8800203B`) and returns `0xFFFFFFFE` when KO'd. Reading
the queue back is the only way to know the confirm took:
```
if ((*(u64*)actor & 0x4000) && *(i16*)(actor+0xBA0) == cmdId)
    handle = *(i32*)(actor + 0xBB8);      // never 0 — self-target substitutes the actor's own handle
```

### 3.4 What `;` should do

```
1. committed = CommittedTarget(leaderActor)        // active (0x710) first, else queued (0xBB8)
   -> speak name + HP  (+ "queued" when not yet acting)
2. else                                            // browsed, or out of battle
   -> SILENT. Speak nothing at all; log the reason.
```
This removes the failure mode outright: hovering an ally never touches `0x710`/`0xBB8`.

> **REVISED at release 0.1 (user instruction).** The original steps 2 and 3 — *"else speak the
> browse cursor `*(P + 0x9FD8)` labelled as browsing"* and *"else `No target`"* — are **STRUCK for
> the `;` key**.
>
> `;` answers exactly one question: **what is the active combat target?** So:
> - **A browsed cursor is not an answer.** `ResolveTarget` only reports `browsing` when there is no
>   commitment, so by construction it is never the active target. The `, browsing` suffix is gone.
> - **Out of battle, `;` does NOTHING.** Not "No target" — *nothing*. Announcing anything is exactly
>   the "it works out of battle" behaviour that was reported. Out of combat there is no commitment
>   anyway (`CommittedTargetOf` tests the queued flag and requires a real action-table row), so the
>   silent path is reached naturally; no new "am I in battle" flag was needed, and none was invented.
>   See [[feedback_never_speak_filler_be_silent]] — silence is the correct output for
>   nothing-to-report.
>
> **The browse fallback still lives inside `ResolveTarget`** and is NOT deleted: `p`
> (`GetLockedTarget` → routing) is the other caller, and routing to a browsed target is still
> wanted. The gate is applied at `SpeakTargetStatus`, so the two callers deliberately differ.
> Anyone tempted to "simplify" by deleting step 2 from `ResolveTarget` would silently change `p`.

**Which actor is "me":** ⛔ **BOTH options here are superseded (S49, 0.99).**

~~reuse the mod's existing, proven test — scan the actor pool for `*(u8*)(*(void**)(actor+0x698) + 5) == 0`~~
— that matches **every** player-roster character, not the leader.

~~The direct global `DAT_0209a1f0[3]` (RVA `0x1F7A1F0`) is a candidate but only 0.93 — HYPOTHESIS, probe P-E.~~
— **refuted.** `DAT_0209a1f0` is 4 × `sceneObj*` in plain roster order (`FUN_003261d0:150-163`);
`FUN_00303cc0` and `FUN_00304180` each use index 3 **and** the leader index in the same expression
as different things. **Probe P-E is cancelled.**

**Use the leader getter — `FUN_00327150` (RVA `0x207150`), read firsthand, reimplemented as pure reads:**
```c
u8 id = *(u8*)(W + 0x5AA4);   if (id > 0x27) -> no leader
BtlChr* chr = (u8*)W + 8 + id * 0x1C8;
for (i = 0; i < actorCount; i++) if (*(void**)(actor_i + 0x698) == chr) return actor_i;
```
`W+0x5AA4` is proven the leader by the party menu's own "make leader" action
(`FUN_0029be50:232` → `FUN_0035baa0` → `FUN_00327850:8`), auto-promotion on leader death
(`FUN_00304180:34-60`), re-pick on reorder (`FUN_0034d4e0`), and control-focus sync (`FUN_003220e0`).

### 3.5 Lock-On

There is **no separate persistent lock-on target**. `DAT_02b47730` (RVA `0x2A27730`) is the free-target
cursor: `+0xC4` browsed handle, `+0xC8` camera look-at, `+0xD0` mode. It **does** drive `P+0x9FD8`,
which is a second, independent reason the current model mis-reports. Confidence 0.95 (negative claim
over a complete caller grep). Consistent with Session 44's "there is no Lock-On key".

### 3.6 What this means for `p`

`p` (route to the battle target) should be re-sourced from the **committed** target too, so `p` and `;`
always agree. The existing `battle_target_reader` two-hook machinery (`FUN_002bfd20` + `FUN_00329220`)
stays — but **demoted to the browse readout only**. It must no longer feed `GetLockedTarget()`.

---

## 4. Combat messaging — text the game itself displays

> The house rule (`memory/feedback_no_fabricated_ui_labels.md`, `CLAUDE.md` "NEVER hardcode user-facing
> speech text") made one question decisive: **does FFXII compose its own combat sentences, or must the
> mod synthesize them?**
>
> **ANSWER: the game composes them, in full, with attacker / target / amount / status / item / action
> substituted — and it keeps its own scrollable battle log.** Read it verbatim. Synthesize nothing.
> This overturns the stored design (`memory/project_combat_log_design.md`), which asserted *"no textual
> 'Vaan attacks Wolf for 12 damage' strings exist in the game"* and built a whole 12-locale template
> phrasebook on that premise. **Strike that claim.** It came from a failed string-grep — FFXII stores
> these as codec-encoded format strings in master data, which a plain-text grep cannot see.

### 4.1 The battle message bus — the game DOES narrate

**`FUN_0046ab10` — abs `0x46AB10` / RVA `0x34AB10`**, signature `(BtlChr* subject, u32 msgId, u64* ctx)`.
Confidence 0.97.

Reached from the damage applier: `FUN_003112f0` → `FUN_00385ba0` (RVA `0x265BA0`) → `FUN_00469ec0`
(RVA `0x349EC0`) → `FUN_0046ab10` → **`FUN_005369c0`** (RVA `0x4169C0`, binds the args) →
**`FUN_00536410`** (RVA `0x416410`, codec-sprintf into a 0x180-byte buffer) → **`FUN_0035b990`**
(RVA `0x23B990`, routes it).

Message context (0x30 bytes), built by `FUN_00469ec0`. The arg-type codes in each message record line
up with it exactly, which is what confirms the mapping:

| off | arg type | meaning |
|---|---|---|
| +0x00 | 1 | attacker / subject BtlChr → name |
| +0x08 | 2 | target BtlChr → name |
| +0x10 | 3 | u32 number |
| +0x14 | **4** | i32 **abs(amount)** — HP or MP, the templated damage/heal number |
| +0x18 | **5** | u32 status bit index → `FUN_0035d330(0x1A, idx)` |
| +0x1C | 6 | u32 number |
| +0x20 | **7** | tagged item id → `FUN_0035d330(1, …)` |
| +0x24 | 9 | u32 number |
| +0x28 | **8** | u64 **action id** → `FUN_0035d330(0x14, id)` |
| +0x2C | — | u16 route selector |

⚠️ **Correction:** an earlier pass in this same investigation read `+0x1C` as "attacker level" and
`+0x28` as a bare action id. `+0x28` is the action id *and* is the field the `< 0x1FF` entry gate tests —
same field. Confidence 0.95.

Names resolve through **`FUN_00536280(nameIdx, isPc)`** (RVA `0x416280`), which is the
`DAT_02ebf170` codec-string-pool recipe (§4.3), or the player-entered name via `FUN_0026dea0` for a PC.
That is an independent confirmation of the text substrate.

> ⚠️ **The id table below is INFERRED from emitter call sites and is LARGELY WRONG.** The shipped data
> was decoded afterwards — **see §4.6 and `notes/battle_message_table_us.txt` for the authoritative
> 102-entry table.** This one is kept only to show which emitter reaches which id.

Inferred message ids (superseded by §4.6):

| id | meaning | emitter |
|---|---|---|
| 0x05–0x07, 0x08, 0x0A, 0x0D–0x0F | action / cast announce variants | `FUN_004697c0`, `FUN_00469a50`, `FUN_00469af0` |
| **0x10** | **KO / defeated** | `FUN_00469bb0` (RVA `0x349BB0`) |
| 0x11 / 0x12 | status 0x1F / petrify inflicted | `FUN_0046a800` |
| **0x13 / 0x14** | **HP restored** / restored to full | `FUN_00469ec0` |
| **0x15 / 0x16** | **MP restored** / MP full | `FUN_00469ec0` |
| 0x17 | no effect (already full) | `FUN_00469ec0` |
| 0x18–0x1B | 1 / multiple visible / hidden statuses inflicted | `FUN_00469ec0` |
| 0x1C | immune / nullified | `FUN_00469ec0` |
| 0x1D | MP unavailable | `FUN_00469ec0` |
| 0x1E–0x23, 0x27–0x2C, 0x33–0x4D | per-action-id and leader-specific variants | `FUN_00469ec0` |

**Two caveats that matter for a log:**
1. **It self-dedupes** — a 10-entry ring at `DAT_02ebb560` (RVA **`0x2D9B560`**) keyed on (subject, msgId)
   with a `frameRate * 0x1c2` window. So it is *not* a complete event stream.
2. **It is behind a camera-distance cull** (`FUN_00469570` → `FUN_002fa3b0`, < 24 world units) unless the
   message definition byte `rec+3 >= 0x80`. Distant events are dropped.

⇒ **Use `FUN_0030f760` / the damage hooks as the event *source*, and this bus for *wording*.** The two
are complementary, not alternatives.

> ⚠️ **RVA arithmetic caution.** Two data-global RVAs in the source reports were computed wrongly and are
> corrected here: `DAT_02ebb560` → `0x2D9B560` (reported as `0x29AB560`), and `DAT_02ebf018` →
> `0x2D9F018` (reported as `0x29CF018`). Both are in the `0x2Exxxxx` band. Per
> `memory/feedback_verify_rva_arithmetic.md`, **re-derive every address in §12 by adding `0x120000`
> back before it is used in code.**

### 4.2 The action currently executing

**`*(u16*)(actor + 0x714)`** is the executing action id (valid `< 0x21F`), `+0x744` the previous one.
Cross-confirmed by two independent research passes (§3.2). Ability row:
```
hdr = *(u64*)DAT_02ebf138                    // NOTE: a POINTER; count +0x04, stride +0x08 (0x3C)
row = FUN_0020e600(*(u32*)(hdr + 0x0c)) + actionId * *(u16*)(hdr + 0x08)
row + 0x34 u16 = NAME index -> the DAT_02ebf170 pool     // <-- the name
row + 0x00     = description id (bank 4)   row + 0x2E = description id (bank 10)
```

> ⛔ **STRUCK (S49, 0.99): `row+0x00` is NOT the name.** It is a `FUN_002f9860` id in the 4000 band
> (`help_action.bin`, a one-line description) — and **`Attack` and all 30+ `Reserve` rows share
> `row+0x00 = 4000`**, so it cannot be a name. The name is **`row+0x34`**, resolved through the
> shared `word.bin` pool, which is exactly what the game's own resolver `FUN_0031c5d0:774-798`
> writes into out-record `+0x18`. Verified against the shipped `action_data.bin`
> (`0x20 + 543*0x3C` = exact file size): `0x000 Cure`, `0x00B Curaja`, **`0x096 Attack`**,
> `0x0A9 Steal`, `0x1ED Megaflare`.
>
> ⚠️ **`row+0x34 != actionId` for the Motes (`0x80`–`0x8E` are permuted), so always READ the field.**
> And note the open contradiction: the data says `0x096` is Attack while `FUN_00385f60:69` and
> `FUN_00387090` treat **`0x9F`** as the basic-attack sentinel. Resolve live before shipping.
`DAT_02ebf138` = RVA `0x2D9F138`. `FUN_0020e600(x) = x + _DAT_01f83530` (RVA `0xEE600`; base global
RVA `0x1E63530`) — all master-data "pointers" in FFXII are u32 offsets needing this relocation.
Confidence 0.90 — **HYPOTHESIS**, probe P-11.

### 4.3 Text substrate recovered

- Master-data table headers: `+0x04` count, `+0x08` stride, `+0x0C` rel-offset to records.
- Shared codec-string pool `DAT_02ebf170` (RVA `0x2D9F170`): nameIdx u16 → chunk = `pool + 4 + (idx>>11)*4`,
  string = `chunk + 4 + (idx & 0x7FF)*4`, both through `FUN_0020e600`.
- `FUN_002f9860(id)` (RVA `0x1D9860`): `bank = id / 1000`, 28 banks, tables `DAT_02ec3d80` (RVA
  `0x2DA3D80`) + `DAT_02f973c0` (RVA `0x2E773C0`).
- `FUN_0035d330` out-record: `+0x18` name codec (known), plus `+0x08` / `+0x10` = two further codec
  pointers.
- **`FUN_0035d330(1, id << 16)` is a universal tagged-id → name resolver** (0.98); tag = `id >> 12`.
- Status-name category is **`FUN_0035d330(0x1a, bitIndex)`** (0.97) — this is the *battle-side* status
  name path, distinct from the menu-side `DAT_022c8b08` table already recorded.

### 4.4 `battle_message.bin` — the literal-path claim is wrong

`CLAUDE.md` states the game "loads `PS2Data\...\battle_message.bin` **by literal path**". A grep of all
33,128 decompiled functions finds **zero** references to that string or its address. The path strings sit
in an `.rdata` path pool whose neighbours are a random mix of every locale and asset type — a baked VBF
directory listing, not a code literal. Confidence 0.97 that no literal xref exists.
String addresses: us `abs 0xBCECF0` / RVA `0xAAECF0`; fr `abs 0x871210` / RVA `0x751210`;
kr `abs 0x8879F0` / RVA `0x7679F0`.

**This does not make the file dead** — files load by VBF filename-hash, and per `CLAUDE.md` a missing
string literal proves nothing. It means the *routing* is by hash.

**STRUCK within this investigation: the "banks 18/19" hypothesis is dead.** `battle_message.bin` is not
one of the 28 `FUN_002f9860` banks at all. It is **its own table with its own string pool**:

**`DAT_02ebf018` — abs `0x2EBF018` / RVA `0x2D9F018`.** Bound as the *first* section in the master-data
binder `FUN_00236c30` (RVA `0x116C30`) from `DAT_0208e680 + 0x0C` (RVA `0x1F6E680`).
Mechanism 0.97; that it is literally `battle_message.bin` 0.93.

```
hdr + 0x04   u32  message count
hdr + 0x08   u32  record stride
hdr + 0x0C   u32  -> message-definition records
hdr + 0x14   u32  -> the message FORMAT-STRING pool, indexed directly by message id

fmt = FUN_0020e600( *(u32*)( FUN_0020e600( *(u32*)( FUN_0020e600(*(u32*)(hdr+0x14)) + 4 ) )
                             + 4 + msgId*4 ) )
```
Same `hdr+0x14` shape as `FUN_002f9860`, i.e. effectively a **29th, battle-only bank**.

Message-definition record: bytes `+0x00`, `+0x01`, `+0x02`, **`+0x05`** = the four **arg-type codes**
(note arg 3's type is at `+0x05`, not `+0x03`); `+0x03` = display style (`>> 2`); `+0x04` bit 0 =
dedup-eligible.

### 4.5 The read point — and the game keeps NO scrollback

> ⚠️ **STRUCK: "FFXII keeps its own scrollable battle log."** An earlier draft of this section said the
> game already has the data structure the user is asking for and that the mod could mirror it. **That is
> wrong.** `logMgr + 0x4098` is the queue of messages **not yet shown**; entries *leave* it the moment
> they appear on screen. The game retains **zero scrollback**, has no viewer, no cursor, no scroll
> offset, and no input handler. Confidence 0.99. **The mod's ring buffer is genuinely new
> functionality, not a wrapper.**

**The manager.** Allocated in `FUN_002bdce0` (abs `0x2BDCE0` / RVA `0x19DCE0`) as one 0x40C8-byte object
(`FUN_00244f50(0x40c8, FUN_002be4f0, …)`), stored at `P + 0x8FA0 + 0x1058`. Initialiser
`FUN_002bed30` (abs `0x2BED30` / RVA `0x19ED30`).

```
P      = *(u64*)(base + 0x1F7BE80)          // battle HUD context (canonical)
mgr    = *(u64*)(P + 0x9FF8)                // == P + 0x8FA0 + 0x1058   CONFIRMED 0.99
    mgr + 0x00D8   40 message nodes, INLINE (not heap), stride 0x198
    mgr + 0x4098   list head: PENDING   (queued, not yet on screen)
    mgr + 0x40A0   list head: DISPLAYED (currently on screen — at most 2)
    mgr + 0x40A8   list head: FREE
    mgr + 0x40B4   fade level        mgr + 0x40B6  bit 0 = suppressed
    mgr + 0x40B8   scroll-in offset
node:
    + 0x000  0x180-byte codec text   (decode with GameText::Decode)
    + 0x180  colour                  + 0x184  per-message dwell (0.90)
    + 0x186  bit 0 = in use          + 0x187  line metric
    + 0x188  next                    + 0x190  prev
```

**Capacity = 40 nodes**, and the arithmetic self-validates: `40 × 0x198 = 0x3FC0`, and
`0xD8 + 0x3FC0 = 0x4098` — exactly where the list heads begin. The initialiser's own
`memset(mgr + 0xD8, 0, 0x3FC0)` and `while (i < 0x28)` loop agree. Confidence 0.99.

**Recycling** (`FUN_002be8d0`, abs `0x2BE8D0` / RVA `0x19E8D0`): pop from FREE → else steal the
**oldest PENDING** node (`FUN_002bf480`, RVA `0x19F480`) → else **drop the message entirely**. A node
returns to FREE the instant its line scrolls off. So under load the game silently discards messages —
another reason the mod must capture at the hook, not by walking the list.

**It is a two-line transient ticker.** Renderer `FUN_002bf230` (abs `0x2BF230` / RVA `0x19F230`) draws
exactly **two** lines from the DISPLAYED list (head and head→next), `0x80` apart plus the scroll-in.
State machine `FUN_002beac0` (RVA `0x19EAC0`): slide in → dwell → advance, or hold `150 × rate` frames
then fade `+0x40B4` to zero and flush.

**★ Preferred hook: `FUN_0028e110` — abs `0x28E110` / RVA `0x16E110`.** `arg0` = the finished string,
`arg1` = the dwell class. It has **exactly one caller** (`FUN_0035b990`), which is what raises it to
**0.98**. Fires once per message, not per frame, and — critically — **before** the recycling logic can
drop it.

When the route selector (`ctx+0x2C`) is non-zero the text instead goes to `_DAT_022ca430 + 0x568`, the
same toast widget the Session-45 "You obtain a Potion" reader already uses. Clean cross-confirmation.

**Style byte — solved (0.98).** `rec[3] >> 2` is two independent fields:
- **low 5 bits = dwell class** → `FUN_005369c0` packs it into `+0x1F` → `FUN_0035b990` →
  `FUN_0028e110` arg1 → `FUN_002bdb20` → `DAT_0209E610[style-1]`, built by `FUN_002bda90` as
  rate × **{12, 15, 20, 25, 30}** frames.
- **bit `0x20` = bypass the 24-unit camera-distance cull** (`FUN_00469570`: `if (0x7f < rec[3]) return 1;`).

Histogram over all 102 records: `0x01`(3) action announces · `0x02`(21) two-line effects ·
`0x03`(7) Paling/Shield/White Wind · `0x21`(17) heals/cures **cull-exempt** ·
`0x22`(30) traps/level-up/obtain/steal/poach **exempt** · `0x23`(3) KO/void/petrify **exempt** ·
`0x25`(20) party-wide and boss lines **exempt**.

### 4.6 The message table — decoded offline, no probe needed

`battle_message.bin` is **already extracted** in the VBF dump at
`..\FFXII-Decompile\extracted\ps2data\image\ff12\test_battle\us\binaryfile\battle_message.bin`
(6,668 bytes, `us` only). Parsed by `..\FFXII-Decompile\tools\parse_battle_message.py`; full decoded
table in `..\FFXII-Decompile\notes\battle_message_table_us.txt`.

**Exactly 102 messages, ids `0x00`–`0x65`.** Confidence 0.99 — the header self-validates: count `0x66`
at `+0x04`, stride 8, records at `0x20`, and `0x20 + 102*8 = 0x350` = exactly the pool offset at `+0x14`;
the string chunk independently reports 102.

> ⚠️ **STRUCK: the inferred message-id table in §4.1 is largely wrong.** It was reconstructed from
> emitter call sites; the shipped data disagrees. Corrections: `0x17` = "HP **and MP** are fully
> restored" (not "no effect" — that is `0x1D`, "{0} had no effect"); `0x18`–`0x1B` = statuses
> **cured / faded** (removal, **not** inflicted); `0x1C` = "regains consciousness" (not immune);
> `0x4F`–`0x62` = magick/magnetic **field** on-off pairs, Magick Pot and "Back attack!" (not stat
> up/down). **Use the decoded table, never the inferred one.**

Level-up (`0x04`), steal (`0x2D`–`0x34`), poach (`0x37`/`0x38`), gil and "You obtain…" (`0x24`–`0x26`)
are all in this table. **No Miss / Block / Critical string exists anywhere in it** — an independent
confirmation of the §5.5 negative findings.

Representative entries:

```
0x04  {0} is now level <n>
0x05  Time exceeded.\n{0} cancels {1}
0x08  {0}'s target is too far from the party leader.
0x0D  {0} begins casting {1}.
0x0F  {0} uses {1}.
0x10  {0} has fallen.
0x13  {0}'s HP is partially restored.
0x1D  {0} had no effect.
0x2D  {0} stole <item> from {1}!
0x33  {0}'s steal failed!\n{1} has nothing to steal.
0x3E  A Paling rises around {0}.\n{1} is immune to physical damage!
```

⚠️ The `{n}` slot numbering in the dump comes only from selector `0x31`; item and numeric arguments are
substituted through other escapes and do not render as slots. **The record's arg-type list is
authoritative for what gets substituted, not the visible slot count.**

---

## 5. Damage, healing and status

### 5.1 The pipeline

```
FUN_00307300 (0x1E7300)  action executor
  ├─ FUN_00385f60 (0x265F60)  DAMAGE CALCULATOR -> fills a 0x148-byte RESULT struct
  └─ FUN_003112f0 (0x1F12F0)  RESULT APPLIER  <- the one chokepoint that sees everything
       ├─ FUN_00385ba0 (0x265BA0) -> FUN_00469ec0 -> FUN_0046ab10   message bus (§4.1)
       ├─ FUN_00300530 (0x1E0530)  HP writer  (target AND attacker -> drain/absorb)
       │    ├─ FUN_003283d0 (0x2083D0)  FLYING NUMBER spawn
       │    ├─ FUN_00469bb0 (0x349BB0)  KO message id 0x10
       │    └─ FUN_0030e360 (0x1EE360)  status apply — bit 0 = KO
       ├─ FUN_00300ce0 (0x1E0CE0)  MP writer
       ├─ FUN_00300bc0 (0x1E0BC0)  Mist bar (BtlChr+0x06)
       ├─ FUN_0030e360 (0x1EE360)  status ADD    (result+0x54 mask)
       └─ FUN_0030e130 (0x1EE130)  status REMOVE (result+0x58 mask)
```

**DrummerIX's `DamageModAOB` resolves inside `FUN_00300530`** (RVA `0x1E0530`), at the `mov rcx, r8`
setting up the first `FUN_0031b860` call. The CE script's `originalcode` block maps instruction-for-
instruction onto the decompiled prologue, and his own cheat logic (`edx < 0` = damage, `[rsi+48]` =
curHP, `[rsi+54]`) matches the parameter roles exactly. Confidence **0.97**; the precise byte offset
needs probe P1. `InfMPAOB` likewise resolves inside `FUN_00300ce0` (0.96).

### 5.2 Every HP / MP writer — a closed set

| field | writers |
|---|---|
| `BtlChr+0x48` curHP | **`FUN_00300530`** (the applier, only delta-based writer) + `FUN_0030fed0` (RVA `0x1EFED0`, a re-clamp, **not an event**) |
| `BtlChr+0x4c` curMP | **`FUN_00300ce0`** + `FUN_0030fed0` + `FUN_0030c470` (level init) |

`FUN_00300520` / `FUN_00300c90` are "set to N" wrappers over the same two. Nothing else writes them.
Confidence 0.99. This is why the old "Frida-watch HP writes and walk the callers" plan is unnecessary.

### 5.3 The flying damage number — the cleanest hook

**`FUN_003283d0` — abs `0x3283D0` / RVA `0x2083D0`.** Confidence **0.99**.

```c
void FUN_003283d0(void* work,    // rcx : actor/battle-work object
                  int   amount,  // edx : SIGNED delta (negative = damage)
                  int   isPos,   // r8d : 1 = positive (heal/restore)
                  int   isMP);   // r9d : 0 = HP, 1 = MP
```
Exactly three callers: `FUN_00300530` (HP), `FUN_00300ce0` (MP), `FUN_0030e060` (mist bar).

Proof it is the damage number: its descriptor goes to `FUN_003bbd00` (RVA `0x29BD00`) → a 64-slot ring →
`FUN_00506700`, which int→ASCII's the value per digit, counts the digits, and loads the **`battle_4.c`**
numeral sprite atlas. Per-digit sprites + numeral atlas + per-value colour = the flying number.

**The popup kind enum is the descriptor byte `+0x0c`:**

| `+0x0c` | producer | RVA | payload | meaning |
|---|---|---|---|---|
| **0** | `FUN_003283d0` | `0x2083D0` | `abs(delta)` | numeric HP/MP damage or heal |
| **1** | `FUN_00328480` | `0x208480` | reaction id 0–5 | word: miss / block / parry / immune / counter / absorb |
| **2** | `FUN_003284d0` | `0x2084D0` | status **bit index** | status name via `FUN_0035d330(0x1a, idx)` |

Confidence 0.98–0.99. **The damage *category* is in the two register args (`isPos`, `isMP`), not in the
colour byte** — read the args.

That gives all four categories the user asked for, directly:

| `isMP` | sign of `amount` | category |
|---|---|---|
| 0 | negative | HP damage |
| 0 | positive | **healing** |
| 1 | negative | **MP damage** |
| 1 | positive | **MP restored** |

**Drain / absorb** is not a fifth kind: the attacker-side change arrives as its own `FUN_003283d0` call
(gated by `result+0x2c & 0x80` for HP, `& 0x2000` for MP), so a drain shows up as two events —
target loses, attacker gains. Reflected damage likewise re-enters `FUN_00307300` with a new target and
arrives as an ordinary second call.

### 5.4 The full-action chokepoint

**`FUN_003112f0` — abs `0x3112F0` / RVA `0x1F12F0`**,
`(result, attackerBc, targetBc, u16 actionId, u32 flags)`. One call = one fully-resolved outcome.
Confidence 0.98.

| result field | applied to | gate |
|---|---|---|
| `+0x24` i32 HP delta | target | `+0x2c & 0x100` |
| `+0x2a` i16 MP delta | target | `+0x2c & 0x4000` |
| `+0x20` i32 HP delta | **attacker** — HP drain / absorb / recoil | `+0x2c & 0x80` |
| `+0x28` i16 MP delta | **attacker** — MP drain (Syphon/Osmose) | `+0x2c & 0x2000` |
| `+0x54` u32 | status bits to INFLICT | — |
| `+0x58` u32 | status bits to CURE | — |
| `+0x04` u8 | outcome code 0..0xB | ~~**value 9 = AI preview — MUST be filtered**~~ **⛔ STRUCK (S49, 0.99)** — 9 is the *default* "run the formula" seed, written by `FUN_00385680` and `FUN_00307300:63`. The preview marker is **`result+0x00 & 0x20`** (`FUN_00308b90`), and previews **never reach this function**, so no filter is needed |
| `+0x1c` u8 | result valid; a message is emitted only when `== 1` | 0.97 |

**For "damage or heal?", use the SIGN of the delta, not the flag bits.** The `+0x2c` bit meanings are
only 0.96 and the flags additionally encode absolute-set modes; the sign is unambiguous. Do not ship the
bit semantics.

### 5.5 What is NOT available

- **No critical-hit flag exists anywhere in the pipeline.** No global, no result bit, no popup kind.
  Consistent with FFXII never displaying the word "Critical". Stated as a **negative finding at 0.85** —
  probe P4 before concluding. If confirmed, the combat log simply has no "Critical" annotation, and the
  stored design's `"{actor} attacks {target} for {N} damage. Critical"` template must be dropped.
- **No combo / hit-index counter.** `result+0x134` was an intermediate hypothesis and is **struck** — it
  is a knockback float from `FUN_00385e50`. The only combo-ish counter is a per-frame HUD widget.
- **No element / weakness / resist signal survives to the apply site.** The damage math runs through a
  global scratch bank (RVA `0x298DFB8`–`0x298E05C`) zeroed per calculation by `FUN_003849b0`; only the
  final integer is kept. Element must come from the **action record** via the action id (probe P7).
- **Reaction words (Miss / Block / Immune / Counter / Absorb) have NO text anywhere — they are pixels.**
  Confidence 0.96. The popup payload for kind 1 is only the `battle_4_p` texture handle plus the id 0–5
  at `slot+0x3F`. **Do not chase the words; use the message bus instead** — id `0x17` = no effect,
  `0x1C` = immune (§4.1). The six *emission conditions* are recovered from `FUN_00307300`
  (RVA `0x1E7300`), so the mod can still tell which reaction occurred; it just cannot read the word.
- Outcome codes 1–5, 0xB semantics: **0.6, do not ship** (probe P3).

### 5.5.1 What IS readable in the popup path

`FUN_00506700` (RVA `0x3E6700`) initialises each of the 64 ring slots at `DAT_02b457d0`
(RVA `0x2B357D0`, stride 0x50):

- **kind 0 (number) — READABLE.** The value is written as an **ASCII string** into `slot + 0x40`, even
  though the glyphs are drawn from the `battle_4_c` atlas. Confidence 0.95.
- **kind 1 (reaction word) — NOT readable** (above).
- **kind 2 (status name) — IS TEXT.** `FUN_0035d330(0x1A, statusIdx)` → `rec+0x18`; suppressed when
  `rec+0x24 == -1`; colour in `rec+0x25`.

**Category `0x1A` is confirmed as the battle status-name category** (0.97), table `DAT_02ebf118`
(RVA **`0x2D9F118`**), indexed by status bit 0..0x1F, name index at `rec+0x00`, and `rec+0x06 & 0x2000`
is what splits messages `0x18`/`0x19` from `0x1A`/`0x1B` (visible vs hidden status class).

⚠️ **STRUCK within this investigation:** an earlier pass concluded *"FFXII probably shows status as icons
only"* because the menu-side status table `DAT_022c8b08` had only 4 xrefs. Wrong — there are **two live
status-name text paths**, and the menu-side table is genuinely a separate table from the battle-side one.

### 5.6 KO

Detected inside `FUN_00300530` when the new HP reaches the per-combatant floor: emits `FUN_00469bb0`
(message 0x10) then `FUN_0030e360(bc, src, 0, …, mode 3)` = set status bit 0.

`FUN_0030e360` (RVA `0x1EE360`) is also the **universal status apply/remove hook**:
`(target, source, statusBit, duration, potency, flags, mode)`; modes 0/1/2 = CLEAR from `+0x64`/`+0x3c`/
`+0x38`, modes 3/4/5 = SET. It early-returns when the effective status word did not change, so reaching
its tail already implies a real transition. Confidence 0.98.

On-demand death test (the game's own, used in 40+ places, 0.99):
`((*(u32*)(bc+0x3c) | *(u32*)(bc+0x64)) & 0x80000003) != 0`.

### 5.7 Hooks that are per-frame — never hook

`FUN_00289a10` (`0x169A10`), `FUN_0028aaa0` (`0x16AAA0`), `FUN_0028a350`, `FUN_00289d70`,
`FUN_003bba60`–`FUN_003bbcf0` (`0x29BA60`–`0x29BCF0`), `FUN_00323350` (`0x203350`).

Also **never hook or call `FUN_0031b860`** (`0x1FB860`) — it linear-scans the whole actor pool 5–15×
per damage event. Use `actor+0x698 == BtlChr` directly.

And **do not hook `FUN_003bbd00`** (the popup ring insert) even though it looks like a tempting universal
point: it sits *after* a 24-unit camera-distance cull, so it silently drops off-screen events. For an
accessibility mod that is the wrong side of the gate.

---

## 6. Faction and hostility — the Neutral category

### 6.1 The engine has a literal third class

**`FUN_00263be0(sceneObj)` — abs `0x263BE0` / RVA `0x143BE0`.** Confidence **0.99** (50-byte body):

```
n = *(u8*)(sceneObj + 0x0E) & 0x0F
n in {1,2,7} -> 0   (FOE)
n == 3       -> 2   (ALLY)
everything else -> 1  (NEUTRAL / neither)
```

Equivalent 5-bucket form used throughout the game — **`FUN_002f8e90(BtlChr)`** (abs `0x2F8E90` /
RVA `0x1D8E90`), confidence 0.99: `0x01` party character, `0x02` guest (charId `0x1B..0x27`),
`0x04` ally NPC, `0x08` **foe**, `0x10` **neutral**.

### 6.2 The game already files Neutrals under Foes

The target-select list builder `FUN_0031eb20` (RVA `0x1FEB20`) delegates to:

- **Foes list** `FUN_0046b870` (RVA `0x34B870`): `if (group == 2) reject; else emit` → accepts groups
  **0 and 1**.
- **Allies list** `FUN_0046b690` (RVA `0x34B690`): only group 2.

> ⛔ **STRUCK (S49, 0.96) — THE OPPOSITE IS TRUE, and this was the whole justification for the
> feature.** Both builders are byte-identical up to the emit branch, and the gate
> (`FUN_0046b870:64`) masks the group with `0xFFFFFFFD` and accepts only a zero result — i.e. `group ∈ {0, 2}`.
> The foes builder emits **group 0 only**; the allies builder group 2 only; and **group 1
> (NEUTRAL) never reaches either emit branch and gets no flag at all.** The engine *excludes*
> Neutrals from target selection rather than filing them under foes.
>
> Free corroboration that the read is right: the emitted value is
> `((so[0x16] & 0x7FF) << 4 | group & 0xF) << 16 | so[0x00]` — **exactly** `FUN_003589b0`'s handle
> encoding, which re-confirms §2 at 0.99.
>
> ⇒ Adding a Neutral category is **not** "faithful to the engine". See §6.5's warning, which still
> stands, and the Ghidra dump of `DAT_01e09ee8` (abs `0x1E09EE8`) that now gates the whole phase.

~~Confidence 0.93. **This is the decisive result for the feature request:** the engine's own definition of
"hostile target" is *"not group 2"*, so a Neutral is already a legal attack target. Putting Neutral next
to Enemy in the scanner is faithful to the engine, not an invention.~~

### 6.3 Hostility is binary; aggression is a separate axis

**`FUN_0030ab40(actorA, actorB, flags)`** (abs `0x30AB40` / RVA `0x1EAB40`), confidence 0.99, returns
0 = no relation, 1 = friendly, 2 = hostile. Its party-side test is
`(BtlChr[5] == 0) || kind == 3` — **byte-for-byte what `battle_target_reader.cpp` already ships. That
code is correct.**

A status bit **inverts** the relation: bit 5 (`0x20`) of `(statusB | statusA)`. Behaviour 0.99; the name
"Confuse" 0.85. Worth speaking as "confused" — friend/foe is currently flipped.

Because the relation is binary, "will it attack me" is an **AI** question:

| signal | read | meaning | conf |
|---|---|---|---|
| **authored** | ⛔ **STRUCK (S49) — `actor+0x6A0` is a POINTER, same bug class as §1.1.** Correct: `aiData = *(u64*)(actor+0x6A0)`; `f = *(u32*)aiData`; `f & 0x20` → never engages; `(f & 0x48) == 0` → attacks on sight (**and** requires `FUN_0030b050` unless actor flag `0x10000000000` is set); else range-gated, radius **`*(f32*)(aiData + 4)`**, additionally requiring `mode == 0` and `radius < 100.0` | the passive/aggressive trait, `FUN_00308040` (RVA `0x1E8040`). **Structure 0.97, semantic label still inferred ⇒ DOES NOT SHIP** | ~~0.85~~ 0.97 |
| **provoked** | `*(u32*)(base + 0x1F6D45C) < *(u32*)(actor + 0xDD4 + partyIdx*4)` | that party member hit it within ~10 s | 0.90 |
| **engaged** | `*(i32*)(actor+0xBC4) > 0` and some `*(u64*)(actor+0xBC8 + i*0x10)` is a party sceneObj | currently acting on the party | 0.93 |

**There is no weighted threat/hate table** (0.90, and no `hate`/`aggro`/`threat` symbol exists in the
9,162 script symbols) — just a per-unit target list plus the grudge timer.

### 6.4 Scene-kind nibble — resolved

Setter `FUN_00269fe0` (RVA `0x149FE0`); the only real writer is `FUN_00238bf0` (RVA `0x118BF0`), which
resolves a long-standing project puzzle:

> **For a player-roster character the nibble is 1 (normal) or 8 (guest) and is NEVER a faction.**
> Every classifier tests `BtlChr[5]` first. This is why the solo-Reks leader's scene-kind was not 3.

For a non-PC it is a straight copy of the authored side byte `actor+0x58`. Values: 1/2 = foe sides
(0.98), **3 = ally (0.99)**, 4 = interactable body (0.90), **5 = removed/dead (0.98)**, 6 =
render-excluded (0.85), 7 = flagged foe (0.97), 8 = flagged party/guest (0.95), 0 and 9–15 = neutral
by default (0.85–0.90).

⚠️ **Caveat (0.95):** `FUN_002675c0` uses a *different* enumeration on the same byte for non-combatant
field objects (scene class 3). **Do not apply the faction reading to objects with no BtlChr.**

### 6.5 Recommended classification

```
sceneObj = *(void**)(actor + 0x10);  bc = *(void**)(actor + 0x698)
live = (*(u8*)(actor + 0x00) & 0x10) != 0;  kind = *(u8*)(sceneObj + 0x0E) & 0x0F

1. if (!live || kind == 5)                        -> DROP  (dead / removed)
2. if (bc && *(i8*)(bc + 0x05) == 0)              -> PARTY   (charId 0x1B..0x27 => "Guest")
3. else if (kind == 3)                            -> ALLY
4. else if (kind == 1 || kind == 2 || kind == 7)  -> ENEMY
5. else                                           -> NEUTRAL
```
Steps 2–5 are literally `FUN_002f8e90`; step 1 mirrors `FUN_0030ab40`'s two rejects. Confidence in the
**partition** 0.98.

~~**⚠️ Confidence that the Neutral bucket is non-empty in real gameplay is only 0.55.**~~
**✅ RESOLVED OFFLINE (S49, Ghidra dump of `DAT_01e09ee8` @ abs `0x1E09EE8`).**

The table is **`{1, 5, 3, 4}`**, and its only two xrefs are both inside `FUN_002396b0`. So the
script native's four selectable side values are exactly:

| script arg | side | meaning |
|---|---|---|
| 0 | 1 | foe |
| 1 | 5 | removed / dead |
| 2 | 3 | ally |
| **3** | **4** | **NEUTRAL** |

**Neutral is a first-class, deliberately authorable faction.** `FUN_002396b0` sets the never-engage
AI bit (`0x20`) precisely when `side == 4`, and `FUN_00238bf0` normalises side 4 → 1 (foe) whenever
that bit is clear — so **a Neutral that can still fight is impossible by construction.**

⇒ **Ship the category as "will never engage", NOT as "a legal attack target"** (§6.2 is struck —
the engine excludes Neutrals from both target lists). That is also the more useful reading for a
blind player: harmless creature vs. threat.

⚠️ **Still open:** whether any *shipped map* actually authors arg 3. That is a frequency question,
not a structural one — `probe_combat_state.js` measures it (it keeps a session-wide histogram of
every scene-kind/bucket combination seen). Do not ship the spoken category until that histogram is
non-empty.

**Keep the two axes separate in speech**, never collapse them:
`"Enemy, <name>"` / `"Enemy, <name>, not yet aware"` / `"Enemy, <name>, fighting"` /
`"Neutral, <name>"` / `"Neutral, <name>, provoked"`. Do not promote a Neutral to Enemy on engagement —
say both, so the difference is learnable.

**Bonus already in reach:** `FUN_002734f0` (RVA `0x1534F0`) is the nameplate's relative-danger index —
0 = party member, else level-difference banded 1/2/3. A ready-made "much stronger than you" cue (0.95).

### 6.6 The naviicon lead — chased and closed

`FUN_003c5130` (RVA `0x2A5130`) assigns minimap dot kinds 1 = party, **2 = neutral**, 3 = ally,
4 = enemy. It confirms the engine's four-way display split but adds nothing the mod cannot compute
directly from §6.5. **Do not hook it.**

---

## 7. Battle lifecycle

### 7.1 "Am I in battle?" — there is no global

FFXII is seamless-battle: no encounter transition, no victory screen. The authority is a per-actor
aggro bitmask, confidence 0.97:

- `*(u32*)(actor + 0xEA4)` — bit *i* set ⇔ actor index *i* has committed an action against this actor.
- `*(u8*)(actor + 0xEA8)` — attacker count. `*(u8*)(actor + 0xEA9)` — this actor's pool index.

~~**In battle** = any living party actor has `+0xEA4 != 0`.~~ ⛔ **STRUCK (S49).** The bit is set in
`FUN_0030f760:84-88` **without any hostility gate**, on the *target*, indexed by the *subject's*
`+0xEA9`. So it false-positives on an out-of-combat Cure and false-negatives while the party
attacks an enemy that has not retaliated. **Use the engine's own derived flag instead —
`*(u32*)(actor + 4) & 0x100000`**, maintained by `FUN_002fead0` (which already excludes the
self-target case). Confidence 0.90: it is only recomputed on disengage, so it may lag the engage
edge by one action — settle with `probe_combat_state.js`. Pure on-demand read either way, no polling.

⚠️ **`FIELD_ACTIVE2` (RVA `0x1F69300`) is "battle-work system alive", NOT "a battle is happening"**
(0.97). Do not use it as such.

### 7.1a RESOLVED (Session 92) — `BattleState::PartyEngaged()`, and the probe is NOT needed

**Shipped.** The struck rule above was struck for its missing FILTER, not for the field it read.
`+0xEA4` itself stands at 0.97, and its meaning — "who has committed an action against me" — is
exactly the question. The false positive S49 caught (an ally's out-of-combat Cure) is removed by
asking *who* set the bit:

> **In battle** = some **living** party-side actor has a `+0xEA4` bit set whose owner is a
> **`Faction::Foe`**.

Every input is already shipped and confirmed: `FactionOf` (read-only reimplementation of
`FUN_002f8e90`), `+0xEA9` = each actor's own pool index (0.97, so a set bit maps back to its owner),
and `BC_CURHP` for the liveness test. Two passes over the ≤40-entry actor pool, no allocation, no
game calls — cheap enough for the audio beacon to poll once per field frame.

**`probe_combat_state.js` was therefore never written, and should not be.** The 0.90
`*(u32*)(actor + 4) & 0x100000` replacement this section recommends is **not used** and needs no
confirmation: it was only ever a workaround for the missing filter, and it carries a documented
"may lag the engage edge" caveat that the filtered version does not.

**Escape mode needs no flag either.** Under this rule the state clears when foes stop targeting the
party, which is what happens once a flee actually breaks away — so the beacon resumes on its own.
Known deviation: it resumes when the escape **succeeds**, not when the player **toggles** it, so
while toggled-to-flee but still being chased the state stays engaged. Accepted as shipped; finding
the real Escape flag is a follow-up, not a prerequisite. **FOUND S179:** `u16` RVA `0x21ABE1A` bit 0 (GameArchitecture.md "Escape (flee) mode"); the route
beacon now resumes while it is set.

Implementation: `src\battle\battle_state.cpp`, `PartyEngaged()`. Consumer:
`src\navigation\audio_beacon.cpp`.

### 7.2 Event hooks

| RVA | function | fires | gives |
|---|---|---|---|
| **`0x1EF760`** | `FUN_0030f760` | per action committed (~1–10/s) | "X uses Y on Z" for **every** combatant incl. AI, **and** the battle-engage edge |
| **`0x1F3B30`** | `FUN_00313b30` | ⛔ ~~per engage / disengage~~ **per ACTION, ~2× each** (S49, 0.96) — both call sites are per-action (`FUN_0030f760:84`, `FUN_00310780:13`), and committing an action first *ends* the previous one. Using it as battle start/end emits a pair per swing | attacker + victim handles ✔ (`ev+0x08`/`+0x0C`) |
| **`0x1F2280`** | `FUN_00312280` | per enemy death ✔ (victim kind `1`, killer kind `0`) | ⛔ **none of EXP/LP/gil/loot is an argument** (S49, 0.95). gil → hook **`FUN_00469e80`** (delta is its only arg); loot → **`FUN_003180f0`** (⛔ ~~4 slots~~ **7 slots** — 5 normal + 2 rare — stride 8, `-1` = empty, and it is a **ground pickup**); EXP/LP → before/after diff of `member+0x18C`/`+0x190`. Chain ✔ in-frame. **SHIPPED S72** as the enemy-defeated + EXP/LP line |
| **`0x1EC650`** | `FUN_0030c650` | ⛔ ~~per level gained~~ **per level-GRANT, possibly multi-level** (S49, 0.94); announces once on the final level. `param_3` bit 1 = silent | "X reached level N" — read `*(u8*)(who+0x1c2)` in a **return** hook |
| ~~`0x23C7B0`~~ **`0x23C8A0`** | ⛔ ~~`FUN_0035c7b0`~~ **`FUN_0035c8a0`** | once per game-over condition **edge** | `ecx` 0xB party wipe / 0x18 guest wipe / 0x17 leader down. **No caller ever passes `0x17` to `FUN_0035c7b0`** — that branch is dead on its path (S49, 0.95) |
| **`0x16EF50` / `0x16EF00`** | `FUN_0028ef50` / `ef00` | chain level-up / break | chain announcements |
| **`0x1F05D0`** | `FUN_003105d0` | per action end | read `actor+0x744`/`+0x740` for what ended |

**Rewards are per-enemy-death, not batched** — there is no victory event (0.95). "Combat ended" =
disengage-to-zero; discriminate victory from escape by counting deaths since the engage edge (0.90,
design inference).

### 7.3 ATB / CT — solved, no hook needed

`CTZeroAOB` resolves exactly to `FUN_002f8410` (RVA `0x1D8410`), 0.99. CT lives on the **actor**:
`+0x768` current, `+0x76C` max, phase `+0x6B4`. **"Ready" is a plain integer compare
`actor[0x768] >= actor[0x76C]`** — an ATB readout is a pure on-demand read. Confidence 0.97.

### 7.4 Chain

The DrummerIX pair is stale (§1.3). The real block, found by behaviour (0.96):
chainLevel RVA `0x21A3158` (0..3), chainCount `0x21A315C`, sameKindStreak `0x21A3160`,
break count `0x21A3164`, chainKey `0x21A3168`, lastMonsterId `0x21A316C`.
Transition function `FUN_003183d0` (RVA `0x1F83D0`, 0.97).

### 7.5 Pause / time-scale — READ ONLY, and not needed

> **The mod must NEVER write any global in this subsection.** Writing the time scale, the sim
> accumulator, the pause bit, or the speed index would *drive* the game — a category change from
> observe to control that the read-only rule forbids without explicit user permission and a design
> discussion.

Speed index `DAT_01fd4a98` (RVA `0x1EB4A98`, i32, domain {0,1,2}), key handler `FUN_00233640`
(RVA `0x113640`) — ~~this is what the game's own `1`/`2`/`3` keys drive~~ (STRUCK S183: the user measured keyboard `1` = pad L1
cycling game speed, `3` = R1, `2` no observed effect; which input reaches this handler is unverified). Multiplier table at RVA
`0x7E8BA8`, values unreadable offline (hypothesis {1,2,4}, 0.85).
Modal state `DAT_02064ad3` (RVA `0x1F44AD3`): 2 = simulating, 3 = full menu (0.90–0.97).
Sim accumulator RVA `0x1F44AC0`; effective speed = `ac8 × ac4`.

**Consequence for the design: the combat log does not need to pause the game.** Every value it wants is
an on-demand read, and the log is not modal (§9). The stored pause-and-narrate design is superseded.

⚠️ **Battle Active-vs-Wait mode is NOT located** (0.30 on any candidate). It gates only the ATB advance,
never the sim accumulator. Two leads and a diff-probe are in the source report; nothing ships from this.

---

## 8. Party vitals — `4` / `5` / `6`

### 8.1 Root cause

See §1.1. The fix is the missing dereference plus validation:

```cpp
void* W = MemRead::PtrAt(Hooks::ResolveRva(0x2D9F190), 0);     // DEREFERENCE
uint32_t magic = 0;
if (!W || !MemRead::SafeReadU32(W, 0x00, &magic) || magic != 0x5071901) return false;
uint8_t bcIdx = 0xFF;
if (!MemRead::SafeReadU8(W, 0x5A7E + slot * 2, &bcIdx)) return false;
if (bcIdx >= 0x28) return false;                                // empty slot
void* bc = static_cast<char*>(W) + 0x08 + size_t(bcIdx) * 0x1C8;
```

All the BtlChr field offsets already recorded in `GameArchitecture.md` (Session 45) are unaffected —
they were always relative to the BtlChr, which is only now being computed from the right base.

### 8.2 Secondary defects to fix in the same pass

1. ~~**Never be silent.** `SpeakSlot` currently says nothing for an empty or unreadable slot, so the
   user cannot distinguish "slot 3 is empty" from "the mod is broken". Speak the mod-emitted
   `"Empty slot"` for a genuinely empty slot; log loudly for an unreadable one.~~

   **STRUCK — REJECTED BY THE USER, never shipped.** An empty or unreadable slot is **SILENT**, like
   every other mod key with nothing to report. `7` (guest) is therefore silent for most of the game,
   which is correct. The premise was wrong twice over: it argued from a session where the mod *was*
   broken, and no phrasebook exists to have "sanctioned" the string — `src/speech/phrasebook.cpp` is
   not a real file and `"Empty slot"` appears in **no** source literal. **Only the "log loudly" half
   survived**, and it fully solves the stated problem: `BattleState::DiagnoseSlot` puts W, the magic,
   and the roster entry in the log, so an empty slot and a broken mod are trivially distinguishable
   there. Do not re-propose speaking filler for empty slots.
2. **Add `PARTY` to the logger's flush list** (`logger.cpp:173`). A diagnostic that vanishes on exit is
   what made this look like an input bug for two sessions.
3. **Roster list 3 has 9 entries**, and FFXII has a 4th (guest) slot. Only slots 0–2 are reachable
   today. Consider a 4th key or making `6` cycle.
4. Correct `Docs/Controls.md:88-108`.

### 8.2a Ticks belong here, not in the combat log (user-specified)

Regen / poison / doom / disease HP drift is deliberately **excluded from the combat log** (§9.1.3e) and
surfaces here instead, as concise state rather than an event stream.

The status word `*(u32*)(bc+0x3c) | *(u32*)(bc+0x64)` is already read by `ReadSlot` (`out.status`) but is
currently **never spoken**. Wire it up: for each set bit, resolve the name from the game via
`FUN_0035d330(0x1A, bitIndex)` — the battle status-name category, table `DAT_02ebf118`
(RVA `0x2D9F118`), name index at `rec+0x00`, suppressed when `rec+0x24 == -1` (§5.5.1). Confidence 0.97.

Result: `"Vaan, HP 412 of 690, MP 30 of 44, poisoned"` — the player learns they are losing HP to poison
without a single tick entering the log.

### 8.3 What to speak

Name + `HP cur of max`, plus `MP cur of max` when the MP guard passes. `"HP"`/`"MP"` are mod-emitted
labels — the game draws those as baked gauge art, so there is no game text to read here and the
phrasebook is the correct home for them. Status names, when added, come from the game
(`FUN_0035d330(0x1a, bitIdx)` or the `DAT_022c8b08` table).

---

## 9. Combat log — design

### 9.1 Key bindings, and a conflict you must decide

Source of truth is `Docs/Controls.md`, per `memory/feedback_check_controls_md_before_input_diag.md`.

| key | DIK | game binding | verdict |
|---|---|---|---|
| `,` | `0x33` | none | **free** |
| `.` | `0x34` | none | **free** |
| **Left Shift** | `0x2A` | **Toggle Walk/Run** | **CONFLICT** |
| Right Shift | `0x36` | not listed | free (unverified, 0.8) |
| `Home` / `End` | `0xC7` / `0xCF` | none | **free** |

**The Shift problem.** The mod cannot swallow keys — it reads the game's own DirectInput buffer through
the `GetDeviceState` hook and is required to pass it as `const` (strict read-only rule, audited S44). So
`Shift+.` is *also* delivered to the game, and the game toggles Walk/Run on it. An odd number of presses
leaves the player walking — an invisible state change for a blind player, exactly the class of side
effect that cost Session 47 an entire investigation.

**DECIDED 2026-07-20 (user): `Home` / `End`.** Unbound in the game, semantically standard, one
keystroke, zero side effects. The `Shift` chord is **not** used. (Rejected alternatives: Right Shift —
probably free but only 0.8 confidence; `Shift+,`/`Shift+.` as originally requested — would flip walk/run
on every press.)

**Final binding set:**

| key | DIK | action |
|---|---|---|
| `,` | `0x33` | step **backwards** (older) |
| `.` | `0x34` | step **forwards** (toward most recent) |
| `Home` | `0xC7` | jump to **oldest** retained entry |
| `End` | `0xCF` | jump to **newest** |

Implementation note: these join the existing `g_extraDown[]` slots in `input_tracker.cpp` and dispatch
through `WM_NAVKEY` → `OnNavKey`, exactly as `4`/`5`/`6` already do. `VK_HOME`/`VK_END` are the dispatch
tokens. **Do not** grow the array without also growing its bound — and note that per §1.1 the
`g_extraDown` array growth was *never* the cause of the `4`/`5`/`6` failure, so it is not a hazard here.

Note the semantics are a *timeline*, not a chat scrollback: `,` goes back in time. That is what was
asked for and it is internally consistent — keep it.

### 9.1.1 CONTENT STRATEGY — the plan in one paragraph

**Read the game's own 102 battle messages as the primary content; cherry-pick which of them also
interrupt in realtime; synthesize our own entries only for events the game has no message for.**
Everything below implements that. Three tiers:

| tier | source | how many | phrasing |
|---|---|---|---|
| **1. Game-supplied** | hook `FUN_0028e110` (RVA `0x16E110`), read `arg0` verbatim | 102 messages | the game's, in all 12 locales, **never ours** |
| **2. Mod-synthesized** | our own hooks, where **no game message exists** | the list in §9.1.3 | phrasebook, 12 locales, ours |
| **3. Mod-only concepts** | things the game has no notion of | target confirmed, battle start/end, low-HP warning | phrasebook |

Tier 1 is the default and the bulk. Tier 2 is deliberately **small and enumerated** — a new synthesized
string requires showing that no game message covers it, because the house rule
(`memory/feedback_no_fabricated_ui_labels.md`) treats invented user-facing text as a last resort.

### 9.1.2 What the 102 messages actually cover

Decoded groups (`notes/battle_message_table_us.txt`):

| ids | group |
|---|---|
| `0x00`–`0x03` | traps |
| `0x04` | **level up** — "{0} is now level N" |
| `0x05`–`0x0C` | **action failed / cancelled** — time exceeded, interrupted, out of range, can't reach |
| `0x0D`–`0x0F` | **action announce** — begins casting / readies / uses |
| `0x10`–`0x12` | **KO** — has fallen / cast into the void / turns to stone |
| `0x13`–`0x1C` | HP/MP restored, statuses **cured**, regains consciousness |
| `0x1D` | "{0} had no effect" |
| `0x1E`–`0x23` | reserve-member (party-wide) restores |
| `0x24`–`0x26`, `0x64`–`0x65` | **You obtain / Added to inventory / gil** |
| `0x27`–`0x2C` | debuff effects (lowers strength / magick / defense / resist) |
| `0x2D`–`0x34` | **steal** results incl. failures |
| `0x35`–`0x3D` | technick outcomes — switches sides, poach, Charge, Libra, Revive |
| `0x3E`–`0x43` | **Paling / Magick Shield / White Wind** on and off |
| `0x44`–`0x4E` | boss mechanics — MP drain, level doubles, HP/MP inverted, calls for help, splits in two |
| `0x4F`–`0x5C` | **party-wide magick/magnetic fields** on and off (can't Attack / Magicks / Technicks / Items) |
| `0x5D`–`0x62` | Magick Pot, Zalera, **"Back attack!"** |
| `0x63` | empty |

### 9.1.3 The gap — what the game does NOT say, so we must

**There is no "X deals N damage to Y" message anywhere in the 102.** Confirmed by reading the whole
decoded table. Per-hit damage and healing are **numeric sprites only**. So the single most important
combat-log stream is Tier 2:

| we synthesize | why no game message | source |
|---|---|---|
| **the whole action line + its number** — `"Vaan attacks dire rat. 25"` | **no "attacks" message exists, no announce names the target, and party members are never announced at all** (§9.1.3a) | **`FUN_003112f0`** (RVA `0x1F12F0`) — attacker, target, action id and every number in ONE call (§9.1.3b) |
| ~~regen / poison / doom tick numbers~~ | **OUT OF SCOPE** — ticks do not enter the log (§9.1.3e); they surface in the `4`/`5`/`6` vitals readout instead | — |
| Miss / Block / Parry / Immune / Counter / Absorb | the words are **pixels** (§5.5) | `FUN_00328480` (RVA `0x208480`), reaction id 0–5 — **needs probe P3 to label them** |
| status **inflicted** | the table only has *cured* / *faded* (`0x18`–`0x1B`) | `FUN_0030e360` mode 3/4 + `FUN_0035d330(0x1A, bit)` for the name |
| EXP / LP amounts | only the level-up line (`0x04`) exists; gil has `0x26` | `FUN_00312280` (RVA `0x1F2280`), diff the snapshots |
| target confirmed | mod-only concept | `FUN_00311cf0` (RVA `0x1F1CF0`) |
| battle start / end | mod-only concept | `FUN_00313b30` (RVA `0x1F3B30`) |
| party member below 20 % HP | mod-only concept | `FUN_00300530` (RVA `0x1E0530`) |

Note the pleasant consequence: **status names, ability names, item names and character names are read
from the game's data in Tier 2 as well** (`FUN_0035d330`), so even our synthesized lines are only
mod-emitted in their connective tissue ("for", "damage", "misses").

### 9.1.3a Attack lines — the game will NEVER say "Vaan attacks the dire rat"

Two hard limits, both read firsthand from `FUN_00469af0` (abs `0x469AF0` / RVA `0x349AF0`):

**(a) There is no "attacks" message, and no announce ever names the target.** The only three announce
forms in all 102 are `0x0D` "{0} begins casting {1}.", `0x0E` "{0} readies {1}.", `0x0F` "{0} uses {1}."
Their arg lists are `[attacker, action]` — **no target slot**. Confidence 0.99 (read from the decoded
table).

**(b) The ANNOUNCE specifically (`0x0D`/`0x0E`/`0x0F`) is emitted only for guests and foes.**

> ⚠️ **SCOPE CORRECTION (user challenge, 2026-07-20).** An earlier draft generalised this to "the game
> says nothing when Vaan acts." **That is wrong.** The faction gate lives in `FUN_00469af0` *only* —
> the announce emitter. **Each message family has its own emitter with its own gate**, and one of them
> is party-**exclusive**. The tester spotted this from message `0x08` ("{0}'s target is too far from the
> party leader"), which is obviously about a party member's action. Correct picture:
>
> | emitter | abs / RVA | ids | faction gate |
> |---|---|---|---|
> | `FUN_004697c0` | `0x4697C0` / `0x3497C0` | `0x05` `0x06` `0x07` — time exceeded / interrupted / out of range | `if (FUN_002f8e90(param_1) != 1) return;` ⇒ **PARTY CHARACTERS ONLY** |
> | `FUN_00469a50` | `0x469A50` / `0x349A50` | `0x08` `0x0A` — target too far (from leader / from you) | **no faction gate at all** — fires for anyone |
> | `FUN_00469af0` | `0x469AF0` / `0x349AF0` | `0x0D` `0x0E` `0x0F` — begins casting / readies / uses | `if ((FUN_002f8e90() & 0x0A) == 0) return;` ⇒ guest \| foe |
>
> So the game **does** narrate party members — just never their *action announce*. Confidence 0.98
> (all three read firsthand).
>
> **Bonus: this also raises confidence in the dropped argument.** `FUN_004697c0` calls
> `FUN_002f8e90(param_1)` with the argument **visible**, and `param_1` is the acting BtlChr placed in
> the attacker context slot — the identical idiom to `FUN_00469af0`'s argument-less call. That lifts
> "the gate tests the ATTACKER" from 0.90 to **0.95**. Probe P-ANN still confirms it.

The announce emitter's gate and category map:

It calls `FUN_002f8e90()` for the 5-bucket faction mask (section 6.1) and **returns immediately
unless the mask has `0x0A` set** — `0x02` guest or `0x08` foe. Past that gate it switches on the
action-record category byte at `+0x1E`:

| category | message |
|---|---|
| 1 | `0x0D` "begins casting" |
| 2, 7, 9 | `0x0E` "readies" |
| 3 | `0x0F` "uses" |
| anything else | returns — NO MESSAGE AT ALL |
A regular party character is bucket `0x01`, and `0x01 & 0x0A == 0` ⇒ **return**. So the game says
nothing when Vaan acts.

- Category→id mapping: **0.98** (explicit constants, unambiguous).
- The `& 0x0A` gate exists: **0.98**.
- That it tests the **attacker's** faction: **0.95** (was 0.90 — raised by the sibling emitter above,
  which shows the same idiom with the argument intact). Probe P-ANN confirms.

⇒ **The attack LINE is still Tier 2, for a simpler reason than the gate: no "attacks" message exists in
the table at all, and no announce carries a target.** We synthesize `"<attacker> <verb> <target>"`.
Names still come from the game (`FUN_0035d330(0x14, actionId)` for the action, the actor pool for the
combatants).

> ~~mirroring the game's own category→verb vocabulary (1 = casts, 2/7/9 = readies, 3 = attacks/uses) so
> our wording matches the game's when both appear~~ — **STRUCK, Session 90.** This sentence shipped the
> bug. `FUN_00469af0` is a **CHARGE-phase** emitter: its single caller is `FUN_00304850` at action
> start, and its three ids all describe an action that is *about to* happen. `DamageLine` runs on the
> **applier** `FUN_003112f0`, after the hit has landed. Mirroring the announce vocabulary there put
> charge-phase wording on an execution event, and a connected enemy ability was narrated
> `"Urstrix A readies Slap on Vaan. 14"` — reported from play.
>
> **The two vocabularies are separate and must stay separate:**
>
> | | verb | where |
> |---|---|---|
> | **Charge** (the game's own sentence, read verbatim) | begins casting / readies / uses | ids `0x0D`/`0x0E`/`0x0F` |
> | **Execution** (ours, `combat_format.cpp`) | attacks (cat 0 + unidentified) / casts (cat 1) / **uses** (cats 2, 3, 5, 6, 7, 9, 10) | `DamageLine` |
>
> "readies" is not an execution verb. Do not reintroduce it into `DamageLine`'s switch.

**Does the game hand us finished text, or just templates?** Finished text — this is already established
and does not need a discovery probe. The chain is `FUN_005369c0` (RVA `0x4169C0`, binds the args) →
`FUN_00536410` (RVA `0x416410`, codec-sprintf into a 0x180-byte buffer) → `FUN_0035b990` (RVA
`0x23B990`, routes), with names resolved by `FUN_00536280(nameIdx, isPc)` (RVA `0x416280`) — including
the player-entered name via `FUN_0026dea0` when the subject is a PC. So `FUN_0028e110`'s `arg0` is a
**fully substituted sentence**, not a template with holes. Confidence 0.95; **probe P-MSG verifies it
live** and is the first probe to run.

### 9.1.3b Pairing the number to its action — one hook does it all

The requested format is `"Vaan attacks dire rat. 25"` — the number **follows its own action line**, and
is never spliced into a game-supplied message.

**`FUN_003112f0` (abs `0x3112F0` / RVA `0x1F12F0`) delivers the whole line in a single call.** Its
signature is `(result, attackerBc, targetBc, u16 actionId, u32 flags)` and the result struct already
carries every number (section 5.4):

```
attacker = arg1        target = arg2        actionId = arg3
targetHP = *(i32*)(result + 0x24)     // negative = damage, positive = heal
targetMP = *(i16*)(result + 0x2a)
attackHP = *(i32*)(result + 0x20)     // drain / absorb / recoil
attackMP = *(i16*)(result + 0x28)
outcome  = *(u8 *)(result + 0x04)     // 9 = AI preview -> DISCARD
valid    = *(u8 *)(result + 0x1c)     // must be 1
```

So **no timing heuristic, no correlation window, no dedup** is needed — which matters, because pairing
by timestamp would have been a debounce in disguise and the house rule forbids that. One call = one
`(action, attacker, target, number)` tuple = one log entry:

```
Vaan attacks dire rat. 25
Vaan casts Cure on Penelo. 50
Vaan uses Ether on Vaan. 25
```

Consequences worth knowing:

- Damage with **no number** (a miss) still produces a `FUN_003112f0` call; the reaction comes from
  `FUN_00328480` (probe P3 pending), so until then a miss logs as the action line with no number.
- Outcome `+0x04 == 9` is the AI/gambit **preview** evaluation — it must be discarded or the log fills
  with actions that never happened.

### 9.1.3c Multi-target actions — AGGREGATE into one line (user-specified)

**One action = one log entry, regardless of how many targets it hit.** A party-wide Cure must NOT
produce three entries.

| shape | line |
|---|---|
| single target | `Vaan attacks dire rat. 25` |
| whole active party | `Ashe casts Cure on party. 403 average` |
| some allies | `Ashe casts Cure on 2 allies. 403 average` |
| several foes | `Fran casts Firaga on 4 enemies. 2900 average` |

("all enemies" is avoided deliberately — the engine has **no** "all foes" targeting bit, so totality
cannot be asserted. See §9.1.3d Q3.)

Rules:
- **Aggregate on `(attacker, actionId)`**, not on call adjacency — another actor's action may interleave.
- **Average the numbers**, and say `"average"` so the figure is never mistaken for a single hit.
- Report the **count** when it is not the whole set (e.g. `"on 2 enemies"`), so "party"/"all enemies"
  stay literally true.
- **Flush on `FUN_003105d0` (RVA `0x1F05D0`), NOT on a countdown** — see §9.1.3d. Deterministic, and no
  timeout anywhere.
- **Naming the set: "party" / "N enemies". Never "Group A"** — entry-groups are unnamed integers
  (§9.1.3d Q2).

### 9.1.3d AoE aggregation — RESOLVED

**The target vector.** `actor+0x778` is a 0x208-byte **inline** vector: `+0x04` i32 count
(= `actor+0x77C`), `+0x08 + i*0x10` sceneObj, **32 slots max**. Self-validating: `8 + 32*0x10 = 0x208`,
exactly the `memset` size in `FUN_00316cf0`. Confidence **0.99**.

**Lifecycle — and the trap.**

| when | what |
|---|---|
| action start `FUN_0030f760` (RVA `0x1EF760`) | clears the vector and appends **only the primary target** ⇒ count is **1** |
| **phase 9** `FUN_00304850` (RVA `0x1E4850`, phase byte `actor+0x6B4`) | calls `FUN_0030d160(actor, id, targetHandle, actor+0x778)` which **clears and rebuilds** with the full AoE set |
| teardown `FUN_003105d0` (RVA `0x1F05D0`) | empties it, sets `0x714 = 0xFFFF`, `0x6B4 = 0` |

⚠️ **Reading `+0x77C` before phase 9 reports a false "single target".** Confidence 0.98–0.99.

**Q1 — completion signal. Do NOT count down.** The count *does* include immune/missed targets
(`FUN_00307300`'s outcome switch has no `return` on any branch; every path falls through to exactly one
`FUN_003112f0` — 0.98). But it is still **not a safe countdown**: `FUN_003191b0` gates each delivery on
`FUN_002ffa60`, and `FUN_00307300` early-returns when the target's live bit `0x10` is clear, so a target
killed by earlier splash yields N−1 calls and the aggregator would hang forever.
⇒ **Flush the pending aggregate when the action tears down: hook `FUN_003105d0` (RVA `0x1F05D0`).**
Deterministic, fires exactly once per action, and needs no timer. Confidence 0.97.

**Q2 — "Group A" is not available.** `actor+0x54` is an entry-group id written only by
`FUN_00239170` (abs `0x239170` / RVA `0x119170`) — the `setentrygroup` VM native — and by AI opcodes
`0x4048`–`0x404F` (groups 0..7) in `FUN_002fdda0` (RVA `0x1DDDA0`). Its only readers are the target
filter `FUN_0030bd00` (RVA `0x1EBD00`, cases 5/7/8) and the gambit condition `FUN_00301ad0`
(RVA `0x1E1AD0`). **No name, text id, string or bestiary link exists anywhere** — no `FUN_0035d330`
category, no `FUN_002f9860` bank indexed by it, and the `.dbg` native list has setters and integer
predicates only. Confidence 0.97, grep-complete (4 writers corpus-wide).
⇒ **Say `"4 enemies"` / `"party"`. Never `"Group A"`.**

**Q3 — target scope from the action row** (decoder `FUN_003230e0`, abs `0x3230E0` / RVA `0x2030E0`):

| field | meaning | conf |
|---|---|---|
| `row+0x0C` bit0 / bit2 / bit3 | self / allies / foes | 0.98 |
| `row+0x0C` bit1 | **whole active party, no distance test** | 0.98 |
| `row+0x0C` bit21 / bit24 / bit25 | area centred on caster / **cone** (half-angle `row[0x07]`°) / **line** (half-width `row[0x07]`) | 0.97 |
| `row+0x06` u8 | **area radius; `0` ⇒ single target** | 0.98 |
| `row+0x2C` bits `0x40`/`0x80` | may target KO'd / petrified | 0.97 |

⚠️ **Negative worth having: there is no "all foes" bit.** "All enemies" is a large radius plus the
relation filter. So the honest vocabulary is **self / a single name / party / area + count** — which is
why the phrasing above is `"N enemies"` rather than a claim of totality. Confidence 0.96, grep-complete
over all seven `row+0x0C` readers.

⚠️ **Attack decodes against the wrong row unless you substitute the id.** The target list is built with
the id from `FUN_00387090` (abs `0x387090` / RVA `0x267090`), which swaps in a per-character id when
`actor+0x714 == 0x9F` (basic Attack) — but `FUN_003112f0` receives the **raw** `0x714`. Decode the row
from the substituted id.

**Q4 — "party" is verifiable.** `FUN_0030d160`'s party branch walks `FUN_0031b810(0..3)` = roster
**list 1** (`W + 0x5A5A`, charIds — note: *not* the mod's list 3 at `+0x5A7E`, which holds BtlChr
indices) and **skips KO'd/petrified members** (`(statusA|statusB) & 0x80000003`); guests are excluded
(they enter only via the radius branch). Confidence 0.98.
⇒ Compare `actor+0x77C` against the occupied active slots and say `"on party"` **only when they match**,
otherwise `"on 2 allies"`.

**Q5 — the one-call-per-target model holds, with three amendments** (all 7 call sites of
`FUN_003112f0` located):

1. ⚠️ **The attacker can be NULL** — but ⛔ **filter on `actionId == 0xFFFF`, NOT on `attacker == 0`**
   (S49, 0.99). All 7 call sites located: `FUN_0030e130:94`, `FUN_0030e360:156` and
   `FUN_00310db0:65` are ticks (`attacker == 0`, `actionId == 0xFFFF`), but **`FUN_00310db0` calls it
   twice more with `attacker == 0` and a REAL action id** (a live value at `:85`, the constant `0xF5` at `:133`) for
   genuine party-wide effects. Filtering on the attacker alone silently drops both. This is also exactly the clean
   discriminator that keeps ticks out of the log per §9.1.3e.
2. No batching within a call (0.99).
3. ⚠️ **The N calls genuinely can interleave.** Two delivery paths exist: a synchronous burst in
   `FUN_00325260` (RVA `0x205260`) that loops the whole list in one frame, **and** per-target animation
   events via `FUN_002eb740` → `FUN_00319f50` (RVA `0x1F9F50`) → `FUN_003191b0` (RVA `0x1F91B0`), one
   target per event spread across frames. Confidence 0.97.
   ⇒ **Key the aggregate on `(attackerBc, actionId)`, never on call adjacency.**

Also: reflect and counter **re-enter** `FUN_00307300` and add extra same-key calls, so `received >
expected` is possible — the aggregator must tolerate it rather than assert. And a separate
**reserve-party burst** exists (`actor+0x6B6 != 0` takes a different branch; its count is `0x6B6`, not
`0x77C`).

### 9.1.3e Ticks are NOT combat-log entries (user-specified)

Regen / poison / doom / disease HP drift must **not** enter the log — they would flood it with
low-information entries between the actions the user actually cares about.

They surface instead in the **`4`/`5`/`6` party vitals readout** (§8), as concise state rather than a
stream of events. The status word (`BtlChr+0x3c | +0x64`) already tells us *which* effects are active,
and the names come from the game (`FUN_0035d330(0x1A, bit)`), so the readout can say e.g.
`"Vaan, HP 412 of 690, poisoned"` without logging a single tick.

Practical consequence: **`FUN_003283d0` is not needed as a log source at all.** Its only unique
contribution over the applier was tick numbers, and those are now out of scope. Keep the applier
(`FUN_003112f0`) as the single Tier-2 source and drop the second hook — fewer hooks, less risk, and it
removes the one path that had no attacker to attribute.

### 9.1.4 Realtime vs log-only — the cherry-pick

Default classification. **Realtime** = spoken immediately with `interrupt=true`; **log-only** = appended
silently, read with `,`/`.`. Every entry lands in the log regardless.

| realtime | ids | rationale |
|---|---|---|
| **YES** | `0x05`–`0x0C` | your command **failed** — you must act again |
| **YES** | `0x10`–`0x12` (party target only) | a party member went down |
| **YES** | `0x1C` | party member revived |
| **YES** | `0x3E`–`0x43` | your damage type is being **nullified** — attacking is pointless until it lifts |
| **YES** | `0x4F`–`0x5C` | a command category is **disabled party-wide** |
| **YES** | `0x61` | "Back attack!" — you are being flanked |
| **YES** | `0x04`, `0x24`–`0x26`, `0x2D`–`0x34`, `0x37`–`0x38`, `0x64`–`0x65` | level up, loot, steal, poach — one-shot results you would otherwise never learn |
| **YES** | `0x44`–`0x4E` | boss mechanics that change the fight |
| **YES** | `0x0D` | ⛔ ~~spam tier~~ **an enemy or guest BEGINS CASTING** — you can still interrupt, guard or move. **Flipped to realtime S72** (tester's call). Note it is style `0x01`, so the bus culls it beyond ~24 units: a distant caster announces nothing, and that limit was accepted rather than hooking the emitter |
| no | `0x0E`–`0x0F` | "readies" / "uses" — fire constantly, this is the spam tier |
| no | `0x13`–`0x1B`, `0x1E`–`0x23` | routine restores and cures |
| no | `0x00`–`0x03`, `0x27`–`0x2C`, `0x35`–`0x36`, `0x39`–`0x3D`, `0x5D`–`0x60`, `0x62` | situational; readable from the log |
| Tier 2 | action + number lines (incl. aggregated AoE) | **log-only** — this is the stream that made linear narration unusable |
| Tier 2 | ~~tick numbers~~ | **not logged at all** (§9.1.3e) — surfaced via `4`/`5`/`6` vitals |
| Tier 2 | party KO, below-20 % HP | **realtime** (§9.4.1) |

Two useful signals the game itself provides, both already decoded: the record's **`dedup` bit** marks
messages the game considers repetitive, and **style bit `0x20`** marks messages important enough to
bypass the distance cull. Neither is a clean importance proxy on its own (style `0x21` is cull-exempt
but covers routine heals), so the table above is by semantics, not by flag.

**Ship this as a data table keyed by message id, not as `if` statements** — the classification is a
tuning knob the user will want to adjust in play, and it should end up in `mod_config.ini` eventually.
Until then, one `static const` table in `combat_format.cpp` with a comment per group.

### 9.2 Storage

```cpp
struct LogEntry {
    uint64_t     seq;      // monotonic, never reused — the cursor's identity anchor
    uint64_t     tickMs;
    EntryKind    kind;     // damage / heal / status / ko / action / reward / chain / system
    std::wstring text;     // fully rendered, ready to speak
};
```

- Fixed-capacity ring, **100** entries, mutex-guarded. Producers are game-thread hooks; the consumer is
  the input thread — the same threading shape `battle_target_reader.cpp` already uses. Reuse that idiom.
- **Render the text at append time, on the game thread.** Game pointers (BtlChr, actor pool, name codecs)
  are only reliably readable there. The input thread must only ever touch the finished `wstring`.
- **Continuous across battles.** Never cleared on battle entry/exit, area change, or save-load; only on
  `Shutdown()`.
- Overflow drops the oldest. Because the cursor is a `seq`, an eviction that passes it is detectable —
  clamp to the oldest survivor and say so once.

### 9.3 Cursor semantics

- Starts at the newest entry and **auto-follows** while it sits there, so a new event does not strand the
  reader in the past.
- Detaches on the first `,`; re-attaches on reaching the newest entry again or pressing jump-to-newest.
- At either end, re-speak the boundary entry rather than emitting an error on every press. Do distinguish
  the genuinely empty log with one spoken line.
- Every navigation speaks with `interrupt=true` (`Speech::Output`, never `Tolk_Speak`).

### 9.4 Event sources → entries

| entry | source | RVA | conf |
|---|---|---|---|
| **the game's own composed sentence** (KO, HP/MP restored, no effect, immune, statuses inflicted, action announces) | ⛔ ~~`FUN_0028e110`~~ **→ `FUN_00536410`** (S49) — the ticker hook never receives the **message id**, only a dwell class, so §9.1.4's id-keyed policy table has nothing to key on there. The codec-sprintf sees **both** in one frame: `msgId = *(u32*)(RCX+4) & 0x7FFF`, finished string into `RDX`. It is also **upstream of the toast fork** (`FUN_0035b990:24`), where `FUN_0028e110` is not called at all | ~~`0x16E110`~~ **`0x416410`** | 0.97 |
| damage / heal / MP damage / MP restore | `FUN_003283d0` | `0x2083D0` | 0.99 |
| authoritative HP change incl. silent regen/poison ticks | `FUN_00300530` | `0x1E0530` | 0.99 |
| status applied / removed, and KO | `FUN_0030e360` (filter mode 3/4, bit 0 = KO) | `0x1EE360` | 0.98 |
| "X uses Y on Z" (all combatants) | `FUN_0030f760` | `0x1EF760` | 0.98 |
| player target confirmed | `FUN_00311cf0` | `0x1F1CF0` | 0.98 |
| battle start / end | `FUN_00313b30` | `0x1F3B30` | 0.96 |
| EXP / LP / gil / loot | `FUN_00312280` | `0x1F2280` | 0.96 |
| level up | `FUN_0030c650` | `0x1EC650` | — |
| game over | `FUN_0035c7b0` | `0x23C7B0` | 0.97 |
| chain level-up / break | `FUN_0028ef50` / `FUN_0028ef00` | `0x16EF50` / `0x16EF00` | 0.95 |

**Wording — SETTLED (§9.1.1).** Tier 1 (102 messages) is read verbatim from the game via
`FUN_0028e110`; Tier 2 is synthesized only for the enumerated gaps in §9.1.3, chief among them the
per-hit damage numbers, which have **no game text at all**.

Consequences:
- **The phrasebook shrinks a lot, but does not vanish.** It carries the mod-emitted strings — `"Empty
  slot"`, `"No target"`, `"HP"`/`"MP"` labels, the faction/aggression words from §6.5, **and the small
  Tier-2 damage templates** ("{actor} hits {target} for {N}", "{actor} heals {target} for {N}"). The
  stored design's *full* twelve-locale combat-sentence set — casting, KO, status, steal, loot — is **not
  needed and must not be built**, because the game supplies all of it.
- **Combo aggregation is dropped.** The stored design merged N same-actor/same-target hits within 750 ms
  into "executes a 3 hit combo". That was a workaround for synthesized text. The game's own bus already
  dedupes (§4.1) and phrases things its own way — imposing our aggregation on top would fight it. It
  would also be a form of dedup, which the house rule forbids without explicit permission.
- The mod still owns entries the game never narrates (target confirmed, battle start/end, our own
  reward summaries). Those, and only those, use mod-emitted phrasing.

### 9.4.1 Critical-event auto-speech — KEPT (user-confirmed 2026-07-20)

Independent of the log, default ON, configurable. The log stays the primary mechanism; these two fire in
real time so emergencies are not missed.

| event | source | detection | speech |
|---|---|---|---|
| party member KO'd | `FUN_0030e360` (RVA `0x1EE360`) | `mode ∈ {3,4}` **and** `statusBit == 0` | `"<name> is KO'd"` |
| party member crosses below 20 % HP | `FUN_00300530` (RVA `0x1E0530`) | read `curHP` **pre-call**, compare post-call against `maxHP / 5` | `"<name> below 20 percent"` |

Both are **edge-triggered — once per crossing**, latched until the unit rises back above the threshold
(or is revived). That latch is a state machine, **not** a debounce or dedup: it detects a transition
rather than suppressing a repeated event, and it is an explicit carve-out of the no-dedup rule (see
CLAUDE.md, "Code quality") — but say so in the commit message, because it will look like one.

Both hooks are already needed by the log (§9.4), so this costs no additional hooks. Gate to **party-side
units only** (`BtlChr[5] == 0` or scene-kind 3, §6.3) — an enemy dropping below 20 % must not fire it.

⚠️ `FUN_0030e360` early-returns when the effective status word did not change, so reaching its tail
already implies a real transition (0.98) — do not add your own change-detection on top of it.

### 9.5 What must NOT be built

- **No polling, no per-frame ticks.** Every entry originates from an event hook.
- **No dedup / debounce of speech.** Two exceptions only: the user asked for it in the current
  conversation, or the announcement hangs off a **per-frame / per-draw** function (and the comment
  names that function). Everything in this design is event-driven, so **none of it qualifies** —
  repeats mean a hook fires more than once per event; fix the hook. See CLAUDE.md for the full rule
  and its carve-outs (state-machine latches, collection dedup, log-only volume control).
- **No writes to game memory**, including the pause/time-scale globals.
- **No fabricated game text.**
- **No modal overlay and no `WH_KEYBOARD_LL` input intercept** — this design has no modal state.

### 9.6 File layout (respects the <500-line rule)

```
src/battle/combat_log.{h,cpp}      ring buffer, cursor, navigation commands
src/battle/combat_events.{h,cpp}   the game hooks -> normalized CombatEvent structs   (only file with RVAs)
src/battle/combat_format.{h,cpp}   CombatEvent -> localized wstring
src/battle/party_status.{h,cpp}    EXISTING — fix per §8
src/ui/battle_target_reader.*      EXISTING — re-source per §3
src/speech/phrasebook.{h,cpp}      NEW — mod-emitted strings, 12 locales
```

Keeping every RVA in `combat_events.cpp` means a future RVA correction touches one file.

---

## 10. Implementation plan

Ordered by dependency and by value-per-risk. **Each phase is gated on its probes (§11) passing.**
Per `CLAUDE.md` FRIDA-FIRST, no C++ is written for a phase until its probe has confirmed the model live,
and the user must give explicit permission to port.

**Phase 1 — `4`/`5`/`6` party vitals, and the codec fix.** §8 and §1.7. Smallest, highest-certainty,
fixes two shipped bugs. Needs **no probe** (both are proven offline — the deref at 0.99 from three write
sites, the escape rule at 0.98 from the game's own interpreter, and the codec fix is testable offline
against the decoded message table) but does need a runtime pass. Also: `PARTY` into the flush list,
"Empty slot" speech, `Controls.md` correction.

**Phase 2 — `;` committed target.** §3. Probes P-B, P-C, P-D, P-E. Re-source `;` and `p` from the
committed target; demote the browse readout. Highest user-visible win.

**Phase 3 — Neutral category + aggression axis.** §6. Probe P-NEUTRAL first — **if the bucket is empty
in practice, ship the aggression axis instead and do not add a third faction name.**

**Phase 4 — combat log skeleton + Tier 1 (the game's own messages).** §9. Ring buffer, cursor,
`,`/`.`/`Home`/`End`, plus the single hook ⛔ ~~`FUN_0028e110`~~ **`FUN_00536410`** (RVA `0x416410`,
S49 — it is the only point carrying the message **id** alongside the finished sentence, and the id
is what §9.1.4's realtime policy table keys on). **One hook, no formatter, and the log
immediately carries real localized combat text** — the fastest path to something useful, and it lets the
navigation UX be tested against real traffic before any synthesis exists. Add the §9.1.4 realtime table
here as a `static const` id→policy map.

**Phase 5 — Tier 2 (synthesized), the action+number line.** §9.1.3a/b. **One hook: `FUN_003112f0`**
(RVA `0x1F12F0`), which yields `"Vaan attacks dire rat. 25"` complete — attacker, target, action and
number in one call, no correlation logic and no second hook (ticks are out of scope, §9.1.3e). This is
the core of the feature: the game has no attack message and no announce ever names a target.
Single-target only in this phase.
**Miss/Block/Immune waits on probe P3** — do not guess those six words.

**Phase 5b — AoE aggregation.** §9.1.3c, mechanics resolved in §9.1.3d. Key on `(attackerBc, actionId)`,
**flush on `FUN_003105d0`** (RVA `0x1F05D0`) — never a timer, never a countdown. Filter
`attacker == NULL || actionId == 0xFFFF` (those are status ticks). Read `actor+0x77C` only at/after
phase 9. Tolerate `received > expected` (reflect/counter re-enter). Decode the action row from the
`FUN_00387090`-substituted id, not the raw `0x714`.

**Phase 5c — status inflicted, ~~EXP/LP~~.** **EXP/LP SHIPPED in Session 72** — hook `FUN_00312280`
(`0x1F2280`), snapshot/diff `BtlChr+0x18C`/`+0x190` across roster list 3 slots 0-8, largest delta,
folded into the enemy-defeated line as `"<enemy> defeated. 34 EXP, 2 LP."` The `FUN_0028fb80` popup
was considered and rejected: its argument identity is only ~0.85 (dropped register args). Status
inflicted remains the open Tier-2 gap.

**Phase 6 — critical-event auto-speech.** §9.4.1. KO and below-20 % HP, edge-triggered, party-side only.
No new hooks — both are already installed by Phase 5.

**Phase 7 — mod-owned entries.** Target confirmed, battle start/end, reward summaries — the entries the
game does not narrate. Phrasebook work belongs here, and it is now small (§9.4).

**Deliberately deferred:** critical-hit annotation (probably does not exist, §5.5), element/weakness
(needs the action-record layout), Active-vs-Wait mode, steal results, gambit-fire narration.

---

## 11. Probe plan — the gate

All probes are **attach-mode, read-only**, and obey the console budget (file-only verbose output; console
output O(unique events), never O(N)) per `memory/feedback_console_budget_frida_only.md`. **Claude writes
them; the user runs them.**

Suggested single script `probe_combat_system.js` with switchable sections, registered in `run_frida.bat`.

| # | claim | conf now | pass condition |
|---|---|---|---|
| **P-A** | `actor+0x08` == own handle | 0.98 | `bad = 0` over a full battle vs the encoder formula |
| **P-B** | `FUN_00311cf0` = the commit, once per confirm | 0.98 | one hit per Confirm press; `0xBB8 == argTgt`, `0xBA0 == argCmd` |
| **P-C** | `0x710`/`0x714`/`0x6B4`/`0x77C` semantics | 0.98 | at `FUN_0030f760`: `0x714 == argId`, `0x710 == argTgt`, `tgtCount >= 1` |
| **P-D** | `P+0x9FD8` is browse-only | 0.99 | scrolling the target list moves `P+0x9FD8` every step while `0x710`/`0xBB8` stay put until Confirm |
| ~~**P-E**~~ | ~~leader getter `DAT_0209a1f0[3]`~~ | — | ⛔ **CANCELLED (S49).** Refuted offline at 0.97; the real getter is `W+0x5AA4` (§3.4), read firsthand at 0.99. No probe needed |
| **P2** | `FUN_003283d0` arg semantics | 0.99 | hit an enemy → `(neg, 0, 0)`; cast Cure → `(pos, 1, 0)`; use an Ether → `(pos, 1, 1)` |
| **P1** | `DamageModAOB` byte offset in `FUN_00300530` | 0.97 | `Memory.scanSync(base+0x1E0530, 0x100, '49 8B C8 44 8B F2')` returns exactly one hit |
| ~~**P3**~~ | ~~reaction-word ids 0–5 and outcome codes 1–5, 0xB~~ | — | ⛔ **CANCELLED (S49).** It required the user to **read the on-screen word — invalid: the tester is blind.** Resolved offline instead by backtracing the processing code: `result+0x04` names the mechanic via the equipment slot that gated the roll (§14.2, 0.98). `FUN_00328480` dropped from the design |
| ~~**P4**~~ | ~~does a critical hit set any flag?~~ | — | ⛔ **CANCELLED (S49, user decision).** No crit flag exists (0.95 negative, grep-complete); FFXII's equivalent is **Combo**, and the tester's call is that combos are **obvious from the audio** so nothing is announced. With no annotation to build, the negative no longer gates anything. See §14 |
| **P-NEUTRAL** | is the Neutral bucket non-empty in real play? | **0.55** | log `kind`, `aiData` dword 0, `+0xBC4`, `+0xDD4`, LOS mask for every non-PC while walking past a passive and an aggressive monster |
| ~~**P-DUMP**~~ | ~~which bank / what is the id→text table~~ | — | **CANCELLED — already answered offline.** The file was extracted in the VBF dump; `tools/parse_battle_message.py` decodes all 102 entries (§4.6). No probe needed. |
| ~~**P-LOG**~~ | ~~the game's own battle-log list is readable~~ | — | **CANCELLED — the premise was wrong.** There is no log; `+0x4098` is a pending queue with zero scrollback (§4.5). |
| **P-MSG** | ⛔ **retargeted (S49):** ~~`FUN_0028e110`~~ → **`FUN_00536410`** sees the message **id** (`RCX+4 & 0x7FFF`) *and* the finished string (`RDX`) in one frame | 0.97 | `probe_combat_messages.js`: every line reads as a complete, correctly punctuated sentence; ids present; no `<ESC?xx>`. **This is the key messaging probe.** |
| **P-ANN** | `FUN_00469af0`'s `& 0x0A` gate tests the **attacker's** faction, so party members are never announced | 0.90 | hook `FUN_00469af0` (RVA `0x349AF0`), log `args[0]` action id + `args[1]`/`args[2]` BtlChr + whether `FUN_0046ab10` was reached. Attack once with a party member and once let an enemy act: expect a message for the enemy and **none** for the party member |
| **P-PAIR** | `FUN_003112f0` fires exactly once per (action × target) and its result numbers match the on-screen sprite | 0.98 | log `(attacker, target, actionId, res+0x24, res+0x2a, res+0x04)`; one basic attack ⇒ one line whose `+0x24` magnitude equals the number shown |
| ~~**P-DWELL**~~ | ~~`node+0x184` receives the per-style dwell, not the 150-frame cap~~ | — | ✅ **CANCELLED — settled offline (S49, 0.98)** by the G2 disassembly. `FUN_0028e110` → `FUN_002bdb20(dwellClass,&A,&B)` gives `A = DAT_0209e610[dwellClass-1]`; `A` goes out as `R9D` to `FUN_002bdb00`, which shuffles it into `FUN_002be8d0`'s 5th stack arg, where `MOVZX EAX, word ptr [RSP+0x70]; MOV word ptr [RBX+0x184],AX` stores it. Same dump literally confirms `mgr = *(u64*)(P+0x8FA0+0x1058)` |
| **P-11** | ability row `+0x00` is the name text id | 0.90 | resolve for Cure and compare against the UI string |

**Nothing from §5.5, §6.3 (authored trait), §7.5 (Active/Wait), or the Neutral category ships until its
probe lands.** With P-DUMP cancelled, **P-MSG is the one to run first** — it is a single hook and it
unblocks the whole Phase-4 log.

---

## 12. Address quick reference

| what | abs | RVA | conf |
|---|---|---|---|
| BtlWork pointer (magic `0x5071901` at `+0`) | `0x2EBF190` | **`0x2D9F190`** | 0.99 |
| actor pool base ptr / count | `0x208E688` / `0x208E6A0` | `0x1F6E688` / `0x1F6E6A0` | canonical |
| battle HUD context ptr `P` | `0x209BE80` | `0x1F7BE80` | canonical |
| **commit (player confirm)** | `0x311CF0` | **`0x1F1CF0`** | 0.98 |
| **action start (all combatants)** | `0x30F760` | **`0x1EF760`** | 0.98 |
| action clear | `0x3105D0` | `0x1F05D0` | 0.98 |
| **HP writer** | `0x300530` | **`0x1E0530`** | 0.99 |
| **MP writer** | `0x300CE0` | **`0x1E0CE0`** | 0.99 |
| **flying number spawn** | `0x3283D0` | **`0x2083D0`** | 0.99 |
| reaction-word popup | `0x328480` | `0x208480` | 0.98 |
| status-name popup | `0x3284D0` | `0x2084D0` | 0.98 |
| result applier (chokepoint) | `0x3112F0` | `0x1F12F0` | 0.98 |
| damage calculator | `0x385F60` | `0x265F60` | 0.98 |
| status apply / remove | `0x30E360` | `0x1EE360` | 0.98 |
| KO message | `0x469BB0` | `0x349BB0` | 0.95 |
| battle message bus | `0x46AB10` | `0x34AB10` | 0.97 |
| message-context builder | `0x469EC0` | `0x349EC0` | 0.95 |
| **`battle_message.bin` table (format strings at `+0x14`)** | `0x2EBF018` | **`0x2D9F018`** | 0.93 |
| message arg binder → codec-sprintf → route | `0x5369C0` / `0x536410` / `0x35B990` | `0x4169C0` / `0x416410` / `0x23B990` | 0.95 |
| **finished-message hook (`arg0` = string)** — one caller only | `0x28E110` | **`0x16E110`** | 0.98 |
| message-ticker manager alloc / init | `0x2BDCE0` / `0x2BED30` | `0x19DCE0` / `0x19ED30` | 0.99 |
| ticker node acquire (steals oldest, else DROPS) | `0x2BE8D0` | `0x19E8D0` | 0.99 |
| ticker renderer (2 lines) / state machine | `0x2BF230` / `0x2BEAC0` | `0x19F230` / `0x19EAC0` | 0.99 |
| **variable-length escape decoder (selector `0x29`)** | `0x3FFBC0` | `0x2DFBC0` | 0.98 |
| name resolver for message args | `0x536280` | `0x416280` | 0.95 |
| message dedup ring | `0x2EBB560` | `0x2D9B560` | 0.95 |
| battle status-name table (cat `0x1A`) | `0x2EBF118` | `0x2D9F118` | 0.97 |
| master-data section binder | `0x236C30` | `0x116C30` | 0.95 |
| STATIC master-data container | `0x208E680` | `0x1F6E680` | 0.95 |
| popup ring (slot `+0x40` = ASCII digits) | `0x2B457D0` | `0x2B357D0` | 0.95 |
| popup slot initialiser | `0x506700` | `0x3E6700` | 0.96 |
| engage / disengage | `0x313B30` | `0x1F3B30` | 0.96 |
| enemy death → rewards | `0x312280` | `0x1F2280` | 0.96 |
| level up | `0x30C650` | `0x1EC650` | 0.96 |
| game over | `0x35C7B0` | `0x23C7B0` | 0.97 |
| chain level-up / break notify | `0x28EF50` / `0x28EF00` | `0x16EF50` / `0x16EF00` | 0.95 |
| chain level / count globals | `0x22C3158` / `0x22C315C` | `0x21A3158` / `0x21A315C` | 0.96 |
| CT max formula | `0x2F8410` | `0x1D8410` | 0.99 |
| **3-way faction classifier** | `0x263BE0` | **`0x143BE0`** | 0.99 |
| 5-bucket faction mask | `0x2F8E90` | `0x1D8E90` | 0.99 |
| relation test (friendly / hostile) | `0x30AB40` | `0x1EAB40` | 0.99 |
| foes list builder / allies list builder | `0x46B870` / `0x46B690` | `0x34B870` / `0x34B690` | 0.93 |
| aggression decider | `0x308040` | `0x1E8040` | 0.85 |
| relative-danger index | `0x2734F0` | `0x1534F0` | 0.95 |
| master-data relocation `x + _DAT_01f83530` | `0x20E600` | `0xEE600` | 0.98 |
| ability/action table | `0x2EBF138` | `0x2D9F138` | 0.95 |
| codec-string pool | `0x2EBF170` | `0x2D9F170` | 0.95 |
| speed index (READ ONLY) | `0x1FD4A98` | `0x1EB4A98` | 0.98 |
| game-over state bitfield | `0x22C83E8` | `0x21A83E8` | 0.97 |
| battle tick counter | `0x208D45C` | `0x1F6D45C` | 0.90 |

---

## 13. Source reports

Full working notes, including every rejected hypothesis and the complete probe pseudo-code, are archived
alongside this document. Per the MANDATORY DECOMPILE ARCHIVE rule they belong in
`..\FFXII-Decompile\notes\` when this work lands:

- `research_combat_messaging.md` — text pipeline, message bus, banks
- `research_target_confirm.md` — browse vs committed target, actor action fields
- `research_damage.md` — damage/heal/status pipeline, result struct, flags
- `research_hostility.md` — faction partition, Neutral, aggression axes
- `research_battle_state.md` — lifecycle, ATB, chain, rewards, pause

---

## 14. Session 49 decisions — critical hits and combos (user, 2026-07-20)

**No "Critical" annotation will be built. No combo announcement either.**

The offline sweep found **no critical-hit flag anywhere** in the pipeline (grep-complete over the
41-slot scratch bank `FUN_003849b0` zeroes, every writer/reader of `result+0x04` and `result+0x18`,
and the random-damage handlers `FUN_0038cc10`/`FUN_0038cd60` — whose `(rand%A+1)*(rand%B+1)` **is**
FFXII's damage variance). Negative finding, 0.95.

What the game has instead is **Combo**: `FUN_00388660` (RVA `0x268660`) rolls the weapon's combo
stat, derives the extra-hit count from the attacker's HP fraction (`<1/16`→64%, `<1/8`→32%,
`<1/4`→16%, else 8%, 11 Bernoulli trials, capped at 11), stores `count<<4` in the per-attacker
scratch byte `+0x12` and sets **`result+0x18` bit 1**. The extra hits replay through the
`actor+0x6b6` queue.

**Tester's call, and it settles the design question:** FFXII's combos are the closest thing it has
to critical hits, and **they are obvious from the audio**, so speaking them adds nothing. Combined
with the negative finding, that closes the item outright:

- ⛔ The stored design's `"{actor} attacks {target} for {N} damage. Critical"` template is **dead**
  — the flag it needs does not exist.
- ⛔ A "Combo ×N" annotation is **not wanted**. Do not build it, even though `result+0x18 & 2`
  makes it cheap.
- ⇒ **Probe P4 (does a critical hit set any flag?) is CANCELLED.** It required ~200 attacks purely
  to close a negative that no longer gates anything.

This also removes the volume burden from `probe_combat_damage.js`: it no longer needs a long
attack sample, only enough calls to answer the applier call-rate question and to name the reaction
words.

### 14.1 Tier-2 source: hook `FUN_003112f0`, but MONITOR ONLY (user decision, 2026-07-20)

**Finding that forced the decision:** `FUN_003112f0` is called **per actor per frame** (~20/sec),
not once per damage event. The frame loop registers `FUN_00233f70` as a per-object update callback
(`*(code**)(obj + 0x38)`), which runs `FUN_00310db0`, which calls
`FUN_003112f0(result, 0, bc, 0xFFFF)` every tick. Live: ~1275 calls against ~10-20 real hits.
§9.1.3b's "one call = one fully-resolved outcome" is true of the call's **content** and wrong about
its **rate**.

**Decision.** Keep `FUN_003112f0` (RVA `0x1F12F0`) as the Tier-2 source — it is the only place that
carries attacker, target, action and number together, so any alternative reintroduces the
correlation logic §9.1.3b was designed to avoid.

**The governing principle (user, verbatim intent): the game having per-frame work is fine; the MOD
must not add to it. We may monitor.** So the hook is permitted, and the constraint moves to the
detour body.

**Mandatory detour shape.** Reject on the cheapest possible test FIRST. The result struct is filled
by `FUN_00385f60` *before* the applier runs, so every field is valid on ENTRY — no return hook.

```cpp
void __fastcall HookedApply(void* result, void* atkBc, void* tgtBc,
                            uint16_t actionId, uint32_t flags) {
    // HOT PATH: ~20 calls/sec/actor. ~99% are status ticks and must die on the first compare.
    if (actionId != 0xFFFF) {                 // F2: filter the ACTION ID, never `attacker == 0`
        uint8_t valid = 0;                    // (FUN_00310db0 makes real calls with a null attacker)
        if (MemRead::SafeReadU8(result, 0x1C, &valid) && valid == 1)
            CombatEvents::OnApply(result, atkBc, tgtBc, actionId);   // rare: the real work
    }
    s_origApply(result, atkBc, tgtBc, actionId, flags);
}
```

**Forbidden in the common path** (anything before the `actionId` compare, or on the tick path):
allocation · locking · `std::wstring` construction · `Log::Write` · actor-pool scans · any SEH read
(the `actionId` compare needs none) · speech. All of that belongs behind the filter, where it runs
a handful of times per battle rather than 20 times a second.

**Precedent:** `battle_target_reader.cpp` already hooks the per-render `FUN_002bfd20` with a cheap
early-out, so this is consistent with shipped practice rather than a new category of risk.

### 14.2 Reaction outcomes — read `result+0x04`, NOT the sprite (backtraced 2026-07-20)

⛔ **STRIKE §5.5's "use the message bus instead; do not chase the words" and probe P3's "user reads
the on-screen word".** The tester is blind — **any step requiring visual confirmation is invalid by
construction**, and designing one was a process failure. The words are also genuinely absent from
the binary. Both problems dissolve once the mechanic is backtraced through the processing code.

**The chain, read firsthand:**

`FUN_00384e50(bc)` returns an equipment mask, built from three tests — `0x1000` is the
nothing-equipped sentinel:

| bit | set when |
|---|---|
| 0 | MAIN-HAND occupied — the `i16` at `bc+0x50` is not `0x1000` |
| 1 | OFF-HAND occupied — the `i16` at `bc+0x52` is not `0x1000` |
| 2 | animation-set hash is `0x2902032B` |
`FUN_00389370:73-81` zeroes each defensive rate unless its gate bit is set:
`DAT_02aedff0` needs **bit1 (off-hand)** · `DAT_02aedff4` needs **bit0 (main-hand)** ·
`DAT_02aedfec` needs **bit2 (animation)**.
`FUN_003896b0:38-70` rolls them in order and writes the outcome into **`result+0x04`**.

**⇒ `result+0x04` alone names the mechanic**, identified by which equipment slot gated the roll —
mechanically unambiguous, and readable at the applier hook the mod already installs.

| `result+0x04` | mechanic | source |
|---|---|---|
| 0 | normal hit | formula ran |
| **1 / 2** | **PARRY** — weapon | rate `bc+0x32`, gate = main-hand `bc+0x50 != 0x1000` |
| **3 / 4** | **BLOCK** — shield | rate `bc+0x33`, gate = off-hand `bc+0x52 != 0x1000` |
| **5** | **EVADE** | rate `bc+0x31`, gate = animation set |
| 6 | no effect / already KO / immune-by-bitmap | `FUN_00385f60:92,184,344` |
| 7 | **nullified** by target attribute | `FUN_00390ab0:152` |
| 8 | **reflected** (retargeted) | `FUN_00390ab0:121` |
| 10 | **absorbed** | `FUN_00385f60:167,347` |
| 0xB | avoided (attr `0x20000` roll) | `FUN_00387fd0:104` |

Confidence **0.98** — gate function and roll ladder both read firsthand; the `+0x50`/`+0x52`
weapon/shield slot semantics are corroborated by the accessory-bonus rows in `FUN_00389370:24-60`.

**Consequences:**
- **`FUN_00328480` is dropped from the design.** One hook, not two — the popup added nothing the
  result struct does not already carry.
- **Probe P3 is CANCELLED.**
- The spoken terms ("blocked", "parried", "evaded") are **mod-emitted by necessity** — no game text
  exists for them, which is exactly the sanctioned exception in `CLAUDE.md`. They belong in the
  phrasebook, and they describe the *mechanic we identified*, not a word we guessed off a sprite.

**Process lesson, recorded so it is not repeated: never design a confirmation step that requires
seeing the screen.** When a value has no text behind it, backtrace the processing code to the
condition that produced it — as was done on DQ7R — rather than asking for a visual reading.

### 14.3 Phase 2 gate — PASSED live, plus one correction to §3.4's resolution order

**The browse-vs-committed divergence is DEMONSTRATED (2026-07-20).** Scrolling the target cursor
across two enemies without confirming:

```
BROWSE#1  cursor=0x20000f | QUEUED act=0x96 tgt=0x20000e | ACTIVE act=0x4021 tgt=0x0
BROWSE#3  cursor=0x200010 | QUEUED act=0x96 tgt=0x20000e | ACTIVE act=0x4021 tgt=0x0
BROWSE#4  cursor=0x20000f | QUEUED act=0x96 tgt=0x20000e | ACTIVE act=0x4021 tgt=0x0
*** CONFIRMED: cursor moved 0x20000f -> 0x200010 while the committed target did NOT change. ***
```

**Stronger than §3.1 predicted:** the committed target `0x20000e` is a **third** enemy, distinct
from both browsed handles. So the shipped `;` readout does not merely lag — it names a unit the
character is not acting on at all, while the real target is one the cursor never visited.
`P + 0x9FD8` is browse-only, confirmed. Confidence **0.99**.

Also confirmed in the same run: **one commit per confirm** (`+0xBA0` = command, `+0xBB8` = target,
flag `0x4000` set) and **zero phantom commits** (`cmdId == 0x113`) during normal play.

#### ⛔ CORRECTION to §3.4 — do not prefer ACTIVE blindly

§3.4 says *"active (`0x710`) first, else queued (`0xBB8`)"*. **That is wrong as written.** In this
capture the leader's ACTIVE slot held **`act=0x4021` with `tgt=0x0`** — an **AI/behaviour opcode**,
not an ability id (the ability table has 543 rows, ids `< 0x21F`; enemy activations were seen
across a whole `0x4000`+ band). Preferring ACTIVE unconditionally would have reported a null target.

**Correct resolution order:**

```c
// ACTIVE is only meaningful when it holds a REAL ability id and a non-null target.
uint16_t aAct = *(u16*)(actor + 0x714);
int32_t  aTgt = *(i32*)(actor + 0x710);
if (aAct != 0xFFFF && aAct < abilityCount && aTgt != 0) return aTgt;   // acting

// else fall back to the queued commitment
if ((*(u64*)actor & 0x4000) && *(i16*)(actor + 0xBA0) != 0) return *(i32*)(actor + 0xBB8);
return 0;                                                             // nothing committed
```

⚠️ **Guard EVERY ability-row lookup with `id < count`.** `actor+0x714` is not exclusively an
ability id, and an unguarded index reads far outside the table. Confidence 0.98.

Note also that `+0xBA0`/`+0xBB8` **retain their last committed values after the queued flag clears**
(`QUEUED(0)` while the fields still read `0x96`/`0x20000e`), so the flag must be tested, not just
the fields.

### 14.4 Neutral vs passive — settled by tester game-knowledge (2026-07-20)

Two independent lines of evidence converge, which resolves what Phase 3 should actually be:

- **Decompile (F20):** the target-list builders emit only their own group — `FUN_0046b870` group 0
  (foes), `FUN_0046b690` group 2 (allies). **Group 1 (NEUTRAL) reaches neither emit branch**, so a
  Neutral is not a legal target.
- **Tester:** *"the only completely neutral characters in the game can't be targeted"*, and the
  monsters people call passive are *"enemies that only become enemies if attacked first."*

⇒ **Engine-NEUTRAL = untargetable NPCs.** They are not a combat category at all, and the field
entity scanner already covers them. **Adding "Neutral" beside "Enemy" in the battle readout would
be wrong**, which retires §6.2's original proposal for good.

⇒ **The real feature is the AGGRESSION axis on foes**: a creature that has not yet engaged versus
one that has. That matches §6.5's "keep the two axes separate" and is more actionable than a static
label — it answers *is this a threat right now*.

Shippable today: **`aiData & 0x20` = never engages** (0.98, `aiData = *(u64*)(actor+0x6A0)`).
NOT shippable: the on-sight vs range-gated split — `FUN_00308040` is a gambit predicate, not a
trait getter (§14.3 note), so that distinction stays inferred until the trait table shows a
measurable difference between a passive and an aggressive creature.

### 14.5 KO announcements come from the GAME, not the mod (2026-07-20)

The tester requires poison / doom / death KOs to be announced. **They already are — by the game —
and the mod must NOT add its own.**

`FUN_00300530` (the HP writer) calls `FUN_00469bb0` on its KO path, which emits **message `0x10`
"{0} has fallen"** whenever its gate passes. That gate is
`FUN_002fa390(bc)` = `(FUN_002f8e90(bc) & 7) != 0` — buckets `0x01 | 0x02 | 0x04` =
**party | guest | ally**. Three consequences:

1. Because it hangs off the **HP writer**, it fires for **any cause** — a hit, a poison tick, doom.
2. Because the gate is party-side, it fires for the units the player cares about and not for foes.
3. Message `0x10` has style `0x23` (**cull-exempt**, bit `0x20` set) and `dedup=0`, and `0x10`–`0x12`
   are already in the Tier-1 realtime list — so it reaches the player verbatim at any distance.

⇒ **The mod emits no KO line of its own.** Doing so would duplicate text the game supplies, which
the "never hardcode user-facing speech when the game provides it" rule forbids. `0x11` (cast into
the void) and `0x12` (turns to stone) are covered by the same path.

**What the game has NO text for is the 20% warning**, so that one stays mod-emitted — and it is the
only reason the tick path is monitored at all.

### 14.6 Status ticks: monitored, never logged (user, 2026-07-20)

Ticks must not produce log entries (§9.1.3e) — regen/poison/doom drift would bury the actions the
player cares about. But they must still be *watched*, because poison and doom kill.

The applier hook therefore splits:
- **`actionId != 0xFFFF`** → full Tier-2 handling (log line + vitals check).
- **`actionId == 0xFFFF`** → **one read** of `result+0x24`; a zero delta (the overwhelming majority)
  exits immediately, and a non-zero one goes to the vitals check ONLY. **No log entry, ever.**

This respects the monitor-only rule of §14.1: the tick path adds a single comparison in the common
case and never allocates, locks or formats.

### 14.7 Readout order: name, STATUSES, HP, MP (user, 2026-07-20)

Statuses go **directly after the character name**, before the numbers. In real-time combat the
player needs to know they are poisoned or stopped before they need an exact HP figure, and waiting
through two numbers to reach it is too slow. Applies to the `4`/`5`/`6`/`7` vitals readout:

    "Vaan, Poison, HP 412 of 690, MP 30 of 44"
