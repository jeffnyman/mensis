# SPDX-License-Identifier: GPL-3.0-or-later
"""The combat data: every weapon's damage, scaling and moves, from the gameplay tables.

    python -I tools/combat.py weapons <base dvdroot> <update dvdroot> <out dir>
    python -I tools/combat.py weapon  <base dvdroot> <update dvdroot> <name>

`weapons` writes weapons.md (every weapon at +0 and its last level), moves/<weapon>.md (every
move of each) and curves.md (the stat curves). `weapon` prints one weapon's tables.

What the tables hold (field meanings from the PARAMDEFs' own descriptions, see
docs/the-combat.md):

* EquipParamWeapon: one row per weapon per upgrade level (Saw Cleaver +0..+10 = 7000000..
  7001000 in steps of 100; ReinforceParamWeapon's six rows are all 1.0). Base damage
  attackBasePhysics / Magic / Fire / Thunder is physical / arcane / fire / bolt. Scaling
  correctStrength / Agility / Magic / Faith is Strength / Skill / Bloodtinge / Arcane (the menu's
  own order: 筋力, 技量, 射撃 "shooting", 魔法 "magic"). correctType picks the stat curve
  (CalcCorrectGraph). saWeaponDamage is the poise ("super armor") base, wepRegainHp the rally
  ("regain") base.
* BehaviorParam_PC: a weapon's moves, id 100000000 + variation * 1000 + judge id. The judge id
  is the one the animation events (TAE) use, so which button plays a move is not in the tables.
  refType 0 points at an AtkParam_Pc row, refType 1 at a Bullet row (whose atkId_Bullet is the
  attack). stamina is the move's stamina cost.
* AtkParam_Pc: per move, multipliers on the weapon's values, in percent: atkPhysCorrection etc.
  (motion values), atkStamCorrection (stamina damage), atkSuperArmorCorrection (poise damage),
  wepRegainHpScale (rally); plus a flat atkRegainHp, charge and throw flags and the hit
  reaction (dmgLevel).

The menu's letter grades for scaling come from MenuValueTableParam rows 100-106 (format 100 of
the scaling properties): S at 110, A 85, B 65, C 55, D 30, E above 0.
"""

from __future__ import annotations

import io
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import items as itemlib

STATS = (
    ("correctStrength", "properStrength", "Str"),
    ("correctAgility", "properAgility", "Skl"),
    ("correctMagic", "properMagic", "Blt"),
    ("correctFaith", "properFaith", "Arc"),
)
DAMAGE = (
    ("attackBasePhysics", "atkPhysCorrection", "phys"),
    ("attackBaseMagic", "atkMagCorrection", "arcane"),
    ("attackBaseFire", "atkFireCorrection", "fire"),
    ("attackBaseThunder", "atkThunCorrection", "bolt"),
)


def grades(w: itemlib.World) -> list[tuple[float, str]]:
    """The scaling grade thresholds, highest first: [(at least, letter)]."""
    import souls

    letters = {}
    for b in souls.bnd4_read(souls.load(w.path("msg/engus/menu.msgbnd.dcx"))):
        if "SP_メニューテキスト" in b.name:
            letters = souls.fmg_read(b.data)
    rows = w.rows("MenuValueTableParam")
    out = [
        (rows[k]["value"], letters.get(rows[k]["textId"], "?"))
        for k in range(100, 107)
        if k in rows
    ]
    return sorted(out, reverse=True)


def grade(value: float, table: list[tuple[float, str]]) -> str:
    if value <= 0:
        return "–"
    return next((g for v, g in table if value >= v > 0), "?")


