# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode Bloodborne's event scripts (EMEVD) using the game's own instruction set (EMEDF).

    python -I tools/events.py check <emedf> <emevd> [<emevd> ...]
    python -I tools/events.py dump  <emedf> <emevd> [--eld <emeld>] [--event ID]
    python -I tools/events.py diff  <emedf> <old.emevd> <new.emevd> <out.md> [--eld <new.emeld>]

Maps and the game itself run "events": small programs of instructions that wait on conditions
(a flag set, the player in a region, a boss dead) and then act (open a door, play a cutscene,
set a flag, spawn an enemy). They are compiled to EMEVD files, one per map plus common.emevd.

The instruction set is not documented by the game, but it is *described* by the game: the file
event/common.emedf.dcx lists every instruction class, instruction and argument, with Japanese
names and argument types:

    [0:0]  条件グループ条件状態で判定  (judge by condition-group state)
           登録条件グループ s8, 条件成立条件状態 u8, ...

EMEDF ("EDF\\0", 64-bit offsets), as found here:
    0x10 { i64 count, i64 offset } x 9: classes (32 B), instructions (32 B), arguments (64 B),
         4 enum tables, a string index, and the UTF-16 string pool.
    class       { i64 id, i64 instruction count, i64 byte offset into instructions, i64 name }
    instruction { i64 id, i64 argument count, i64 byte offset into arguments,  i64 name }
    argument    { i64 name, i32 type, i32, ... }   names are byte offsets into the string pool
EMEVD ("EVD\\0", 64-bit):
    0x10 events {count, offset}, 0x20 instructions, 0x40 layers, 0x50 parameters,
         0x60 linked files, 0x70 argument data {length, offset}, 0x80 strings
    event       { i64 id, i64 instruction count, i64 instruction offset, i64 parameter count,
                  i64 parameter offset, u32 rest behaviour, u32 pad }               48 B
    instruction { i32 class, i32 id, i64 argument length, i64 argument offset, i64 layer }  32 B
    parameter   { i64 instruction index, i64 target byte, i64 source byte, i32 length, i32 }  32 B
EMELD ("ELD\\0"): event names; { i64 event id, i64 name offset } records and a string pool.

Argument types (checked against every instruction in every map: see `check`):
    0 u8, 1 u16, 2 u32, 3 s8, 4 s16, 5 s32, 6 f32, 8 u32
Arguments are packed in order, each aligned to its own size, and the block padded to 4.
"""

from __future__ import annotations

import argparse
import difflib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls  # noqa: E402

# [2000:00] イベント起動 ("start an event") takes any number of extra 32-bit arguments after its
# declared ones; they are passed to the event it starts. That is how one event definition serves
# many doors, enemies or items.
VARIADIC = {(2000, 0)}

ARG_TYPES = {0: ("B", 1), 1: ("H", 2), 2: ("I", 4), 3: ("b", 1), 4: ("h", 2), 5: ("i", 4), 6: ("f", 4), 8: ("I", 4)}


def _wstr(d: bytes, off: int) -> str:
    end = off
    while d[end:end + 2] != b"\0\0":
        end += 2
    return d[off:end].decode("utf-16-le")


class InstructionSet:
    def __init__(self, data: bytes):
        tabs = [struct.unpack_from("<qq", data, 0x10 + i * 16) for i in range(9)]
        (ncls, ocls), (nins, oins), (narg, oarg) = tabs[0], tabs[1], tabs[2]
        pool = tabs[8][1] if tabs[8][1] else tabs[7][1]
        # the pool is the last table before the end of the file
        pool = max(o for _, o in tabs)
        self.classes = {}
        self.instr = {}
        for c in range(ncls):
            cid, cnt, ioff, cname = struct.unpack_from("<qqqq", data, ocls + c * 32)
            self.classes[cid] = _wstr(data, pool + cname)
            for k in range(cnt):
                iid, acnt, aoff, iname = struct.unpack_from("<qqqq", data, oins + ioff + k * 32)
                args = []
                for a in range(acnt):
                    aname, atype = struct.unpack_from("<qq", data, oarg + aoff + a * 64)
                    args.append((_wstr(data, pool + aname), atype & 0xFFFFFFFF))  # i32 type, i32 other
                self.instr[(cid, iid)] = (_wstr(data, pool + iname), args)

    def layout(self, key):
        """[(name, struct format, offset)] and total length for an instruction's arguments."""
        name, args = self.instr[key]
        out, off = [], 0
        for aname, atype in args:
            fmt, size = ARG_TYPES[atype]
            off = (off + size - 1) // size * size
            out.append((aname, fmt, off))
            off += size
        return out, (off + 3) // 4 * 4


class Event:
    def __init__(self, eid, rest):
        self.id, self.rest, self.instructions, self.params = eid, rest, [], []


def read_emevd(data: bytes) -> list[Event]:
    (nev, oev), (nins, oins) = struct.unpack_from("<qq", data, 0x10), struct.unpack_from("<qq", data, 0x20)
    npar, opar = struct.unpack_from("<qq", data, 0x50)
    alen, oargs = struct.unpack_from("<qq", data, 0x70)
    events = []
    for e in range(nev):
        eid, icnt, ioff, pcnt, poff, rest = struct.unpack_from("<qqqqqI", data, oev + e * 48)
        ev = Event(eid, rest)
        for k in range(icnt):
            cls, iid, ln, ao, _layer = struct.unpack_from("<iiqqq", data, oins + ioff + k * 32)
            ev.instructions.append((cls, iid, data[oargs + ao:oargs + ao + ln]))
        for k in range(pcnt):
            idx, dst, src, ln = struct.unpack_from("<qqqi", data, opar + poff + k * 32)
            ev.params.append((idx, dst, src, ln))
        events.append(ev)
    return events


