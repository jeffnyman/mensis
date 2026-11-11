# SPDX-License-Identifier: GPL-3.0-or-later
"""Enemy movesets: every enemy's attacks, from its animations, its moves and its AI.

    python -I tools/enemies.py tables <base dvdroot> <update dvdroot> <out dir>
    python -I tools/enemies.py enemy  <base dvdroot> <update dvdroot> <name or NpcParam row>
    python -I tools/enemies.py hit    <base dvdroot> <update dvdroot> <NpcParam row> <judge id> <level> [<armor piece>...]

`tables` writes enemies.md (one line per enemy) and enemies/<name> <row>.md (each enemy's
attacks). `enemy` prints one enemy's attacks. `hit` applies one enemy attack to a hunter of a
given level wearing the named armor pieces (each a quoted EquipParamProtector name, such as
"Hunter Hat"), with the enemy's area level and nothing else.

How the pieces connect (docs/the-combat.md, "Enemy movesets"):

* NpcParam row r uses the model c<(r mod 1000000) // 100>: 500000 (Cleric Beast) is c5000,
  and so is its Chalice Dungeon copy 10500000. The model's animations are in
  chr/c5000.anibnd.dcx. Rows below 100000 (tests, and the human characters, who carry weapons)
  aren't covered here.
* Names: an enemy is called by the English name (NPC名) of any row with its model, or else by
  its first row's working name. The param files name every row (params.Param.row_names), and
  an attack row's name says what the attack is: AtkParam_Npc 500130 is
  "c5000_教区長_踏みつけ", the Cleric Beast's stomp.
* The archive's TAE file gives each animation's events. An "invoke attack behavior" (type 1)
  or "invoke bullet behavior" (type 2) event names a judge id at a time in the animation, the
  same as the player's (tools/tae.py). An animation can import another's events; an enemy's
  imports name an animation of the same file.
* BehaviorParam row 200000000 + behaviorVariationId * 1000 + judge id is the move. refType 0
  points at an AtkParam_Npc row, refType 1 at a Bullet row whose atkId_Bullet is the attack.
* AtkParam_Npc holds an enemy attack's own values, not multipliers: atkPhys / atkMag /
  atkFire / atkThun (physical, arcane, fire, bolt), atkStam (stamina damage), atkSuperArmor
  (poise damage), dmgLevel (the reaction it causes) and atkAttribute (1 slash, 2 blunt,
  3 thrust, 4 standard). Special effects then scale them (New Game+, for one: tools/effects.py).
* Area level: NpcParam spEffectID0-7 are 常駐特殊効果, "resident special effects". Most enemies
  have one named for its area's level, such as 7003 "レベル3：聖堂B・神父" (level 3: Cathedral B,
  the Father), which multiplies its HP (maxHpRate), attack (physicsAttackPowerRate and the other
  three), defense and stamina damage. The sheets give these; `hit` multiplies the attack by them
  (rates of several effects multiply, as the defense and damage-cut gatherers were seen to do).
* Against the player (Hit_ApplyReceived, 01e2a770): the player's defense is curve 102 of the
  six stats' sum, which is level + 50 (Defense_Compute); the damage formula reduces each
  element (combat.damage_share); then the four armor pieces' cut rates multiply it
  (ArmorCut_Product, 0197abc0, through player slot 0x230): the physical rate by attack type,
  1 slashDamageCutRate, 2 blow, 3 thrust, 4 shotDamageCutRate ("standard", in its own
  description), anything else 1.0.
* NpcThinkParam row aiThinkId names the battle script, <battleGoalID>_battle.lua, in one of
  script/*.luabnd.dcx. Its action functions (<working name>_Act01 ...) queue animations with
  AddSubGoal; an animation number in an action's constants is an attack it can make.
"""

from __future__ import annotations

import collections
import glob
import io
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import combat
import items as itemlib
import lua50
import souls
import tae