class Weapons:
    def __init__(self, w: itemlib.World):
        self.w = w
        self.rows = w.rows("EquipParamWeapon")
        self.names = w.names.get("EquipParamWeapon", {})
        self.behaviors = w.rows("BehaviorParam_PC")
        self.attacks = w.rows("AtkParam_Pc")
        self.bullets = w.rows("Bullet")
        self.table = grades(w)

    def families(self) -> dict[int, list[int]]:
        """{+0 row id: [row ids by level]} for every named weapon."""
        fam: dict[int, list[int]] = {}
        for rid in sorted(self.rows):
            name = (self.names.get(rid) or "").strip()
            if not name or name == "NOT NEEDED":
                continue
            fam.setdefault(rid - rid % 10000, []).append(rid)
        return {k: v for k, v in fam.items() if (self.names.get(k) or "").strip()}

    def name(self, rid: int) -> str:
        return re.sub(r" \+\d+$", "", (self.names.get(rid) or str(rid)).strip())

    def moves(self, variation: int) -> list[dict]:
        out = []
        for bid, b in sorted(self.behaviors.items()):
            if b["variationId"] != variation:
                continue
            atk_id, via = b["refId"], ""
            if b["refType"] == 1:
                bullet = self.bullets.get(b["refId"])
                atk_id = bullet["atkId_Bullet"] if bullet else -1
                via = f"bullet {b['refId']}"
            a = self.attacks.get(atk_id) if b["refType"] in (0, 1) else None
            out.append(
                {
                    "judge": b["behaviorJudgeId"],
                    "stamina": b["stamina"],
                    "atk": atk_id,
                    "via": via,
                    "a": a,
                }
            )
        return out


def _pct(v) -> str:
    return "" if v is None else f"{v}"


def weapon_tables(ws: Weapons, base_id: int, levels: list[int]) -> list[str]:
    r0 = ws.rows[levels[0]]
    name = ws.name(base_id)
    out = [f"# {name}\n"]
    req = ", ".join(f"{s} {r0[p]}" for _, p, s in STATS if r0[p])
    out.append(
        f"Requirements: {req or 'none'}. Stat curve {r0['correctType']}. "
        f"Moves: behavior variation {r0['behaviorVariationId']}.\n"
    )
    out.append(
        "| Level | "
        + " | ".join(d for _, _, d in DAMAGE)
        + " | "
        + " | ".join(s for _, _, s in STATS)
        + " | poise | rally | stamina dmg |"
    )
    out.append("|---" * (1 + len(DAMAGE) + len(STATS) + 3) + "|")
    for lvl, rid in enumerate(levels):
        r = ws.rows[rid]
        dmg = " | ".join(str(r[b]) if r[b] else "" for b, _, _ in DAMAGE)
        sc = " | ".join(
            f"{r[c]:g} {grade(r[c], ws.table)}" if r[c] else "" for c, _, _ in STATS
        )
        out.append(
            f"| +{lvl} | {dmg} | {sc} | {r['saWeaponDamage']} | "
            f"{r['wepRegainHp']} | {r['attackBaseStamina']} |"
        )
    out.append("")
    moves = ws.moves(r0["behaviorVariationId"])
    out.append("## Moves\n")
    out.append(
        "Percentages of the weapon's values. Judge ids are the animation events'; "
        "a trick weapon's two sets of moves are 400 apart.\n"
    )
    out.append(
        "| Judge | Stamina | Damage % | Stamina dmg % | Poise % | Rally % | "
        "Flat rally | Hit level | Notes |"
    )
    out.append("|---:|---:|---:|---:|---:|---:|---:|---:|---|")
    for m in moves:
        a = m["a"]
        if a is None:
            out.append(f"| {m['judge']} | {m['stamina']} | | | | | | | no attack row |")
            continue
        parts = {d: a[c] for _, c, d in DAMAGE}
        dmg = (
            str(parts["phys"])
            if len(set(parts.values())) == 1
            else ", ".join(f"{d} {v}" for d, v in parts.items())
        )
        notes = [
            x
            for x in (
                "charged" if a["isChargeAtk"] else "",
                f"throw type {a['throwTypeId']}" if a["throwTypeId"] else "",
                m["via"],
                f"rally slot {a['regainableSlotId']}"
                if a["regainableSlotId"] >= 0
                else "",
            )
            if x
        ]
        out.append(
            f"| {m['judge']} | {m['stamina']} | {dmg} | {a['atkStamCorrection']} | "
            f"{a['atkSuperArmorCorrection']} | {a['wepRegainHpScale']} | "
            f"{a['atkRegainHp'] or ''} | {a['dmgLevel']} | {', '.join(notes)} |"
        )
    return out


