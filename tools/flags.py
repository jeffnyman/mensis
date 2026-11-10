# SPDX-License-Identifier: GPL-3.0-or-later
"""Every place an event flag is read or set: event scripts, talk scripts and param tables.

    python -I tools/flags.py index <base dvdroot> <update dvdroot> <out.tsv>
    python -I tools/flags.py show  <base dvdroot> <update dvdroot> <flag | lo-hi> ...

Bloodborne keeps its world state (doors opened, bosses dead, NPCs moved on, items picked up) in
numbered event flags. Nothing in the files names them, but every reader and writer can be found:

* Event scripts (event/*.emevd). Each instruction with a flag argument is a read (the condition
  classes [3:..] and [1003:..]) or a set (everything else: [2003:02] set, [2003:22] set a range,
  and the like). The EMEDF names the arguments, so the flag arguments are found by name, not by a
  hand-made list. Many events are templates whose flags are arguments: [2000:00] starts an event
  in a slot with arguments, and the template's parameters copy them into its instructions. Each
  start is followed (also from map scripts into common.emevd) so a template's flags are listed
  per caller. Started in slot s, an event's own flag is its id + s, set when that copy ends.
* Talk scripts (script/talk/*.talkesdbnd). f15(flag) reads a flag and c1_11(flag, state) sets
  one (the Dark Souls numbering: GetEventStatus and SetEventState). State groups are called with
  arguments (c6_<group>(...), read inside as StateGroupArg(n)); those are followed the same way.
  A talk script t<id>.esd belongs to the characters whose map placement has talk id <id>.
* Param tables. ItemLotParam.getItemFlagId is set when the lot is picked up;
  ShopLineupParam.qwcId unlocks a shop row and eventFlag is the row's own; ActionButtonParam and
  ObjActParam gate prompts and object actions on a flag.

The Chalice Dungeons' 2,641 layout scripts (event/m29_*/) are left out: their flags are the
rituals' (see docs/the-chalice-events.md). The m29.emevd library they start is included.
"""

from __future__ import annotations

import collections
import glob
import io
import os
import re
import struct
import sys
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import esd as esdlib
import events as evlib
import items as itemlib
import souls

START = (2000, 0)  # start an event: slot, event id, arguments...
TYPE_ARG = (
    "基準イベントフラグIDタイプ"  # 0: the id is a flag (1, 2: an event id or slot)
)

# Short English labels for the instructions that touch flags (the EMEDF's names are Japanese).
LABEL = {
    (3, 0): "condition: flag",
    (3, 1): "condition: flag range",
    (3, 10): "condition: count of flags on",
    (3, 12): "condition: value",
    (3, 20): "condition: value vs value",
    (1003, 0): "wait for flag",
    (1003, 1): "skip if flag",
    (1003, 2): "end if flag",
    (1003, 3): "skip if flag range",
    (1003, 4): "end if flag range",
    (1003, 101): "jump if flag",
    (1003, 103): "jump if flag range",
    (2003, 2): "set flag",
    (2003, 9): "toggle flag",
    (2003, 17): "set a random flag in range",
    (2003, 22): "set flag range",
    (2003, 25): "summon sign",
    (2003, 31): "value + 1",
    (2003, 32): "value clear",
    (2003, 41): "value operation",
    (2003, 42): "store item count as value",
    (2003, 43): "give item, count from value",
    (2003, 51): "summon NPC",
    (2004, 32): "AI command over flag range",
    (2004, 45): "register NPC humanity",
    (2005, 9): "object damage",
    (2005, 12): "delete object event",
    (2009, 0): "register ladder",
    (2009, 1): "init wandering demon",
    (2009, 2): "register wandering demon",
    (2009, 3): "register lamp",
    (2009, 5): "register fountain",
}
# The instruction set's own enums: ON・OFF・CHANGE (0 OFF, 1 ON, 2 CHANGE) for one flag, and
# 論理演算タイプ ("logical operation type": 全てON, 全てOFF, どれかがON, どれかがOFF) for a range.
SET_STATE = {0: "off", 1: "on", 2: "toggled"}
TEST_STATE = {0: "off", 1: "on", 2: "changed"}
RANGE_STATE = {0: "all on", 1: "all off", 2: "any on", 3: "any off"}
READ_CLASSES = (3, 1003)
SKIPPED: collections.Counter[str] = (
    collections.Counter()
)  # references left out, by reason


