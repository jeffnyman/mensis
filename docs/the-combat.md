# Combat: the numbers behind a hit

What a weapon does when it lands: its damage, how that grows with stats and upgrades, each
move's share of it, and what a hit does to stamina, poise and rally. This page reads the
**data**: the gameplay tables and what their own descriptions say about them. How the
game combines these numbers (the scaling curve's exact shape, defense, the rally timer) is
code in the executable, and is the next step.

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
carry an Arcane scaling with no arcane base damage (the Saw Cleaver's is 33 at +0). By the
usual reading of these games, Arcane scales the elemental damage a weapon gains from
elsewhere. That reading belongs to the formula step.

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
50 they barely do. The curve rows hold an exponent for each stage too, all 1 for curve 0, so
each stage is a straight line as far as the data says. Rows 100 onward are other curves, some with
negative exponents. Which stat each is for, and exactly how an exponent bends a stage, is
code.

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

## What this doesn't cover yet

* **The formulas.** How a stat, its curve and a scaling become attack rating, how defense and
  damage type reduce a hit, and how the regain amount, timer and rate combine, are all code.
  They are the next step, in the executable.
* **Move names.** Which button plays a judge id is in the animation events (TAE), which aren't
  read yet.
* **Throws and visceral attacks.** `ThrowParam` (221 rows) links an attacker's and a victim's
  animations, but its damage path isn't followed here.
* **Effects.** Blood gems, runes and buffs work through `SpEffectParam`, which isn't covered
  here.

## Files

| Path | |
|---|---|
| `tools/combat.py` | the weapon, move and curve tables |
| `work/combat/weapons.md`, `moves/`, `curves.md` | every weapon at +0 and its last level, each weapon's moves, the stat curves |