ATTRIBUTE = {1: "slash", 2: "blunt", 3: "thrust", 4: "standard"}
# ArmorCut_Product (0197abc0): the physical cut rate by atkAttribute
PHYSICAL_CUT = {
    1: "slashDamageCutRate",
    2: "blowDamageCutRate",
    3: "thrustDamageCutRate",
    4: "shotDamageCutRate",
}
ELEMENT_CUT = ("magicDamageCutRate", "fireDamageCutRate", "thunderDamageCutRate")
# an effect's attack-power rates, element by element, and its other scalings
ATTACK_RATES = (
    "physicsAttackPowerRate",
    "magicAttackPowerRate",
    "fireAttackPowerRate",
    "thunderAttackPowerRate",
)
SCALES = (
    ("maxHpRate", "HP"),
    ("physicsAttackPowerRate", "attack"),
    ("physicsDiffenceRate", "defense"),
    ("staminaAttackRate", "stamina damage"),
)
ELEMENTS = (
    ("atkPhys", "phys"),
    ("atkMag", "arcane"),
    ("atkFire", "fire"),
    ("atkThun", "bolt"),
)


class Enemies:
    def __init__(self, w: itemlib.World):
        self.w = w
        self.behaviors = w.rows("BehaviorParam")
        self.attacks_param = w.rows("AtkParam_Npc")
        self.bullets = w.rows("Bullet")
        self.think = w.rows("NpcThinkParam")
        self.npc_labels = w.row_names("NpcParam")
        self.effects = w.rows("SpEffectParam")
        self.effect_labels = w.row_names("SpEffectParam")
        self.attack_labels = w.row_names("AtkParam_Npc")
        self._scripts: dict[str, str] | None = None
        self.english: dict[int, str] = {}
        for nid, r in sorted(w.npcs.items()):
            n = w.npc_names.get(r["nameId"], "").strip()
            if nid >= 100000 and n:
                self.english.setdefault(model_of(nid), n)

    def name(self, nid: int) -> str:
        return self.english.get(model_of(nid)) or self.label(nid)

    def label(self, nid: int) -> str:
        return self.npc_labels.get(nid, "").replace("\u3000", " ").strip()

    def resident(self, nid: int) -> list[int]:
        """The resident special effects that scale something (spEffectID0-7)."""
        r = self.w.npcs[nid]
        out = []
        for i in range(8):
            e = self.effects.get(r[f"spEffectID{i}"])
            if e and any(
                e[f] != 1.0 for f, _ in SCALES + tuple((x, "") for x in ATTACK_RATES)
            ):
                out.append(r[f"spEffectID{i}"])
        return out

    def rate(self, nid: int, field: str) -> float:
        v = 1.0
        for e in self.resident(nid):
            v *= self.effects[e][field]
        return v

    def archive(self, model: int) -> str | None:
        for root in (self.w.update, self.w.base):
            p = os.path.join(root, "chr", f"c{model:04d}.anibnd.dcx")
            if os.path.exists(p):
                return p
        return None

    def groups(self) -> dict[tuple[int, int], list[int]]:
        """{(model, behaviorVariationId): [named NpcParam rows]} for models with animations."""
        out: dict[tuple[int, int], list[int]] = collections.defaultdict(list)
        for nid, r in sorted(self.w.npcs.items()):
            model = model_of(nid)
            if nid < 100000 or not self.archive(model):
                continue
            out[(model, r["behaviorVariationId"])].append(nid)
        return out

    def animations(self, model: int) -> dict[int, list[tuple[float, int]]]:
        """{animation: [(time in seconds, judge id)]} for every animation that invokes a move."""
        path = self.archive(model)
        out: dict[int, list[tuple[float, int]]] = {}
        if not path:
            return out
        for b in souls.bnd4_read(souls.load(path)):
            if not b.name.endswith(".tae"):
                continue
            try:
                _, anims = tae.parse_tae(b.data)
            except ValueError:
                continue
            for aid, (imported, events) in anims.items():
                if imported is not None:
                    events = anims.get(imported % 1000000, (None, []))[1]
                hits = sorted(
                    (round(e[1], 2), struct.unpack_from("<i", e[3], 8)[0])
                    for e in events
                    if e[0] in tae.BEHAVIOR_EVENTS
                )
                if hits and aid not in out:
                    out[aid] = hits
        return out

    def attack_id(self, variation: int, judge: int) -> int | None:
        """The AtkParam_Npc row behind a judge id, directly or through a bullet."""
        b = self.behaviors.get(200000000 + variation * 1000 + judge)
        if b is None:
            return None
        if b["refType"] == 1:
            bullet = self.bullets.get(b["refId"])
            return bullet["atkId_Bullet"] if bullet else None
        return b["refId"] if b["refType"] == 0 else None

    def move(self, variation: int, judge: int) -> tuple[dict | None, str]:
        """(AtkParam_Npc row, how it was reached) for one judge id."""
        b = self.behaviors.get(200000000 + variation * 1000 + judge)
        if b is None:
            return None, "no behavior row"
        if b["refType"] == 1:
            bullet = self.bullets.get(b["refId"])
            if bullet is None:
                return None, f"bullet {b['refId']} (no row)"
            return self.attacks_param.get(
                bullet["atkId_Bullet"]
            ), f"bullet {b['refId']}"
        if b["refType"] == 0:
            return self.attacks_param.get(b["refId"]), ""
        return None, f"refType {b['refType']}"

    def status(self, variation: int, judge: int) -> list[tuple[str, int]]:
        """[(condition, buildup)] a hit carries: the effects of its attack row and, for a
        bullet, the bullet's own (tools/status.py)."""
        import status as statuslib

        ids = []
        b = self.behaviors.get(200000000 + variation * 1000 + judge)
        if b is None:
            return []
        if b["refType"] == 1:
            bullet = self.bullets.get(b["refId"])
            if bullet:
                ids += [bullet[f"spEffectId{i}"] for i in range(5)]
        a_id = self.attack_id(variation, judge)
        a = self.attacks_param.get(a_id) if a_id is not None else None
        if a:
            ids += [a[f"spEffectId{i}"] for i in range(5)]
        out: list[tuple[str, int]] = []
        for i in ids:
            if i > 0:
                out += statuslib.buildup(self.effects.get(i))
        return out

    def script_index(self) -> dict[str, str]:
        """{script file name: luabnd path}, the update's archives over the base's."""
        if self._scripts is None:
            self._scripts = {}
            for root in (self.w.base, self.w.update):
                for p in sorted(
                    glob.glob(os.path.join(root, "script", "*.luabnd.dcx"))
                ):
                    for b in souls.bnd4_read(souls.load(p)):
                        self._scripts[os.path.basename(b.name.replace("\\", "/"))] = p
        return self._scripts

    def actions(
        self, nid: int, attack_anims: set[int]
    ) -> tuple[str, dict[str, list[int]]]:
        """(script name, {action function: [attack animations it can queue]})."""
        t = self.think.get(self.w.npcs[nid]["aiThinkId"])
        if not t or t["battleGoalID"] <= 0:
            return "", {}
        fname = f"{t['battleGoalID']}_battle.lua"
        path = self.script_index().get(fname)
        if not path:
            return fname, {}
        chunk = lua50.chunks(path).get(fname)
        if chunk is None:
            return fname, {}
        out: dict[str, list[int]] = {}
        for g, lines in lua50.named_functions(lua50.Reader(chunk).function()).items():
            if not re.search(r"_Act\d+$", g):
                continue
            found = []
            for ln in lines:
                m = re.match(r"\s*LOADK\s+r\d+ (\d+)$", ln)
                if (
                    m
                    and int(m.group(1)) in attack_anims
                    and int(m.group(1)) not in found
                ):
                    found.append(int(m.group(1)))
            out[g] = found
        return fname, out