@dataclass(frozen=True, order=True)
class Ref:
    lo: int
    hi: int
    how: str  # read | set | own (an event's own flag) | param
    what: str  # what the reference does
    where: str  # file and event / talk state / param row
    note: str = ""  # event name, caller, character, items


def _flag_args(iset: evlib.InstructionSet, key) -> list[tuple]:
    """For one instruction: [(kind, (fmt, offset), (fmt, offset) | None)] of its flag arguments.

    kind "one" (a flag), "range" (start, end) or "bits" (start, bit count)."""
    layout, _ = iset.layout(key)
    named = {n: (f, o) for n, f, o in layout}
    names = [n for n, _, _ in layout]
    out = []
    used = set()
    for n in names:
        if n in used or "フラグ" not in n:
            continue
        if any(s in n for s in ("状態", "タイプ", "数", "設定")):
            continue
        for a, b in (("開始", "終了"), ("Min", "Max"), ("_開始", "_終了")):
            if a in n:
                pair = n.replace(a, b)
                if pair in named:
                    out.append(("range", named[n], named[pair]))
                    used |= {n, pair}
                    break
        else:
            if "先頭" in n:
                # its bit count: "...先頭左辺値" pairs with "使用フラグ数左辺値(Bit数)"
                suffix = n[n.index("先頭") + 2 :]
                bits = next(
                    (m for m in sorted(names, key=len) if "Bit数" in m and suffix in m),
                    None,
                )
                out.append(("bits", named[n], named[bits] if bits else None))
            else:
                out.append(("one", named[n], None))
            used.add(n)
    return out


def _u(raw: bytes, fo) -> int | None:
    fmt, off = fo
    if off + struct.calcsize(fmt) > len(raw):
        return None
    return struct.unpack_from("<" + fmt, raw, off)[0]


