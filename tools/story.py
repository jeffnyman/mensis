# SPDX-License-Identifier: GPL-3.0-or-later
"""The NPCs' questlines: each character's state flags, and what they say in each state.

    python -I tools/story.py ranges <base dvdroot> <update dvdroot>
    python -I tools/story.py sheet  <base dvdroot> <update dvdroot> <lo-hi> <out.md>
    python -I tools/story.py sheets <base dvdroot> <update dvdroot> <out dir>

Common event 50 (プリコンストラクタ, "pre-constructor") gives each NPC's range of state flags its
starting value: "[1003:03] skip 1 if any of lo-hi is on; [2003:02] set lo on". `ranges` lists
them, named by the events that set flags in them (see docs/the-flags.md).

A sheet is one range, built from the flag index (tools/flags.py) and the talk scripts:

* the states: each flag of the range, the events and talk states that set it, and how often
  it is read;
* the conversations: for each of the character's talk scripts (the one that reads or sets the
  range most, and those with its character number or NpcParam family), every state that speaks
  or hands an item over, with the path to it (the debug labels, c1_0, of the states on the way,
  and the branches taken), the flags it sets, and the whole conversation. A talk ID is a TalkParam row; one call plays the rows after it too.
  Lines are spoken by c1_1(<talk id>), or by a helper state group called with talk IDs as
  arguments (c6_<group>(...)), usually a pair the helper chooses between.

The sheets quote the game's dialogue in full, so they belong in work/, not in the repository.
"""

from __future__ import annotations

import collections
import io
import itertools
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import esd as esdlib
import events as evlib
import flags as flaglib
import items as itemlib
import souls


def ranges(w: itemlib.World) -> list[tuple[int, int]]:
    """The NPC state ranges common event 50 initializes, in order."""
    out = []
    ev = next(
        e
        for e in evlib.read_emevd(souls.load(w.path("event/common.emevd.dcx")))
        if e.id == 50
    )
    ins = ev.instructions
    for (c, i, raw), (c2, i2, raw2) in itertools.pairwise(ins):
        if (c, i) == (1003, 3) and (c2, i2) == (2003, 2):
            # skip count, state, id type, start, end   /   flag, state
            skip, _state, kind, lo, hi = struct.unpack_from("<BBBxII", raw)
            first = struct.unpack_from("<I", raw2)[0]
            if skip == 1 and kind == 0 and first == lo:
                out.append((lo, hi))
    return out


def working_name(refs: list[flaglib.Ref], lo: int, hi: int) -> str:
    """The most common character prefix of the events that set flags in a range."""
    who: collections.Counter[str] = collections.Counter()
    for r in refs:
        if (
            lo <= r.lo <= hi
            and r.lo == r.hi
            and r.how == "set"
            and " event " in r.where
        ):
            name = r.note.split(";")[0]
            if (
                name
                and not name.startswith("started")
                and "プリコンストラクタ" not in name
            ):
                who[re.split(r"[_＿]", name)[0]] += 1
    return ", ".join(n for n, _ in who.most_common(2))


FLAG_TEST = re.compile(r"f15\((\d+)\) ~continue (==|!=) ([01])")


def readable(cond: str) -> str:
    """A talk condition with its flag tests spelled out."""

    def flag(m: re.Match) -> str:
        on = (m[2] == "==") == (m[3] == "1")
        return f"flag {m[1]} {'on' if on else 'off'}"

    out = FLAG_TEST.sub(flag, cond).replace(" ~continue", "")
    m = re.fullmatch(r"\((flag \d+ (?:on|off))\)", out)
    return m[1] if m else out


def negate(cond: str) -> str:
    """The opposite of a readable condition (a single flag test flips; anything else is
    wrapped)."""
    m = re.fullmatch(r"\(?flag (\d+) (on|off)\)?", cond)
    if m:
        return f"flag {m[1]} {'off' if m[2] == 'on' else 'on'}"
    return f"not {cond}"


PLUMBING = ("(CallResult() != CallOngoing())", "1")


def _speech(cmds: list, rows: dict[int, str]) -> list[int]:
    """The talk IDs a state speaks: c1_1's first argument, or a helper group's arguments."""
    out = []
    for b, i, args in cmds:
        if (b, i) == (1, 1) or b == 6:
            for a in args if b == 6 else args[:1]:
                if a.isdigit() and esdlib.conversation(rows, int(a)):
                    out.append(int(a))
    return out