def model_of(nid: int) -> int:
    return nid % 1000000 // 100


def attack_cells(a: dict | None) -> list[str]:
    if a is None:
        return ["", "", "", "", "", "", "", ""]
    return [
        *(str(a[f]) if a[f] else "" for f, _ in ELEMENTS),
        ATTRIBUTE.get(a["atkAttribute"], f"type {a['atkAttribute']}"),
        str(a["atkStam"]),
        str(a["atkSuperArmor"]),
        str(a["dmgLevel"]),
    ]


def sheet(
    en: Enemies, model: int, variation: int, rows: list[int]
) -> tuple[list[str], dict]:
    w = en.w
    first = w.npcs[rows[0]]
    names = sorted({en.name(r) for r in rows})
    anims = en.animations(model)
    script, acts = en.actions(rows[0], set(anims))
    used_by: dict[int, list[str]] = collections.defaultdict(list)
    for g, al in acts.items():
        for a in al:
            used_by[a].append(re.sub(r"^.*_(Act\d+)$", r"\1", g))
    labels = {}
    for aid in anims:
        for _, judge in anims[aid]:
            atk_id = en.attack_id(variation, judge)
            if atk_id is not None:
                labels[judge] = en.attack_labels.get(atk_id, "")
    prefix = os.path.commonprefix([v for v in labels.values() if v])
    prefix = prefix[: prefix.rfind("_") + 1] if "_" in prefix else ""
    working = sorted({en.label(r) for r in rows if en.label(r)})
    out = [f"# {' / '.join(names)}\n"]
    out.append(
        f"Working names: {'; '.join(working[:6])}{' …' if len(working) > 6 else ''}. "
        f"Attack names are the "
        f"AtkParam_Npc rows' own, without their shared start ({prefix or 'none'}).\n"
    )
    shown = ", ".join(str(r) for r in rows[:12]) + (
        f" and {len(rows) - 12} more" if len(rows) > 12 else ""
    )
    out.append(
        f"NpcParam rows {shown}; model c{model:04d}; behavior "
        f"variation {variation}. HP {first['hp']}. Defense: physical {first['def_phys']} "
        f"(slash {first['def_slash']:+}%, blunt {first['def_blow']:+}%, thrust "
        f"{first['def_thrust']:+}%), arcane {first['def_mag']}, fire {first['def_fire']}, bolt "
        f"{first['def_thunder']}. Poise {first['superArmorDurability']}. AI: "
        f"{script or 'none'}.\n"
    )
    res = en.resident(rows[0])
    if res:
        out.append(
            f"Resident effects of row {rows[0]}: "
            + "; ".join(
                f"{e} ({en.effect_labels.get(e, '').replace(chr(0x3000), ' ')}): "
                + ", ".join(f"{label} ×{en.effects[e][f]:g}" for f, label in SCALES)
                for e in res
            )
            + f". With them its HP is {round(first['hp'] * en.rate(rows[0], 'maxHpRate'))}, and "
            "its attacks below are multiplied by the attack rate.\n"
        )
    out.append(
        "Generated by `tools/enemies.py`. One line per hit: the animation, when in it the hit "
        "is judged, its judge id and the attack's own values (AtkParam_Npc, before any special "
        "effect). AI actions are the battle script's functions whose constants name the "
        "animation.\n"
    )
    out.append(
        "| Animation | Time (s) | Judge | Attack | Phys | Arcane | Fire | Bolt | Type | "
        "Stamina dmg | Poise dmg | Hit level | AI actions | Notes |"
    )
    out.append("|---:|---:|---:|---|---:|---:|---:|---:|---|---:|---:|---:|---|---|")
    stats = {"hits": 0, "max": 0, "anims": len(anims), "acts": len(acts)}
    for aid in sorted(anims):
        for k, (time, judge) in enumerate(anims[aid]):
            a, via = en.move(variation, judge)
            stats["hits"] += 1
            if a:
                stats["max"] = max(
                    stats["max"],
                    a["atkPhys"] + a["atkMag"] + a["atkFire"] + a["atkThun"],
                )
            out.append(
                f"| {aid if k == 0 else ''} | {time:g} | {judge} | "
                f"{labels.get(judge, '')[len(prefix) :].replace(chr(0x3000), ' ')} | "
                + " | ".join(attack_cells(a))
                + f" | {', '.join(used_by.get(aid, [])) if k == 0 else ''} | "
                + "; ".join(
                    x
                    for x in (
                        via,
                        ", ".join(f"{n} +{v}" for n, v in en.status(variation, judge)),
                    )
                    if x
                )
                + " |"
            )
    return out, stats


