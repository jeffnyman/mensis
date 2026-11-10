# Combat: the numbers behind a hit

What a weapon does when it lands: its damage, how that grows with stats and upgrades, each
move's share of it, and what a hit does to stamina, poise and rally. This page reads the
**data** first: the gameplay tables and what their own descriptions say about them. Then
[the formulas](#the-formulas) that combine those numbers, read from the executable: the stat
curve, attack rating, the requirement penalty, defense from stats, and the cost of a level.

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
the hardware doesn't use it directly. The animation events name the moves, so which button
plays which judge id isn't in these tables. Each row gives the move's **stamina** cost and its
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
moves at 300 and 700, 340 and 740). For a trick weapon these are very likely its two forms,
but the tables don't name them.

A few moves stand out:

* **300 and 700** do no damage, but 300% stamina damage.
* **310, 340, 710 and 740** carry a `throwTypeId`, "the ID that links to the throw
  parameters", so they lead into a throw (`ThrowParam`).
* **`dmgLevel`** is "which damage motion the enemy plays when hit": 1 for the light chain, 2 for
  heavier blows, 5 and 6 for the special moves.

### Poise

A player's attack deals poise damage of `saWeaponDamage` × `atkSuperArmorCorrection`%. The flat
`atkSuperArmor` field is "NPC only". The weapon bases are small and few: Bare Fists and the Blade
of Mercy 5, the Saw Cleaver and Chikage 15, the Burial Blade 20, the Hunter Axe 30, the
Kirkhammer 37. In every move the Saw Cleaver makes, the poise multiplier equals the damage one.

### Rally

Rally is **リゲイン, "regain"**, in the tables. A hit's regain comes from three fields:

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

with exponents 1, −1.1, −1.2 and 1 between the points. How defense then reduces a hit's damage is
in the hit code, not followed here.

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

## What this doesn't cover yet

* **The hit itself.** How a hit's attack meets the target's defense, and what damage type
  (slash, blunt, thrust) changes, is in the hit code. So is how the regain amount, timer and
  rate combine into rally. Neither is followed yet.
* **Which calculation a real hit uses.** Attack rating's mode 4, physical scaled by Bloodtinge,
  is traced only as far as the five functions that call it.
* **Move names.** Which button plays a judge id is in the animation events (TAE), which aren't
  read yet.
* **Throws and visceral attacks.** `ThrowParam` (221 rows) links an attacker's and a victim's
  animations, but its damage path isn't followed here.
* **Effects.** Blood gems, runes and buffs work through `SpEffectParam`, which isn't covered
  here.

## Files

| Path | |
|---|---|
| `tools/combat.py` | the weapon, move and curve tables; `ar` and `levels` compute with the formulas |
| `tools/names/eboot-1.09.hand.tsv` | the formula functions' names and evidence |
| `work/combat/weapons.md`, `moves/`, `curves.md` | every weapon at +0 and its last level, each weapon's moves, the stat curves |