def event_refs(w: itemlib.World) -> list[Ref]:
    iset = evlib.InstructionSet(souls.load(w.path("event/common.emedf.dcx")))
    files = sorted(
        f[: -len(".emevd.dcx")]
        for f in os.listdir(os.path.join(w.base, "event"))
        if f.endswith(".emevd.dcx")
    )
    evs: dict[tuple[str, int], evlib.Event] = {}
    names: dict[tuple[str, int], str] = {}
    for f in files:
        for e in evlib.read_emevd(souls.load(w.path(f"event/{f}.emevd.dcx"))):
            evs[(f, e.id)] = e
        eld = w.path(f"event/{f}.emeld.dcx")
        if os.path.exists(eld):
            for eid, n in evlib.read_emeld(souls.load(eld)).items():
                names[(f, eid)] = n

    def target(f: str, eid: int) -> tuple[str, int] | None:
        for k in ((f, eid), ("common", eid)):
            if k in evs:
                return k
        return None

    def concrete(e: evlib.Event, k: int, raw: bytes, args: bytes) -> tuple[bytes, set]:
        """An instruction's arguments with this copy's parameters filled in, and the offsets
        that came from parameters (or could not be filled: no arguments)."""
        b = bytearray(raw)
        from_param = set()
        for idx, dst, src, ln in e.params:
            if idx == k:
                from_param.add(dst)
                if src + ln <= len(args):
                    b[dst : dst + ln] = args[src : src + ln]
        return bytes(b), from_param

    # copies of each event: {(file, id): {(slot, args): first caller}}; roots run with no args
    called = set()
    for (f, _), e in evs.items():
        for c, i, raw in e.instructions:
            if (c, i) == START and len(raw) >= 8:
                t = target(f, struct.unpack_from("<I", raw, 4)[0])
                if t:
                    called.add(t)
    copies: dict[tuple[str, int], dict[tuple[int, bytes], str]] = (
        collections.defaultdict(dict)
    )
    work = []
    for k in evs:
        if k not in called:
            copies[k][(0, b"")] = ""
            work.append((k, 0, b""))
    while work:
        k, _, args = work.pop()
        f, eid = k
        e = evs[k]
        for n, (c, i, raw) in enumerate(e.instructions):
            if (c, i) != START or len(raw) < 8:
                continue
            raw2, pset = concrete(e, n, raw, args)
            if pset and not args:
                continue  # a template's start with its own (unknown) arguments
            slot, tid = struct.unpack_from("<iI", raw2)
            t = target(f, tid)
            if t is None:
                continue
            key = (slot, raw2[8:])
            if key not in copies[t]:
                copies[t][key] = f"{f} {eid}"
                work.append((t, slot, raw2[8:]))

    refs: set[Ref] = set()
    flagargs: dict = {}
    for k, e in evs.items():
        f, eid = k
        name = names.get(k, "")
        for (slot, args), caller in copies.get(k, {}).items():
            where = f"{f} event {eid}"
            via = f"started by {caller}, slot {slot}" if caller else ""
            note = "; ".join(x for x in (name, via) if x)
            refs.add(
                Ref(
                    eid + slot,
                    eid + slot,
                    "own",
                    "set when the event ends",
                    where,
                    note,
                )
            )
            for n, (c, i, raw) in enumerate(e.instructions):
                key = (c, i)
                if key not in iset.instr:
                    continue
                if key not in flagargs:
                    flagargs[key] = _flag_args(iset, key)
                if not flagargs[key]:
                    continue
                raw2, pset = concrete(e, n, raw, args)
                if pset and not args:
                    # a template no one starts: its flags are arguments with no values
                    SKIPPED[f"event template never started ({f})"] += 1
                    continue
                layout, _ = iset.layout(key)
                fields = {nm: (fm, of) for nm, fm, of in layout}
                if TYPE_ARG in fields and _u(raw2, fields[TYPE_ARG]) != 0:
                    continue  # the id is an event id or slot, not a flag
                label: str = LABEL.get(key) or str(iset.instr[key][0])
                state = next(
                    (nm for nm in fields if nm in ("条件成立フラグ状態", "設定フラグ")),
                    None,
                )
                how = "read" if c in READ_CLASSES else "set"
                if state is not None:
                    sv = _u(raw2, fields[state])
                    if how == "set":
                        enum = SET_STATE
                    elif any(k == "range" for k, _, _ in flagargs[key]):
                        enum = RANGE_STATE
                    else:
                        enum = TEST_STATE
                    label += f" {enum.get(sv, sv)}" if sv is not None else ""
                for kind, a, b in flagargs[key]:
                    lo = _u(raw2, a)
                    if lo is None or lo <= 0:
                        continue
                    if kind == "one":
                        hi = lo
                    elif kind == "range":
                        hi = _u(raw2, b) if b else lo
                        hi = hi if hi and hi >= lo else lo
                    else:
                        bits = _u(raw2, b) if b else 1
                        hi = lo + max((bits or 1) - 1, 0)
                    refs.add(Ref(lo, hi, how, label, where, note))
    return sorted(refs)


GROUP_ARG = re.compile(r"StateGroupArg\((\d+)\)")
ARITH = re.compile(r"[\d\s+\-*()]+")


def _fill(text: str, args: tuple[str, ...]) -> str:
    """Substitute StateGroupArg(n) with a call's arguments and fold plain integer arithmetic."""

    def sub(m: re.Match) -> str:
        n = int(m[1])
        return f"({args[n]})" if n < len(args) else m[0]

    t = GROUP_ARG.sub(sub, text)
    if ARITH.fullmatch(t) and re.search(r"\d", t):
        try:
            return str(eval(t, {"__builtins__": {}}))  # digits, + - * and brackets only
        except (SyntaxError, ZeroDivisionError):
            pass
    return t


def _calls(text: str, name: str) -> list[list[str]]:
    """The argument lists of every call to `name` in an expression's text."""
    out = []
    for m in re.finditer(re.escape(name) + r"\(", text):
        depth, k, args, cur = 1, m.end(), [], ""
        while k < len(text) and depth:
            ch = text[k]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if not depth:
                    break
            if ch == "," and depth == 1:
                args.append(cur.strip())
                cur = ""
            else:
                cur += ch
            k += 1
        args.append(cur.strip())
        out.append(args)
    return out


