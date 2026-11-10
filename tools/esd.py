# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode Bloodborne's talk scripts (ESD state machines) and diff them between builds.

    python -I tools/esd.py check <talkesdbnd ...>
    python -I tools/esd.py dump  <talkesdbnd | .esd> [name-filter] [--talk msgbnd]
    python -I tools/esd.py diff  <old talkesdbnd> <new talkesdbnd> <out.md> [--talk msgbnd]

NPC dialogue (and some menus) are state machines compiled to ESD ("fsSL", 64-bit). A script is a
set of state groups; each state has entry, exit and while commands, and conditions that lead to
other states. Command arguments and conditions are tiny stack-machine expressions.

Container layout follows SoulsFormats' ESD.cs (JKAnderson, MIT); the expression bytecode follows
ESDLang's EzInfixor (thefifthmatt, MIT):

  0x00-0x7F push (b - 64)          0x80 push f32      0x81 push f64     0x82 push i32
  0xA5 push UTF-16 string           0x84-0x8A call function with 0..6 args (id pushed first)
  0x8C + 0x8D neg 0x8E - 0x8F * 0x90 /  0x91 <= 0x92 >= 0x93 < 0x94 > 0x95 == 0x96 !=
  0x98 && 0x99 || 0x9A !           0xA7-0xAE SetREGn, 0xAF-0xB6 GetREGn
  0xA6 "continue if false"          0xB7 "abort if false"   0xB8 StateGroupArg
  0xB9 call result, 0xBA call ongoing, 0xA1 end

