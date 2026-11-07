# SPDX-License-Identifier: GPL-3.0-or-later
"""The Chalice Dungeons' event scripts: which templates each layout uses, and what the rites'
special enemies and special characters resolve to.

    python -I tools/chalice_events.py <base dvdroot> <update dvdroot> <out.md>

How the scripts are organised (read from the files, see docs/the-chalice-events.md):

    event/m29.emevd           a library of 428 named template events ("_XX" = takes arguments)
    event/m29_AB_CD_00/<layout>.emevd
                              one small script per layout (2,641): almost always a single
                              constructor (event 0) that only starts library templates with that
                              layout's entity IDs as arguments ([2000:00], variadic)

Sub-feature templates keep or remove a set of placed entities depending on the rite's event flag
(DungeonFeatureParam.eventflagId N -> flag 92905300 + N):

    special enemy, placement 1 / 2   arguments X0..X24 = up to 7 characters, X28 = one object
                                     placement 1 kept if     placement 2 kept if
        12904919 / 12905042  specially spawned  20, 24, 25, 26         20
        12904979 / 12905097  NPC enemy          21, 27, 28             21, 24
        12905000 / 12905108  strengthening      22, 29                 22, 25, 27
        12905013 / 12905119  undying            23                     23, 26, 28, 29
        12905026 / 12905130  no feature         20-29 all off          20-29 all off

The output lists these conditions as read from m29.emevd (flag_tests below), so a change in the
data shows there rather than silently disagreeing with this table.

Every character argument is resolved through the layout's MSB (entity ID -> part -> model and
NpcParam row -> English name where the text has one), every object argument to its treasure.
"""

from __future__ import annotations

import collections
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import events  # noqa: E402
import items as itemlib  # noqa: E402
import msb as msblib  # noqa: E402
import souls  # noqa: E402

START = (2000, 0)                 # [2000:00] start event (slot, event id, arguments...)
FLAG_BASE = 92905300              # sub-feature flag = FLAG_BASE + DungeonFeatureParam.eventflagId

SPECIAL = {                       # template -> (placement, kind)
    12904919: (1, "specially spawned"), 12904979: (1, "NPC enemy"), 12905000: (1, "strengthening"),
    12905013: (1, "undying"), 12905026: (1, "no feature"),
    12905042: (2, "specially spawned"), 12905097: (2, "NPC enemy"), 12905108: (2, "strengthening"),
    12905119: (2, "undying"), 12905130: (2, "no feature"),
}
CHARACTER = {                     # special-character templates (features 30-34)
    12905188: "shop character: shop", 12905190: "shop character: infighting",
    12905192: "shop character: no feature", 12905198: "no-shop character: infighting",
    12905201: "no-shop character: no feature",
}
WIDOW = {12907401: "placed widow", 12907405: "appearing widow"}
BOSS = {                          # one part-damage or behaviour template per boss, to name it
    12904506: "フレッシュゴーレム", 12904723: "嵐の獣（骨）", 12904728: "月の落とし子",
    12904852: "銀の獣の眷属", 12904884: "血に渇いた獣", 12904896: "銀の獣", 12906795: "デブ",
    12906829: "月からの使者（大）", 12906831: "嵐の獣", 12906841: "炎の獣", 12906855: "始祖ビースト",
    12906865: "邪神", 12906882: "白痴の蜘蛛", 12904026: "花嫁",
}