def _assumed(conds: list[str]) -> dict[int, str]:
    """The flag values a list of conditions requires: {flag: "on" | "off"}."""
    out = {}
    for c in conds:
        m = re.fullmatch(r"flag (\d+) (on|off)", c)
        if m:
            out[int(m[1])] = m[2]
    return out


def _overridden(cond: str, sets: list[str]) -> bool:
    """Whether a state's flag sets make an earlier test of that flag stale."""
    m = re.fullmatch(r"flag (\d+) (?:on|off)", cond)
    return bool(m) and any(x.split()[0] == m[1] for x in sets)


def _branches(
    c: dict, pre: list[str], before: list[str], edges: list[tuple[int, list[str]]]
) -> None:
    """Add a condition's branches to edges: its target taken when it holds and the earlier
    top-level conditions (before) don't; nested conditions add to their parent's."""
    cond = readable(esdlib.expr(c["eval"]))
    here = pre + ([cond] if cond not in PLUMBING else [])
    if c["target"] is not None:
        edges.append((c["target"], [negate(x) for x in before] + here))
    for sub in c["subs"]:
        _branches(sub, here, before, edges)


def script_sheet(
    e: esdlib.Esd,
    name: str,
    rows: dict[int, str],
    focus: set[int],
    gifts: dict[int, str],
) -> list[str]:
    """Every state of one talk script that speaks (or sets a flag of the range), with the path
    that leads to it: the debug labels (c1_0) of the states on the way, and the conditions of
    the branches taken, with the branches before them not taken."""
    caller_label: dict[int, list[str]] = collections.defaultdict(list)
    info: dict[tuple[int, int], dict] = {}
    for gid, states in e.groups.items():
        for sid, s in states.items():
            cmds = [
                (b, i, [esdlib.expr(a) for a in args])
                for part in ("entry", "exit", "while")
                for b, i, args in s[part]
            ]
            label = [a[0].strip('"') for b, i, a in cmds if (b, i) == (1, 0) and a]
            for b, i, _ in s["entry"]:
                if b == 6:
                    caller_label[i].extend(label)
            # branches in order: (target, conditions to take it)
            edges: list[tuple[int, list[str]]] = []
            before: list[str] = []
            for c in s["conds"]:
                _branches(c, [], before, edges)
                cond = readable(esdlib.expr(c["eval"]))
                if cond not in PLUMBING:
                    before.append(cond)
            sets = [
                f"{a[0]} {'on' if a[1] == '1' else 'off'}"
                for b, i, a in cmds
                if (b, i) == (1, 11) and len(a) > 1 and a[0].isdigit()
            ]
            # a gift: a flag that a copy of the item template waits on, set here or by a
            # helper group this state calls with the flag as an argument
            given = [
                gifts[int(x)]
                for b, i, a in cmds
                if (b, i) == (1, 11) or b == 6
                for x in (a[:1] if b == 1 else a)
                if x.isdigit() and int(x) in gifts
            ]
            info[(gid, sid)] = {
                "label": label,
                "edges": edges,
                "speaks": _speech(cmds, rows),
                "sets": sets,
                "gives": given,
            }

    out = []
    for gid in sorted(e.groups):
        # paths from the group's entry (state 0) to each state, first few kept
        paths: dict[int, list[tuple[list[str], list[str]]]] = collections.defaultdict(
            list
        )
        stack: list[tuple[int, list[str], list[str], frozenset]] = [
            (0, [], [], frozenset([0]))
        ]
        steps = 0
        while stack and steps < 20000:
            steps += 1
            sid, labels, conds, seen = stack.pop()
            st = info.get((gid, sid))
            if st is None:
                continue
            labels = labels + [x for x in st["label"] if not labels or labels[-1] != x]
            if len(paths[sid]) < 3 and (labels, conds) not in paths[sid]:
                paths[sid].append((labels, conds))
            # what this path now knows about flags: its tests, updated by this state's sets
            known = _assumed(conds)
            for x in st["sets"]:
                f, v = x.split()
                known[int(f)] = v
            for target, c in reversed(st["edges"]):
                if target in seen:
                    continue
                if any(known.get(f, v) != v for f, v in _assumed(c).items()):
                    continue  # contradicts what the path already knows: never taken
                kept = [x for x in conds if not _overridden(x, st["sets"])]
                stack.append((target, labels, kept + c, seen | {target}))
        for sid in sorted(e.groups[gid]):
            st = info[(gid, sid)]
            spoken = st["speaks"]
            sets = st["sets"]
            if (
                not spoken
                and not st["gives"]
                and not any(int(x.split()[0]) in focus for x in sets)
            ):
                continue
            group = "/".join(dict.fromkeys(caller_label.get(gid, [])))
            out.append(f"**{name} {gid}/{sid}**" + (f" ({group})" if group else ""))
            if sid not in paths:
                out.append(
                    "- **not reached** from the group's entry: no branch leads here"
                )
            for labels, conds in paths.get(sid, []):
                if labels:
                    out.append("- path: " + " › ".join(labels))
                if conds:
                    out.append("  when " + ", ".join(dict.fromkeys(conds)))
            if sets:
                out.append("- sets " + ", ".join(sets))
            for g in st["gives"]:
                out.append(f"- **gives** {g}")
            seen_text: list[list[str]] = []
            for tid in spoken:
                lines = esdlib.conversation(rows, tid)
                if lines in seen_text:
                    out.append(f"- says {tid}: (the same)")
                    continue
                seen_text.append(lines)
                out.append(f"- says {tid}:")
                out.extend(f"  > {line}" for line in lines)
            out.append("")
    return out


