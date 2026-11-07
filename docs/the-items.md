# Where the items are

Where every item in Bloodborne comes from (treasure, enemy drops, event scripts and shops),
worked out entirely from the game's own files. Nothing here uses a wiki or a guide. Every link in
the chain below is read from the data, and the result can be checked against what players know.
It is also the first use of the data readers together: map layouts, gameplay tables, event
scripts and text, joined into one answer.

## The chain

```
map layout (MSB)   treasure event ──► item lot(s) ──► item id + category ──► English name
                   placed enemy ──► NpcParam ──► blood echoes + item lots ──► …
event script       [2003:04] アイテム取得 ("obtain item")(lot) ──► …
shop table         ShopLineupParam row ──► item + price + unlock flag ──► "owns <badge>" …
```

Each step uses a tool from earlier work:

| Link | Read by | Source |
|---|---|---|
| treasure events, placed enemies | `tools/msb.py` | `map/mapstudio/<map>.msb.dcx` |
| shops and their unlock flags | `tools/params.py`, `tools/events.py` | `ShopLineupParam`, the event scripts |
| item lots, NPC drops, level-dependent items | `tools/params.py` | `ItemLotParam`, `NpcParam`, `ItemLotLvdepParam` |
| items given by scripts | `tools/events.py` | `event/<map>.emevd.dcx` |
| names | `tools/souls.py` (FMG) | `msg/engus/item.msgbnd.dcx` |