def flag_tests(ev) -> list[tuple[str, int, int]]:
    """The sub-feature flags an event tests: [(instruction, state, flag or start-end)]."""
    out = []
    for cls, iid, raw in ev.instructions:
        if (cls, iid) == (3, 0):                       # judge one flag: group, state, type, id
            _, state, _, flag = struct.unpack_from("<bBBxI", raw)
            if FLAG_BASE <= flag < FLAG_BASE + 100:
                out.append(("flag", state, flag - FLAG_BASE))
        elif (cls, iid) == (1003, 4):                  # end if a range of flags: type, op, _, start, end
            _, op, _, lo, hi = struct.unpack_from("<BBBxII", raw)
            if FLAG_BASE <= lo < FLAG_BASE + 100:
                out.append((["all on", "all off", "any on", "any off"][op], lo - FLAG_BASE, hi - FLAG_BASE))
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    w = itemlib.World(sys.argv[1], sys.argv[2])
    lib_path = w.path("event/m29.emevd.dcx")
    lib = {e.id: e for e in events.read_emevd(souls.load(lib_path))}
    names = events.read_emeld(souls.load(w.path("event/m29.emeld.dcx")))

    def who(p) -> str:
        if p is None:
            return "(missing)"
        if "npc" not in p:
            return f"object {p['model']}"
        r = w.npcs.get(p["npc"])
        n = w.npc_names.get(r["nameId"], "").strip() if r else ""
        return f"{p['model']} {p['npc'] % 1000000:06d}" + (f" {n}" if n else "")

    use_layouts = collections.defaultdict(set)    # template -> layouts starting it
    use_starts = collections.Counter()
    own_events = []                                # layouts defining more than the constructor
    resolved = collections.defaultdict(collections.Counter)   # (template, role) -> who -> count
    treasure = collections.defaultdict(collections.Counter)
    interesting = set(SPECIAL) | set(CHARACTER) | set(WIDOW) | set(BOSS)
    n_layouts = 0
    for path in sorted(glob.glob(os.path.join(sys.argv[1], "event", "m29_*", "*.emevd.dcx"))):
        lay = os.path.basename(path)[: -len(".emevd.dcx")]
        folder = os.path.basename(os.path.dirname(path))
        n_layouts += 1
        evs = events.read_emevd(souls.load(path))
        if len(evs) > 1:
            own_events.append(lay)
        starts = []
        for ev in evs:
            for cls, iid, raw in ev.instructions:
                if (cls, iid) == START:
                    _, tid = struct.unpack_from("<iI", raw)
                    use_layouts[tid].add(lay)
                    use_starts[tid] += 1
                    if tid in interesting:
                        raw = raw.ljust(40, b"\0")
                        starts.append((tid, struct.unpack_from(f"<{(len(raw) - 8) // 4}i", raw, 8)))
        if not starts:
            continue
        mp = w.path(f"map/mapstudio/{folder}/{lay}.msb.dcx")
        if not os.path.exists(mp):
            continue
        m = msblib.Msb(souls.load(mp))
        ent = {p["entity"]: p for p in m.parts if p["entity"] > 0}
        lots = {}
        for e in m.events:
            if e.get("item_lots") and e.get("treasure_part"):
                tp = next((p for p in m.parts if p["name"] == e["treasure_part"]), None)
                if tp:
                    lots[tp["entity"]] = e["item_lots"]
        for tid, args in starts:
            if tid in SPECIAL:
                for k, a in enumerate(args[:7]):
                    if a > 0:
                        resolved[(tid, "first" if k == 0 else "others")][who(ent.get(a))] += 1
                if len(args) > 7 and args[7] > 0:
                    for lot in lots.get(args[7], []):
                        for item, _, _ in w.lot(lot):
                            treasure[tid][item] += 1
            else:
                for k, a in enumerate(args[:2]):
                    p = ent.get(a)
                    if p is not None and "npc" in p:
                        resolved[(tid, f"arg{k}")][who(p)] += 1

    out = ["# The Chalice Dungeons' event scripts (1.09)\n",
           f"Generated by `tools/chalice_events.py` from `{os.path.relpath(lib_path)}` and the "
           f"{n_layouts} per-layout scripts.\n",
           f"Layouts with events of their own besides the constructor: {len(own_events)} "
           f"({', '.join(own_events)}).\n",
           "## Special enemies (Rotted Offering)\n"]
    for tid, (pl, kind) in SPECIAL.items():
        tests = flag_tests(lib[tid])
        out.append(f"### Placement {pl}: {kind} ({tid}, {len(use_layouts[tid])} layouts)\n")
        out.append("Kept when: " + "; ".join(
            f"feature {a} {'on' if s else 'off'}" if t == "flag" else f"features {s}-{a} {t}" for t, s, a in tests) + "\n")
        for role in ("first", "others"):
            top = resolved[(tid, role)].most_common(8)
            out.append(f"* {role}: " + ", ".join(f"{n} ×{c}" for n, c in top))
        if treasure[tid]:
            out.append("* treasure: " + ", ".join(f"{n} ×{c}" for n, c in treasure[tid].most_common(8)))
        out.append("")
    out.append("## Special characters (features 30-34)\n")
    for tid, label in CHARACTER.items():
        tests = flag_tests(lib[tid])
        out.append(f"* **{label}** ({tid}, {len(use_layouts[tid])} layouts), tests "
                   + (", ".join(f"{a}:{'on' if s else 'off'}" if t == "flag" else f"{s}-{a} {t}" for t, s, a in tests) or "nothing")
                   + "; " + ", ".join(f"{n} ×{c}" for n, c in resolved[(tid, 'arg0')].most_common(5)))
    out.append("\n## Widows\n")
    for tid, label in WIDOW.items():
        out.append(f"* **{label}** ({tid}, {len(use_layouts[tid])} layouts in "
                   f"{len({l[:10] for l in use_layouts[tid]})} folders): "
                   + ", ".join(f"{n} ×{c}" for n, c in resolved[(tid, 'arg0')].most_common(4)))
    out.append("\n## Bosses: internal labels and the names the text gives\n")
    out.append("| Label in event names | Resolves to | Layouts |\n|---|---|---:|")
    for tid, label in BOSS.items():
        c = resolved[(tid, "arg0")] + resolved[(tid, "arg1")]
        named, models = collections.Counter(), collections.Counter()
        for n, k in c.items():
            parts = n.split(" ", 2)
            (named if len(parts) > 2 else models)[parts[2] if len(parts) > 2 else f"model {parts[0]}"] += k
        shown = named.most_common(2) or models.most_common(2)
        out.append(f"| {label} | {', '.join(n for n, _ in shown)} | {len(use_layouts[tid])} |")
    out.append("\n## Every template\n")
    out.append("| Event | Name | Layouts | Starts |\n|---|---|---:|---:|")
    for tid in sorted(lib, key=lambda t: (-len(use_layouts.get(t, ())), t)):
        out.append(f"| {tid} | {names.get(tid, '')} | {len(use_layouts.get(tid, ()))} | {use_starts.get(tid, 0)} |")
    with open(sys.argv[3], "w", encoding="utf-8") as fo:
        fo.write("\n".join(out) + "\n")
    print(f"{len(lib)} templates, {n_layouts} layout scripts -> {sys.argv[3]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