def talk_owners(w: itemlib.World) -> dict[int, set[str]]:
    """{talk id: {character names}} from every map's placements."""
    out = collections.defaultdict(set)
    for m in w.maps():
        for p in w.layout(m).parts:
            if p.get("talk", 0) > 0:
                r = w.npcs.get(p["npc"])
                # a human NPC's nameId is 0; its name, if it has one, is keyed by the NpcParam ID
                key = r["nameId"] if r and r["nameId"] > 0 else p["npc"]
                n = w.npc_names.get(key, "").strip()
                out[p["talk"]].add(n or f"{p['model']} NpcParam {p['npc']}")
    return out


def talk_refs(w: itemlib.World) -> list[Ref]:
    owners = talk_owners(w)
    refs: set[Ref] = set()
    for path in sorted(glob.glob(os.path.join(w.base, "script/talk/*.talkesdbnd.dcx"))):
        path = w.path(os.path.relpath(path, w.base))
        mapname = os.path.basename(path).split(".")[0]
        for sname, data in esdlib.scripts(path).items():
            e = esdlib.Esd(data)
            tid = (
                int(sname[1:].split(".")[0])
                if sname[1:].split(".")[0].isdigit()
                else -1
            )
            who = ", ".join(sorted(owners.get(tid, ())))
            # the text of every command and condition, by (group, state)
            sites = collections.defaultdict(list)  # group -> [(state, kind, text)]

            def walk(gid, sid, c, sites=sites):
                sites[gid].append((sid, "if", esdlib.expr(c["eval"])))
                for b, i, a in c["pass"]:
                    sites[gid].append((sid, f"c{b}_{i}", [esdlib.expr(x) for x in a]))
                for s in c["subs"]:
                    walk(gid, sid, s)

            for gid, states in e.groups.items():
                for sid, s in states.items():
                    for part in ("entry", "exit", "while"):
                        for b, i, a in s[part]:
                            sites[gid].append(
                                (sid, f"c{b}_{i}", [esdlib.expr(x) for x in a])
                            )
                    for c in s["conds"]:
                        walk(gid, sid, c)
            called = {
                int(k[3:])
                for g in sites.values()
                for _, k, _ in g
                if k.startswith("c6_")
            }
            copies = collections.defaultdict(set)
            work: list[tuple[int, tuple[str, ...]]] = [
                (g, ()) for g in e.groups if g not in called
            ]
            for g, a in work:
                copies[g].add(a)
            while work:
                g, a = work.pop()
                for _, k, x in sites.get(g, []):
                    if k.startswith("c6_"):
                        t = int(k[3:])
                        filled = tuple(_fill(v, a) for v in x)
                        if t in e.groups and filled not in copies[t]:
                            copies[t].add(filled)
                            work.append((t, filled))
            for g, cs in copies.items():
                for a in cs:
                    for sid, k, x in sites.get(g, []):
                        where = f"{mapname} {sname} group {g} state {sid}"
                        found = []
                        if k == "c1_11" and x:
                            st = _fill(x[1], a) if len(x) > 1 else "?"
                            found.append(
                                (
                                    "set",
                                    "set flag "
                                    + (
                                        SET_STATE.get(int(st), st)
                                        if st.isdigit()
                                        else st
                                    ),
                                    x[0],
                                )
                            )
                        texts = [x] if k == "if" else x
                        for t in texts:
                            for args in _calls(t, "f15"):
                                if args:
                                    found.append(("read", "talk: flag", args[0]))
                        for how, what, arg in found:
                            v = _fill(arg, a)
                            if v.lstrip("-").isdigit():
                                if int(v) > 0:
                                    refs.add(Ref(int(v), int(v), how, what, where, who))
                            else:
                                SKIPPED[f"talk flag not a constant: {v[:40]}"] += 1
    return sorted(refs)