Command and function names for Bloodborne are not public, so they print as c<bank>_<id> and
f<id>. Integer arithmetic is folded (the compiler leaves 10000000 + 241*10000 + 7810 unfolded).
With --talk, folded integers that are talk IDs are annotated with the first line they speak (a
talk ID is a TalkParam row; its msgId keys the text), which is usually what makes a dialogue
change readable.
"""

from __future__ import annotations

import argparse
import difflib
import io
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls

OPS = {
    0x8C: "+",
    0x8E: "-",
    0x8F: "*",
    0x90: "/",
    0x91: "<=",
    0x92: ">=",
    0x93: "<",
    0x94: ">",
    0x95: "==",
    0x96: "!=",
    0x98: "&&",
    0x99: "||",
}
UNARY = {0x8D: "-", 0x9A: "!"}


def expr(b: bytes, talk: dict[int, str] | None = None) -> str:
    """Decode one ESD expression to infix text, folding arithmetic on constants.

    The compiler leaves ID arithmetic unfolded (10000000 + 241*10000 + 7810 + 20*n), so integers
    are folded here and only the folded values are looked up in the talk text: a raw literal such
    as 10000000 would otherwise be "annotated" with whatever line happens to have that ID.
    """
    st: list[tuple[str, object]] = []  # (text, constant value or None)

    def push_int(v):
        st.append((str(v), v))

    i = 0
    while i < len(b):
        x = b[i]
        if x <= 0x7F:
            push_int(x - 64)
        elif x == 0x80:
            v = struct.unpack_from("<f", b, i + 1)[0]
            st.append((f"{v:g}", v))
            i += 4
        elif x == 0x81:
            v = struct.unpack_from("<d", b, i + 1)[0]
            st.append((f"{v:g}", v))
            i += 8
        elif x == 0x82:
            push_int(struct.unpack_from("<i", b, i + 1)[0])
            i += 4
        elif x == 0xA5:
            j = i + 1
            while b[j : j + 2] != b"\0\0":
                j += 2
            st.append(('"' + b[i + 1 : j].decode("utf-16-le") + '"', None))
            i = j + 1
        elif 0x84 <= x <= 0x8A:
            n = x - 0x84
            args = st[len(st) - n :] if n else []
            del st[len(st) - n :]
            fid = st.pop()[0]
            st.append(
                (f"f{fid}(" + ", ".join(_show(t, v, talk) for t, v in args) + ")", None)
            )
        elif x in OPS:
            (rt, rv), (lt, lv) = st.pop(), st.pop()
            if isinstance(lv, int) and isinstance(rv, int) and OPS[x] in "+-*":
                push_int(
                    lv + rv if OPS[x] == "+" else lv - rv if OPS[x] == "-" else lv * rv
                )
            else:
                st.append(
                    (f"({_show(lt, lv, talk)} {OPS[x]} {_show(rt, rv, talk)})", None)
                )
        elif x in UNARY:
            t, v = st.pop()
            if isinstance(v, int) and UNARY[x] == "-":
                push_int(-v)
            else:
                st.append((f"{UNARY[x]}{_show(t, v, talk)}", None))
        elif 0xA7 <= x <= 0xAE:
            t, v = st.pop()
            st.append((f"SetREG{x - 0xA7}({_show(t, v, talk)})", None))
        elif 0xAF <= x <= 0xB6:
            st.append((f"GetREG{x - 0xAF}()", None))
        elif x == 0xA6:
            t, v = st.pop()
            st.append((_show(t, v, talk) + " ~continue", None))
        elif x == 0xB7:
            t, v = st.pop()
            st.append((_show(t, v, talk) + " ~abort", None))
        elif x == 0xB8:
            t, v = st.pop()
            st.append((f"StateGroupArg({t})", None))
        elif x == 0xB9:
            st.append(("CallResult()", None))
        elif x == 0xBA:
            st.append(("CallOngoing()", None))
        elif x == 0xA1:
            pass
        else:
            st.append((f"?{x:02X}", None))
        i += 1
    if len(st) != 1:
        raise ValueError(f"expression left {len(st)} values: {b.hex()}")
    return _show(*st[0], talk)


def _show(text, value, talk):
    """A finished operand: integers that are talk IDs get their subtitle line."""
    if isinstance(value, int) and talk and value in talk:
        return f'{value} /* "{talk[value][:70]}" */'
    return text


class Esd:
    """64-bit ESD ('fsSL'). States keyed by (group id, state id)."""

    def __init__(self, data: bytes):
        if data[:4] != b"fsSL":
            raise ValueError("not a 64-bit ESD")
        (
            _nsg_size,
            nsg,
            st_size,
            _nst,
            _cs,
            _ncond,
            _cc,
            _ncmd,
            _ca,
            _narg,
            _coo,
            _coc,
            _name_off,
            _name_len,
        ) = struct.unpack_from("<14i", data, 0x24)
        ds = 0x6C
        self.d, self.ds = data, ds
        p = ds + 0x18  # past i32 1, Unk70..7C, i32 0
        _sg_off, _, _nm_off, _nm_len, _, _ = struct.unpack_from("<6q", data, p)
        p += 48
        groups = []
        for _ in range(nsg):
            gid, soff, scount, _ = struct.unpack_from("<4q", data, p)
            p += 32
            groups.append((gid, soff, scount))
        self.groups: dict[int, dict[int, dict]] = {}
        for gid, soff, scount in groups:
            states = {}
            offs = {}
            for k in range(scount):
                o = soff + k * st_size
                s = self._state(o)
                states[s["id"]] = s
                offs[o] = s["id"]
            for s in states.values():
                for c in s["conds"]:
                    self._resolve(c, offs)
            self.groups[gid] = states

    def _q(self, off, n=1):
        return struct.unpack_from(f"<{n}q", self.d, self.ds + off)

    def _cmds(self, off, n):
        out = []
        for k in range(n):
            bank, cid, aoff, acnt = struct.unpack_from(
                "<iiqq", self.d, self.ds + off + k * 24
            )
            args = []
            for a in range(acnt):
                bo, bl = self._q(aoff + a * 16, 2)
                args.append(self.d[self.ds + bo : self.ds + bo + bl])
            out.append((bank, cid, args))
        return out

    def _cond(self, off):
        target, pco, pcc, sco, scc, eo, el = self._q(off, 7)
        return {
            "target": target,
            "pass": self._cmds(pco, pcc),
            "subs": [self._cond(o) for o in (self._q(sco, scc) if scc else ())],
            "eval": self.d[self.ds + eo : self.ds + eo + el],
        }

    def _state(self, off):
        sid, co, cc, eo, ec, xo, xc, wo, wc = self._q(off, 9)
        return {
            "id": sid,
            "conds": [self._cond(o) for o in (self._q(co, cc) if cc else ())],
            "entry": self._cmds(eo, ec),
            "exit": self._cmds(xo, xc),
            "while": self._cmds(wo, wc),
        }

    def _resolve(self, c, offs):
        c["target"] = offs.get(c["target"], None) if c["target"] != -1 else None
        for s in c["subs"]:
            self._resolve(s, offs)

    def render(self, talk=None) -> list[str]:
        out = []

        def cmds(label, cs, ind):
            for bank, cid, args in cs:
                out.append(
                    f"{ind}{label} c{bank}_{cid}("
                    + ", ".join(expr(a, talk) for a in args)
                    + ")"
                )

        def cond(c, ind):
            tgt = f"-> state {c['target']}" if c["target"] is not None else "(nested)"
            out.append(f"{ind}if {expr(c['eval'], talk)} {tgt}")
            cmds("  pass", c["pass"], ind + "  ")
            for s in c["subs"]:
                cond(s, ind + "    ")

        for gid in sorted(self.groups):
            out.append(f"group {gid}")
            for sid in sorted(self.groups[gid]):
                s = self.groups[gid][sid]
                out.append(f"  state {sid}")
                cmds("entry", s["entry"], "    ")
                cmds("exit", s["exit"], "    ")
                cmds("while", s["while"], "    ")
                for c in s["conds"]:
                    cond(c, "    ")
        return out


def scripts(path: str) -> dict[str, bytes]:
    data = souls.load(path)
    if data[:4] == b"BND4":
        return {
            os.path.basename(b.name.replace("\\", "/")): b.data
            for b in souls.bnd4_read(data)
        }
    return {os.path.basename(path): data}


def talk_text(msgbnd: str | None) -> dict[int, str]:
    """For each talk ID, the first line its conversation speaks (see talk_rows)."""
    rows = talk_rows(msgbnd)
    out = {}
    for k in rows:
        lines = conversation(rows, k)
        if lines:
            out[k] = lines[0]
    return out


def conversation(rows: dict[int, str], tid: int) -> list[str]:
    """The lines c1_1(tid) plays: rows tid, tid + 1, ... while they exist. Blank rows (pauses,
    or the other speaker's turns) are skipped."""
    out = []
    while tid in rows:
        if rows[tid].strip():
            out.append(rows[tid])
        tid += 1
    return out


def talk_rows(msgbnd: str | None) -> dict[int, str]:
    """Subtitle text by talk ID, blank rows included: what a script's c1_1(<talk id>) says.

    A talk ID is a TalkParam row, and the row's msgId (its first field) is the key of the line
    in the conversation (会話) table of the menu msgbnd. TalkParam is read from the game params
    of the same dvdroot (<root>/msg/engus/menu.msgbnd.dcx -> <root>/param/gameparam/...).
    (An earlier version keyed the table by talk ID directly, which quoted the wrong lines.)
    """
    if not msgbnd:
        return {}
    lines: dict[int, str] = {}
    for b in souls.bnd4_read(souls.load(msgbnd)):
        if "会話" in b.name:
            lines = {
                k: v.replace("\n", " ") for k, v in souls.fmg_read(b.data).items() if v
            }
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(msgbnd))))
    gp = os.path.join(root, "param", "gameparam", "gameparam.parambnd.dcx")
    for b in souls.bnd4_read(souls.load(gp)):
        if os.path.basename(b.name.replace("\\", "/")).startswith("TalkParam"):
            count = struct.unpack_from("<H", b.data, 0x0A)[0]
            out = {}
            for i in range(count):
                tid, off = struct.unpack_from("<i4xq", b.data, 0x40 + i * 24)
                msg = struct.unpack_from("<i", b.data, off)[0]  # msgId
                out[tid] = lines.get(msg, "")
            return out
    raise ValueError(f"no TalkParam in {gp}")


