# The Chalice Dungeons

How Bloodborne's Chalice Dungeons are put together, read from the game's own tables and layouts.
The short version: **a "random" Root Chalice dungeon is one of 100 or 200 pre-built layouts**,
chosen by number (steered toward unique items you haven't picked up yet), and the rites
(Fetid, Rotted, Cursed, Sinister) change which enemy and gem tables those layouts draw on, or set
event flags that decide which of the variants already placed in the layout survive.

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
rites: DungeonFeatureParam ──► NPC row offset, gem-generation offset, enemy-wide special effect,
       and an event flag (92905300 + N) the event scripts use to keep or remove placed variants
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
| … | | | (all 15 Root chalices in `work/chalice/chalice-1.09.md`) |

`mapBaseId` *ABC* names folder `m29_AB_C…`: 100 → `m29_10_00_00`, 302 → `m29_30_20_00`. Depths 1–3
have 100 variations in one folder, depths 4–5 have 200 split across two. The variations are
genuinely different dungeons. Five of the Pthumeru Root's 100 have 760–969 map pieces, 152–208
enemies and 38–57 treasures.

So a Root Chalice ritual picks one layout out of a fixed, finite set that ships with the game.
How it picks the number is [below](#how-a-ritual-picks-its-layout). The dungeon "glyph" players
share isn't derived from that number: glyphs are assigned and resolved by the server
([below](#glyphs)).

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
| **Sinister Bell** | 40 | sets flag 92905340, which wakes the bell-ringing women placed in the Sinister chalices' layouts ([the event scripts](the-chalice-events.md#the-bell-ringing-women)) |
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
base × 1000 + variant × 100 + rite (10 Fetid, 20 Rotted, 40 Sinister Bell, 70 Curse), where the
variant is the chalice's own (2 for the Sinister chalices, 1 for Cursed and Defiled, matching
their `mapBaseId`s). Each has its own row in
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
| 20 | `特殊生成敵` "specially spawned enemy" | specially spawned |
| 21 | `NPC敵` "NPC enemy" (a hostile hunter) | NPC enemy |
| 22 | `周囲強化敵` "enemy that strengthens those around it" | strengthening |
| 23 | `不死敵` "undying enemy" | undying |
| 24 | specially spawned | NPC enemy |
| 25 | specially spawned | strengthening |
| 26 | specially spawned | undying |
| 27 | NPC enemy | strengthening |
| 28 | NPC enemy | undying |
| 29 | strengthening | undying |

Four kinds of special enemy, singly or in every possible pair: 4 + 6 = 10 variants, across a
layout's two special-enemy placements (event names `サブ特徴_特殊敵_1配置_…` / `_2配置_…`,
"sub-feature: special enemy, placement 1 / 2"). *An earlier version of this table left placement
2 empty for 20–23; the event templates show a single variant fills both placements.* What each
kind is and does is in [the Chalice Dungeons' event scripts](the-chalice-events.md#special-enemies-what-rotted-offering-adds),
and how the layouts carry every variant at once in
[rites are templates that remove things](the-chalice-events.md#rites-are-templates-that-remove-things).

**The undying enemy can't be reached.** Across all 150 rows, the Rotted Offerings at the depths
the game has (1–5) only ever pick 20, 21, 22, 24, 25 or 27. Every variant involving the undying
enemy (23, 26, 28, 29) appears only in the rows for depths 6 and 7, the unused rituals below.
The undying groups are even placed in the layouts (the placement-1 template is started in
1,069 of them), but no ritual in the shipped game can select them, so the event script removes
every one when the dungeon loads.

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
Claw only in the Ailing Loran story chalice. That share of layouts isn't the chance of meeting an
item, because a ritual is steered toward unique items not yet picked up
([how a ritual picks its layout](#how-a-ritual-picks-its-layout)).

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
      if (cVar6 != '\0')
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
string at 94000992 + …. Sixteen bytes is exactly eight UTF-16 characters, the size of a glyph. (What
the 32-bit value is, and the rest of the record, is in [how a ritual picks its layout](#how-a-ritual-picks-its-layout).)
The helper's other steps (`ExecGet` / `WaitUpload_forPresentOffering`,
`…_forChangeShareLevel`, `Exec_forJoin`, `Exec_forQuickJoin`, `Exec_forRandomJoin`) show that
creating, sharing and joining a dungeon are server operations.

**What follows.** Nothing in the executable builds a glyph from dungeon data: the entry screen is
the only code that touches the alphabet. Glyphs are **assigned and resolved by the server**, not
computed from the layout number on the console. The glyph search request, `ChannelWordSearch`,
sends nothing but the eight characters (`SearchWord`) and a data version
([the online boundary](the-online-boundary.md#each-requests-fields)), and the server answers with
a dungeon. So how a glyph maps to a dungeon can't be read from the game files at all.

## How a ritual picks its layout

**When.** The ritual screen (the menu that shows a chalice's `SubCost`, `TotalCost` and
`ChannelInfo`) rolls the dungeon as soon as it is given a chalice. One function does the roll
(`ChaliceRitual_PickDungeon` in the hand names, `01eaeec0`). Every caller passes it the same
random-number generator.

**The roll**, in the order the code does it:

1. **A plain roll.** `variation = rng() % variationNum`.
2. **Unclaimed unique items.** For each of the chalice's up to 15 unique items, if its range is
   defined (`uniqueItemNN_head` ≠ 255) and its flag (`uniqueBaseFlagId` + *NN*) is **not yet
   set**, the item's range `[head, tail]` joins a list, weighted by the range's size.
3. **If the list isn't empty, the plain roll is replaced.** A uniform float in [0, 1)
   (`(rng() >> 9 | 0x3f800000)` read as a float, minus 1.0) picks an item from the list by
   weight, and a second roll picks a variation inside that item's range:
   `variation = head + rng() % (tail − head + 1)`.
4. **Sub-features.** The same function rolls the chalice's sub-feature lots: five "fixed"
   (`fixedSf_1..5_subFeatureLotId`), three "free" (`freeSf_1..3`) and five "group"
   (`groupSf_1..5`). Each picks one feature by weight (`DungeonSubFeatLotParam` `rate_1..10`).
   These are the hidden rows that pick the special character and the gem-drop correction
   ([the event scripts](the-chalice-events.md#special-characters)). Lot 994500000 (feature 45),
   if present, gets special handling.

So a ritual only rolls a fully random layout once every unique item of that chalice has been
picked up. Until then it **lands in the range of a unique item you don't have yet**.

**The ranges guarantee the item.** A unique item is a treasure whose pickup flag
(`getItemFlagId`) is `uniqueBaseFlagId` + *NN*. Checking every layout (`tools/chalice.py`, the
"Unique items" table in its output): for every Root chalice except Isz, **every layout inside
item *NN*'s range places item *NN***. The one exception is Pthumeru Root's Bone Marrow Ash, in
19 of its 20 (`m29_10_00_48` lacks it). The same items also turn up in many layouts outside
their range, so the range is a guarantee, not the only place to find them:

| Chalice | Unique items (range of variations) |
|---|---|
| Pthumeru Root (6100) | Uncanny Saw Cleaver (0–19), Frenzied Coldblood (7) (20–39), Bone Marrow Ash (40–59), Fire Paper (60–79), a blood gem (80–99) |
| Ailing Loran Root (6420) | Uncanny Beast Claw (0–29), Lost Hunter Axe (30–59), Uncanny Tonitrus (60–87), Lost Rifle Spear (88–115), three blood gems (116–199) |

(all of them in `work/chalice/chalice-1.09.md`). A Sinister chalice shares its plain twin's
`uniqueBaseFlagId` (6300 and 6302 both use 5160), so picking up an item in one counts for both.

**Isz is the exception.** For the Isz Root chalices (6530, 6532), only the first ranges
guarantee their item. Lost Kirkhammer is in 16 of 18 layouts, and the blood gems and **Blood
Rock** (range 136–151) in as few as 5 of 16. The last two "unique items", 10 and 11, are both
**Great One's Wisdom** on flags 5290 and 5291 (lots 200530100 and 200530110). **No layout
places those lots, and nothing else sets those flags.** By the code above, they can never be
claimed:

* their ranges (168–183 and 184–199, 32 of the 200 variations) are always on the list;
* once every other Isz unique item is picked up, **every new Isz Root ritual lands in variations
  168–199**, and never on a fully random layout.

**The generator.** The random-number generator belongs to a large global game object, created
at start-up. Its constructor seeds it from the clock (`gettimeofday`: seconds × 1,000,000 +
microseconds, cut to 32 bits) and fills a 624-word state with the Mersenne Twister's
initialization (`x' = (x ^ x >> 30) × 0x6c078965 + i`). It then applies a period check whose
constant, `0x13c9e684`, is the SFMT-19937 parity word: **SIMD-oriented Fast Mersenne Twister**.
The same generator serves other game systems too, so which layout a ritual gives depends on when
the game started and on everything drawn from it since. It can't be predicted from the save.

**What's saved.** The dungeon is stored as a record that is the same whether it was made locally
or fetched from the server. The server's version is the `FormData` blob from
[the online boundary](the-online-boundary.md#each-requests-fields). Its parser accepts two
versions, checked by a size field (4: 0x7c bytes, 5: 0x8c). Both carry the packed map ID, the
chalice, the sub-feature picks and the 16-byte glyph field. Version 5 also carries a name; for
version 4 the parser fills the name in from the local character, so the name is evidently the
dungeon's creator. The set-up step writes the record into the save slot's event flags at
94000000 + slot × 100000:

| Flags (+ slot × 100000) | Holds |
|---|---|
| 94000032, 94000064 | a 64-bit ID, in two halves |
| 94000096 (4 × 8 bits) | a packed map ID: one byte each for area (29), block, sub-block, variant |
| 94000128 (32 bits) | the chalice: its `HolygrailExParam` row (the value the parser looks up there) |
| 94000160 … | nine pairs of 32-bit values (the sub-feature picks) |
| 94000736 (16 bytes) | the glyph |
| 94000992 (48 bytes) | the creator's name, as text |

The engine's packed map IDs are four bytes, and three functions turn an `m29` one back into its
chalice row as `6000 + block × 10 + sub-block / 10`. For example `m29_30_20_xx` gives 6302,
Sinister Lower Pthumeru. Variation *v* of the ritual with `mapBaseId` *ABC* is the layout
`m29_AB_(C×10 + v / 100)_(v mod 100)`. A 200-variation ritual spans two folders:
`m29_40_10_xx` for 0–99, `m29_40_11_xx` for 100–199.

**The name** is composed in code just as [above](#dungeon-names-are-composed) describes: the
chalice's `dungeonNameBaseId`, plus each picked feature's `dungeonNameModifierId`, taken in
`dungeonNamePriority` order.

**A developer menu.** One more caller (`01eb9270`) is a menu handler. Its item 0 picks *any*
`HolygrailExParam` row, item 400 sets the variation directly (0 to `variationNum` − 1), and items
300–312 toggle and choose each sub-feature pick. No ritual a player can perform offers that, so
it is a tool for building a specific dungeon on demand. Whether it can be reached on a retail
console hasn't been examined.

## Not yet established

* What the nine sub-feature pairs in a saved dungeon hold exactly, and what the 64-bit ID is.
* How the `m29_0x` tile library relates to the pre-built layouts.

(The Sinister Bell's feature, the four kinds of special enemy and the m29 event script, open
questions in earlier versions of this page, are answered in
[the Chalice Dungeons' event scripts](the-chalice-events.md).)

## Files

| Path | |
|---|---|
| `work/chalice/chalice-1.09.md` | every ritual with its depth, base, name, variations, layout folders and costs; the unused rituals; the rites; every rite menu with its cost and weights; every unique item with its range and how many layouts inside and outside the range place it |