`tools/items.py` ties them together. It reads the game as patched (1.09's files laid over
1.00's):

```
python -I tools/items.py world work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/items/items-1.09.md
python -I tools/items.py find  work/base/dvdroot_ps4 work/update/dvdroot_ps4 "Saw Spear"
```

`world` writes the whole index (every map's treasure with position and pickup flag, every
enemy's echoes and drops, every event-given item, every shop row, and the Chalice Dungeons'
treasure). `find` searches it.

## Item lots

A treasure event in a map layout doesn't name an item. It names up to three **item lots**, rows of
`ItemLotParam`. A lot has eight slots, each `{item id, category, count, weight}`, plus the event
flag that records the pickup (`getItemFlagId`, whose display name is ザクザクフラグ, roughly "loot
flag"). A slot's chance is its weight over the total of all eight, and a weighted slot with no
item means "nothing". Enemy drops work the same way: `NpcParam` has six lot fields
(`itemLotId_1`–`_6`) and an amount of blood echoes (`getSoul`, the Dark Souls "soul" name, kept).

The category numbers aren't documented, so they were worked out by matching each category's IDs
against the item tables:

| Category | Is | Evidence |
|---:|---|---|
| 0 | weapon | IDs found in `EquipParamWeapon` with names |
| 1 | armour | all 93 distinct IDs in `EquipParamProtector` |
| 4 | goods | consumables and key items, `EquipParamGoods` |
| 8 | **blood gem** | 463 of 485 IDs are `GemGenParam` rows. Gems are *generated* from effect tables, so they have no fixed name |
| 15 | **level-dependent** | all 16 IDs are rows of `ItemLotLvdepParam`, each holding the real `{category, item id}` |

## What it finds

Spot checks against well-known locations:

| Item | Found |
|---|---|
| Saw Spear | Central Yharnam, on corpse object `o240611_5400` at (−126.4, −68.0, −103.7) |
| Blood Rock | Nightmare of Mensis and Fishing Hamlet, one each |
| Rune Workshop Tool | Hemwick Charnel Lane |
| Bold Hunter's Mark | several, e.g. ×2 in Old Yharnam, ×3 in Cathedral Ward |
| Simon's Bowblade | given by event `秘密を探るもの_死亡時アイテム入手` ("the seeker of secrets: obtain item on death") in the Hunter's Nightmare or the Research Hall, depending on where Simon dies |
| Rakuyo | given by event `漁村_魚人大兄弟_撃破` ("Fishing Hamlet: big fishman brother defeated"), and listed as that enemy's own drop |

Across the 16 maps, counting each map's first layout: **659 treasure spots** and **1,774 placed
enemies** (all 24 layout files, with the later-night variants below: 1,058 and 2,890). The event
scripts give items in **181** places.

### Layouts that change over the night

Several areas have more than one layout: Old Yharnam (`m23_00_00_00`, `_01`), Cathedral Ward,
Central Yharnam (three: `_00`, `_01`, `_11`), Upper Cathedral Ward, the Forbidden Woods,
Yahar'gul and Byrgenwerth. The variants have the same treasure but slightly different enemies:
Cathedral Ward's two have 139 and 140, Byrgenwerth's 80 and 83. Bloodborne changes its world
as the night progresses, and these are that, as separate layout files.

## Shops, and a wrong guess corrected

The first version of this page said the Chikage must be "handed over by an NPC in a talk
script", because it wasn't in any item lot. That was tested rather than assumed: ranking every
talk command by how often its arguments are real item-lot IDs found no item-giving command (the
best scored about 30%, and those were flags that share lot numbering). Searching the other way,
from the Chikage's weapon ID, found it in no lot at all, but in five rows of **`ShopLineupParam`**.
It is *sold*.

### The shop table

| Series (row id ÷ 10000) | `shopType` | What the rows contain | Is |
|---|---:|---|---|
| 0 | 0 | Saw Cleaver, Hunter Axe, Threaded Cane at price 0 | the starting-weapon choice |
| 10–14 | 0 | the same 144 items in each, prices rising tier by tier (Blood Vial 180 → 360 → 540 → 720 → 900) | the Bath Messengers' blood-echo shop, five price tiers |
| 20–24 | 5 | 51 items at prices 1–60 (Small Resonant Bell 1, Blood Rock 60) | the Bath Messengers' insight shop |
| 51–54, 61–64 | 0 | 38 items each, priced well above the Bath Messengers | two more echo shops, four tiers each (which in-game shops isn't established) |

Insight is "SAN値" (SAN value) internally. The shop table has a `value_SAN` price field, and the
Hunter's Dream event `売買メッセンジャー（SAN値）アニメ` ("trading messenger (SAN value)
animation") tests `SAN値で判定` ("judge by SAN value") ≥ 1 before that messenger appears, which
is the insight shop appearing once you have insight. What selects the price tier isn't
established yet.

### Unlock flags

A row is for sale only once the event flag in its `qwcId` field is set. In Dark Souls that field
held a world-tendency parameter ID; Bloodborne reuses it for the unlock flag. The Chikage's flag
is 12101007. No event script, talk script, table or event argument writes that flag, and the
reason is an engine convention:

* Event 12101000, `売買メッセンジャー_ラインナップ拡充_XX` ("trading messenger: line-up
  expansion"), is a template that waits until the player owns a goods item (`[3:04]
  アイテムの所有状態で判定`, "judge by item ownership") passed in as an argument.
* It is started ten times, in slots 0–9, once per **hunter badge**.
* When an event finishes, the engine sets the event's own flag: its ID plus its slot.

So flag 12101000 + *slot* means "owns that badge", and 12101007 is **"owns the Cainhurst Badge"**.
The Chikage is bought from the Bath Messengers for 50,000 echoes once you have it. The same
template explains the rest of the badge groups: the Saw Hunter Badge (slot 0, flag 12101000)
unlocks the Saw Cleaver, Hunter Axe, Threaded Cane and Saw Spear rows.

`items.py` finds these flags generically, in two patterns:

* **An event that waits on item ownership with the item from an argument**, started with that
  argument in slot *s*, defines "flag = event + *s* → owns that item". That gives 47 flags: the
  ten badges, the messengers' decorations ("owns Messenger Top Hat", the items that change the
  Bath Messengers' look), and the Uncanny/Lost versions of the DLC weapons (5500–5521, so the
  shop re-sells them once you have them).
* **An event that tests fixed item IDs and then sets a flag itself.** That gives 20 more,
  chiefly a family of 18 in `common` (6680–6697), one per weapon, each set once you own that
  weapon **at any upgrade level, storage included** (`[3:16]`, `倉庫含むアイテムの所有状態で判定`,
  "judge item ownership, storage included"). Flag 6680 tests the eleven IDs of Beast Claw +0 to
  +10.

That is **67** "owns" flags in all. Shop rows with one of them show their condition in words:

```
shop  Bath Messengers, echoes, tier 1   Chikage: price 50000, unlocked by flag 12101007 (owns Cainhurst Badge)
```

The **Beast Claw** is also a shop row (34,000, flag 6680). It isn't placed in any main map
layout; it is found first in the Chalice Dungeons (below). Flag 6680 is "owns a Beast Claw, at
any upgrade level", so the Bath Messengers sell it once you've found one, and keep selling it
if you put yours in storage. *An earlier version of this page couldn't say what set 6680. The
detector only knew the first pattern above.*

### The insight shop's line-up flags

The insight shop's later rows (Blood Stone Chunk, Blood Rock, …) are unlocked by flags
5900–5904. The Hunter's Dream event `売買メッセンジャー＿ラインナップ拡充_時間帯フラグ` ("trading
messenger: line-up expansion, time-of-day flags") sets each from one other flag:

| Line-up flag | Set when | Source flag |
|---:|---|---|
| 5900 | it is Evening | 9800 |
| 5901 | it is Night | 9801 |
| 5902 | the Eccentric Moon has risen | 9802 |
| 5903 | The One Reborn is defeated (Yahar'gul) | 12801800, `ボス撃破_なりそこないの邪神` |
| 5904 | a Nightmare of Mensis boss is defeated | 12601800, `ボス撃破_レッサーデーモン（死と闇）` |
| all five | any of the three endings is reached | 6603 |

**The time of day is three event flags.** 9800–9802 aren't set by any event or talk script.
The executable reads and writes them together as one 3-bit value (`FUN_017cfd80` /
`FUN_017d0060` with flag 9800 and width 3). The values 0, 4, 6 and 7 are its four stages, which
a debug display names "15o'clock", "Evening", "Night" and "EccentricMoon". Each later stage sets
one more flag. The event scripts and talk scripts only ever test them.

The two boss events are completion flags, the same convention as the badges: an event's own
flag is set when it finishes. The Nightmare of Mensis boss of 12601800
(`レッサーデーモン（死と闇）`, "lesser demon (death and darkness)") has model c5510 and no name in
the text. That is the model of the Chalice Dungeons' `トラップロード移植` boss
([the event scripts](the-chalice-events.md#bosses)). `find` spells the line-up flags out too:

```
shop  Bath Messengers, insight, tier 1   Blood Rock: price 60, unlocked by flag 5904 (after the Nightmare of Mensis boss of event 12601800 is defeated, or after an ending)
```

## The Chalice Dungeons

Each Root chalice is 100 or 200 pre-built layouts, each placing its own treasure
([the Chalice Dungeons](the-chalice-dungeons.md)). The index reads all 2,685 Chalice layouts,
attributes each layout folder to its chalice through the ritual table, and counts how many of a
chalice's layouts place each item. That is the share of layouts, not the chance of meeting the
item: a ritual is steered towards unique items not yet picked up
([how a ritual picks its layout](the-chalice-dungeons.md#how-a-ritual-picks-its-layout)).
The 152,959 treasure lot references give 166 distinct items:

| Item | Chalice | Layouts with it |
|---|---|---:|
| Beast Claw | Ailing Loran Chalice (story) | 2 of 2 |
| Uncanny Beast Claw | Ailing Loran Root Chalice | 107 of 200 |
| Lost Beast Claw | Lower Ailing Loran Root Chalice | 49 of 200 |
| Lost Beast Claw | Sinister Lower Loran Root Chalice | 42 of 200 |
| Uncanny Chikage | Cursed and Defiled Root Chalice | 96 of 200 |
| Lost Chikage | Pthumeru Ihyll Root Chalice | 50 of 200 |

The pattern for the weapon variants is visible here: the plain weapon in the story chalice,
*Uncanny* in a depth-4 Root chalice, *Lost* in a depth-5 one. With the Chalice layouts included,
`world` takes about 25 seconds.

## What it doesn't cover yet

* **Three event awards** (event 9440, `誓約ボディジェム入手`, "obtain covenant body gem", lots
  10500/11500/12500) point at lots that don't exist in `ItemLotParam`, so they must resolve some
  other way.
* **Blood gems have no fixed name, by design.** A treasure's gem is a `GemGenParam` row, a recipe
  for a *random* gem: up to six effect slots, each drawn from an effect table with a chance to
  appear and a chance to turn negative, plus slot-shape rates. None of the 1,337 generators is
  marked unique. The menu's gem-name table (`魔石名`, 3,148 names such as "Hard Adept Blood Gemstone
  (2)") names the possible *outcomes*, which the game composes when the gem is rolled (through
  `GemeffectParam`'s `gemNameIdOffset`). So the index lists a gem as its generator, which is the
  most a treasure spot can say.

## Files

| Path | |
|---|---|
| `work/items/items-1.09.md` | the full generated index (not in the repository: it is derived game data) |
