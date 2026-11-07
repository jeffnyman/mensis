# Where the items are

The first topic of the data track: where every item in Bloodborne comes from, worked out
entirely from the game's own files. Nothing here uses a wiki or a guide. Every link in the
chain below is read from the data, and the result can be checked against what players know.

## The chain

```
map layout (MSB)   treasure event ──► item lot(s) ──► item id + category ──► English name
                   placed enemy ──► NpcParam ──► blood echoes + item lots ──► …
event script       [2003:04] アイテム取得 ("obtain item")(lot) ──► …
```

Each step uses a tool from earlier work:

| Link | Read by | Source |
|---|---|---|
| treasure events, placed enemies | `tools/msb.py` | `map/mapstudio/<map>.msb.dcx` |
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
enemy's echoes and drops, every event-given item) in under a second. `find` searches it.

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

## What it doesn't cover yet

* **Items given in conversation.** Some items (the Chikage, for one) are handed over by an NPC
  in a talk script. Bloodborne's talk commands are unnamed, so the item-giving command hasn't been
  identified. Finding it, by looking for a talk command whose argument is always a valid item
  lot, would close the gap.
* **Chalice Dungeons.** Their treasure is generated from the dungeon tables (`DungeonFeatureParam`,
  `DungeonSubFeatLotParam`, …), not placed in layouts. The Beast Claw, for example, comes from
  there.
* **Shops.** `ShopLineupParam` lists what each shop sells (it's where 1.09's Blood Rock rows are),
  but isn't joined into the index yet.
* **Three event awards** (event 9440, `誓約ボディジェム入手`, "obtain covenant body gem", lots
  10500/11500/12500) point at lots that don't exist in `ItemLotParam`, so they must resolve some
  other way.
* **Blood gems** are listed by generator ID only. Naming them would mean following
  `GemGenParam` → `GemeffectParam` the way the game builds a gem's name.

## Files

| Path | |
|---|---|
| `work/items/items-1.09.md` | the full generated index (not in the repository: it is derived game data) |
