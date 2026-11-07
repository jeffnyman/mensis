# SPDX-License-Identifier: GPL-3.0-or-later
"""Disassemble Lua 5.0 bytecode (Bloodborne's enemy AI) and diff it between builds.

    python -I tools/lua50.py dis  <file.lua | file.luabnd.dcx> [name-filter]
    python -I tools/lua50.py diff <old.luabnd.dcx> <new.luabnd.dcx> <out.md>

The AI scripts in script/*.luabnd.dcx are compiled Lua 5.0 chunks ("\\x1bLuaP"). There is no
source, but the 5.0 format is small:

  header   "\\x1bLua", 0x50, endianness, sizeof int / size_t / Instruction, bit widths of the
           OP / A / B / C fields (6/8/9/9), sizeof lua_Number (8), test number 31415926.535...
  function source (string), line defined (int), #upvalues, #params, is_vararg, max stack (bytes),
           line info (int n + n ints), locals (n x {string, int startpc, int endpc}),
           upvalue names (n strings), constants (n x {type, value}), nested functions
           (n x function), code (n x instruction)
  string   size_t length including the NUL (0 = none), then the bytes

An instruction is 32 bits: OP in bits 0-5, C 6-14, B 15-23, A 24-31; Bx is B:C as 18 bits,
sBx = Bx - 131071. An RK operand >= 250 (MAXSTACK) names constant (operand - 250).

For diffing, each function is rendered with constants and global names resolved, without source
line numbers or instruction numbers, and with jump targets relative. A recompiled but unchanged
function then compares equal, a changed number shows up as a changed number, and an inserted
block doesn't renumber everything after it. Functions are matched by the global name they are
assigned to.
"""

from __future__ import annotations

import difflib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls  # noqa: E402

OPS = ["MOVE", "LOADK", "LOADBOOL", "LOADNIL", "GETUPVAL", "GETGLOBAL", "GETTABLE", "SETGLOBAL",
       "SETUPVAL", "SETTABLE", "NEWTABLE", "SELF", "ADD", "SUB", "MUL", "DIV", "POW", "UNM", "NOT",
       "CONCAT", "JMP", "EQ", "LT", "LE", "TEST", "CALL", "TAILCALL", "RETURN", "FORLOOP",
       "TFORLOOP", "TFORPREP", "SETLIST", "SETLISTO", "CLOSE", "CLOSURE"]
MAXSTACK = 250


class Func:
    def __init__(self):
        self.source = self.locals = self.upvals = self.consts = self.protos = self.code = None
        self.nparams = self.vararg = 0


class Reader:
    def __init__(self, data: bytes):
        self.d, self.p = data, 0
        if data[:5] != b"\x1bLuaP":
            raise ValueError("not a Lua 5.0 chunk")
        sizes = data[6:14]
        if tuple(sizes) != (4, 8, 4, 6, 8, 9, 9, 8):
            raise ValueError(f"unexpected Lua 5.0 build sizes {tuple(sizes)}")
        self.p = 14 + 8  # header + test number

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def i32(self):
        v = struct.unpack_from("<i", self.d, self.p)[0]
        self.p += 4
        return v

    def string(self):
        n = struct.unpack_from("<Q", self.d, self.p)[0]
        self.p += 8
        if n == 0:
            return None
        s = self.d[self.p:self.p + n - 1].decode("shift_jis", "replace")
        self.p += n
        return s

    def function(self) -> Func:
        f = Func()
        f.source = self.string()
        self.i32()  # line defined
        self.u8()   # nups
        f.nparams, f.vararg = self.u8(), self.u8()
        self.u8()   # max stack
        n = self.i32()
        self.p += 4 * n  # line info
        f.locals = []
        for _ in range(self.i32()):
            f.locals.append(self.string())
            self.i32(); self.i32()
        f.upvals = [self.string() for _ in range(self.i32())]
        f.consts = []
        for _ in range(self.i32()):
            t = self.u8()
            if t == 3:  # number
                f.consts.append(struct.unpack_from("<d", self.d, self.p)[0])
                self.p += 8
            elif t == 4:  # string
                f.consts.append(self.string())
            elif t == 0:
                f.consts.append(None)
            else:
                raise ValueError(f"constant type {t}")
        f.protos = [self.function() for _ in range(self.i32())]
        f.code = [struct.unpack_from("<I", self.d, self.p + 4 * i)[0] for i in range(self.i32())]
        self.p += 4 * len(f.code)
        return f


def kfmt(v):
    if isinstance(v, str):
        return repr(v)
    if isinstance(v, float):
        # Lua 5.0 numbers are doubles; show whole numbers exactly (event flags, entity IDs)
        return str(int(v)) if v.is_integer() and abs(v) < 2**53 else repr(v)
    return "nil"


