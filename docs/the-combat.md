# Combat: the numbers behind a hit

What a weapon does when it lands: its damage, how that grows with stats and upgrades, each
move's share of it, and what a hit does to stamina, poise and rally. This page reads the
**data** first: the gameplay tables and what their own descriptions say about them. Then
[the formulas](#the-formulas) that combine those numbers, read from the executable: the stat
curve, attack rating, the requirement penalty, defense from stats, the cost of a level, max
HP, how rally wins health back, and how a hit's attack meets the target's defense. Which button
plays each move comes from the [animations](#which-button-plays-which-move). Then [the
effects](#effects-blood-gems-buffs-and-new-game) behind blood gems, buffs and New Game+, and
[the enemies' movesets](#enemy-movesets): every enemy's attacks, by name, and [how an enemy
chooses](#how-an-enemy-chooses) between them. And last, [status buildup](#status-buildup): slow
poison, rapid poison, frenzy and beasthood.

```
python -I tools/combat.py weapons work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/combat
python -I tools/combat.py weapon  work/base/dvdroot_ps4 work/update/dvdroot_ps4 Saw Cleaver
```

`weapons` writes `work/combat/weapons.md` (95 weapons), a table of moves per weapon
(`work/combat/moves/`), and the stat curves (`curves.md`).

## Where the numbers are

Five tables hold a weapon's combat. Their PARAMDEFs ([the data](the-data.md)) give every field
a Japanese name and usually a description, and the descriptions are what settle the
meanings below:

| Table | Holds | Key fields |
|---|---|---|
| `EquipParamWeapon` | one row per weapon **per upgrade level** | base damage, stat scaling and requirements, the stat curve, poise and rally bases |
| `BehaviorParam_PC` | a weapon's moves | stamina cost, and which attack each move uses |
| `AtkParam_Pc` | one row per attack | the move's multipliers on the weapon's values |
| `Bullet` | gun shots | the attack row a shot uses (`atkId_Bullet`) |
| `CalcCorrectGraph` | stat curves | how much of a scaling a stat earns |

**Upgrades are rows, not formulas.** The Saw Cleaver is rows 7000000 to 7001000, one per level
from +0 to +10, each with its own numbers. Physical damage goes 90, 99, 108, … 180, and
Strength scaling goes 40, 42, 44, … 60. `ReinforceParamWeapon`, which in other games scales a
weapon per level, has six rows here, and every rate in them is 1.0. The Chalice Dungeons'
Uncanny and Lost versions have rows of their own, with the same numbers as the plain weapon.

**The stats keep Dark Souls' names.** The four scaling fields are `correctStrength`,
`correctAgility`, `correctMagic` and `correctFaith`: strength, agility, magic, faith.
Bloodborne's stats are Strength, Skill, Bloodtinge and Arcane. The menu's scaling properties
list them as 筋力, 技量, 射撃 ("shooting") and 魔法 ("magic"), in field order, and the English
stat names come in the same order (Strength, Skill, Bloodtinge, Arcane). The weapons agree:

* the guns and the Chikage scale on `correctMagic` (Evelyn 65, Hunter Pistol 45, Chikage 60),
  and Evelyn's requirement there is 18. That field is Bloodtinge.
* the Holy Moonlight Sword's requirement in `properFaith` is 14. That field is Arcane.

**Damage comes in four kinds.** `attackBasePhysics`, `…Magic`, `…Fire` and `…Thunder` are
physical, arcane, fire and bolt. The arcane weapons have the second (Blade of Mercy 30, Holy
Moonlight Sword 50, Burial Blade 30), and the Tonitrus has 40 of the fourth. Most weapons also
carry an Arcane scaling with no arcane base damage (the Saw Cleaver's is 33 at +0). The code
settles what it is for: Arcane scales arcane, fire and bolt damage ([the
formulas](#attack-rating)), so it matters once a weapon has elemental damage.

## Scaling and its letter

A scaling value is a percentage. The menu shows it as a letter. The properties that display
the four scalings (`補正：筋力` and the rest) use format 100, and `MenuValueTableParam` rows
100–106 are that format:

| At least | 110 | 85 | 65 | 55 | 30 | above 0 | 0 |
|---|---|---|---|---|---|---|---|
| Letter | S | A | B | C | D | E | – |

The rows say "compare type 1". That reading of it, "at least", is inferred from the
thresholds running downward. The Saw Cleaver's Strength 40 is a D, and its 60 at +10 a C.

**How much of the scaling a stat earns** is a curve. Every weapon but Bare Fists uses curve 0:

| Stat | 1 | 10 | 25 | 50 | 99 |
|---|---:|---:|---:|---:|---:|
| Share of the scaling earned | 0% | 5% | 50% | 85% | 100% |

So the stat points that pay most are those from 10 to 25. Points up to 50 still pay, and after
50 they barely do. The curve rows hold an exponent for each stage too. They are all 1 for curve 0, and the code
confirms what that means: each stage is a straight line ([a stat curve](#a-stat-curve)). Rows
100 onward are other curves, some with negative exponents: 102 is defense and 200 the cost of
a level ([below](#defense)).

## Moves

A weapon's `behaviorVariationId` (the Saw Cleaver's is 700) selects its moves:
`BehaviorParam_PC` rows `100000000 + variation × 1000 + judge id`. The **judge id** is "the
behavior judge ID entered in TimeActEditor", in the field's own description, which adds that
the hardware doesn't use it directly. Which button plays which judge id isn't in these tables
but in the animations ([which button plays which move](#which-button-plays-which-move)). Each
row gives the move's **stamina** cost and its
attack: an `AtkParam_Pc` row, or for a gun a `Bullet` row whose attack is `atkId_Bullet`.

An attack row is mostly multipliers on the weapon's values, in percent:

| Field | Its description | Multiplies |
|---|---|---|
| `atkPhysCorrection` (and magic, fire, thunder) | "PC only: the multiplier applied to the base physical attack" | the weapon's damage: the **motion value** |
| `atkStamCorrection` | stamina attack correction | the weapon's stamina damage (`attackBaseStamina`) |
| `atkSuperArmorCorrection` | "PC only: the correction applied to the weapon's base value" | the weapon's poise damage (`saWeaponDamage`, "super armor base attack") |
| `wepRegainHpScale` | "weapon regain amount correction" | the weapon's rally (`wepRegainHp`) |

The Saw Cleaver's first moves:

| Judge | 0 | 10 | 20 | 30 | 40 | 100 | 120 | 170 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Stamina | 20 | 20 | 20 | 20 | 20 | 35 | 35 | 50 |
| Damage | 100% | 102% | 104% | 106% | 109% | 118% | 122% | 190% |

Judge ids 0–40 look like a chain of light attacks, each a little stronger. 170 is flagged
`isChargeAtk`, "is it a charged attack?". Its 190% is the largest share in the first set.
The table splits into two parallel sets, 400 apart (0–395 and 400–795, with the same special
moves at 300 and 700, 340 and 740). The tables alone don't name them. The animations do:
0–40 are the R1 chain, 170 the fully charged R2, and the two sets are a trick weapon's two
forms. 100 and 120 turn out to be rows that none of the Saw Cleaver's animations play
([which button plays which move](#which-button-plays-which-move)).

A few moves stand out:

* **300 and 700** do no damage, but 300% stamina damage.
* **310, 340, 710 and 740** carry a `throwTypeId`, "the ID that links to the throw
  parameters", so they lead into a throw (`ThrowParam`).
* **`dmgLevel`** is "which damage motion the enemy plays when hit": 1 for the light chain, 2 for
  heavier blows, 5 and 6 for the special moves.

### Which button plays which move

The judge id is invoked by the animations. Three files link it to a button:

```
python -I tools/tae.py sets  work/base/dvdroot_ps4 work/update/dvdroot_ps4
python -I tools/tae.py moves work/base/dvdroot_ps4 work/update/dvdroot_ps4 Saw Cleaver
```

1. **The weapon picks an animation set.** `WepMotion_AnimOffset` (`01cc0b40` in the 1.09
   executable) takes the weapon's `wepmotionCategory`, 武器モーションカテゴリ ("weapon motion
   category"). Categories 20 to 48 go through a jump table; the rest are used as they are, so
   the Threaded Cane (127) and the DLC weapons (140 to 153) name their set directly. The result
   times 1,000,000, plus the animation's own number, is the animation played: the Saw Cleaver's
   first light attack is `a112_030000`. A non-zero `spAtkcategory`, 特殊攻撃カテゴリ ("special
   attack category"), would replace the set; no weapon sets it.

   | Category | Set | Weapons |
   |---:|---:|---|
   | 22 | a116 | Stake Driver |
   | 27 | a113 | Blade of Mercy |
   | 28 | a112 | Saw Cleaver |
   | 29 | a114 | Chikage |
   | 30 | a103 | Hunter Axe |
   | 33 | a109 | Tonitrus |
   | 34 | a120 | Ludwig's Holy Blade |
   | 35 | a110 | Logarius' Wheel |
   | 36 | a104 | Rifle Spear |
   | 37 | a121 | Burial Blade |
   | 38 | a122 | Saw Spear |
   | 40 | a124 | Reiterpallasch |
   | 42 | a108 | Bare Fists, Kos Parasite |
   | 47 | a106 | Beast Claw |
   | 48 | a105 | Kirkhammer |

   The guns, torches and shields map the same way: 24 → a117 (Hunter Pistol), 41 → a102
   (Hunter Blunderbuss), 31 → a118 (Cannon), 39 → a123 (Rosmarinus), 25 → a107 (torches) and
   20 → a125 (shields, which have no set file).

   Six of the table's entries were first guessed by matching each set's judge ids against
   each weapon's moves. The jump table confirmed all six: Hunter Axe a103, Saw Cleaver a112,
   Beast Claw a106, Blade of Mercy a113, Logarius' Wheel a110 and Tonitrus a109. Matching alone
   couldn't place the others, because most sets use the same judge numbers.

2. **The set's animation events invoke the judge id.** `chr/c0000.anibnd.dcx` holds one TAE
   ("time act event") file per set, `a<set>.tae`. At a moment in an animation, an event of
   type 1 ("invoke attack behavior") or type 2 ("invoke bullet behavior") names a judge id in
   its third value. Some animations import another's events instead: the Saw Cleaver's
   forward roll attack (`032210`) uses the events of its unlocked one (`032200`).

3. **The behavior graph names the animation.** The same archive's
   `Export/Behaviors/c0000.hkx` is the Havok behavior graph. Its states have English names,
   each holding a selector with one clip per set: the state `AtkRightNormal01` holds
   `a103_030000`, `a112_030000` and so on.

So the Saw Cleaver's moves read:

| Judge | State | Move | Stamina | Damage |
|---:|---|---|---:|---:|
| 0–40 | `AtkRightNormal01`–`05` | R1, the five light attacks of the chain | 20 | 100%–109% |
| 150 | `AtkPowerNormalHit01_ModeA` | R2, released before a full charge | 35 | 120% |
| 170 | `AtkPowerHardHit01_ModeA` | R2, fully charged | 50 | 190% |
| 320, 330 | `AtkRightDash`, `AtkRightDashHard` | running R1, running R2 | 30, 45 | 109%, 125% |
| 390 | `ChangeGimmick_RightSide_ModeA` | transform attack (L1) | 23 | 130% |
| 400 | `AtkbothNormal01` | R1, transformed | 24 | 97% |
| 550, 570 | `AtkPowerNormalHit01`, `AtkPowerHardHit01` | R2, transformed, released early and fully charged | 38, 50 | 125%, 170% |
| 790 | `ChangeGimmick_RightSide_ModeB` | transform attack back (L1) | 30 | 130% |

This settles the two sets 400 apart. The states for 0–395 say `Right` or `ModeA`, and those
for 400–795 say `Both` or `ModeB`. `ChangeGimmick` ("change the trick") states sit in both.
A trick weapon's two forms are its two sets.
The state names also show that the Saw Cleaver's R2 is a charge attack (`AtkPower…`). Its 150
is the R2 released early, and its 170 the full charge. Other weapons have a separate
`AtkRightHard` chain: the Hunter Axe's transformed R2 is judge 500, `AtkBothHard01`.

The plain names come from the state names, and `tae.py`'s `GLOSS` table holds the
translation. `Normal` is R1, `Hard` R2, `Power` a charged R2, `Dash` running, `Rolling` out
of a roll, `Step` out of a quickstep, `Fall` plunging, `L2` the L2 attacks and `Throw` a
visceral or grab. Each moves table in `work/combat/moves/` has a Move column with these names,
and an Animations column with the animations and states behind each.

**Not every row is played.** Across the 40 distinct weapons, the sets' events invoke 982 of
the 1,530 move rows. A DLC weapon's rows are nearly all played: the Beasthunter Saif's 36 of
39, the Church Pick's 38 of 41. A base weapon has rows its own set never invokes. For the Saw
Cleaver these are 22 rows: 100–130, 240, 260–300, 500, 510, 650–700, 800 and 900–950. Some of
them are moves of other sets: 500 is the Hunter Axe's transformed R2. Others, 16 of the 22, no
animation of any set invokes: 100–130, 260–300 (300 being the stamina-only move), 660–700, 900
and 901. Those may be unused, or started another way, by the action script or a throw. That
isn't followed here. Their own labels say what they were meant to be
([the names come with the data](#the-names-come-with-the-data)). Two sets, a155 and a156, have attack events, but no weapon's category
leads to them.

### Poise

A player's attack deals poise damage of `saWeaponDamage` × `atkSuperArmorCorrection`%. The flat
`atkSuperArmor` field is "NPC only". The weapon bases are small and few: Bare Fists and the Blade
of Mercy 5, the Saw Cleaver and Chikage 15, the Burial Blade 20, the Hunter Axe 30, the
Kirkhammer 37. In every move the Saw Cleaver makes, the poise multiplier equals the damage one.

### Rally

Rally is **リゲイン, "regain"**, in the tables ([how it wins health
back](#how-rally-wins-health-back) is below). A hit's regain comes from three fields:

* the weapon's `wepRegainHp`, "weapon regain amount": the Saw Cleaver's goes 35 → 55 over its
  levels, the Hunter Axe's 75 → 125, the Kirkhammer's 60 → 120;
* the move's `wepRegainHpScale`, a percentage of that;
* the attack's own `atkRegainHp`, "attack regain amount", a flat value on a few moves.

Two more fields shape the window, `regainableTimeScale` and `regainableHpRateScale` ("regainable
time" and "regainable rate" multipliers, 1.0 on every Saw Cleaver move). `regainableSlotId` says
"if not −1, attacks with the same ID combine their regainable amounts".

The rally scale is **tuned per move**, not tied to the damage. Averaged over each set of moves,
rally per unit of damage comes out as follows:

| Weapon | First set | Second set |
|---|---:|---:|
| Saw Cleaver | 1.00 | 1.31 |
| Burial Blade | 0.97 | 1.42 |
| Chikage | 1.00 | 1.29 |
| Kirkhammer | 0.75 | 0.84 |
| Ludwig's Holy Blade | 0.75 | 0.96 |
| Logarius' Wheel | 2.27 | 1.90 |
| Hunter's Torch | 0 | 0 |

## The formulas

How the game combines those numbers, read from the 1.09 executable in Ghidra
([loading it](ghidra.md)). The functions are recorded in `tools/names/eboot-1.09.hand.tsv`, and
`combat.py` implements them:

```
python -I tools/combat.py ar     work/base/dvdroot_ps4 work/update/dvdroot_ps4 Saw Cleaver +0 12 10 9 9
python -I tools/combat.py levels work/base/dvdroot_ps4 work/update/dvdroot_ps4 4 30
python -I tools/combat.py hit    work/base/dvdroot_ps4 work/update/dvdroot_ps4 Saw Cleaver +10 25 25 9 9 0 271000
```

**Finding them.** The param repository names its tables in one list: `{name, index}` pairs at
`05755ad0`, `EquipParamWeapon` 0 through `MenuValueTableParam` 61, with `CalcCorrectGraph` at
32. Table *n* sits at `SoloParamRepository + 0x70 + n × 0x48`, the layout the Chalice work
had already found for its own tables. So the code that reaches CalcCorrectGraph reads `+0x970`
and `+0x978`. A sweep of the code for that pair found its getter (`02314b60`), and the getter's
callers led to the rest.

### A stat curve

`CalcCorrectGraph_Evaluate` (`01979240`) takes a stat `x` and a curve row:

1. it caps `x` at the last stage point, and finds the stage `i` with
   `stageMaxVal[i] < x ≤ stageMaxVal[i+1]`;
2. it takes `r = (x − stageMaxVal[i]) / (stageMaxVal[i+1] − stageMaxVal[i])` and the exponent
   `e = adjPt_maxGrowVal[i]`, the exponent stored with the stage's **first** point;
3. it returns `g[i] + (g[i+1] − g[i]) × r^e` when `e ≥ 0`, and `g[i] + (g[i+1] − g[i]) ×
   (1 − (1 − r)^−e)` when `e < 0`. Here `g` is `stageMaxGrowVal`, and the result is held between
   the two.

A positive exponent above 1 starts a stage slowly, and a negative one starts it fast. Curve 0's
are all 1, so the weapons' curve is the straight-line stages shown above.

### Attack rating

`AttackRating_Compute` (`01976df0`) works damage type by damage type. Each stat gives a
multiplier:

* **requirement met** (or none): `1 + correct/100 × curve(stat)/100`, where `correct` is the
  weapon's scaling times its upgrade row's rate (all 1.0, as above), and `curve` is the
  weapon's `correctType`. With no scaling the multiplier is 1;
* **requirement not met:** a penalty. With `m` the shortfall in percent of the requirement,
  capped at 20, the multiplier falls along a parabola from 1.0 at the requirement to **0.198**
  at 20% or more short. Its constants, 20 and 20, are in the executable's data (`05527fe0`,
  `05527fe4`). The parabola's direction depends on a flag set at run time (`05956720`), but
  its ends don't.

The damage types then use them like this:

| Damage | Base | Stats |
|---|---|---|
| physical | `attackBasePhysics` | Strength and Skill, **added**: `1 + (Str − 1) + (Skl − 1)`, or the smaller of the two if either is a penalty |
| arcane, fire, bolt | `attackBaseMagic`, `…Fire`, `…Thunder` | **Arcane** alone |
| physical, in mode 4 | `attackBasePhysics` | **Bloodtinge** alone |

and each is

    base × upgrade rate × the move's motion value / 100 × the stat multiplier × buffs

So Arcane scales every element, which settles the question left open above. Bloodtinge isn't in
the normal physical calculation at all. The function takes a mode argument. Its three callers pass one
through, and one of them (`01a274d0`) is called by five small functions, each taking a different
output, one of them with mode 4. They look like the menu's five attack values, with Bloodtinge's
being the "blood" one, but that isn't followed further here.

Three more things are in the same code:

* **Visceral attacks:** in a throw, the result is also multiplied by `(100 + throwAtkRate)/100`,
  the weapon's `throwAtkRate`.
* **Luck:** a Dark Souls luck bonus (up to ×1.21) applies to weapons with `isLuckCorrect`. No
  Bloodborne weapon has it set.
* **Two-handing:** another Dark Souls survival multiplies Strength under a flag, by a constant
  that is 1.0 in the file (`05527fdc`).

| Saw Cleaver | Str 12, Skl 10 | Str 25, Skl 25 | Str 99, Skl 99 | Str 5 (needs 8) |
|---|---:|---:|---:|---:|
| +0 | 94.9 | | | 17.8 |
| +10 | | 270.0 | 360.0 | |

### Defense

`Defense_StatSum` (`0197b180`) adds up six stats (with weights, all 1.0 in the file). In the
same structure, attack rating reads Strength, Skill, Bloodtinge and Arcane at `+0x58`–`+0x70`.
The six are those four and the two before them, presumably Vigor and Endurance. Curve **102**
of that sum, rounded, is each damage type's base defense, which effects then multiply. The
armor's flat defenses would be added, but every piece's are 0; armor works by
[cutting damage](#what-a-hit-costs-a-hunter) instead:

| Stat sum | 1 | 60 | 120 | 195 | 600 |
|---|---:|---:|---:|---:|---:|
| Base defense | 10 | 10 | 180 | 255 | 300 |

with exponents 1, −1.1, −1.2 and 1 between the points. How defense then reduces a hit is [attack against defense](#attack-against-defense).

### Leveling up

`LevelUp_TotalCost` (`01f2e460`) uses curve row **200**, through the four fields that only that
row fills: `init_inclination_soul` 0.1, `boundry_value` 92, `boundry_inclination_soul` 0.02,
`adjustment_value` 1. The echoes to go from level *L* to the next are, with `x = L + 81`:

    x² × (max(0, x − 92) × 0.02 + 0.1) + 1,   rounded

| From level | 4 | 5 | 10 | 11 | 12 | 20 | 50 | 99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Echoes | 724 | 741 | 829 | 847 | 1,039 | 2,857 | 15,103 | 60,265 |

Past level 11 (`x > 92`) this is `0.02L³ + 3.12L² + 111.78L − 786.32`. That is exactly the
Demon's Souls and Dark Souls cost, `0.02x³ + 3.06x² + 105.6x − 895`, at `x = L + 1`. Below
it, the cost is `x²/10 + 1`.

### How rally wins health back

Rally runs in two places. One is where a hit is put together. The other is a small state machine
that the developers' own debug menu names.

**A hit's regain amount.** `HitRecord_Build` (`023137a0`) fills in the record a hit carries. Its
regain amount is

    wepRegainHp × wepRegainHpScale / 100 + atkRegainHp

the weapon's regain, scaled by the move, plus the attack's flat amount. Beside it the record
keeps the attack's `regainableSlotId`, `regainableHpRateScale` and `regainableTimeScale` (with
no attack row: slot −1, rate 1.0, time 1.0). The moves tables in `work/combat/moves/` now give
each move's amount at +0 and at the last level. The Saw Cleaver's first attack carries 35 at +0
and 55 at +10.

**The grace HP.** In the code, rally is **猶予HP**, "grace HP": the damage you can still win
back, the orange part of the health bar. Each character has a `ChrHpRegainSlot`. Its constructor
(`01905510`) registers a debug page, `HpRegain`, whose labels name every tuning value:

| Value | Label | In the file |
|---|---|---:|
| `05527b10` | DelayTime 猶予時間, "grace time" | 3.0 seconds |
| `05527b14` | DelayRate 猶予率, "grace rate" | 1.0 |
| `05527b0c` | VanishDelayRate 猶予消滅率 [MaxHp/Sec], "grace vanishing rate" | 0.25 |
| `05527b18` | RegainRate リゲイン率, "regain rate" | 0.6 |
| `05527b1c` | FollowTartgetDelay 目標猶予追従, "follow the target grace" | on |
| `05952d74`, `05952d75` | StackDelayHp 猶予HP蓄積, "stack grace HP"; ResetDelayTime 猶予時間リセット, "reset grace time" | off (zeroed memory) |

Its live readouts are current HP, target grace HP, grace HP, vanishing grace HP per second,
**奪還HP ("recaptured HP", RegainHp)**, and remaining grace time. `ChrHpRegainSlot_Update`
(`01905a40`) runs every frame:

* **Taking damage** sets the grace HP to the HP before the hit, less `damage × (1 − DelayRate)`.
  With the rate at 1.0, all of the damage can be won back. The timer is set to 3 seconds.
* **Recapturing:** while there is recaptured HP waiting, `max(it × RegainRate, 1)` is added to
  the HP, up to the grace HP, rounded down. So 60% of the waiting amount becomes health, at
  least 1 point. If the timer has already run out, it restarts at 3 seconds. While it runs, a
  hit doesn't extend it, because ResetDelayTime is off.
* **When the timer runs out**, the grace HP drains toward current HP at a quarter of max HP per
  second. With FollowTartgetDelay on, the bar shown follows it at the same rate. Once the bar
  reaches current HP, the orange is gone.
* **Healing** above the grace HP carries it up.

The step between the two isn't followed here: how a landed hit's regain amount becomes the slot's
recaptured HP. That includes the slot ID's combining, and where the attack's time and rate scales
apply. If the amount passes straight through, the Saw Cleaver's first hit at +10 recaptures
`55 × 0.6 = 33` HP.

### Health

`MaxHp_FromVigor` (`0197eaf0`) is curve **100** of the first stat:

| Vigor | 1 | 15 | 30 | 50 | 99 |
|---|---:|---:|---:|---:|---:|
| Max HP | 400 | 682 | 1,100 | 1,500 | 1,900 |

The exponents between the points are 1.1, 1.2, −1.2 and −1.1. So each stage starts a little
slowly up to 30, and after 30 the gains come early in each stage. Curve **101** is read from
the second stat (presumably Endurance) and rises to 450. What it sets isn't followed here.

### Attack against defense

`Damage_Compute` (`01976240`) is the hit's formula. It was found from the defender's side. An
enemy's defense lives in NpcParam: `def_phys`, `def_mag`, `def_fire` and `def_thunder`, plus
`def_slash`, `def_blow` and `def_thrust` described as percentages. Only one function reads all
seven: `NpcDefense_Compute` (`0197b320`). It is called through slot `0x220` of the characters'
virtual tables (the constructors store `0578ebc0` for an enemy, `0578f310` and `0578fa80` for
the players), and a sweep for calls through that slot found three. The hit-receiving function
(`01e2a770`) is one of them: it reads the hit record's four attack values, gets the defense,
and passes both to `Damage_Compute`.

**An enemy's defense:**

* **physical** is `def_phys × (100 + p) / 100`, where `p` is the enemy's percentage for the
  attack's physical type: `def_slash`, `def_blow` (blunt) or `def_thrust`. The attack's
  `atkAttribute` picks which, 1, 2 or 3 in that order;
* **arcane, fire and bolt** are `def_mag`, `def_fire` and `def_thunder`.

Each is then multiplied and added to by effects (`FUN_01be76b0`). A player's defense comes from
[the stat sum and armor](#defense) through the same slot. Father Gascoigne (NpcParam 271000) has
90 physical, 80 arcane, 65 fire and 120 bolt, and 0 for slash, blunt and thrust.

**The formula** takes each damage type separately, with `r = attack / defense`. The game keeps a
**reduction percentage** and lets through the rest:

| Ratio `r` | Reduction | Damage |
|---|---|---|
| no defense | 10% | 0.9 × attack |
| `r ≥ 8` | 10% | 0.9 × attack |
| `2.5 < r < 8` | `10 + 0.6612 × (r − 8)²` | 0.9 down to 0.7 |
| `1 ≤ r ≤ 2.5` | `30 + 13.333 × (r − 2.5)²` | 0.7 down to 0.4 |
| `0.12 < r < 1` | `90 − 38.74 × (r − 0.12)²` | 0.4 down to 0.1 |
| `r ≤ 0.12` | 90% | 0.1 × attack |

This is **Dark Souls' damage curve, kept as percentages**. An earlier version of this page recorded
the formula as not found: a search for Dark Souls' constants (19.2/49, 0.8/121) came up empty,
because the game stores them differently. 0.6612/100 is 0.8/121, and 13.333/100 is 0.4/3, the same as in that
game. Only the bottom piece moves: its bend is at 0.12 rather than 0.125, with 0.3874 in place of
19.2/49, so it still meets 0.4 at `r = 1`.

The four results are then multiplied by factors the caller passes (the weakness multipliers
below, and two vectors of effect multipliers), summed, and scaled by three more:

* **× 1.4** when bit `0x80` of a character state word (`+0x1bc`) is set (whose, the code read
  here doesn't settle);
* **× 1.125 or × 0.8** when one of two flags in the hit record (`+0x1c0`, `+0x1c1`) is set and no
  other value is given;
* **× a final factor** the caller passes.

What those flags mean isn't named in the code read here. Before any of this, the receiving
function multiplies each attack value by **weakness factors**, one for each of four bits of the
target's NpcParam flags (`+0x145`), among them `isWeakSaint` ("weak to holy"), `isWeakA` and
`isWeakB`.

Because each type is judged against its own defense, **splitting damage costs damage**. A
small arcane part meets the full arcane defense on its own:

```
python -I tools/combat.py hit work/base/dvdroot_ps4 work/update/dvdroot_ps4 Holy Moonlight Sword +10 16 12 9 50 0 271000
phys    attack   225.0  defense   90.0  damage   157.5  (0.700)
arcane  attack    92.5  defense   80.0  damage    42.5  (0.459)
total     200.0
```

| Saw Cleaver +10, Str 25, Skl 25, against Father Gascoigne | Attack | Ratio | Damage |
|---|---:|---:|---:|
| judge 0 (100%) | 270.0 | 3.0 | 198.4 |
| judge 170, charged (190%) | 513.0 | 5.7 | 443.8 |

`hit` leaves out the weakness multipliers, effects and the three flags, so its numbers are a
hit on an unbuffed, unflagged target.

## Effects: blood gems, buffs and New Game+

Everything that changes a number for a while, or for good, is a **special effect**, a row of
`SpEffectParam` (7,855 rows, 228 fields). Blood gems, runes, buffs and New Game+ all come down
to these rows.

```
python -I tools/effects.py tables work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/combat
python -I tools/effects.py effect work/base/dvdroot_ps4 work/update/dvdroot_ps4 7421
```

`tables` writes `work/combat/gems.md` (every gem effect, by category and rank), `ngplus.md`
(the New Game+ effects), `runes.md` (every rune's gem effects) and `buffs.md` (the
consumables). A field counts as **changed** when it differs from the value most rows
hold: 1.0 for a rate, 0 or −1 for most others. A few gem effects show no changed number; their
work is done through a state ID (`stateInfo`) or by code, not by a field.

### Two kinds of attack multiplier

The effect fields keep attack and damage apart, and their own names say so:

| Field | Its name | What it scales |
|---|---|---|
| `physicsAttackPowerRate` (and magic, fire, thunder) | 物理攻撃力倍率, "physical attack power rate" | attack rating |
| `physicsAttackRate` (and the others) | 攻撃側：物理ダメージ倍率, "attacker side: physical damage rate" | damage |
| `slashAttackPowerRate`, `blowAttackPowerRate`, `thrustAttackPowerRate` | attack power by physical type | attack of that type |
| `physicsAttackPower` (and the others) | 物理攻撃力[point] | a flat amount of attack |
| `physicsDiffenceRate`, `physicsDiffence` (and the others) | defense rate, flat defense | defense |
| `slashDamageCutRate` (and the other types) | damage cut by type | damage taken |

**Stacking.** Two of the functions that gather a character's active effects were read, and both
**multiply** rates and **add** flat values. `SpEffect_DefenseRates` (`01be76b0`) multiplies the
defense rates of every active effect and sums the flat defenses. `SpEffect_DamageCutRates`
(`01be7a80`) multiplies the damage-cut rates for the damage type at hand, skipping effects of
a few categories (110, and in one mode 158 and 204). So two gems of ×1.10 give ×1.21, not ×1.20.
The attacker's own gatherer does the same with attack rates
([how an attacker's effects combine](#how-an-attackers-effects-combine)).

### Blood gems

A gem is made of **gem effects**, rows of `GemeffectParam` (16,101). Each names its special
effect, a category (`GemCategoryParam`, 444), a rank, and the ranks it can drop at. It also has
an optional second effect for when an attack hits (`speffectId_forAtk`). Gem generators
(`GemGenParam`, 1,337) roll up to six effects each, with a chance for each to appear and to turn
negative. **The game text names every effect**: the menu text's 魔石効果 ("gem effect") table
is keyed by the `GemeffectParam` row. 12,579 of its 12,585 lines match a row.

The text is presentation, and the effect row is what applies:

* **The "Add … ATK" gems are whole numbers in the data.** "Add blood ATK +0.5" stores 0, so the
  rank-1 gem adds nothing. "+1.1" stores 1, "+1.5" stores 2, "+2.5" stores 2, "+2.8" stores 3.
  The elemental "Add" gems are the same: "Add arcane ATK +3.5" stores 4. Those elemental gems
  also set `weaponConvertAttribute` (1 arcane, 2 fire, 3 bolt), which marks a weapon as
  converted. In attack rating, an attribute weapon (`isAttributeWep`) can lose its physical
  base under a context flag, but that path isn't followed here.
* **The percentage gems mostly match.** Of 6,279 effects whose text gives one percentage and
  whose row changes one rate, 6,036 agree to a twentieth of a percent. "Physical ATK UP +2.7%"
  is ×1.027. "Blunt ATK UP +0.4%" is ×1.0036, which the text rounds.
* **"Boosts rally potential" stores four times what it says.** "+1.8%" is `regainRate` ×1.072,
  and 241 of the 244 rally effects are within rounding of 4×.
* **"Falling damage reduced 30%" stores ×0.5**, a 50% reduction; "50%" stores ×0.25 and "100%"
  ×0.

### Runes

Caryll runes are gems too: the ones that go in the body's slots (`GemGenParam` rows with
`equipableSegmentBody` set) rather than a weapon's. Each carries gem effects, and each gem
effect's name is the gem-name text (`魔石名`) at 11,000,000 plus its `gemNameIdOffset`. The
Caryll Rune items in the inventory (goods 1600–1612) only point at effects labelled `原石`, "raw
stone", which change nothing. `runes.md` lists all 83 gem effects a rune can carry:

| Rune | Text | What the effect holds |
|---|---|---|
| Clockwise Metamorphosis | Boosts max HP +5% / +10% / +15% | `maxHpRate` ×1.05 / 1.1 / 1.15 |
| Anti-Clockwise Metamorphosis | Boosts max stamina +10% / +15% / +20% | `maxStaminaRate` ×1.1 / 1.15 / 1.2 |
| Clawmark | Ups visceral attack ATK +10% / +20% / +30% | the four damage rates ×1.1 / 1.2 / 1.3, for throws only (`throwAttackParamChange`) |
| Great Lake | All DMG reduction +3% / +4% / +5% | every damage-cut rate ×0.97 / 0.96 / 0.95 |
| Arcane Lake | Arcane DMG reduction +5% / +7% / +10% | `magicDamageCutRate` ×0.95 / 0.93 / 0.9 |
| Moon | More echoes from slain enemies | `soulRate` ×1.1 / 1.2 / 1.3 |

Clawmark raises the visceral attack's **damage** rates (攻撃側：ダメージ倍率), not its attack
power, so it multiplies after defense.

### Consumables

An item whose use applies an effect is an `EquipParamGoods` row with `refCategory` 2 and the
effect in `refId`. `buffs.md` lists the 37 that change something:

* **Blood Vial** (`HP回復【40%】`, "HP recovery 40%") restores 40% of max HP (`transfusionHpRate`
  −40). Iosefka's restores 70%. The bloods of Arianna and Adella restore 25%, and
  Adeline's 50%.
* **Fire Paper** and **Bolt Paper** (`武器強化【炎】`, `【雷】`, "weapon buff: fire, bolt") add a
  **flat** 80 fire or bolt attack for 60 seconds. They are flagged `bAdjustFaithAblity`, scaling
  with Arcane ("faith" being Arcane's old name, as in the [scaling](#scaling-and-its-letter)
  fields). The **Empty Phantasm Shell** adds 80 arcane the same way.
* **Bone Marrow Ash** (`武器強化【銃】`, "weapon buff: gun") holds no number: it flags guns
  (`wepParamChange` 2) and Bloodtinge (`bAdjustMagicAblity`), and the boost is the code's.
* **Blue Elixir** cuts how far enemies see (`sightSearchEnemyCut` 70) and hear (×0.7) for 30
  seconds.

### How an attacker's effects combine

The attacker's gatherer is `SpEffect_AttackRates` (`01be5a30`). For each active effect it takes
the attack-power rates (physical, slash, blunt, thrust, neutral, and the elements) and
**multiplies** them into a running product. It adds the flat attack values into a running sum,
after scaling each positive one by a product of four factors held elsewhere (not identified
here; the papers' stat scaling is the likely use). Effects marked `bGameClearBonus` are first
scaled by the New Game+ table, as on the defense side. So attack rates stack like defense
rates: two effects of ×1.10 give ×1.21. The enemy numbers in [what a hit costs a
hunter](#what-a-hit-costs-a-hunter) were computed that way, and the assumption now has its
code.

### New Game+

Each `NpcParam` row names a `GameClearSpEffectID`, the effect that character gets in New
Game+. There are 95 such effects, and they all have `bGameClearBonus` set: 周回ボーナス用か, "is
it for the cycle bonus?". The effect gatherer handles that flag itself. In the first playthrough
the effect does nothing. From the second, it is multiplied by a factor for the playthrough,
read from a table in the executable (`04b31ed0`), indexed by a count in the game data and capped
at the sixth:

| Playthrough | 1 | 2 | 3 | 4 | 5 | 6 | 7 on |
|---|---:|---:|---:|---:|---:|---:|---:|
| Factor | — | ×1.0 | ×1.07 | ×1.1 | ×1.125 | ×1.15 | ×1.2 |

The effects scale an enemy's max HP, attack, defense, stamina damage and echoes. Early enemies
gain the most, which brings them up toward the late game's:

| Effect | Used by (among others) | HP | Attack | Defense | Echoes |
|---:|---|---:|---:|---:|---:|
| 7421 | Father Gascoigne | ×6.08 | ×2.328 | ×1.24 | ×19.107 |
| 7403 | Cleric Beast, Vicar Amelia | ×4.118 | ×1.683 | ×1.326 | ×6.895 |
| 7408 | Rom, the Vacuous Spider | ×2.496 | ×1.334 | ×1.176 | ×5.072 |
| 7410 | The One Reborn | ×1.883 | ×1.248 | ×1.137 | ×4.217 |
| 7425 | Gehrman, the First Hunter | ×1.456 | ×1.099 | ×1.031 | ×3.064 |
| 7413 | Moon Presence | ×1.34 | ×1.089 | ×1.016 | ×2.965 |

(Effect 9200 adds a further, multiplayer scaling through curve 121. That isn't followed here.)

## Enemy movesets

An enemy attacks the same way the player does: an animation invokes a judge id, the judge id
picks a behavior row, and the behavior row points at an attack. The enemy's AI picks the
animations.

```
python -I tools/enemies.py tables work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/combat
python -I tools/enemies.py enemy  work/base/dvdroot_ps4 work/update/dvdroot_ps4 Cleric Beast
```

`tables` writes `work/combat/enemies.md`, one line per enemy, and a sheet per enemy in
`work/combat/enemies/`. There are 148 sheets, with 8,096 hits between them, and 7,636 of those
hits lead to an attack row.

### From an NpcParam row to its attacks

* **Model.** The maps say which model each placed row uses (MSB parts: a model and an
  NpcParam row). For a row the maps don't place, the row number names it:
  c<(*r* mod 1,000,000) / 100>, so 500000 is c5000, and so is its Chalice Dungeon copy
  10500000. The model's animations are in `chr/c5000.anibnd.dcx`. Rows below 100000 (tests,
  and the human characters, who carry weapons) aren't covered.

  The number alone misleads more often than not. 282 of the 575 placed rows use a model their
  number doesn't name. The Cleric Beast fought on the Great Bridge is row **500241**, placed as
  c5000, though its number reads c5002. The first version of these sheets used the number
  alone, so it sent such rows to the wrong animations or skipped them. It also gave the Cleric
  Beast row 500000's figures, a copy the maps never place. Each sheet now lists its placed rows
  first, and the representative row is a placed one. A row placed with several models takes
  the one its number names: 507000 is the One Reborn as c5070, and also a c1050 crowd figure.
* **Animations.** The archive's TAE file gives each animation's events, as for the player.
  The Cleric Beast's animation 3004 invokes judge 140 at 0.73 seconds, then 141, 142 and 143:
  one animation, four hits.
* **Moves.** `BehaviorParam` row 200000000 + `behaviorVariationId` × 1000 + judge id is the
  move, pointing at an `AtkParam_Npc` row or at a `Bullet` row whose `atkId_Bullet` is the
  attack.
* **Attacks.** Unlike the player's, an enemy's attack row holds the attack's own values, not
  multipliers. `atkPhys`, `atkMag`, `atkFire` and `atkThun` are its physical, arcane, fire and
  bolt attack. `atkStam` is its stamina damage, `atkSuperArmor` its poise damage, `dmgLevel`
  the reaction it causes, and `atkAttribute` its type: 1 slash, 2 blunt, 3 thrust, and 4
  standard ([what a hit costs a hunter](#what-a-hit-costs-a-hunter)). Special effects scale these values afterwards, New Game+
  among them ([New Game+](#new-game)).
* **AI.** `NpcThinkParam` row `aiThinkId` names the battle script, `<battleGoalID>_battle.lua`,
  in one of `script/*.luabnd.dcx`. Its action functions queue animations with `AddSubGoal`.
  The sheets list, for each animation, the actions whose constants name it. The Cleric Beast's
  `Kyoukuchou500000_Act01` names 3000, 3001, 3011 and 3017.

### The names come with the data

Every param row has a name. The row header (24 bytes each, from 0x40) holds the row id, its
data offset and the offset of a UTF-16 string, the developers' working label for the row.
`params.Param.row_names()` reads it. For an enemy's attacks these labels say what the attack is:

| Animation | Judge | `AtkParam_Npc` row name | In English | Physical | Type | Poise damage | Hit level |
|---:|---:|---|---|---:|---|---:|---:|
| 3000 | 100 | `c5000_教区長_右フック` | Cleric Beast: right hook | 229 | slash | 50 | 2 |
| 3001 | 110 | `左フック` | left hook | 194 | slash | 50 | 2 |
| 3002 | 120 | `叩きつけ` | slam | 379 | slash | 100 | 6 |
| 3003 | 130 | `踏みつけ` | stomp | 379 | slash | 200 | 6 |
| 3004 | 140–143 | `左右ラッシュ1撃目【右手】`… | left-right rush, 1st hit (right hand) to 4th | 194, 136, 153, 194 | slash | 50 | 2, 2, 2, 3 |
| 3005 | 150, 151 | `超叩きつけ1撃目`, `2撃目（引っこ抜き）` | super slam, 1st hit; 2nd hit (pulling out) | 333, 275 | slash | 150, 100 | 6, 9 |

(`教区長`, "parish head", is the Cleric Beast's working name.) The sheets keep the labels in
Japanese, with the enemy's shared start removed. The vocabulary is too varied (1,694 distinct
words) for a word list to translate faithfully.

The NpcParam rows are named too, so an enemy without an English name still has one: 540000 is
`悪夢を継ぐもの（第2形態）`, "the one who inherits the nightmare (2nd form)". A sheet takes its title
from the English name of any row with its model, and otherwise from its first row's working
name.

**The player's rows confirm the animation names.** `BehaviorParam_PC` rows are labelled the
same way, and the moves tables now show the label beside the move. Of the 815 played moves
whose animation name says what they are, 791 agree with their label. `小1`–`小5` ("light
1–5") are the R1 chain, `溜め小` and `溜め大` ("charge, small" and "large") the R2 released
early and fully charged, `回避小` ("evasion, light") the attack out of a roll and
`モードチェンジ攻撃` ("mode change attack") the transform attack. Of the 24 that don't agree, 16
are transformed charge attacks the labels call `L小` and `L大` (Chikage, Hunter Axe, Burial
Blade, Kirkhammer, Beast Claw and others). The other 8 are second hits riding on a move, such
as the Kirkhammer's `衝撃波` ("shockwave") during its transformed R1 chain. The labels also name
rows no animation plays. The Saw Cleaver's 100–130 are `大1`–`大4` ("heavy 1–4"), a heavy chain
its animations never use. Its 300 is `近距離` ("close range") and its 900 `ガード` ("guard").

### The heaviest blows

Each boss's largest attack, as the sum of its four values before special effects:

| Boss | Animation | Judge | Label | In English | Attack |
|---|---:|---:|---|---|---:|
| Celestial Emissary | 3010 | 180 | `月からの使者（大）_魔法1` | messenger from the moon (large): magic 1 | 540 |
| Ludwig the Accursed | 3017 | 461 | `ルドウイーク_飛び降りで現れる` | Ludwig: appears by jumping down | 530 |
| Amygdala | 3026 | 367 | `邪神（脚怒り）_大ジャンプ攻撃（足）` | evil god (legs, angered): big jump attack (feet) | 490 |
| Darkbeast Paarl | 3003007 | 540 | `嵐の獣（骨）_ジャンプ叩きつけ_上半身_第3段階` | storm beast (bone): jump slam, upper body, 3rd stage | 460 |
| Laurence, the First Vicar | 1003003 | 315 | `教区長Ω_踏みつけ（炎属性付加後）` | parish head Ω: stomp (after fire is added) | 454 |
| The One Reborn | 3008 | 140 | `なりそこないの邪神_単発_両腕叩きつけ` | failed evil god: single, both-arm slam | 448 |
| Gehrman, the First Hunter | 3031 | 300 | `拠点老人（覚醒）_月光浴` | old man of the base (awakened): moonlight bath | 390 |
| Orphan of Kos | 3024 | 192 | `ラスボス_溜め振り下ろし` | last boss: charged downward swing | 390 |
| Cleric Beast | 3002 | 120 | `教区長_叩きつけ` | parish head: slam | 379 |
| Ebrietas, Daughter of the Cosmos | 3018 | 280 | `月の落とし子_中距離右腕叩きつけ` | child of the moon: mid-range right-arm slam | 379 |
| Vicar Amelia | 3007 | 170 | `聖女ビースト_両手振り下ろし` | saint beast: two-handed downward swing | 373 |
| Lady Maria of the Astral Clocktower | 2003016 | 715 | `時計塔ボス_解放（爆発）` | Clocktower boss: release (explosion) | 360 |
| Rom, the Vacuous Spider | 3001 | 110 | `白痴の蜘蛛_魔法B（爆発）` | idiot spider: magic B (explosion) | 340 |
| Father Gascoigne | 3004 | 140 | `ガスコイン神父（獣）_両手叩きつけコンボ` | Father Gascoigne (beast): two-handed slam combo | 333 |

The Orphan of Kos is `ラスボス`, "last boss", in its own data. Gehrman is `拠点老人`, "the old
man of the base", the Hunter's Dream being the base. Animation numbers above a million are
in the same TAE file, in a numbered group: Laurence's `1003003` is animation 3003 of group 1,
and Paarl's `3003007` animation 3007 of group 3. What each group stands for isn't read here.

One attack is off the scale. The Moon Presence's `見つめる` ("gaze", animation 3013, judge
320) goes through bullet 540040 to an attack of 9,999 physical, 9,999 fire and 9,999 bolt,
flagged `isDisableNoDamage`. In play the gaze is known to leave the player at 1 HP rather than
kill, so something else caps it. That isn't read here.

### What a hit costs a hunter

```
python -I tools/enemies.py hit work/base/dvdroot_ps4 work/update/dvdroot_ps4 500241 120 30 "Hunter Hat" "Hunter Garb" "Hunter Gloves" "Hunter Trousers"
```

Four things stand between an enemy's attack row and the hunter's health.

**1. The enemy's area level.** NpcParam's `spEffectID0`–`7` are 常駐特殊効果, "resident special
effects". Most enemies carry one named for the level of the place they stand. Father
Gascoigne's 7003 is `レベル3：聖堂B・神父` ("level 3: Cathedral B, the Father"), and Ludwig's
7481 is `レベル17：聖堂街D・ボス` ("level 17: Cathedral Ward D, boss"). The effect multiplies HP,
attack, defense and stamina damage. Gascoigne's 1,440 HP becomes 2,032, and Ludwig's 2,400
becomes 16,658. The sheets now give each enemy's effect and its HP with it. New Game+ multiplies
on top of this ([New Game+](#new-game)).

**2. The hunter's defense.** It is curve 102 of the six stats' sum
([defense](#defense)), and the starting origins show that sum is always the level plus 50.
Milquetoast is level 10 with stats adding to 60, and Waste of Skin level 4 with 54. So a hunter's
defense is 10 at level 10, 71 at 30, 129 at 50, 180 at 70 and 214 at 100. Armor adds nothing
here: every piece's flat defenses (`defensePhysics` and the others) are 0.

**3. Attack against defense**, the same formula an enemy's defense meets
([attack against defense](#attack-against-defense)).

**4. Armor.** Each piece holds damage multipliers, and the function that reads them
(`ArmorCut_Product`, `0197abc0`) multiplies them across the four pieces. It picks the physical
one by the attack's type: `slashDamageCutRate` for slash, `blowDamageCutRate` for blunt,
`thrustDamageCutRate` for thrust, and for type 4 `shotDamageCutRate`. That field's own
description explains it: "the multiplier on standard damage (the name says shot; internally it
is standard)". So `atkAttribute` 4 is **standard** damage. Arcane, fire and bolt take their own
rates. The player's character table reaches this function through slot 0x230
(`PlayerIns_GetArmorCutRates`, `01d02650`), next to the defense slot 0x220. The hit function
(`Hit_ApplyReceived`) passes the four results into `Damage_Compute`, which multiplies each
element's damage by them after the defense curve. The full Hunter set (hat, garb, gloves,
trousers) multiplies slash damage by 0.95 × 0.89 × 0.95 × 0.94 = 0.755.

Each boss's heaviest attack ([the heaviest blows](#the-heaviest-blows)), against a hunter in the
full Hunter set, with no buffs or runes. The row is the placed one:

| Boss (row) | Area level effect | Row's attack | × | Attack | Level 30 | Level 100 |
|---|---|---:|---:|---:|---:|---:|
| Celestial Emissary (257010) | 7016, level 16: Cathedral C, second half | 540 | 2.45 | 1,323 | 1,022 | 997 |
| Ludwig the Accursed (451000) | 7481, level 17: Cathedral Ward D, boss | 530 | 2.488 | 1,319 | 896 | 874 |
| Laurence, the First Vicar (450000) | 7484, level 20.5: Cathedral Ward D, hidden boss | 454 | 2.771 | 1,258 | 828 | 729 |
| Orphan of Kos (454000) | 7486, level 22: fishing village, boss | 390 | 2.839 | 1,107 | 737 | 623 |
| Gehrman, the First Hunter (805000) | 7021, level 21: the base, awakened old man | 390 | 2.788 | 1,087 | 840 | 787 |
| The One Reborn (507000) | 7015, level 15: the sacrifice town | 448 | 2.354 | 1,055 | 717 | 667 |
| Amygdala (512000) | 7029, level 11-2: home town, second half | 490 | 2.075 | 1,017 | 691 | 637 |
| Lady Maria (452000) | 7483, level 20: the Clocktower, boss | 360 | 2.754 | 991 | 674 | 618 |
| Ebrietas (251000) | 7017, level 17: Cathedral C, second half, hidden | 379 | 2.488 | 943 | 641 | 580 |
| Darkbeast Paarl (508000) | 7009, level 9: the sacrifice town, first visit | 460 | 2.003 | 921 | 616 | 472 |
| Rom, the Vacuous Spider (510000) | 7013, level 13: the university | 340 | 2.163 | 735 | 568 | 481 |
| Vicar Amelia (502000) | 7006, level 6-3: Cathedral A | 373 | 1.624 | 606 | 412 | 331 |
| Cleric Beast (500241) | 7002, level 2: Cathedral B, Cleric Beast | 379 | 1.037 | 393 | 255 | 190 |
| Father Gascoigne, beast (272000) | 7003, level 3: Cathedral B, the Father | 333 | 1.119 | 373 | 239 | 175 |

The area level decides the order. The early bosses barely scale: the Cleric Beast's 2 and
Gascoigne's 3 add 4% and 12%. The late ones more than double, and the Celestial Emissary's
magic, at ×2.45, hits hardest of all. (An earlier version of this table used unplaced rows: the
Emissary's 257000, which has no level effect, and the Cleric Beast's 500000, which has a later
area's level.) Levels help less than they seem to: from level 30 to
100, defense triples (71 to 214), but a 1,000-point attack is far above both. In the damage
formula's terms, attack is many times defense either way, where every point of defense removes
little. Against Ludwig, the 70 levels save 22 health.

The attacker's rates multiply across effects ([how an attacker's effects
combine](#how-an-attackers-effects-combine)). The numbers leave out a few things:
* the third set of rates the hit function passes (character slot 0x238);
* nothing for the hunter's own effects (runes, buffs, and the 1.4, 1.125 and 0.8 multipliers
  of [attack against defense](#attack-against-defense)).

They are the hit's damage before those.

## How an enemy chooses

```
python -I tools/ai.py tables work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/combat
python -I tools/ai.py enemy  work/base/dvdroot_ps4 work/update/dvdroot_ps4 Cleric Beast
```

`tables` writes `work/combat/ai.md`, one line per enemy, and a sheet for each of the 137
enemies with a battle script in `work/combat/ai/`. Each sheet shows the chance of every action
by distance (113 enemies) and health (27), the attacks each action makes, the timers and state
numbers each one sets, and what changes when the state does.

### The choosing function

The enemy movesets above list, for each attack, the battle-script actions (`…_Act01`,
`…_Act02`) that can make it. One function picks between those actions: `<name>Battle_Activate`.
It fills a table of **weights**, one per action, and hands it to `Common_Battle_Activate` in
`aicommon.luabnd` (`common_battle_func.lua`). That common function adds the weights up, rolls
`GetRandam_Int(1, total)` and walks the table. **Each action's chance is its weight divided by
the sum.**

What decides the weights is whatever `Activate` asks the game:
* the distance to the player, `GetDist(TARGET_ENE_0)`;
* its own health, `GetHpRate(TARGET_SELF)`;
* where the player is, `IsInsideTarget(TARGET_ENE_0, AI_DIR_TYPE_B, 140)` meaning "in the 140°
  behind me";
* state it keeps: `GetNumber`/`SetNumber`, timers (`IsFinishTimer`), and variables of the
  script itself that persist between calls;
* special effects it carries (`HasSpecialEffectId`), requests from the map's event script
  (`GetEventRequest`), and its own random rolls.

### Reading it

The scripts are compiled Lua 5.0 with no source. `tools/ai.py` runs `Activate` on the bytecode
for one state of the world at a time. Everything the game would answer is fixed by that state,
and only the script's own random rolls fork the run, each branch weighted by its roll's range.
The tool finds the distance and health bands from the thresholds the runs compare against,
running again until no new ones appear. Then it fills each band's table and tries each other
answer the script tests for, to show what it changes.

The default state is the fight in progress. Timers have finished, state numbers are 0, no
special effects are active, no event request is pending, and the player is in front (inside the
"front" cones, outside the "back" and side ones). The enemy's own placement
(`GetNpcThinkParamID`) is its row's `aiThinkId`. Each assumption is stated on every sheet. A
few results the tool had to be taught, each found by checking a sheet against its bytecode:
* **Calls into the script's own functions.** Father Gascoigne's `Activate` hands over to
  `Gascoyne271000Battle`.
* **Script variables** that keep state between calls (`GETUPVAL`/`SETUPVAL`). They start at the
  value the script gives them, which the tool reads from the chunk's own code.
* **Opening moves.** When every band gives one action, a state number usually holds the enemy
  to its opener. The tool then shows the table for the value that opens the choice up, and
  names the action that sets it.

### The Cleric Beast

| Distance | Act01 | Act02 | Act03 | Act04 | Act05 | Act07 | Act08 |
|---|---:|---:|---:|---:|---:|---:|---:|
| < 3 | 20% | 30% | | | | 30% | 20% |
| 3–6 | 20% | 20% | 20% | | 10% | 20% | 10% |
| 6–10 | 36% | 9% | 27% | 9% | 18% | | |
| 10–15 | 25% | | 38% | 25% | 12% | | |
| ≥ 15 | 25% | | 25% | 38% | 12% | | |

Act01 is the hook combination (`右フック`, `左フック`, "right hook", "left hook"). Act03 is the slam
(`叩きつけ`), Act04 the stomp (`踏みつけ`) and Act05 the left-right rush. Act07 and Act08 are the
close punch and the close grab (`近接投げ`). Up close it grabs and punches; at range it stomps and
slams. Hand-checking the bytecode at 15 m or more: the weights are 10, 10, 15 and 5, so
25%, 25%, 37.5% and 12.5%.

Its state changes the picture:
* **Cooldowns.** Act04 starts timer 0 and Act05 timer 1, both for 10 seconds. While a timer
  runs, its action's weight is 0, so the beast doesn't stomp or rush twice in a row.
* **The player behind it** (`AI_DIR_TYPE_B`, 140°). On a roll of 70 or less it turns with
  Act20, so 70% in every band.
* **A broken part.** Effects 480–484 are `ボス 部位損傷 その1`–`5` ("boss part damage 1–5").
  Any of 480, 483 or 484 sets state number 3, which brings in Act12–Act15: the moving lariat
  (`移動ラリアット`), the pole-vault leaps (`棒高跳び`) and a back-step attack. At range the lariat
  and the leaps then take most of the weight.
* **The event request** (`GetEventRequest(1)`, from the map's event script) skips the table.
  The beast queues 3010 and 3016 directly (with a back step, 701, when within 5 m) and sets
  numbers 1, 2 and 3. From then on, the hooks (Act01) and the slam (Act03) weigh half as much.
  Act06 (the super slam, `超叩きつけ`), Act09, Act10, Act16 and Act17 (the left-arm rush) join in.

### Other bosses

* **Ludwig the Accursed** opens with Act04, the jump slam (`ジャンプ叩きつけ`), every time.
  While state number 1 is 0, his `Activate` offers nothing else, and Act04 sets it to 1. From
  then on his options grow as his health falls. Above 90% health and within 1.5 m, it's only
  Act13 (`顔面でぶん殴る`, "smash with the face"). Below 75%, Act16 (`大ジャンプで消える`, "vanishes
  with a big jump", on a 45-second timer) takes 40–67% of the choices within 6 m. At 8–10 m and above 85% health, the big jump stomp (Act07,
  `大ジャンプ踏みつけ`) takes 57%. Below 85%, it shares the range with the two long-range
  breaths (`遠ブレス`, Act11 and Act12), a quarter each.
* **Father Gascoigne** picks from eight actions by distance while his health is at 67% or
  more. Below that he picks Act28 every time. Act28 plays animation 3028, which has no hit in
  it. His transformation is something else: the event script requests it below 34% health
  ([what the map scripts ask](#what-the-map-scripts-ask)), and it is Act29, animation 3030. An
  earlier version of this page guessed that Act28 was the transformation.
* **Rom, the Vacuous Spider** chooses little herself. Her `Activate` picks Act11, which makes
  no attack, until the map's event script sends a request (100, 101, 110 or 111). Each request
  names the actions to choose from.
* **Six enemies open with a fixed move**: Ludwig, Laurence, the Maneater Boar, and three others.

### Inside an action

Each action is a function too: it measures, rolls and queues animations with `AddSubGoal`. The
same run, applied to the 2,741 action functions, gives each one's sequences by distance band
with their chances. The sheets put them under "Inside the actions". Four actions loop and
aren't followed.

Two things made this work:
* **Calls to the common approach routine** (`Approach_Act`, from `aicommon`). They become an
  "approach" step.
* **The script's own distances.** The action functions compare the distance with values the
  chunk works out when it loads, such as reach minus body: the Cleric Beast's script begins
  `4.2 − 1.1`, `6.2 − 1.1` and so on, 48 of them. The tool evaluates that arithmetic, so a
  comparison like `GetDist(TARGET_ENE_0) <= variable1` gets its number (3.1 m).

The Cleric Beast's Act01, the hook combination:

| Distance | Queues |
|---|---|
| < 3.1 | 701 → 3000 `右フック` (right hook) → 3001 `左フック` (left hook) |
| ≥ 3.1 | approach → 3000 → 3001 |

The variations come from the state. After the event request sets number 2, the hooks end in
a finisher: 3011 (`追加拳骨`, "extra fist") up close, 3017 (`近接掌底`, "close palm strike")
further off. Half the time both hooks come first, and half the time only the right hook. Once
number 1 is set, it skips the approach.

Father Gascoigne's Act01 is his axe combination. After the approach it ends one of three ways:
35% two axe blows and a gunshot (`斧コンボ（１撃目）`, `２撃目`, `銃で撃つ`), 35% three axe blows,
and 30% three axe blows and the gunshot.

### What the map scripts ask

Some choices come from outside the AI. The map's event script sends a request with
`[2004:17]` `AI命令要求` ("AI command request": an entity, a command and a slot), and the battle
script reads it with `GetEventRequest(slot)` until a request of −1 clears it. The tool finds
every request in every running copy of every event, fills in the template arguments as the
flag index does, and joins each request to the enemy whose placement has that entity ID. Each
AI sheet's "What the map scripts request" lists the event, its working name, the path through
the event to the request, and what the command does to its choices, averaged over the sheet's
distance and health bands.

**Reading the path.** An event's conditions work in groups. Each condition instruction (class
3 and 4) registers into a numbered group. A negative group holds when any of its conditions
does, and a positive group when all do. `[0:00]` puts one group inside another. Registering
into group 0 makes the event wait there and then. After that wait, the groups start empty
again, but the event can still ask how a group finished (`[1000:07]`, `[1000:08]`,
`[1000:107]`). Skips, ends and label jumps test those groups, a flag, the multiplayer state or
two numbers, and that is how an event branches.

The tool walks every path through each copy of each event. A wait becomes "wait for" with its
groups written out as and/or. A branch becomes "if" or "unless", and a request reached on the
path is reported with everything before it. A jump back to an earlier label is a loop, which
isn't followed. A flag that is an event's own (its ID) is shown with that event's working
name. A flag tested relative to the event (`基準イベントフラグIDタイプ` 1, "base event-flag ID
type") reads as the event's own flag: the event is running for the first time or not. 55 AI
sheets have requests, on 2,079 paths.

* **The Cleric Beast's heat-up** (`ボスヒートアップ_教区長`, "boss heat-up: Cleric Beast"). Unless
  flag 12411700 (`ボス撃破_教区長`, "boss defeated: Cleric Beast") is on, it waits for the
  beast's health to drop below 70%, then sends command 1 on slot 1. That is the forced combo
  (3010 → 3016, the left-arm rush) that sets numbers 1–3 and makes it fight harder. When its
  AI answers with event message 100, the event clears the request. Each part break
  (`ボス部位損傷_教区長_XX`) waits for (the part's HP at 0 **or** the beast's health at 0). Unless
  it was the beast's death that ended the wait, the event sends command 1 on slot 0 thirty
  seconds later, which picks Act27.
* **Rom's two stages** (`ヒートアップ_白痴の蜘蛛`, "heat-up: the idiot spider"). Command 100 at
  75% health and 110 at 50%, which her AI answers with Act10. After it reports back (message
  10) and 3 seconds pass, 101 or 111 opens her attacks: half Act02 and half Act11 after the
  first, a spread of four after the second.
* **Father Gascoigne.** `ボス変身_ガスコイン神父` ("boss transformation: Father Gascoigne")
  sends command 40, his Act29, below 34% health. `ガスコイン（ボス部屋）オルゴールで特殊アニメ再生`
  ("Gascoigne (boss room): play a special animation with the music box") sends command 60, his
  Act27, while he has the music box's effect (4640, `m24_01聖堂街B_オルゴール`) and 34% health or
  more. Below 34%, the same event sends 40.
* **Ludwig's four heat-ups** (`ルドウイーク_ヒートアップ0`–`3`) send commands 100–103 at falling
  health. Each one tips his choices toward one move, averaged over his bands: 100 the biting
  advance (Act10, 4% to 16%), 101 the left-arm breath (Act12, 11% to 33%), 102 the strong charge
  (`突撃（強）`, Act15, 8% to 79%) and 103 the vanishing leap (Act16, 9% to 50%).
  `ルドウイーク_人モード崩れ` ("Ludwig: human mode collapses") sends 110 below 35%.
  Each heat-up waits for (its health threshold **and** his AI's event message 10 **and** the
  previous heat-up's flag) **or** a message of its own (20, 30, 40, 50). It sends its command
  only if the threshold, not that message, ended the wait. So the heat-ups go in order, and
  only when his AI says it is ready. Each has two sets of thresholds, chosen by flag 13400999:
  90, 80, 70 and 50% when it is on, 90, 85, 80 and 75% when off. Nothing sets that flag. The one other event that tests
  it is `TGS版と通常版の切り替え`, "switching between the TGS version and the normal version":
  the Tokyo Game Show demo. The game as sold uses the second set, and so does Ludwig's own
  `Activate`, which tests the same flag.

### What isn't followed

* **Combinations of states.** Each state number or flag is varied on its own from the default,
  not in combination (number 1 and number 2 together, say).
* **Four looping actions.** Their loops aren't followed.
* **Event loops.** A path that jumps back to an earlier label stops there. What a looping
  event does on later rounds isn't listed.
* **Zero weights.** Some placements end with every weight 0 in the default state (the first
  row of the fallen hunters, the Executioner beyond 20 m). What `Common_Battle_Activate` does
  then isn't read.

## Status buildup

```
python -I tools/status.py tables work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/combat
python -I tools/status.py resist work/base/dvdroot_ps4 work/update/dvdroot_ps4 40 30 "Hunter Hat" "Hunter Garb" "Hunter Gloves" "Hunter Trousers"
```

`tables` writes `work/combat/status.md`. It holds the resistance curves, every effect that
builds a condition up, what a hunter can inflict, and each enemy's resistances with what its
attacks build up. The enemy sheets now note a hit's buildup too.

### Old names, new conditions

The data keeps Dark Souls' names for the conditions, and Bloodborne shows them under new ones.
Three things tie them together:
* **The menu's own rows.** `MenuPropertySpecParam` 410–413 are labelled `【防具】毒耐性` ("[armor]
  poison resistance"), `出血耐性` ("bleeding resistance"), `発狂耐性` ("frenzy resistance") and
  `獣化耐性` ("beasthood resistance"). Their captions are "Slow Poison RES", "Rapid Poison RES",
  "Frenzy RES" and "Beasthood".
* **The blood gems' text.** All 472 gem effects that add `registBlood` ("bleeding") read "Add
  rapid poison effect", and the 468 that add `poizonAttackPower` read "Add slow poison effect".
* **The level-sync curves.** Curve 122 is `遅効毒耐性値` ("slow-acting poison resistance") and
  123 `劇毒耐性値` ("virulent poison resistance"). They begin at 30 and 40 at stat 10, as curve 110
  `毒` ("poison") and curve 112 `出血` ("bleeding") do.

| In the data | Buildup field | Armor field | Bloodborne |
|---|---|---|---|
| 毒, poison | `poizonAttackPower` | `resistPoison` | Slow Poison |
| 出血, bleeding | `registBlood` | `resistBlood` | Rapid Poison |
| 呪, curse (`呪（発狂）`, "curse (frenzy)", in the curve's name) | `registCurse` | `resistCurse` | Frenzy |
| 獣化, beasthood | `registTherianthrope` | `resistTherianthrope` | Beasthood |
| 疫病 or 猛毒, plague or deadly poison | `registIllness` | `resistDisease` (always 0) | none |

The fifth slot is half there. Effects build it up (`猛毒ダメージ`, "deadly poison damage", up to
1,280 points per tick) and enemies have `resist_desease` values. But it has no menu row, no
armor gives it, and no code asks for its curve (111). The player can't resist it. Many of its
sources are Dark Souls leftovers, such as the Bullet table's `ドラゴンゾンビ　アッシドブレス` (the
Dragon Zombie's acid breath) and `白竜シース` (White Dragon Seath's breath).

### How a condition builds up

A hit applies special effects: an enemy attack row's `spEffectId0`–`4` and a bullet's own, a
blood gem's on-hit effect (`speffectId_forAtk`), or a weapon's `spEffectBehaviorId`. An effect
that builds up holds an amount in its field. The field's description reads "the value added to
the target's [poison resistance value] when it hits". When the gauge reaches the target's
resistance, the condition sets in. The same effect then says what it does: a share of max HP
(`changeHpRate`) and a number of points (`changeHpPoint`), each tick (`motionInterval`) for a
duration (`effectEndurance`). The fields' descriptions say "per second", but frenzy's
70% applied over 0.03 seconds only makes sense as one tick, so the amounts are per tick.

| Condition | A typical effect | Buildup | What it does |
|---|---|---:|---|
| Slow Poison | 5250 `毒ダメージ１` ("poison damage 1") | 40 | 12 points + 0.1% of max HP every second for 40 seconds |
| Rapid Poison | 5271 `出血ダメージ２` ("bleeding damage 2") | 35 | 250 points + 3% of max HP, in a 2-second effect with a 3-second tick |
| Rapid Poison | 6401 `出血_2` ("bleeding 2") | 33 | 30% of max HP, the same way |
| Frenzy | 5230 `発狂ダメージ１　本体` ("frenzy damage 1, main body") | — | 70% of max HP, at once |
| Beasthood | 6650 `獣化_小` ("beasthood, small") | 9 | no damage; carried by the bullets of a beasthood roar (`獣化_L2咆哮`, "beasthood: L2 roar") |

Slow poison wears you down. Rapid poison and frenzy strike once, for a large share of max HP.

### A hunter's resistance

Four functions compute it (`01e97e90`, `01e97fd0`, `01e98110` and `01e98250` in the 1.09
executable), one per condition. Each one adds three things:
* the four armor pieces' values, each times its `ReinforceParamProtector` rate. Every piece
  uses reinforce type 0, whose rates are 1.0, so in practice the values just add.
* a flat amount kept on the character, presumably from effects such as runes;
* a stat curve: **110 of Endurance** for Slow Poison and **112 of Endurance** for Rapid Poison.
  Frenzy and Beasthood use **113** and **114** of a value at `+0x124` of the stat block.

The stat block's layout comes from the functions beside these. Max HP reads Vigor at `+0x108`,
and defense sums `+0x108` to `+0x11c`, the six stats in the menu's order. So `+0x10c` is
Endurance. `+0x124` lies past the six stats, and its curves start at 0 rather than 1 and fall
as it rises. That is very likely **Insight**, though no code here was found naming it.

| Stat | Slow Poison | Rapid Poison | Frenzy | Beasthood |
|---:|---:|---:|---:|---:|
| 0 | | | 100 | 300 |
| 10 | 30 | 40 | 100 | 213.3 |
| 20 | 73.3 | 73.3 | 86.7 | 146.7 |
| 30 | 100 | 100 | 60 | 100 |
| 50 | 114.5 | 114.5 | 33.3 | 40 |
| 99 | 150 | 150 | 10 | 5 |

Insight costs resistance quickly. Frenzy resistance from Insight falls from 100 to 60 between
15 and 30 Insight. The Hunter set adds 119 frenzy resistance, more than the curve gives past
Insight 20. A hunter with Endurance 40 and Insight 30 in the full Hunter set resists slow poison
to 148, rapid poison to 187, frenzy to 179 and beasthood to 212.

### Who inflicts what

* **A hunter.** Poison Throwing Knives (`毒投げナイフ`) add 70 slow poison. Blood gems add up to 30
  slow poison or 22 rapid poison per hit. Of the weapons, only the Chikage carries a buildup
  effect, 30 rapid poison, in an effect labelled `刀メニュー表示用ダミー出血効果` ("dummy bleeding
  effect for the katana's menu display").
* **Enemies.** 227 enemy attack rows build up slow poison, 120 rapid poison and 13 the unused
  deadly poison. Only 3 build up frenzy, all `発狂（確定）` ("frenzy (certain)", 99,999), the
  throw of the large Amygdala-type enemy (`c5140_邪神（大）_ゆっくり投げ`, "evil god (large): slow
  throw"). Other frenzy sources aren't attacks. Effects such as `発狂ダメージ１　耐性削り`
  ("frenzy damage 1: resistance shaving") add a little every interval; what applies them isn't
  followed here.
* **Enemy resistance.** Most bosses have 999 in slow poison, rapid poison and frenzy. Among
  the ones that don't (by each one's placed row):

  | Boss | Slow Poison | Rapid Poison |
  |---|---:|---:|
  | Loran Darkbeast | 70 | 999 |
  | Blood-starved Beast | 999 | 180 |
  | The Witch of Hemwick | 200 | 180 |
  | Living Failures | 250 | 180 |
  | Celestial Emissary | 300 | 180 |

  Common enemies mostly resist slow poison at 120 (680 of the 1,291 story rows) and rapid
  poison at 180 (873). 5,723 of all 6,064 NpcParam rows have frenzy 999, so frenzy is almost
  only the player's problem.

## What this doesn't cover yet

* **The hit's flags and multipliers.** What the 1.4, 1.125 and 0.8 multipliers in
  [attack against defense](#attack-against-defense) stand for, the weakness factors' values,
  and the effect multipliers. Also how a landed hit's regain amount reaches the grace HP
  ([rally](#how-rally-wins-health-back)).
* **Which calculation a real hit uses.** Attack rating's mode 4, physical scaled by Bloodtinge,
  is traced only as far as the five functions that call it.
* **The moves no animation plays.** Rows like the Saw Cleaver's 100–130 and 900–950 aren't
  invoked by any player animation. Whether the action script or the throws start them isn't
  followed, nor are the fallback sets `WepMotion_AnimOffset` tries before the weapon's own,
  the shot of guns whose sets have no behavior event, or the unused sets a155 and a156
  ([which button plays which move](#which-button-plays-which-move)).
* **Throws and visceral attacks.** `ThrowParam` (221 rows) links an attacker's and a victim's
  animations, but its damage path isn't followed here.
* **Looping events.** The odds of each action, the sequences inside each, and the paths by
  which the map scripts request them are read ([how an enemy chooses](#how-an-enemy-chooses)).
  Events that loop back are followed only up to the loop.
* **The rest of a hit on the player.** The third set of rates the hit function passes (slot
  0x238), the hunter's own effects, and what holds the Moon Presence's gaze at 1 HP
  ([what a hit costs a hunter](#what-a-hit-costs-a-hunter)).
* **The rest of status.** How a gauge drains between hits, whether a resistance of 999 is
  immunity or only a number no hit reaches, and what applies the frenzy "resistance shaving"
  effects ([status buildup](#status-buildup)).
* **Elemental conversion, and the flat-bonus factors.** What a converted weapon's attack
  rating does, and the four factors that scale a positive flat attack bonus, aren't read yet
  ([how an attacker's effects combine](#how-an-attackers-effects-combine)).

## Files

| Path | |
|---|---|
| `tools/combat.py` | the weapon, move and curve tables; `ar` and `levels` compute with the formulas |
| `tools/tae.py` | each weapon's animation set, and the animations and state names behind each judge id |
| `tools/ai.py` | runs each battle script's Activate function and its actions: the chance of each action, and of each sequence an action queues, by distance, health and state |
| `work/combat/ai.md`, `ai/` | one line per enemy; each enemy's choices |
| `tools/status.py` | the resistance curves, the condition effects, who inflicts what, enemy resistances; `resist` for a hunter |
| `work/combat/status.md` | those tables |
| `tools/enemies.py` | every enemy's attacks: animation, time, judge id, labelled attack row, AI actions, area level; `hit` applies one to a hunter |
| `work/combat/enemies.md`, `enemies/` | one line per enemy; each enemy's attacks |
| `tools/effects.py` | the gem and New Game+ tables, and any special effect's changes |
| `tools/names/eboot-1.09.hand.tsv` | the formula functions' names and evidence |
| `work/combat/gems.md`, `ngplus.md` | every blood gem effect with its text and numbers; the New Game+ effects and who uses them |
| `work/combat/weapons.md`, `moves/`, `curves.md` | every weapon at +0 and its last level, each weapon's moves with their names, the stat curves |