def talk_scripts(w: itemlib.World) -> dict[tuple[str, str], esdlib.Esd]:
    """Every talk script: {(map, script name without .esd): decoded script}."""
    out = {}
    folder = os.path.join(w.base, "script", "talk")
    for f in sorted(os.listdir(folder)):
        if f.endswith(".talkesdbnd.dcx"):
            m = f.split(".")[0]
            for s, data in esdlib.scripts(w.path(f"script/talk/{f}")).items():
                out[(m, s.removesuffix(".esd"))] = esdlib.Esd(data)
    return out


# The common item templates, started with (flag, item lot): "NPC item obtained", and "NPC item
# obtained, any number of times" (which clears its flag after the handover)
GIFTS = {
    "NPCアイテム入手_XX": "",
    "NPCアイテム入手_何度でも_XX": ", any number of times",
}


def gifts(w: itemlib.World) -> dict[int, str]:
    """{flag: what is handed over}: every start of the common item templates. A talk script (or
    an event) sets the flag, and the template awards the lot."""
    names = evlib.read_emeld(souls.load(w.path("event/common.emeld.dcx")))
    templates = {k: GIFTS[v] for k, v in names.items() if v in GIFTS}
    out = {}
    for f in sorted(os.listdir(os.path.join(w.base, "event"))):
        if not f.endswith(".emevd.dcx"):
            continue
        for ev in evlib.read_emevd(souls.load(w.path(f"event/{f}"))):
            for c, i, raw in ev.instructions:
                if (c, i) == (2000, 0) and len(raw) >= 16:
                    _, target, flag, lot = struct.unpack_from("<iIii", raw)
                    if target in templates:
                        items = ", ".join(
                            f"{n} ×{k}" if k > 1 else n for n, k, _ in w.lot(lot)
                        )
                        out[flag] = (
                            f"{items or 'nothing'} (lot {lot}, flag {flag}"
                            f"{templates[target]})"
                        )
    return out


def placed_npcs(w: itemlib.World) -> dict[int, set[int]]:
    """{talk id: {NpcParam rows of the characters placed with it}} from every map."""
    out: dict[int, set[int]] = collections.defaultdict(set)
    for m in w.maps():
        for p in w.layout(m).parts:
            if p.get("talk", 0) > 0:
                out[p["talk"]].add(p["npc"])
    return out