def read_emeld(data: bytes) -> dict[int, str]:
    n, off = struct.unpack_from("<qq", data, 0x10)
    pool = struct.unpack_from("<q", data, 0x38)[0]
    names = {}
    for i in range(n):
        eid, noff = struct.unpack_from("<qq", data, off + i * 16)
        try:
            names[eid] = _wstr(data, pool + noff)
        except Exception:
            pass
    return names


def fmt_value(v):
    return f"{v:g}" if isinstance(v, float) else str(v)


def render(ev: Event, iset: InstructionSet, names: dict[int, str] | None = None) -> list[str]:
    """One event as readable text; parameterised arguments show as X<source byte>."""
    title = f"Event {ev.id}" + (f"  {names[ev.id]}" if names and ev.id in names else "") + f"  (rest {ev.rest})"
    lines = [title]
    subst = {}
    for idx, dst, src, ln in ev.params:
        subst[(idx, dst)] = f"X{src}_{ln}"
    for i, (cls, iid, raw) in enumerate(ev.instructions):
        key = (cls, iid)
        if key not in iset.instr:
            # not described by the EMEDF: show the argument block as 32-bit values
            vals = [subst.get((i, o)) or str(struct.unpack_from("<i", raw, o)[0]) for o in range(0, len(raw) - 3, 4)]
            lines.append(f"    [{cls}:{iid:02d}] (not in EMEDF)(" + ", ".join(vals) + ")")
            continue
        name, _ = iset.instr[key]
        layout, length = iset.layout(key)
        args = []
        for aname, fmt, off in layout:
            v = subst.get((i, off))
            if v is None:
                v = fmt_value(struct.unpack_from("<" + fmt, raw, off)[0]) if off + struct.calcsize(fmt) <= len(raw) else "?"
            args.append(f"{aname}={v}")
        if key in VARIADIC:
            # extra 32-bit arguments, passed on to the started event
            for k, off in enumerate(range(length, len(raw) - 3, 4)):
                args.append(f"+{k + 1}={subst.get((i, off)) or struct.unpack_from('<i', raw, off)[0]}")
        lines.append(f"    [{cls}:{iid:02d}] {name}(" + ", ".join(args) + ")")
    return lines


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("emedf"); c.add_argument("emevd", nargs="+")
    d = sub.add_parser("dump")
    d.add_argument("emedf"); d.add_argument("emevd"); d.add_argument("--eld"); d.add_argument("--event", type=int)
    f = sub.add_parser("diff")
    f.add_argument("emedf"); f.add_argument("old"); f.add_argument("new"); f.add_argument("out"); f.add_argument("--eld")
    a = ap.parse_args()
    iset = InstructionSet(souls.load(a.emedf))

    if a.cmd == "check":
        total = bad = unknown = 0
        for path in a.emevd:
            for ev in read_emevd(souls.load(path)):
                for cls, iid, raw in ev.instructions:
                    total += 1
                    if (cls, iid) not in iset.instr:
                        unknown += 1
                        continue
                    want = iset.layout((cls, iid))[1]
                    ok = want == len(raw) or ((cls, iid) in VARIADIC and len(raw) > want and (len(raw) - want) % 4 == 0)
                    if not ok:
                        bad += 1
        print(f"{total} instructions: {unknown} not in the EMEDF, {bad} with argument length not matching")
        print(f"instruction set: {len(iset.classes)} classes, {len(iset.instr)} instructions")
        return 0 if bad == 0 and unknown == 0 else 1

    if a.cmd == "dump":
        names = read_emeld(souls.load(a.eld)) if a.eld else None
        for ev in read_emevd(souls.load(a.emevd)):
            if a.event is None or ev.id == a.event:
                print("\n".join(render(ev, iset, names)))
        return 0

    names = read_emeld(souls.load(a.eld)) if a.eld else None
    old = {e.id: e for e in read_emevd(souls.load(a.old))}
    new = {e.id: e for e in read_emevd(souls.load(a.new))}
    added = sorted(set(new) - set(old))
    removed = sorted(set(old) - set(new))
    changed = []
    for eid in sorted(set(old) & set(new)):
        lo, ln = render(old[eid], iset, names), render(new[eid], iset, names)
        if lo != ln:
            changed.append((eid, list(difflib.unified_diff(lo, ln, "1.00", "1.09", lineterm="", n=2))))
    out = [f"# Event diff: {os.path.basename(a.old)} → {os.path.basename(a.new)}\n",
           f"{len(old)} → {len(new)} events: {len(changed)} changed, {len(added)} added, {len(removed)} removed\n"]
    for eid in added:
        out += ["", f"## Added: event {eid}", "```", *render(new[eid], iset, names), "```"]
    for eid in removed:
        out += ["", f"## Removed: event {eid}", "```", *render(old[eid], iset, names), "```"]
    for eid, d in changed:
        out += ["", f"## Changed: event {eid}" + (f" {names[eid]}" if names and eid in names else ""), "```diff", *d, "```"]
    with open(a.out, "w", encoding="utf-8") as fo:
        fo.write("\n".join(out) + "\n")
    print(out[1].strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