def render(f: Func, name: str = "main", depth: int = 0) -> list[str]:
    """Function as text: one line per instruction, constants and globals resolved, no line numbers."""
    ind = "  " * depth
    out = [f"{ind}function {name} ({f.nparams} params{', vararg' if f.vararg else ''})"]
    rk = lambda x: kfmt(f.consts[x - MAXSTACK]) if x >= MAXSTACK else f"r{x}"
    for pc, ins in enumerate(f.code):
        op = OPS[ins & 0x3F] if (ins & 0x3F) < len(OPS) else f"op{ins & 0x3F}"
        c, b, a = (ins >> 6) & 0x1FF, (ins >> 15) & 0x1FF, ins >> 24
        bx = ins >> 6 & 0x3FFFF
        sbx = bx - 131071
        if op in ("LOADK", "GETGLOBAL", "SETGLOBAL"):
            arg = f"r{a} {kfmt(f.consts[bx])}"
        elif op in ("JMP", "FORLOOP", "TFORPREP"):
            arg = f"r{a} -> {sbx:+d}"  # relative, so an insertion elsewhere doesn't change it
        elif op == "CLOSURE":
            arg = f"r{a} = function #{bx}"
        elif op in ("GETTABLE", "SELF"):
            arg = f"r{a} r{b}[{rk(c)}]"
        elif op == "SETTABLE":
            arg = f"r{a}[{rk(b)}] = {rk(c)}"
        elif op in ("ADD", "SUB", "MUL", "DIV", "POW", "EQ", "LT", "LE"):
            arg = f"r{a} {rk(b)} {rk(c)}"
        else:
            arg = f"{a} {b} {c}"
        out.append(f"{ind}  {op:9} {arg}")  # no pc: an insertion would renumber every later line
    for i, p in enumerate(f.protos):
        out += render(p, f"{name}/#{i}", depth + 1)
    return out


def named_functions(main: Func) -> dict[str, list[str]]:
    """Top-level functions by the global name the chunk assigns them to (CLOSURE then SETGLOBAL)."""
    names = {}
    pending = {}
    for ins in main.code:
        op, a, bx = ins & 0x3F, ins >> 24, ins >> 6 & 0x3FFFF
        if OPS[op] == "CLOSURE":
            pending[a] = bx
        elif OPS[op] == "SETGLOBAL" and a in pending:
            names[main.consts[bx]] = pending.pop(a)
    out = {"(chunk)": render(Func.__new__(Func) if False else _strip(main))}
    for gname, idx in names.items():
        out[str(gname)] = render(main.protos[idx], str(gname))
    return out


def _strip(main: Func) -> Func:
    f = Func()
    f.__dict__.update(main.__dict__)
    f.protos = []
    return f


def chunks(path: str) -> dict[str, bytes]:
    data = souls.load(path)
    if data[:4] == b"BND4":
        return {os.path.basename(b.name.replace("\\", "/")): b.data
                for b in souls.bnd4_read(data) if b.data[:5] == b"\x1bLuaP"}
    return {os.path.basename(path): data}


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    if cmd == "dis":
        for name, data in chunks(sys.argv[2]).items():
            if len(sys.argv) > 3 and sys.argv[3] not in name:
                continue
            print(f"== {name}")
            print("\n".join(render(Reader(data).function())))
        return 0
    if cmd == "diff":
        old, new = chunks(sys.argv[2]), chunks(sys.argv[3])
        lines = [f"# AI script diff: {os.path.basename(sys.argv[2])}\n"]
        summary = []
        for fname in sorted(set(old) | set(new)):
            if fname not in old or fname not in new:
                summary.append(f"| {fname} | {'added' if fname in new else 'removed'} | |")
                continue
            if old[fname] == new[fname]:
                continue
            fo = named_functions(Reader(old[fname]).function())
            fn = named_functions(Reader(new[fname]).function())
            changed = [g for g in sorted(set(fo) | set(fn)) if fo.get(g) != fn.get(g)]
            if not changed:
                summary.append(f"| {fname} | recompiled only (same code; line numbers differ) | |")
                continue
            summary.append(f"| {fname} | changed | {', '.join(changed)} |")
            for g in changed:
                d = difflib.unified_diff(fo.get(g, []), fn.get(g, []), "1.00", "1.09", lineterm="", n=2)
                lines += ["", f"## {fname}: {g}", "```diff", *d, "```"]
        out = lines[:1] + ["| File | Status | Functions changed |", "|---|---|---|"] + summary + lines[1:]
        with open(sys.argv[4], "w", encoding="utf-8") as f:
            f.write("\n".join(out) + "\n")
        print("\n".join(out[:len(summary) + 3]))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
