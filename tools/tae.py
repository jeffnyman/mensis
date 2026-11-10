# SPDX-License-Identifier: GPL-3.0-or-later
"""The player's animations: which animation plays each move, and what the game calls it.

    python -I tools/tae.py moves <base dvdroot> <update dvdroot> <weapon name>
    python -I tools/tae.py sets  <base dvdroot> <update dvdroot>

`moves` prints a weapon's judge ids with the animations that invoke them and their names.
`sets` lists every weapon's animation set.

Three pieces link a move (a BehaviorParam_PC row, keyed by judge id) to a button
(docs/the-combat.md, "Which button plays which move"):

* chr/c0000.anibnd.dcx holds one TAE ("time act event") file per animation set, a<set>.tae.
  Each animation's events include, at a point in time, "invoke attack behavior" (event type 1)
  or "invoke bullet behavior" (type 2), whose third int is the judge id. An animation can instead import another's events
  (its file type 1, with the full id <set> * 1000000 + <animation>).
* The same archive's behavior graph (Export/Behaviors/c0000.hkx) names each animation: a state
  such as AtkRightNormal01 holds a selector whose clips are a103_030000, a112_030000 and so
  on, one per animation set.
* A weapon's set comes from its wepmotionCategory through WepMotion_AnimOffset (01cc0b40 in
  the 1.09 executable): categories 20 to 48 go through a jump table (MOTION_SETS below),
  the others are used as they are. A non-zero spAtkcategory replaces the result. The animation
  played is <set> * 1000000 + the animation's own number.
"""

from __future__ import annotations

import collections
import io
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import items as itemlib
import souls

# WepMotion_AnimOffset's jump table (01cc0b40, table at 01cc1114): wepmotionCategory -> set.
# 23 gives 100; 44 to 46, and anything outside 20..48, stay as they are.
MOTION_SETS = {
    20: 125, 21: 115, 22: 116, 23: 100, 24: 117, 25: 107, 26: 101, 27: 113, 28: 112,
    29: 114, 30: 103, 31: 118, 32: 119, 33: 109, 34: 120, 35: 110, 36: 104, 37: 121,
    38: 122, 39: 123, 40: 124, 41: 102, 42: 108, 43: 111, 47: 106, 48: 105,
}  # fmt: skip

# "invoke attack behavior" and "invoke bullet behavior": ints (?, ?, judge id, ...)
BEHAVIOR_EVENTS = (1, 2)


def motion_set(weapon: dict) -> int:
    special = int(weapon.get("spAtkcategory") or 0)
    if special:
        return special
    c = int(weapon["wepmotionCategory"])
    return MOTION_SETS.get(c, c)


def parse_tae(data: bytes) -> tuple[int, dict[int, tuple[int | None, list]]]:
    """(set, {animation: (imported full id or None, [(type, start, end, params)])}).

    The 64-bit TAE layout used by Bloodborne (version 0x1000C)."""
    q = lambda o: struct.unpack_from("<q", data, o)[0]
    i = lambda o: struct.unpack_from("<i", data, o)[0]
    f = lambda o: struct.unpack_from("<f", data, o)[0]
    if data[:4] != b"TAE " or i(8) != 0x1000C:
        raise ValueError("not a 64-bit TAE")
    count, table = i(0x54), q(0x58)
    anims = {}
    for k in range(count):
        aid, off = q(table + 16 * k), q(table + 16 * k + 8)
        heads, afile, n = q(off), q(off + 24), i(off + 32)
        events = []
        for e in range(n):
            h = heads + 24 * e
            d = q(h + 16)
            events.append((i(d), f(q(h)), f(q(h + 8)), data[q(d + 8) : q(d + 8) + 32]))
        imported = i(afile + 24) if q(afile) == 1 else None
        anims[aid] = (imported, events)
    return i(0x50) - 2000, anims


def behavior_names(data: bytes) -> dict[tuple[int, int], str]:
    """{(set, animation): state name} from the behavior graph's strings: each
    "<state> CustomManualSelectorGenerator" is followed by its clips, a<set>_<animation>."""
    strings = [m.group().decode() for m in re.finditer(rb"[\x20-\x7e]{5,}", data)]
    clip = re.compile(r"^a(\d{3})_(\d{6})(?:_hkx_AutoSet_\d+)?$")
    out: dict[tuple[int, int], str] = {}
    state = None
    for s in strings:
        if s.endswith(" CustomManualSelectorGenerator"):
            state = s[: -len(" CustomManualSelectorGenerator")]
            continue
        m = clip.match(s)
        if m and state:
            out.setdefault((int(m.group(1)), int(m.group(2))), state)
        else:
            state = None
    return out


