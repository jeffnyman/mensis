# Leftovers: what the data carries from elsewhere

Bloodborne's tables hold more than Bloodborne. Every param row has a working name, the
developers' own label for it (read by `params.Param.row_names()`, see [enemy
movesets](the-combat.md#the-names-come-with-the-data)). Read across all 79,732 rows, those
labels point to other games, a second game's prototype, event builds, tests, and Dark Souls
words for things Bloodborne renamed.

```
python -I tools/leftovers.py work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/leftovers.md
```

`work/leftovers.md` lists every row found, by kind. This page reads the results. The labels are
the developers' own words; where this page says what they refer to, it is reading the name,
not anything the game does with it.

## Enemies with no model

3,517 rows name 116 character models that have no files in the game: no `chrbnd`, so nothing
can place them, and no map does. Almost all are enemy attacks (`AtkParam_Npc`) and their
behavior rows (`BehaviorParam`), with some bullets. Many names are Dark Souls enemies by their
Japanese names:

| Model | Rows | Label | The name |
|---|---:|---|---|
| c3430 | 241 | `ヘルカイト` | the Hellkite |
| c5290 | 104 | `白竜シース` | Seath the Scaleless, "the white dragon" |
| c2790 | 102 | `黒騎士` | the Black Knight |
| c5350, c5351 | 188 | `ガーゴイル`, `ガーゴイル（色違い）` | the Gargoyles, one "in another color" |
| c5260 | 77 | `貪食ドラゴン` | the Gaping Dragon |
| c5280 | 66 | `焔の魔女クラグスパイダー` | Quelaag, "the witch of flame" |
| c5370 | 56 | `薪の王` | the Lord of Cinder, Gwyn's title |
| c5220 | 22 | `墓王` | the Gravelord, Nito's title |
| c2390 | 22 | `ダークレイス` | the Darkwraith |
| c3270 | 15 | `バジリスク` | the Basilisk |
| c3300 | 6 | `結晶トカゲ` | the Crystal Lizard |
| c4170–c4172 | 12 | `人間性（大）`, `（中）`, `（小）` | Humanity, large, medium and small |

Many more have names this page doesn't identify, such as `四騎士Ｃ（雷）` ("four knights C,
lightning"), `湖獣` ("lake beast"), `金キメラ` ("gold chimera") and `キングイザリス` ("King
Izalith"). The largest, c3430 and c5250 (`爛れ`), have over 200 rows each. Many of the rows are
`共通攻撃` ("common attacks") such as a fall or a landing, the shared attacks every large enemy
gets.

Two model numbers are reused: rows labelled `c2710_クリスタルガード` ("crystal guard") use the
number that is Father Gascoigne's model now, and `c5140（DS２プロト）_飛竜の眷属` ("DS2 prototype:
wyvern kin") uses the large Amygdala-type enemy's.

## A second game's prototype

A few rows are marked for "DS2", the second Dark Souls:

| Row | Label | In English |
|---|---|---|
| NpcParam 200 | `ＤＳ２　お手軽ロジックテスト用` | DS2: for an easy logic test |
| NpcParam 500 | `DS２プロト　ハシゴが大好きな騎士` | DS2 prototype: a knight who loves ladders |
| NpcParam 501 | `DS２プロト　仲間を助けるデカ獄吏` | DS2 prototype: a big jailer who helps his comrades |
| NpcParam 510 | `ＤＳ２テスト　騎士想定` | DS2 test: meant as a knight |
| AtkParam_Npc, Bullet 51400 | `c5140（DS２プロト）_飛竜の眷属_ブレス弾丸` | c5140 (DS2 prototype): wyvern kin, breath bullet |

Ten `NpcThinkParam` rows are labelled `DS2Test`. The NpcParam rows are all below 100000, the
range the enemy tools leave out as tests.

## Event builds

| Marker | Rows | What they are |
|---|---:|---|
| `TGS版`, `2016TGS用` | 29 | Tokyo Game Show builds: starting characters, faces, item lots, a return point |
| `gamescom` | 6 | a gamescom build: a gem for a pistol character raising `最大MP` ("max MP", ×1.852), and two effects adjusting the starting bullet count (`初期弾数調整`) |
| `DLCレビュー用` | 51 | starting characters "for the DLC review" |
| `チャネルプロト` | 70 | item lots of a "channel prototype": treasure chests and treasure corpses |

The starting characters spell out their loadouts. The four `TGS版` ones are `都市ハンター`
("city hunter") with `ノコナタ`, `斧竿` or `剣槌` (the Saw Cleaver, the Hunter Axe, and a
"sword-hammer"), and `切り裂きジャック` ("Jack the Ripper") with `双剣`, the Blade of Mercy. The
four `2016TGS用` ones are themed: `連盟の長` ("the League's
leader", the whirligig saw), `古狩人` ("old hunter"), `秘密を探るもの` ("the one who seeks the
secret") and `メンシス学派` ("the School of Mensis"). The DLC-review characters pair the DLC's
weapons in sets of five: with a piercing rifle (`貫通`), the knuckles (`ナックル`), a grenade
(`グレネード`), the Gatling gun with `噴霧器` (the Rosmarinus), or an element shield (`属性盾`).

The Ludwig fight still carries its show variant: his heat-ups have a second set of health
thresholds behind a flag that only an event named `TGS版と通常版の切り替え`, "switching between
the TGS version and the normal version", tests ([what the map scripts
ask](the-combat.md#what-the-map-scripts-ask)).

## Dark Souls words for Bloodborne's things

Some of Bloodborne's own systems keep their Dark Souls names:

| Label | Dark Souls | In Bloodborne |
|---|---|---|
| `ダークリング` | the Darksign | the Hunter's Mark (goods 100 and its effect 2100, which sets `clearSoul`: you lose your echoes) |
| `かがり火回復効果1`–`7` | resting at a bonfire | recovery effects, presumably for the lamps; the 3rd restores bleeding resistance, which is [rapid poison](the-combat.md#status-buildup) |
| `エスト効果量+10%`, `エスト所持上限+10` | Estus | gem effects for "Estus" healing and carry limit, presumably the blood vials |
| `人間性` | humanity | Insight: a new character's `baseHeroPoint` (人間性), the `GetHeroPoint` script function, and `heroPointDamage` −1 in the Madman's Knowledge's effect, labelled `SAN値回復＋1` ("sanity +1") |
| `呪`, `出血` | curse, bleeding | frenzy and rapid poison ([status buildup](the-combat.md#status-buildup)) |

## Working names

The weapons have working names too, the start of every move label:

| Weapon | Working name | In English |
|---|---|---|
| Saw Cleaver | `ノコナタ` | saw-hatchet |
| Hunter Axe | `斧竿` | axe-pole |
| Chikage | `刀` | katana |
| Blade of Mercy | `双剣` | twin swords |
| Kirkhammer | `鍵鞘` | key-sheath |
| Beast Claw | `拳獣` | fist-beast |
| Rifle Spear | `銃槍` | gun-spear |
| Stake Driver | `パイルバンカー` | pile bunker |
| Logarius' Wheel | `車輪` | wheel |
| Tonitrus | `爆発槌` | explosion mace |
| Threaded Cane | `杖鞭` | cane-whip |
| Beasthunter Saif | `ノコギリ曲剣` | saw-curved-sword |
| Beast Cutter | `ノコギリ鞭` | saw-whip |
| Amygdalan Arm | `多関節` | many-jointed |
| Holy Moonlight Sword | `月光剣` | moonlight sword |
| Rakuyo | `双刃` | twin blades |
| Boom Hammer | `ガンハンマ` | gun hammer |
| Bloodletter | `十字メイス` | cross mace |
| Church Pick | `ウォーピック` | war pick |
| Whirligig Saw | `丸鋸チェンソー` | circular-saw chainsaw |
| Simon's Bowblade | `変形弓` | transforming bow |
| Kos Parasite | `素手（なめくじ）` | bare hands (slug) |

## Tests and the rest

1,095 rows are marked `テスト`, "test": 422 starting characters, 179 gem generators, 137
NpcParam rows, 133 item lots and more. Eighteen enemy attacks are marked `未使用`, "unused",
mostly the crowd's ground-shockwave attacks (`c1030_群衆B_…（地面衝撃波）（未使用）`). Two
objects are `削除予定`, "to be deleted", and seven rows are `（仮）`, "provisional".

## Files

| Path | |
|---|---|
| `tools/leftovers.py` | reads every row's working name and sorts out the leftovers |
| `work/leftovers.md` | every row found, by kind |
