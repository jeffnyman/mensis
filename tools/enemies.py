# SPDX-License-Identifier: GPL-3.0-or-later
"""Enemy movesets: every enemy's attacks, from its animations, its moves and its AI.

    python -I tools/enemies.py tables <base dvdroot> <update dvdroot> <out dir>
    python -I tools/enemies.py enemy  <base dvdroot> <update dvdroot> <name or NpcParam row>

`tables` writes enemies.md (one line per enemy) and enemies/<name> <row>.md (each enemy's
attacks). `enemy` prints one enemy's attacks.

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
  3 thrust). Special effects then scale them (New Game+, for one: tools/effects.py).
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
import items as itemlib
import lua50
import souls
import tae

ATTRIBUTE = {1: "slash", 2: "blunt", 3: "thrust"}
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
                + f" | {', '.join(used_by.get(aid, [])) if k == 0 else ''} | {via} |"
            )
    return out, stats


def safe(name: str) -> str:
    return re.sub(r'[\\/:*?"<>|]', "_", name)


def main() -> int:
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    a = sys.argv
    if len(a) < 5 or a[1] not in ("tables", "enemy"):
        print(__doc__)
        return 2
    en = Enemies(itemlib.World(a[2], a[3]))
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
