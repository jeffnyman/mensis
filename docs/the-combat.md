# Combat: the numbers behind a hit

What a weapon does when it lands: its damage, how that grows with stats and upgrades, each
move's share of it, and what a hit does to stamina, poise and rally. This page reads the
**data** first: the gameplay tables and what their own descriptions say about them. Then
[the formulas](#the-formulas) that combine those numbers, read from the executable: the stat
curve, attack rating, the requirement penalty, defense from stats, the cost of a level, max
HP, how rally wins health back, and how a hit's attack meets the target's defense. Which button
plays each move comes from the [animations](#which-button-plays-which-move). And last, [the
effects](#effects-blood-gems-buffs-and-new-game) behind blood gems, buffs and New Game+.

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
isn't followed here. Two sets, a155 and a156, have attack events, but no weapon's category
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
of that sum, rounded, is each damage type's base defense, before the four armor pieces are added
and effects multiply it:

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

`tables` writes `work/combat/gems.md` (every gem effect, by category and rank) and `ngplus.md`
(the New Game+ effects). A field counts as **changed** when it differs from the value most rows
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
The attacker's own attack-rate gathering isn't read yet, so attack rates are only likely to
stack the same way.

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
* **The attacker's effect gathering, and elemental conversion.** How an attacker's own
  attack rates combine, and what a converted weapon's attack rating does, aren't read yet
  ([effects](#effects-blood-gems-buffs-and-new-game)).

## Files

| Path | |
|---|---|
| `tools/combat.py` | the weapon, move and curve tables; `ar` and `levels` compute with the formulas |
| `tools/tae.py` | each weapon's animation set, and the animations and state names behind each judge id |
| `tools/effects.py` | the gem and New Game+ tables, and any special effect's changes |
| `tools/names/eboot-1.09.hand.tsv` | the formula functions' names and evidence |
| `work/combat/gems.md`, `ngplus.md` | every blood gem effect with its text and numbers; the New Game+ effects and who uses them |
| `work/combat/weapons.md`, `moves/`, `curves.md` | every weapon at +0 and its last level, each weapon's moves with their names, the stat curves |