def curves(w: itemlib.World) -> list[str]:
    g = w.rows("CalcCorrectGraph")
    out = [
        "# Stat curves (CalcCorrectGraph)\n",
        (
            "Each curve is five stage points: at a stat of `stageMaxVal`, the scaling has grown to "
            "`stageMaxGrowVal` percent, with the exponent `adjPt_maxGrowVal` for the stage "
            "before it. Rows 0-16 are the weapons' `correctType`; the others are other curves.\n"
        ),
        "| Curve | Stat points | Growth % | Exponents |",
        "|---:|---|---|---|",
    ]
    for k, r in sorted(g.items()):
        pts = " / ".join(f"{r[f'stageMaxVal{i}']:g}" for i in range(5))
        grow = " / ".join(f"{r[f'stageMaxGrowVal{i}']:g}" for i in range(5))
        adj = " / ".join(f"{r[f'adjPt_maxGrowVal{i}']:g}" for i in range(5))
        out.append(f"| {k} | {pts} | {grow} | {adj} |")
    return out


def main() -> int:
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    a = sys.argv
    if len(a) < 5 or a[1] not in ("weapons", "weapon"):
        print(__doc__)
        return 2
    ws = Weapons(itemlib.World(a[2], a[3]))
    fams = ws.families()
    if a[1] == "weapon":
        want = " ".join(a[4:]).lower()
        hit = next((k for k in fams if ws.name(k).lower() == want), None)
        if hit is None:
            print(f"no weapon {want!r}")
            return 1
        print("\n".join(weapon_tables(ws, hit, fams[hit])))
        return 0
    out = a[4]
    os.makedirs(os.path.join(out, "moves"), exist_ok=True)
    index = [
        "# Weapons\n",
        (
            "Generated by `tools/combat.py weapons`. Each weapon at +0 and at its last level; "
            "scaling in percent with the menu's letter grade. How scaling becomes attack "
            "rating is code, not data (see docs/the-combat.md).\n"
        ),
        (
            "| Weapon | Requirements | Damage (+0 → last) | Scaling (+0 → last) | Poise | "
            "Rally (+0 → last) | Moves |"
        ),
        "|---|---|---|---|---:|---|---|",
    ]
    seen = set()
    for k, levels in fams.items():
        name = ws.name(k)
        r0, rN = ws.rows[levels[0]], ws.rows[levels[-1]]
        fname = re.sub(r"[^\w' -]", "", name).strip() + f" {k}.md"
        with open(
            os.path.join(out, "moves", fname), "w", encoding="utf-8", newline="\n"
        ) as fo:
            fo.write("\n".join(weapon_tables(ws, k, levels)) + "\n")
        req = ", ".join(f"{s} {r0[p]}" for _, p, s in STATS if r0[p])
        dmg = ", ".join(
            f"{d} {r0[b]} → {rN[b]}" for b, _, d in DAMAGE if r0[b] or rN[b]
        )
        sc = ", ".join(
            f"{s} {grade(r0[c], ws.table)} → {grade(rN[c], ws.table)}"
            for c, _, s in STATS
            if r0[c] or rN[c]
        )
        index.append(
            f"| {name} | {req} | {dmg} | {sc} | {r0['saWeaponDamage']} | "
            f"{r0['wepRegainHp']} → {rN['wepRegainHp']} | `moves/{fname}` |"
        )
        seen.add(k)
    with open(
        os.path.join(out, "weapons.md"), "w", encoding="utf-8", newline="\n"
    ) as fo:
        fo.write("\n".join(index) + "\n")
    with open(
        os.path.join(out, "curves.md"), "w", encoding="utf-8", newline="\n"
    ) as fo:
        fo.write("\n".join(curves(ws.w)) + "\n")
    print(f"{len(seen)} weapons -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