def param_refs(w: itemlib.World) -> list[Ref]:
    refs = []
    for lot, r in w.lots.items():
        f = r.get("getItemFlagId", 0)
        if f > 0:
            items = ", ".join(n for n, _, _ in w.lot(lot))
            refs.append(
                Ref(f, f, "set", "item picked up", f"ItemLotParam {lot}", items)
            )
    shop_names = {0: "EquipParamWeapon", 1: "EquipParamProtector", 3: "EquipParamGoods"}
    for rid, r in w.rows("ShopLineupParam").items():
        item = (
            w.names.get(shop_names.get(r["equipType"], ""), {}).get(r["equipId"])
            or f"item {r['equipId']}"
        ).strip()
        if r["qwcId"] > 0:
            refs.append(
                Ref(
                    r["qwcId"],
                    r["qwcId"],
                    "read",
                    "shop row unlocked",
                    f"ShopLineupParam {rid}",
                    item,
                )
            )
        if r["eventFlag"] > 0:
            refs.append(
                Ref(
                    r["eventFlag"],
                    r["eventFlag"],
                    "param",
                    "shop row's own flag (eventFlag)",
                    f"ShopLineupParam {rid}",
                    item,
                )
            )
    for table, fields in (
        ("ActionButtonParam", ("invalidFlag", "grayoutFlag")),
        ("ObjActParam", ("spQualifiedPassEventFlag",)),
    ):
        for rid, r in w.rows(table).items():
            for fld in fields:
                if r.get(fld, 0) > 1:
                    refs.append(
                        Ref(r[fld], r[fld], "read", f"{table}.{fld}", f"{table} {rid}")
                    )
    return sorted(refs)


def all_refs(w: itemlib.World) -> list[Ref]:
    return sorted(event_refs(w) + talk_refs(w) + param_refs(w))


def main() -> int:
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    a = sys.argv
    if len(a) >= 5 and a[1] in ("index", "show"):
        w = itemlib.World(a[2], a[3])
        refs = all_refs(w)
        if a[1] == "index":
            with open(a[4], "w", encoding="utf-8", newline="\n") as fo:
                fo.write("flag\tto\thow\twhat\twhere\tnote\n")
                fo.writelines(
                    f"{r.lo}\t{r.hi if r.hi != r.lo else ''}\t{r.how}\t{r.what}"
                    f"\t{r.where}\t{r.note}\n"
                    for r in refs
                )
            n = len({r.lo for r in refs if r.hi == r.lo})
            print(
                f"{len(refs)} references to {n} single flags (and {sum(r.hi != r.lo for r in refs)} ranges) -> {a[4]}"
            )
            for why, k in sorted(SKIPPED.items()):
                print(f"  left out: {k} x {why}")
            return 0
        meanings = {**itemlib.LINEUP_FLAGS, **itemlib.ownership_flags(w)}
        for q in a[4:]:
            lo, _, hi = q.partition("-")
            lo_i, hi_i = int(lo), int(hi or lo)
            hits = [r for r in refs if r.lo <= hi_i and r.hi >= lo_i]
            print(f"== {q}" + (f"  ({meanings[lo_i]})" if lo_i in meanings else ""))
            heads = {
                "set": "set by",
                "own": "an event's own flag",
                "param": "param",
                "read": "read by",
            }
            for how, head in heads.items():
                # one line per reference; a param table's rows that differ only by id share one
                lines: dict[tuple, list[str]] = {}
                for r in hits:
                    if r.how != how:
                        continue
                    table, _, rid = r.where.partition(" ")
                    param = table.endswith("Param") and rid.isdigit()
                    key = (r.lo, r.hi, r.what, table if param else r.where, r.note)
                    lines.setdefault(key, []).append(rid if param else "")
                if lines:
                    print(head)
                for (lo_r, hi_r, what, where, note), ids in lines.items():
                    span = f"{lo_r}" if lo_r == hi_r else f"{lo_r}-{hi_r}"
                    if ids[0]:
                        where += " " + ", ".join(ids)
                    print(
                        f"  {span:>17}  {what:34} {where}"
                        + (f"  [{note}]" if note else "")
                    )
            if not hits:
                print("  no references")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
