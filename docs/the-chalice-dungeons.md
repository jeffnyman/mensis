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
| **Rotted Offering** | 20–29 | no offsets; works through sub-feature group 2 (`DungeonSubFeatLotParam`) |
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

## Not yet established

* How a Root ritual picks its variation number, and how glyphs encode it.
* What the sub-features (`DungeonSubFeatLotParam`, 150 rows with rates) add, Rotted and
  Sinister in particular.
* How the `m29_0x` tile library relates to the pre-built layouts.
* The Chalice Dungeon treasure: the 100/200 layouts each place their own treasure, which the item
  index ([where the items are](the-items.md)) doesn't cover yet.
* The m29 event script (428 events), which runs the dungeons' doors, levers and bosses.

## Files

| Path | |
|---|---|
| `work/chalice/chalice-1.09.md` | every ritual with its depth, base, name, variations, layout folders and costs; the unused rituals; the rites |