def main():
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("bnd", nargs="+")
    d = sub.add_parser("dump")
    d.add_argument("bnd")
    d.add_argument("filter", nargs="?")
    d.add_argument("--talk")
    f = sub.add_parser("diff")
    f.add_argument("old")
    f.add_argument("new")
    f.add_argument("out")
    f.add_argument("--talk")
    a = ap.parse_args()

    if a.cmd == "check":
        n = states = exprs = 0
        for path in a.bnd:
            for name, data in scripts(path).items():
                e = Esd(data)
                n += 1
                states += sum(len(g) for g in e.groups.values())
                exprs += sum(1 for line in e.render())
        print(
            f"{n} scripts, {states} states, {exprs} rendered lines; every expression reduced to one value"
        )
        return 0

    talk = talk_text(a.talk)
    if a.cmd == "dump":
        for name, data in scripts(a.bnd).items():
            if a.filter and a.filter not in name:
                continue
            print(f"== {name}")
            print("\n".join(Esd(data).render(talk)))
        return 0

    old, new = scripts(a.old), scripts(a.new)
    out = [
        f"# Talk script diff: {os.path.basename(a.old)}\n",
        "| Script | Status | Changed lines |",
        "|---|---|---:|",
    ]
    body = []
    for name in sorted(set(old) | set(new)):
        if name not in old or name not in new:
            out.append(f"| {name} | {'added' if name in new else 'removed'} | |")
            continue
        if old[name] == new[name]:
            continue
        lo, ln = Esd(old[name]).render(talk), Esd(new[name]).render(talk)
        if lo == ln:
            out.append(f"| {name} | rebuilt, same logic | 0 |")
            continue
        dl = list(difflib.unified_diff(lo, ln, "1.00", "1.09", lineterm="", n=3))
        out.append(
            f"| {name} | changed | {sum(1 for x in dl if x[:1] in '+-' and x[:3] not in ('+++', '---'))} |"
        )
        body += ["", f"## {name}", "```diff", *dl, "```"]
    with open(a.out, "w", encoding="utf-8") as fo:
        fo.write("\n".join(out + body) + "\n")
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