class Animations:
    def __init__(self, w: itemlib.World):
        self.sets: dict[int, dict] = {}
        self.names: dict[tuple[int, int], str] = {}
        for b in souls.bnd4_read(souls.load(w.path("chr/c0000.anibnd.dcx"))):
            if b.name.endswith(".tae"):
                s, anims = parse_tae(b.data)
                self.sets[s] = anims
            elif b.name.replace("\\", "/").endswith("Behaviors/c0000.hkx"):
                self.names = behavior_names(b.data)

    def events(self, s: int, aid: int, depth: int = 0) -> list:
        imported, ev = self.sets.get(s, {}).get(aid, (None, []))
        if imported is not None and depth < 4:
            return self.events(imported // 1000000, imported % 1000000, depth + 1)
        return ev

    def judges(self, s: int) -> dict[int, list[int]]:
        """{judge id: [animations of set s that invoke it]}."""
        out: dict[int, list[int]] = collections.defaultdict(list)
        for aid in sorted(self.sets.get(s, {})):
            for e in self.events(s, aid):
                if e[0] in BEHAVIOR_EVENTS:
                    j = struct.unpack_from("<i", e[3], 8)[0]
                    if aid not in out[j]:
                        out[j].append(aid)
        return out

    def name(self, s: int, aid: int) -> str:
        return self.names.get((s, aid), "")


# The state names, in plain words. "Right" and ModeA are a trick weapon's first form, "Both" and
# ModeB its transformed one (their judge ids are 400 apart).
GLOSS = (
    (r"^Atk(?:Right|Both)Normal(\d+)", lambda m: f"R1 #{int(m[1])}"),
    (r"^Atk(?:Right|Both)Hard(\d+)", lambda m: f"R2 #{int(m[1])}"),
    (r"^AtkBothDashL2", "running L2"),
    (r"^Atk(?:Right|Both)DashHard", "running R2"),
    (r"^Atk(?:Right|Both)Dash", "running R1"),
    (r"^Atk(?:Right|Both)RollingHard", "R2 out of a roll"),
    (r"^Atk(?:Right|Both)Rolling", "R1 out of a roll"),
    (r"^Atk(?:Right|Both)StepHard", "R2 out of a quickstep"),
    (r"^Atk(?:Right|Both)Step", "R1 out of a quickstep"),
    (r"^Atk(?:Right|Both)Jump", "jump attack"),
    (r"^AtkFall(Start|Loop|End)", lambda m: f"plunging attack ({m[1].lower()})"),
    (r"^AtkPower(?:Start|Loop)(B?)", lambda m: "charging R2" + (" (B)" if m[1] else "")),
    (
        r"^AtkPowerNormalHit(B?)(\d+)",
        lambda m: "R2, released before full charge" + (" (B)" if m[1] else ""),
    ),
    (
        r"^AtkPowerHardHit(B?)(\d+)",
        lambda m: "R2, fully charged"
        + (f" #{int(m[2])}" if int(m[2]) > 1 else "")
        + (" (B)" if m[1] else ""),
    ),
    (r"^Atk(?:Right|Both)NormalDraw", "R1 (draw)"),
    (r"^ChangeGimmick_\w+_ModeA", "transform attack (L1)"),
    (r"^ChangeGimmick_\w+_ModeB", "transform attack back (L1)"),
    (r"^AtkBothL2_from(R1|R2|L2)", lambda m: f"L2 after {m[1]}"),
    (r"^AtkBothL2_(\d+)", lambda m: f"L2 #{int(m[1])}"),
    (r"GunShot", "gun shot"),
    (r"^AtkLeft_Spray", "spray"),
    (r"^Bow_", "bow shot"),
    (r"^Throw", "visceral or grab"),
)


def gloss(name: str) -> str:
    for pat, say in GLOSS:
        m = re.search(pat, name, re.IGNORECASE)
        if m:
            return say(m) if callable(say) else say
    return ""


def describe(an: Animations, s: int, aids: list[int]) -> tuple[str, str]:
    """(plain move names, "animation state" list) for the animations invoking one judge id."""
    words, states = [], []
    for aid in aids:
        n = an.name(s, aid)
        g = gloss(n)
        if g and g not in words:
            words.append(g)
        states.append(f"{aid:06d} {n}".strip())
    return ", ".join(words), "; ".join(states)


def main() -> int:
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    a = sys.argv
    if len(a) < 4 or a[1] not in ("moves", "sets") or (a[1] == "moves" and len(a) < 5):
        print(__doc__)
        return 2
    import combat

    w = itemlib.World(a[2], a[3])
    ws = combat.Weapons(w)
    an = Animations(w)
    fam = ws.families()
    if a[1] == "sets":
        for k in fam:
            r = ws.rows[k]
            s = motion_set(r)
            print(
                f"{ws.name(k)}: wepmotionCategory {r['wepmotionCategory']} -> set a{s}"
                f"{'' if s in an.sets else ' (no TAE)'}, {len(an.judges(s))} judge ids"
            )
        return 0
    want = " ".join(a[4:]).lower()
    k = next((k for k in fam if ws.name(k).lower() == want), None)
    if k is None:
        print(f"no weapon {' '.join(a[4:])!r}")
        return 1
    r = ws.rows[k]
    s = motion_set(r)
    js = an.judges(s)
    print(f"{ws.name(k)}: animation set a{s}")
    for m in ws.moves(r["behaviorVariationId"]):
        words, states = describe(an, s, js.get(m["judge"], []))
        print(f"  {m['judge']:4}  {words or '-'}  [{states}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