def armor_cuts(pieces: list[dict], attribute: int) -> list[float]:
    """ArmorCut_Product (0197abc0): each element's damage multiplier, the product over the
    pieces worn; the physical one by attack type, 1.0 for a type outside 1-4."""
    cuts = [1.0, 1.0, 1.0, 1.0]
    for p in pieces:
        cuts[0] *= p[PHYSICAL_CUT[attribute]] if attribute in PHYSICAL_CUT else 1.0
        for i, f in enumerate(ELEMENT_CUT, 1):
            cuts[i] *= p[f]
    return cuts


def player_defense(w: itemlib.World, level: int) -> float:
    """Defense_Compute (0197a240): curve 102 of the six stats' sum (level + 50), rounded."""
    return float(round(combat.curve(w.rows("CalcCorrectGraph")[102], level + 50)))


def hit_on_player(
    a: dict,
    level: int,
    pieces: list[dict],
    w: itemlib.World,
    rates: tuple[float, ...] = (1.0, 1.0, 1.0, 1.0),
) -> list[tuple[str, float, float, float, float]]:
    """[(element, attack, defense, after defense, after armor)] for each element present; the
    attack is the row's value times the attacker's rate for that element."""
    d = player_defense(w, level)
    cuts = armor_cuts(pieces, a["atkAttribute"])
    out = []
    for (f, name), cut, rate in zip(ELEMENTS, cuts, rates):
        if a[f] > 0:
            value = a[f] * rate
            after = value * combat.damage_share(value, d)
            out.append((name, value, d, after, after * cut))
    return out


