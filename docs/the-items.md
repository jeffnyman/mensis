# Where the items are

The first topic of the data track: where every item in Bloodborne comes from, worked out
entirely from the game's own files. Nothing here uses a wiki or a guide. Every link in the
chain below is read from the data, and the result can be checked against what players know.

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
enemy's echoes and drops, every event-given item, every shop row) in under a second. `find`
searches it.

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

Across the 16 main layouts: **659 treasure spots** and **1,774 placed enemies**. The event scripts
give items in **181** places.

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

`items.py` finds these flags generically: any event that waits on item ownership with the item
from an argument, started with that argument in slot *s*, defines "flag = event + *s* → owns
that item". That finds **47** such flags: the ten badges, the messengers' decorations ("owns
Messenger Top Hat", the items that change the Bath Messengers' look), and the Uncanny/Lost
versions of the DLC weapons (5500–5521, so the shop re-sells them once you have them). Shop rows
with one of these flags show their condition in words:

```
shop  Bath Messengers, echoes, tier 1   Chikage: price 50000, unlocked by flag 12101007 (owns Cainhurst Badge)
```

The **Beast Claw** is also a shop row (34,000, flag 6680). It isn't placed in any main map
layout, which fits it being found first in the Chalice Dungeons; flag 6680 isn't one of the
derived "owns" flags, so what sets it isn't established.

## What it doesn't cover yet

* **Chalice Dungeons.** Their treasure is generated from the dungeon tables
  (`DungeonFeatureParam`, `DungeonSubFeatLotParam`, …), not placed in layouts.
* **Unlock flags that aren't "owns X".** Flag 6680 (Beast Claw), and the line-up flags 5900–5904,
  which the messengers' event sets from progress flags (see [the patch](the-patch.md)).
* **Three event awards** (event 9440, `誓約ボディジェム入手`, "obtain covenant body gem", lots
  10500/11500/12500) point at lots that don't exist in `ItemLotParam`, so they must resolve some
  other way.
* **Blood gems** are listed by generator ID only. Naming them would mean following
  `GemGenParam` → `GemeffectParam` the way the game builds a gem's name.

## Files

| Path | |
|---|---|
| `work/items/items-1.09.md` | the full generated index (not in the repository: it is derived game data) |