def sheet(
    refs: list[flaglib.Ref],
    rows: dict[int, str],
    talk: dict[tuple[str, str], esdlib.Esd],
    owners: dict[int, str],
    npcs: dict[int, set[int]],
    gifts: dict[int, str],
    lo: int,
    hi: int,
) -> list[str]:
    name = working_name(refs, lo, hi)
    inrange = [r for r in refs if lo <= r.lo <= hi and r.lo == r.hi]
    out = [f"# {lo}–{hi}: {name or '(no event sets a state)'}\n"]
    out.append("## States\n")
    out.append("| Flag | Set by | Reads |")
    out.append("|---:|---|---:|")
    for f in range(lo, hi + 1):
        mine = [r for r in inrange if r.lo == f]
        setters = []
        for r in mine:
            if r.how != "set":
                continue
            if " event " in r.where:
                ev = r.where.split(" event ")[1]
                setters.append(f"event {ev} `{r.note.split(';')[0]}`")
            elif ".esd" in r.where:
                setters.append(r.where.split()[1].removesuffix(".esd") + " (talk)")
            else:
                setters.append(r.where)
        reads = sum(r.how == "read" for r in mine)
        if setters or reads:
            out.append(
                f"| {f} | {'<br>'.join(dict.fromkeys(setters)) or '—'} | {reads} |"
            )
    # The character's talk scripts. A talk ID is map + character number (t240316 at the chapel
    # and t241316 at the window are one character), so: the script that reads or sets the
    # range most, and every script in any map with the same last three digits. Reference counts
    # alone would not do: every chapel resident's script checks on the others.
    scripts: collections.Counter[str] = collections.Counter()
    for r in inrange:
        if ".esd" in r.where:
            scripts[r.where.split()[1].removesuffix(".esd")] += 1
    focus = set(range(lo, hi + 1))
    if not scripts:
        return out
    # ...and the scripts placed with the same NpcParam family (row / 10) as the most-referencing
    # placed script: one character can be several rows (6110 and 6113, 6151 and 6155)
    top = scripts.most_common(1)[0][0]
    placed = [s for s, _ in scripts.most_common() if npcs.get(int(s[1:]))]
    family = (
        {n // 10 for n in npcs.get(int(placed[0][1:]), ()) if n < 10000}
        if placed
        else set()
    )  # human characters only: the window talkers all share c9010 row 901000
    suffixes = {top[-3:]} | ({placed[0][-3:]} if placed else set())
    mine = sorted(
        (
            k
            for k in talk
            if k[1][-3:] in suffixes
            or {n // 10 for n in npcs.get(int(k[1][1:]), ())} & family
        ),
        key=lambda k: (-scripts[k[1]], k[1]),
    )
    for m, s in mine:
        out.append(
            f"\n## {s} ({m}; {owners.get(int(s[1:]), '') or 'not placed'};"
            f" {scripts[s]} references to the range)\n"
        )
        out.extend(script_sheet(talk[(m, s)], s, rows, focus, gifts))
    return out


def main() -> int:
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    a = sys.argv
    if len(a) < 4 or a[1] not in ("ranges", "sheet", "sheets"):
        print(__doc__)
        return 2
    w = itemlib.World(a[2], a[3])
    refs = flaglib.all_refs(w)
    if a[1] == "ranges":
        for lo, hi in ranges(w):
            print(f"{lo}-{hi}  {working_name(refs, lo, hi)}")
        return 0
    rows = esdlib.talk_rows(w.path("msg/engus/menu.msgbnd.dcx"))
    talk = talk_scripts(w)
    owners = {k: ", ".join(sorted(v)) for k, v in flaglib.talk_owners(w).items()}
    npcs = placed_npcs(w)
    gift = gifts(w)
    if a[1] == "sheet" and len(a) == 6:
        lo, _, hi = a[4].partition("-")
        with open(a[5], "w", encoding="utf-8", newline="\n") as fo:
            fo.write(
                "\n".join(
                    sheet(refs, rows, talk, owners, npcs, gift, int(lo), int(hi or lo))
                )
                + "\n"
            )
        print(a[5])
        return 0
    if a[1] == "sheets" and len(a) == 5:
        os.makedirs(a[4], exist_ok=True)
        index = [
            "# NPC questlines\n",
            "| Range | Working name | Sheet |",
            "|---|---|---|",
        ]
        for lo, hi in ranges(w):
            fname = f"{lo}-{hi}.md"
            with open(
                os.path.join(a[4], fname), "w", encoding="utf-8", newline="\n"
            ) as fo:
                fo.write(
                    "\n".join(sheet(refs, rows, talk, owners, npcs, gift, lo, hi))
                    + "\n"
                )
            index.append(f"| {lo}–{hi} | {working_name(refs, lo, hi)} | `{fname}` |")
        with open(
            os.path.join(a[4], "index.md"), "w", encoding="utf-8", newline="\n"
        ) as fo:
            fo.write("\n".join(index) + "\n")
        print(f"{len(index) - 3} sheets -> {a[4]}")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