def safe(name: str) -> str:
    return re.sub(r'[\\/:*?"<>|]', "_", name)


def main() -> int:
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    a = sys.argv
    if len(a) < 5 or a[1] not in ("tables", "enemy", "hit"):
        print(__doc__)
        return 2
    en = Enemies(itemlib.World(a[2], a[3]))
    if a[1] == "hit":
        if len(a) < 7:
            print(__doc__)
            return 2
        nid, judge, level = int(a[4]), int(a[5]), int(a[6])
        armor = en.w.rows("EquipParamProtector")
        anames = en.w.names.get("EquipParamProtector", {})
        pieces = []
        for want in a[7:]:
            rid = next(
                (
                    k
                    for k, v in sorted(anames.items())
                    if (v or "").strip() == want and k in armor
                ),
                None,
            )
            if rid is None:
                print(f"no armor piece {want!r}")
                return 1
            pieces.append(armor[rid])
        if nid not in en.w.npcs:
            print(f"no NpcParam row {nid}")
            return 1
        atk, via = en.move(en.w.npcs[nid]["behaviorVariationId"], judge)
        if atk is None:
            print(f"no attack for judge {judge}: {via}")
            return 1
        print(
            f"{en.name(nid)} judge {judge}, {ATTRIBUTE.get(atk['atkAttribute'], atk['atkAttribute'])}; "
            f"level {level}, defense {player_defense(en.w, level):g}; armor: {', '.join(a[7:]) or 'none'}"
        )
        rates = tuple(en.rate(nid, f) for f in ATTACK_RATES)
        res = en.resident(nid)
        print(
            "resident effects: "
            + (
                ", ".join(
                    f"{e} {en.effect_labels.get(e, '').replace(chr(0x3000), ' ')}"
                    for e in res
                )
                or "none"
            )
            + f"; attack ×{rates[0]:g}"
        )
        total = 0.0
        for name, value, d, after, final in hit_on_player(
            atk, level, pieces, en.w, rates
        ):
            total += final
            print(
                f"{name:7} attack {value:6g}  after defense {after:7.1f}  after armor {final:7.1f}"
            )
        print(f"total   {total:.1f}")
        return 0
    groups = en.groups()
    if a[1] == "enemy":
        want = " ".join(a[4:])
        for (model, var), rows in groups.items():
            if want in map(str, rows) or want.lower() in {
                en.name(r).lower() for r in rows
            }:
                print("\n".join(sheet(en, model, var, rows)[0]))
                return 0
        print(f"no enemy {want!r}")
        return 1
    os.makedirs(os.path.join(a[4], "enemies"), exist_ok=True)
    index = [
        "# Enemies\n",
        (
            "Generated by `tools/enemies.py tables`. One line per model and behavior variation, "
            "with its named NpcParam rows. Strongest hit is the largest sum of an attack's four "
            "damage values, before special effects.\n"
        ),
        "| Enemy | NpcParam rows | Model | HP | Attack animations | Hits | Strongest hit | AI actions |",
        "|---|---|---|---:|---:|---:|---:|---:|",
    ]
    for (model, var), rows in sorted(
        groups.items(), key=lambda kv: (en.name(kv[1][0]), kv[0])
    ):
        lines, st = sheet(en, model, var, rows)
        if not st["hits"]:
            continue
        fname = safe(f"{en.name(rows[0])} {rows[0]}") + ".md"
        with open(
            os.path.join(a[4], "enemies", fname), "w", encoding="utf-8", newline="\n"
        ) as fo:
            fo.write("\n".join(lines) + "\n")
        shown = ", ".join(str(r) for r in rows[:4]) + (" …" if len(rows) > 4 else "")
        index.append(
            f"| {' / '.join(sorted({en.name(r) for r in rows}))} | {shown} | c{model:04d} | "
            f"{en.w.npcs[rows[0]]['hp']} | {st['anims']} | {st['hits']} | {st['max']} | "
            f"{st['acts']} |"
        )
    with open(
        os.path.join(a[4], "enemies.md"), "w", encoding="utf-8", newline="\n"
    ) as fo:
        fo.write("\n".join(index) + "\n")
    print(f"{len(index) - 4} enemies -> {a[4]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
