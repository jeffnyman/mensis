# The Chalice Dungeons

How Bloodborne's Chalice Dungeons are put together, read from the game's own tables and layouts.
The short version: **a "random" Root Chalice dungeon is one of 100 or 200 pre-built layouts**,
chosen by number, and the rites (Fetid, Rotted, Cursed, Sinister) change which enemy and gem
tables those layouts draw on.

```
python -I tools/chalice.py work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/chalice/chalice-1.09.md
```

`chalice.py` regenerates every table on this page from the data (1.09 over 1.00).

## The chain

```
chalice item (EquipParamGoods id) == HolygrailExParam row id
    ├── ritualLv          depth 1–5
    ├── holygrailTypeId   0 Pthumeru, 1 Hintertomb, 2 Loran, 3 Isz
    ├── variationNum      how many layouts the ritual can produce
    ├── mapBaseId ──► layout folder(s) map/mapstudio/m29_AB_C?_00/, one MSB per variation
    ├── requiredMaterialId ──► RitualRequiredMatParam: echoes + up to five materials
    └── dungeonNameBaseId ──► place-name table (地名)
rites: DungeonFeatureParam ──► NPC row offset, gem-generation offset, enemy-wide special effect
```

Every link was checked against the data. The ritual row's ID **is** the chalice's item ID (6100
is the Pthumeru Root Chalice), and `variationNum` matches the number of layout files exactly for
every chalice in the game.

## Root chalices: 100 or 200 layouts, not random generation

The `m29` layout folders fall into two families. In one, many folders hold **exactly 100**
layouts each. Matching them to the rituals' `mapBaseId` settles what they are:

| Chalice | Depth | Variations | Layout folders |
|---|---:|---:|---|
| Pthumeru Root Chalice (6100) | 1 | 100 | `m29_10_00_00` (100 files) |
| Central Pthumeru Root Chalice (6200) | 2 | 100 | `m29_20_00_00` (100) |
| Hintertomb Root Chalice (6210) | 2 | 100 | `m29_21_00_00` (100) |
| Lower Pthumeru Root Chalice (6300) | 3 | 100 | `m29_30_00_00` (100) |
| Sinister Lower Pthumeru Root Chalice (6302) | 3 | 100 | `m29_30_20_00` (100) |
| Cursed and Defiled Root Chalice (6401) | 4 | 200 | `m29_40_10_00` + `m29_40_11_00` (100 + 100) |
| Pthumeru Ihyll Root Chalice (6500) | 5 | 200 | `m29_50_00_00` + `m29_50_01_00` (100 + 100) |
| … | | | (all 14 Root chalices in `work/chalice/chalice-1.09.md`) |

`mapBaseId` *ABC* names folder `m29_AB_C…`: 100 → `m29_10_00_00`, 302 → `m29_30_20_00`. Depths 1–3
have 100 variations in one folder, depths 4–5 have 200 split across two. The variations are
genuinely different dungeons. Five of the Pthumeru Root's 100 have 760–969 map pieces, 152–208
enemies and 38–57 treasures.

So a Root Chalice ritual picks one layout out of a fixed, finite set that ships with the game.
How the game picks the number, and how that number relates to the dungeon "glyph" players share,
isn't established yet.

**Story chalices** (the `xx09` IDs, e.g. 6109 Pthumeru Chalice) have `variationNum` 1, and a
`…_90_00` folder each: the one fixed layout. Most of these folders hold a second, near-identical
file (`_90_01`: same map pieces, objects and treasure, one more enemy and two more dummy
enemies), the same pattern as the main maps' state variants.

The other family, `m29_01_…` to `m29_09_…`, looks like a library of tiles and blocks:
`m29_01_00_00` holds everything (2,010 parts, including 233 enemies and 52 treasures), while
siblings such as `m29_01_00_01` hold only map pieces and collision. How the library relates to the
pre-built layouts isn't established yet.

## What a ritual costs

`RitualRequiredMatParam` holds the echoes and materials. A few, for scale:

| Chalice | Echoes | Materials |
|---|---:|---|
| Pthumeru Root Chalice | 1,000 | Ritual Blood (1) ×2, Tomb Mold (1) ×3 |
| Hintertomb Root Chalice | 1,800 | Ritual Blood (2) ×8, Tomb Mold (2) ×10, Bloodshot Eyeball ×3 |
| Great Pthumeru Ihyll Chalice | 11,500 | Ritual Blood (5) ×9, Red Jelly ×4, Arcane Haze ×25, Living String ×1 |
| Isz Root Chalice | 11,500 | Ritual Blood (5) ×13, Tomb Mold (5) ×5, Pearl Slug ×3, Red Jelly ×1 |

## Dungeon names are composed

The place-name table has a block of Chalice Dungeon names whose IDs spell out what they are:

| ID part | Means |
|---|---|
| depth × 100000 | 3, 4, 5 |
| base × 1000 | 0 Pthumeru, 1 Hintertomb, 2 Loran, 3 Isz |
| last three digits | 000 plain, 110 Fetid, 200 Rotted, 400 Sinister, 700 Cursed, 009 Fixed |

So 502700 is "Cursed Lower Loran", and 300009 "Lower Pthumerian Labyrinth (Fixed)". The ritual's
`dungeonNameBaseId` gives the base name.

## Rites

The optional rites are rows of `DungeonFeatureParam`, named from the menu table
`SP_メニューテキスト`. Each can shift which rows the layout's references resolve to:

| Rite | Feature rows | Mechanism |
|---|---|---|
| **Fetid Offering** | 11 | NPC rows **+2,000,000**: a placed enemy resolves to a different `NpcParam` row |
| **Rotted Offering** | 20–29 | no offsets: sets an event flag the dungeon event script reacts to ([below](#what-rotted-offering-does)) |
| **Sinister Bell** | 40 | sub-feature group 4 |
| **Curse** | 70, 73 | gem generation **+1,000,000**, and an enemy-wide special effect (7150, 7160) |
| (unnamed) | 71, 72 | gem generation +2,000,000, effects 7151, 7152 |

Checked against the data:

* **Fetid.** Of the 86 enemy types placed in a Pthumeru Root layout, 28 have a +2,000,000
  counterpart row, and the pair differ in their resident effects (row 20110080 carries effect
  5000; 22110080 doesn't). The other 58 have no Fetid row, so those enemies stay as they are.
* **Curse.** `GemGenParam` has 331 rows at +1,000,000 and 331 at +2,000,000, mirroring the base
  gem tables, and the menu's gem-name table (`魔石名`) names gems such as "Cursed Tempering Blood
  Gemstone (1)" from 1,100,000 up. Effect 7150 sets enemies' attack-power rates to ×0.9 and their
  echoes to ×1.2 (`haveSoulRate`); 7151 gives echoes ×1.4.

### Rite menus and costs

The rites offered for a chalice are rows of `DungeonSubFeatLotParam`, numbered depth × 10000 +
base × 1000 + rite (010 Fetid, 020 Rotted, 070 Curse, 240 Sinister). Each has its own row in
`RitualRequiredMatParam`, its cost, and picks a feature by weight:

| Row | Rite | Cost | Picks |
|---:|---|---|---|
| 30010 | Fetid Offering (depth 3, Pthumeru) | Sage's Wrist ×4 | feature 11 |
| 30020 | Rotted Offering (depth 3, Pthumeru) | Inflicted Organ ×3 | 20, 21, 22, 24, 25, 27 (16–17% each) |
| 30240 | Sinister Bell (depth 3, Pthumeru) | Arcane Haze ×19 | feature 40 |
| 40170 | Curse (depth 4, Pthumeru) | Bastard of Loran ×3 | feature 73 |

(All rows are in `work/chalice/chalice-1.09.md`.) The costs are the ones players know: Sage's
Wrist (Sage's Hair from depth 4) for Fetid, Inflicted Organ for Rotted, Bastard of Loran for
Curse, Arcane Haze for Sinister.

### What Rotted Offering does

Features 20–29 change no offsets. Instead each has `eventflagId`, displayed as
`イベントフラグID下２桁` ("event flag ID, lower two digits"): feature *N* sets flag
**929053*N***, and the dungeon event script (`m29.emevd`) has sub-feature events that test those
flags. Their names say what each variant does:

| Feature | Placement 1 | Placement 2 |
|---:|---|---|
| 20 | `特殊生成敵` "specially spawned enemy" | |
| 21 | `NPC敵` "NPC enemy" (a hostile hunter) | |
| 22 | `周囲強化敵` "enemy that strengthens those around it" | |
| 23 | `不死敵` "undying enemy" | |
| 24 | specially spawned | NPC enemy |
| 25 | specially spawned | strengthening |
| 26 | specially spawned | undying |
| 27 | NPC enemy | strengthening |
| 28 | NPC enemy | undying |
| 29 | strengthening | undying |

Four kinds of special enemy, singly or in every possible pair: 4 + 6 = 10 variants. So a Rotted
Offering adds one or two special enemies to the dungeon (event names
`サブ特徴_特殊敵_1配置_…` / `_2配置_…`, "sub-feature: special enemy, placement 1 / 2").

**The undying enemy can't be reached.** Across all 150 rows, the Rotted Offerings at the depths
the game has (1–5) only ever pick 20, 21, 22, 24, 25 or 27. Every variant involving the undying
enemy (23, 26, 28, 29) appears only in the rows for depths 6 and 7, the unused rituals below.
The event script implements placing an undying enemy (events 12905013 and 12905119), but no
ritual in the shipped game can select it.

### Other feature rows

Other feature rows carry only a gem-drop correction (`gemDropCorrectParamId` 1011–1045, by depth)
or a sub-feature group, and several are named `消すべからず` ("do not delete"), placeholder rows.

## Defined but not in the game

`HolygrailExParam` has **17 rituals the game doesn't contain**: rows 6601–6809, at depths 6, 7 and
8, for Pthumeru, Loran and Isz. None has a chalice item, a dungeon name or layout files, and most
have no material row (some carry only a placeholder 300 echoes). The table describes deeper
chalices than the game ever offers.

Two more rows, 6900 and 6901, have no chalice item and point straight at a map
(`directMapUid` 29019998 and 29019999, the layouts `m29_01_99_98` and `m29_01_99_99`), with the
Pthumeru Root Chalice's costs. Their purpose isn't evident from the data.

## Treasure

Each layout places its own treasure. The item index counts, per chalice, how many of its layouts
hold each item ([where the items are](the-items.md#the-chalice-dungeons)). For example, the
Uncanny Beast Claw is in 107 of the Ailing Loran Root Chalice's 200 layouts, and the plain Beast
Claw only in the Ailing Loran story chalice.

## Glyphs

A **Chalice Glyph** is the short code players share to visit each other's dungeons. The menu
text has "Search by Chalice Glyph" and "Enter Chalice Glyph", and two telling messages: *"Chalice
Glyph cannot be used in offline mode"* and *"No dungeons found using the given Chalice Glyph."*
The executable shows why.

**What a glyph is.** The glyph alphabet is a UTF-16 string at `0x4b349a0`:
`23456789abcdefghijkmnpqrstuvwxyz`, 32 characters with `0`, `1`, `l` and `o` left out so that no
two can be mistaken for each other. It was found by searching the executable for alphabet-like
strings. Ghidra hadn't typed it as a string, so finding its user took a raw-address reference
search (`RefsTo.java`).

**The entry screen.** Exactly one function uses that alphabet, `FUN_01ebd5b0`, now named
`ChaliceGlyphEntry_OnInput`:

```c
    uVar9 = param_3 - 100;                    // menu items 100-107: the 8 glyph slots
    if (uVar9 < 8) {
      ... find the slot's current character in L"23456789abcdefghijkmnpqrstuvwxyz" ...
      cVar6 = FUN_02dbe460(param_2, &local_fc, 0, 0x1f, 1, 0, 1);   // pick 0..31
      if (cVar6 != ' ')
        *(short *)(param_1 + 0x668 + uVar9 * 2) = alphabet[local_fc];
  ...
  if (param_3 == 2000 && confirmed) {
      if (g_FrpgNetMan online, not busy, ...) {
        FUN_01e9f050(param_1 + 0x528, param_1 + 0x668);            // send the 8 characters
```

So a glyph is **8 characters from a 32-letter alphabet** (40 bits' worth), and entering one sends
the text to the network.

**Where a dungeon keeps its state.** The ritual helper's set-up step
(`CSDungeonRitualHelper::STEP_Exec_forSetupHolygrail`) saves each dungeon slot (0–6) into the
save's event flags, using flag-manager helpers that store *data* rather than single flags: a
32-bit value at flag 94000128 + slot × 100000, 16 bytes at 94000736 + …, and 48 bytes of a wide
string at 94000992 + …. Sixteen bytes is exactly eight UTF-16 characters, the size of a glyph.
The helper's other steps (`ExecGet` / `WaitUpload_forPresentOffering`,
`…_forChangeShareLevel`, `Exec_forJoin`, `Exec_forQuickJoin`, `Exec_forRandomJoin`) show that
creating, sharing and joining a dungeon are server operations.

**What follows.** Nothing in the executable builds a glyph from dungeon data: the entry screen is
the only code that touches the alphabet. Together with "cannot be used in offline mode", that
points to glyphs being **assigned and resolved by the server**, not computed from the layout
number on the console. If so, how a glyph encodes a dungeon can't be read from the game files
at all.

## Not yet established

* How a Root ritual picks its variation number when a dungeon is created (glyphs, it turns out,
  are resolved by the server, [above](#glyphs)), and what the 32-bit value saved per dungeon slot
  holds.
* What the Sinister Bell's feature (40) does in the event script, and what each kind of special
  enemy is.
* How the `m29_0x` tile library relates to the pre-built layouts.
* The m29 event script (428 events), which runs the dungeons' doors, levers and bosses.

## Files

| Path | |
|---|---|
| `work/chalice/chalice-1.09.md` | every ritual with its depth, base, name, variations, layout folders and costs; the unused rituals; the rites; every rite menu with its cost and weights |
