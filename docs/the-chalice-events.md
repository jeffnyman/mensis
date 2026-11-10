# The Chalice Dungeons' event scripts

How 2,641 Chalice Dungeon layouts are run by one script, and what the rites actually change
inside a dungeon: the four kinds of special enemy a Rotted Offering adds, the special characters
every dungeon rolls for, the bell-ringing women, and the bosses. It continues
[the Chalice Dungeons](the-chalice-dungeons.md), which covers the rituals, layouts and rite
tables. Everything here is read from the event scripts and resolved through the map layouts and
gameplay tables with the readers from [the data](the-data.md).

```
python -I tools/chalice_events.py work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/chalice/events-1.09.md
```

## One library, 2,641 callers

The Chalice Dungeons have two kinds of event script:

| File | Count | Holds |
|---|---:|---|
| `event/m29.emevd` | 1 | a library of **428 named template events**, e.g. `宝箱_XX` ("treasure chest"), `レバー扉_XX` ("lever door"), `ボス撃破` ("boss defeated") |
| `event/m29_AB_CD_00/<layout>.emevd` | 2,641 | one per layout. All but 7 contain only a constructor (event 0), which does nothing but **start library templates** with that layout's entity IDs as arguments |

The `_XX` suffix in a template's name marks one that takes arguments. A typical Pthumeru Root
layout's constructor makes about 440 starts: 42 of `寡婦死亡でザコ死亡_XX`, 28 of
`扉アクション削除_XX`, 14 treasure chests, and so on, each pointing the template at particular
doors, levers and enemies in that layout's map. So a layout is its map file plus a list of
template calls, and the behavior itself lives once, in `m29.emevd`. (The 7 exceptions, in the
`m29_00`–`m29_04` folders, define events of their own. The counts differ from the 2,685 layout
files because 48 layouts have no script, and 4 scripts have no layout. The 48 are 40 development
layouts in `m29_0x` and the story chalices' 8 unused `_90_01` twins
([the rest of m29](the-chalice-dungeons.md#the-rest-of-m29-the-tile-kit-and-the-development-layouts)).)

Usage across all layouts shows what every dungeon has and what is rare:

| Template | Layouts | |
|---|---:|---|
| `ボス部屋入場_ゲスト`, `ボスBGM_有効化`, `ボス入口扉開放`, … | 2,411 | boss-room entry, music and doors, in practically every layout |
| `扉アクション削除_XX` | 2,376 | doors (63,827 starts) |
| `宝箱_XX` | 2,361 | treasure chests (43,883 starts) |
| `メインブロック扉_…` | ~2,330 | the main block door and its lever |
| `寡婦_登場_出現寡婦_XX` | 2,302 | the "appearing widow" ([below](#the-bell-ringing-women)) |
| `サブ特徴_特殊キャラ_…` | 1,884–2,301 | the special character ([below](#special-characters)) |
| `サブ特徴_特殊敵_1配置_…` | 1,218 | special-enemy placement 1 ([below](#special-enemies-what-rotted-offering-adds)) |
| `サブ特徴_特殊敵_2配置_…` | 995 | special-enemy placement 2 |
| `ボスラッシュ_…` | 1–8 | the "boss rush" set, in a handful of layouts |

32 templates are never started by a layout. Six of those are started by the library's own
constructor (the four warp-object templates, `PCHP減少特殊効果付与` and the play-log clear
timer). The other 26 are started by nothing in the Chalice scripts, among them a second copy of
Rom's events without the `_XX`, and the `ボスラッシュ` ("boss rush") entry set.

## Rites are templates that remove things

Each rite sets an event flag: feature row *N* in `DungeonFeatureParam` sets **92905300 + *N***
([the Chalice Dungeons](the-chalice-dungeons.md#what-rotted-offering-does)). The layouts don't
add anything when a flag is set. **Every variant is already placed in the map**, and each
sub-feature template removes its own set unless its flag says to keep it:

```
Event 12904919  サブ特徴_特殊敵_1配置_特殊生成敵_XX   (special enemy, placement 1: specially spawned)
    [3:00]    judge flag 92905320 on   ┐
    [3:00]    judge flag 92905324 on   │ any of these
    [3:00]    judge flag 92905325 on   │
    [3:00]    judge flag 92905326 on   ┘
    [1000:02] end the event if so                 ← this variant was chosen: leave it
    [2004:05] disable character X0 … X24          ← otherwise: up to seven characters
    [2004:29] remove them from back-reading
    [2005:03] disable object X28, its action and its treasure
```

The "no feature" templates are the reverse: they keep the dungeon's ordinary enemies only while
**all** of flags 20–29 are off. (The instruction's argument is a "logical operation type", which
the instruction set names `全てON`, `全てOFF`, `どれかがON`, `どれかがOFF`: all on, all off, any
on, any off.)

So a spot in a layout that can hold a special enemy carries five overlapping groups: the
ordinary enemies and four special variants, of which the event script leaves exactly one.

## Special enemies: what Rotted Offering adds

A Rotted Offering picks one of the features 20–29 ([the Chalice Dungeons](the-chalice-dungeons.md#what-rotted-offering-does)).
Each layout has up to two special-enemy *placements*, and the templates read the flags like this:

| Feature | Placement 1 | Placement 2 |
|---:|---|---|
| 20 | specially spawned | specially spawned |
| 21 | NPC enemy | NPC enemy |
| 22 | strengthening | strengthening |
| 23 | undying | undying |
| 24 | specially spawned | NPC enemy |
| 25 | specially spawned | strengthening |
| 26 | specially spawned | undying |
| 27 | NPC enemy | strengthening |
| 28 | NPC enemy | undying |
| 29 | strengthening | undying |

(The first version of [the Chalice Dungeons](the-chalice-dungeons.md) page left placement 2
empty for 20–23. The templates show the single variants fill both placements.)

Resolving each template's arguments through the layout's map and `NpcParam` gives the same
enemy family every time. Ordinary enemies have no names in the game's text (only bosses and
characters do), so they are given here by model and `NpcParam` row (the row's last six digits;
the leading digits vary by depth):

| Kind | Event name | Who, in all layouts | What it does |
|---|---|---|---|
| **specially spawned** | `特殊生成敵` | a leader, model c2100 (row 210080, which the text names **"The Witch of Hemwick"**), with up to six c2050 (205080) as its retinue | the summoner: see below |
| **NPC enemy** | `NPC敵` | a hostile hunter, model c0000, rows 7120 (most), 7150 or 7160 | no template of its own beyond being kept; each drops a blood gem with 20% chance (lots 71000, 75000, 76000) |
| **strengthening** | `周囲強化敵` | a leader, c3000 (300002), with up to six c3010 (301010 and 301040–301060) | makes the enemies around it stronger: see below |
| **undying** | `不死敵` | a group of c2040 (204080) | can't be killed one at a time: see below |
| *no feature* | `特徴なし` | the dungeon's ordinary enemies, c2010 (201020) and c1240 (124090) | |

### The summoner

Four templates run a specially spawned group. A counter (3 bits of event flags, at most 6)
goes up when a retinue member is spawned and down when one dies. While the leader is fighting
(AI state 1 or 3), alive and loaded, and the count is **2 or fewer**, the leader plays animation
3011 and is given AI command 10 (the template is `特殊生成行動リクエスト`, "special-spawn action
request"). When the leader then signals (event message 20), its generators switch on for about
three seconds. So killing the retinue only works for so long: the leader keeps bringing it back
toward six.

### The strengthener

Two mechanisms appear, in different layouts:

* **Possession** (`強化憑依`, 1,019 layouts). When a nearby enemy gains special effect 5622, it
  plays an animation and the strengthener **disappears** (it is disabled). Effect 5622 doubles
  the enemy's maximum HP and physical attack, raises stamina ×1.5 and adds poise. The
  strengthener has gone into the enemy.
* **Infection** (`感染_周囲強化`, 242 layouts). When the strengthener signals, every target
  within 25 m that is alive is ordered (AI command 200) to respond, and then gains effect
  5645 for 20 seconds: HP ×1.5, physical attack ×1.4, every defense ×0.8. A variant
  (`…_強化時毒化`, "poisoned while strengthened") adds a second effect for as long as 5645 lasts.

### The undying

Two templates (`不死敵の初期設定` "initial setup", `不死敵の死亡` "death"):

1. Each member is set **unkillable** (`キャラ不死設定`) with its HP gauge hidden, and drained to
   zero (effect 5626, HP change +100%). The group lies there.
2. When the player comes within **7 m** of any of them, all get a full heal (effect 5625,
   −100%) and their HP gauges appear. They get up.
3. The setup template then waits for every member to have effect 5913 (a 5-second effect) at
   the same moment. 5913 is what they carry while down, so this is "all of them down at once".
   When that happens it sets a flag.
4. On that flag, each member, after a random 0–50 frames, is made killable and forced to die,
   giving its echoes.

So the undying group can be knocked down but not killed one at a time. Each one gets back up
unless the whole group is down together.

**The undying enemies are placed but never used.** As [the Chalice Dungeons](the-chalice-dungeons.md#what-rotted-offering-does)
showed, the rite menus at depths 1–5 only ever pick features 20, 21, 22, 24, 25 and 27. Yet the
undying groups are in the layouts: placement 1's undying template is started in **1,069
layouts** and placement 2's in 844, across the shipped depths. Every one of those groups is
removed when the dungeon loads.

### Treasure

Each variant's template also controls one object (X28). For the non-summoner variants it is a
treasure, so the variant that stays brings its own pickup. The items are ordinary Chalice loot
(Blood Vials, Ritual Blood (5), Sage's Hair, Quicksilver Bullets, …). For the summoner it is an
object without treasure.

## Special characters

Every Root dungeon also rolls a special character, without any rite. The rite-menu table has
hidden rows `99<depth><base>030` with no cost and no name, which pick one of features 30–34. At
normal depths the weights are **7 / 10 / 3 / 14 / 6**. The rows ending `230` (a 2 where the
Sinister chalices' map base IDs have one, e.g. 302) pick only 30 (21) or 32 (9).

Two spots are involved, each with an ordinary-enemy stand-in for when nothing is picked:

* **The shop spot** (`ショップあり_…`, "with shop") holds two alternative sets. Set A is
  **Patches** alone: model c1110 (row 111011, talk script 290329), whose entity also runs the
  `クモ男パッチ` ("Patches the spider-man") events. Set B is **two trading messengers** (c9020,
  rows 902030 and 902031) and their two objects: a shop.
* **The no-shop spot** (`ショップなし_…`) holds a c7500 (750080), kept by `…_同士討ち`
  ("infighting").

The features combine them:

| Feature | Weight | Shop spot | No-shop spot |
|---:|---:|---|---|
| 30 | 7 | the messengers' shop | ordinary enemy |
| 31 | 10 | a c7500 instead | infighting |
| 32 | 3 | **Patches**, if his story state is 1423 or 1432; otherwise the messengers' shop | ordinary enemy |
| 33 | 14 | the messengers' shop | infighting |
| 34 | 6 | **Patches**, as for 32 | infighting |

*An earlier version of this table had Patches present for 30 and 33 too. The template's full
logic keeps set A (Patches) only on the 32/34 branch with his story state, and set B (the
messengers) otherwise.*

**Patches' story state** is one of a group of flags, 1420–1437. Each event that changes it
clears the group and sets one:

| Flag | Set by | Meaning |
|---:|---|---|
| 1420 | `common`'s pre-constructor | the initial state |
| 1421 | `クモ男パッチ_対面` ("face to face"), Byrgenwerth (m32) | met him |
| 1422 | `クモ男パッチ_旅立ち直前` ("just before setting out"), Byrgenwerth | met him, his talk flag 73200324 is set, and both 9461 and 9466 are set: the boss-defeat events `ボス撃破_悪夢の主` ("master of the nightmare", Nightmare of Mensis) and `ボス撃破_邪神` ("evil god", Nightmare Frontier; Amygdala in [Bosses](#bosses)) |
| 1423 | `クモ男パッチ_ダンジョンにいる` ("is in the dungeon"), `common` | 1422, talk flag 73200326, and the player not in Byrgenwerth |
| 1430 | `クモ男パッチ_敵対した` ("turned hostile"), Byrgenwerth | |
| 1431 | `クモ男パッチ_死亡` ("dead"), Byrgenwerth or a Chalice dungeon | |
| 1432 | `クモ男パッチ_敵対_敵対フラグON` ("hostile flag on"), a Chalice dungeon | turned hostile in a dungeon |

The two talk flags come from his talk script in Byrgenwerth. It sets 73200324 after one line
and 73200326 after the line *"Oh, think not of poor Amygdala."* (talk 3901400). So Patches
**moves to the Chalice Dungeons**: once he has said his piece after both bosses are dead, and
you leave Byrgenwerth, his state becomes "in the dungeon". From then on, a dungeon that rolls 32
or 34 has him in the shop spot, even if you've made him hostile (1432). Once he is dead (1431),
it never does. Byrgenwerth's own event `クモ男パッチ_立ち位置切り替え` ("switch his standing
position") tests 1423.

The Patches events' names say what he does in a dungeon: he turns hostile after a count of
attacks (`攻撃回数カウント`), switching team type, and **falls** (`落下する`) both when talked to
(`話しかけたら`) and when damaged (`ダメージを受けたら`).

## The bell-ringing women

The library has two kinds of "widow" (`寡婦`, model c1050), each with a bell-control template
(`寡婦_鐘制御`):

* **Appearing widows** (`出現寡婦`, rows 1059x0) are in **2,302 layouts**, in all 23 Root
  folders and two others (`m29_01_00_00`, `m29_01_99_00`). Each stays hidden until the game is **online** and either the host (level ≥ 30) has
  at least one cooperator, or the player has the effect of the **Sinister Resonant Bell**
  (effect 9025, which the item applies: `EquipParamGoods` 225). (In some layouts a further flag
  waives the cooperator requirement.) Ten seconds later a message appears (109000), the widow
  plays her entrance and her AI starts.
* **Placed widows** (`配置寡婦`, rows 1058x0) are in 802 layouts, all in the **Sinister
  chalices'** folders (`m29_30_20_00`, `m29_31_20_00`, `m29_50_20/21_00`, `m29_52_20/21_00`,
  `m29_53_20/21_00`) plus two test folders. They stand inert unless flag **92905340** is set,
  the flag of feature 40, the **Sinister Bell** rite.

That settles what the Sinister Bell rite does, which earlier versions of
[the Chalice Dungeons](the-chalice-dungeons.md#rites) left open. Its rite menus (30240, 31240, 50240, 52240, 53240) exist for exactly the depths and
bases that have a Sinister chalice, and its flag is what wakes the widows placed in those
chalices' layouts. The ringing itself is multiplayer logic: while a widow is up,
`マルチ侵入管理` ("multiplayer invasion management") gives the host effect 9020 and shows message
100002 as long as there is room for another client of type 1 (fewer than two with both widows
up, fewer than one with one): by the context, room for an invader. The bell-control template
switches the widow's behavior (effect 9100) for as long as the host has 9020.

The flag is also read by `サブ特徴_マルチ召喚敵_…` ("sub-feature: multiplayer summon enemy",
strong and weak versions, kept only when online). Those templates are started only in the
development folders `m29_01_00_00`, `m29_01_01_00` and `m29_05_00_00`, not in any Root or story
layout.

## Bosses

Boss templates carry the developers' working names for the bosses. Resolving each template's
boss entity to its `NpcParam` row gives the name the game shows:

| Name in the events | Literally | The game's name | Layouts |
|---|---|---|---:|
| `フレッシュゴーレム` | flesh golem | Undead Giant | 1,077 |
| `デブ` | fatty | Merciless Watcher (its templates include `ボスが三体動き出す`, "three bosses start moving") | 665 |
| `炎の獣` | flame beast | Watchdog of the Old Lords | 613 |
| `銀の獣の眷属` | kin of the silver beast | Loran Silverbeast | 550 |
| `始祖ビースト` | progenitor beast | Bloodletting Beast | 535 |
| `邪神` | evil god | Amygdala | 477 |
| `血に渇いた獣` | blood-starved beast | Blood-starved Beast | 344 |
| `嵐の獣` | storm beast | Loran Darkbeast | 328 |
| `月の落とし子` | child dropped from the moon | Ebrietas, Daughter of the Cosmos | 234 |
| `嵐の獣（骨）` | storm beast (bones) | Darkbeast Paarl | 201 |
| `月からの使者（大）` | envoy from the moon (large) | Celestial Emissary | 107 |
| `白痴の蜘蛛` | idiot spider | Rom, the Vacuous Spider | 43 |
| `花嫁` | bride | Pthumerian Bride | 21 |
| `銀の獣` | silver beast | Great One Beast | 3 |

Two more labels:
* `トラップロード移植_…` ("Trap Lord transplant"): seven events for a boss (model c5510, no name
  in the text) whose event names describe darkness magic (`暗闇魔法`), random warps and a second
  copy that "sneaks in" (`2体目がこっそり出現`). Started in 7 layouts.
* `月の落とし子_コジマ粒子` ("child dropped from the moon: Kojima particles"), an Ebrietas event
  sharing its name with the fictional energy of FromSoftware's *Armored Core 4*.

Of the counts above, a boss's layout count is how many of the 2,641 layouts start that boss's
templates, not how often players meet it. That depends on which chalices players run.

## What this doesn't cover

* The library's many trap, door and elevator templates beyond their names. (The 7 layouts with
  events of their own are development layouts; see
  [the rest of m29](the-chalice-dungeons.md#the-rest-of-m29-the-tile-kit-and-the-development-layouts).)
* Which in-game enemies the unnamed models are. The game's text doesn't name ordinary enemies,
  and naming them from outside knowledge is left to a reader who can check in the game.
* The AI side of each special enemy (the Lua scripts behind AI commands 10 and 200, and the
  messages 10 and 20 they send back to the events).

## Files

| Path | |
|---|---|
| `work/chalice/events-1.09.md` | generated: every special-enemy, special-character, widow and boss template with its flags and what it resolves to, and every template's usage across all layouts |
