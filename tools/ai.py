# SPDX-License-Identifier: GPL-3.0-or-later
"""How an enemy's AI chooses: the odds of each action by distance, health and state.

    python -I tools/ai.py tables <base dvdroot> <update dvdroot> <out dir>
    python -I tools/ai.py enemy  <base dvdroot> <update dvdroot> <name or NpcParam row>
    python -I tools/ai.py paths  <base dvdroot> <update dvdroot> <NpcParam row> [<distance>]

`tables` writes ai.md (one line per enemy) and ai/<name> <row>.md for every enemy with a battle
script. `enemy` prints the enemy's choices: for each distance (and health) band, the chance of each
action and the attacks it makes; then what each state flag changes. `paths` prints every path
through the choosing function, with its conditions and weights.

How the choice works (docs/the-combat.md, "How an enemy chooses"):

* The battle script's <name>Battle_Activate function fills a table of weights, one per
  action (odds[n] = weight for <name>_ActNN), and hands it to Common_Battle_Activate
  (aicommon.luabnd, common_battle_func.lua). That function sums the weights, rolls
  GetRandam_Int(1, sum) and walks the table: each action's chance is its weight / the sum.
* The weights depend on what Activate asks the game: the distance to the player
  (GetDist(TARGET_ENE_0)), its own health (GetHpRate(TARGET_SELF)), state numbers it keeps
  (GetNumber / SetNumber), timers (IsFinishTimer), special effects (HasSpecialEffectId),
  events (GetEventRequest), and its own random rolls (GetRandam_Int).
* This tool runs Activate for one state of the world at a time: the values the game supplies
  (distance, health, numbers, timers, effects) are fixed, and only its random rolls fork the
  run, each path weighted by the chance of its roll's range. Every path ends with a weight
  table, or with animations queued directly (AddSubGoal), reported as forced. The bands of
  distance and health come from the thresholds the runs meet, found by running again until no
  new ones appear.
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
import enemies as enemylib
import events as evlib
import flags as flaglib
import items as itemlib
import lua50

OPS = lua50.OPS
MAXSTACK = lua50.MAXSTACK
DIMENSIONS = ("GetDist(TARGET_ENE_0)", "GetHpRate(TARGET_SELF)")


class Sym:
    """A value the game supplies, or a random roll (rand=(lo, hi))."""

    def __init__(self, text: str, rand: tuple[int, int] | None = None):
        self.text, self.rand = text, rand

    def __repr__(self):
        return self.text


class Table:
    def __init__(self, tid: int):
        self.tid, self.items = tid, {}


class Global:
    def __init__(self, name):
        self.name = name


class Method:
    def __init__(self, name):
        self.name = name


def show(v) -> str:
    if isinstance(v, float):
        return str(int(v)) if v.is_integer() else f"{v:g}"
    if isinstance(v, Global):
        return v.name
    if isinstance(v, bool):
        return "true" if v else "false"
    if v is None:
        return "nil"
    return str(v)


class Path:
    def __init__(self):
        self.pc = 0
        self.regs: dict[int, object] = {}
        # (game value, comparison, (constant, which side the game value is on), outcome)
        self.cons: list[tuple[Sym, str, tuple[object, str], bool]] = []
        self.calls: list[tuple[str, list]] = []
        self.upv: dict[int, object] = {}  # upvalues written during this run
        self.done = False
        self.result: dict | None = None

    def copy(self) -> Path:
        p = Path()
        p.pc, p.regs = self.pc, dict(self.regs)
        p.cons, p.calls = list(self.cons), list(self.calls)
        p.upv = dict(self.upv)
        return p


def holds(op: str, a, b) -> bool:
    if op == "EQ":
        return a == b
    try:
        return a < b if op == "LT" else a <= b
    except TypeError:
        return False


def satisfiable(cons: list, sym: Sym) -> bool:
    """Is there a value for sym meeting all the constraints on it?"""
    mine = [(op, k, side, t) for s, op, (k, side), t in cons if s is sym]
    consts = [k for _, k, _, _ in mine if isinstance(k, (int, float))]
    cands = [None, True, False] + [c + d for c in consts for d in (-0.5, 0, 0.5)]
    if sym.rand:
        cands = list(range(sym.rand[0], sym.rand[1] + 1))
    for v in cands:
        if all(
            holds(op, v, k) == t if side == "L" else holds(op, k, v) == t
            for op, k, side, t in mine
        ):
            return True
    return False


class Runner:
    """Runs one function symbolically: every comparison with a game value forks."""

    def __init__(
        self,
        f: lua50.Func,
        env: dict | None = None,
        limit: int = 20000,
        upvalues: list | None = None,
    ):
        self.f, self.limit = f, limit
        self.env = env or {}
        self.upvalues = upvalues or []  # the chunk-level variables' starting values
        self.syms: dict[str, Sym] = {}
        self.rolls = 0
        self.paths: list[Path] = []
        self.seen: dict[str, set] = collections.defaultdict(
            set
        )  # game value: constants met

    def sym(self, text: str) -> Sym:
        if text not in self.syms:
            self.syms[text] = Sym(text)
        return self.syms[text]

    def rk(self, p: Path, x: int):
        return self.f.consts[x - MAXSTACK] if x >= MAXSTACK else p.regs.get(x)

    def value(self, s: Sym):
        if s.text in self.env:
            return self.env[s.text]
        m = re.fullmatch(r"variable(\d+)", s.text)
        if m and int(m.group(1)) < len(self.upvalues):
            return self.upvalues[int(m.group(1))]
        return default(s)

    def run(self) -> list[Path]:
        start = Path()
        start.regs = {0: Global("ai"), 1: Global("goal")}
        todo = [start]
        while todo:
            p = todo.pop()
            while not p.done:
                forked = self.step(p)
                if forked:
                    todo.append(forked)
                if len(self.paths) + len(todo) > self.limit:
                    raise RuntimeError("too many paths")
            self.paths.append(p)
        return self.paths

    def compare(self, p: Path, op: str, a, b, want: bool) -> Path | None:
        """Branch on (a op b) == want: the true outcome runs the next instruction (a JMP)."""
        sa, sb = isinstance(a, Sym), isinstance(b, Sym)
        if not sa and not sb:
            res = (
                holds(op, a, b)
                if not (isinstance(a, Table) or isinstance(b, Table))
                else a is b
            )
            p.pc += 1 if res == want else 2
            return None
        s, k, side = (a, b, "L") if sa else (b, a, "R")
        if sa and sb:  # two game values: treat the comparison itself as one value
            s, k, side, op = self.sym(f"({a} {op} {b})"), True, "L", "EQ"
        if not s.rand:  # a game value: fixed by the state being run
            self.seen[s.text].add(k)
            v = self.value(s)
            res = holds(op, v, k) if side == "L" else holds(op, k, v)
            p.pc += 1 if res == want else 2
            return None
        q = p.copy()
        p.cons.append((s, op, (k, side), want))  # p: the comparison comes out as `want`
        q.cons.append((s, op, (k, side), not want))
        p.pc += 1
        q.pc += 2
        ok_p, ok_q = satisfiable(p.cons, s), satisfiable(q.cons, s)
        if ok_p and ok_q:
            return q
        if ok_q:
            p.pc, p.regs, p.cons = q.pc, q.regs, q.cons
        return None

    def step(self, p: Path) -> Path | None:
        if p.pc >= len(self.f.code):
            p.done = True
            return None
        ins = self.f.code[p.pc]
        op = OPS[ins & 0x3F]
        c, b, a = (ins >> 6) & 0x1FF, (ins >> 15) & 0x1FF, ins >> 24
        bx = ins >> 6 & 0x3FFFF
        sbx = bx - 131071
        R = p.regs
        if op == "MOVE":
            R[a] = R.get(b)
        elif op == "LOADK":
            R[a] = self.f.consts[bx]
        elif op == "LOADBOOL":
            R[a] = bool(b)
            if c:
                p.pc += 1
        elif op == "LOADNIL":
            for r in range(a, b + 1):
                R[r] = None
        elif op == "GETUPVAL":
            # a chunk-level variable: memory kept between calls. Written earlier in this run,
            # or else its value from the state (by default, the value the chunk starts it at)
            if b in p.upv:
                R[a] = p.upv[b]
            elif f"variable{b}" in self.env:
                R[a] = self.env[f"variable{b}"]
            elif b < len(self.upvalues):
                R[a] = self.upvalues[b]  # a constant the chunk set, such as a distance
            else:
                R[a] = self.sym(f"variable{b}")
        elif op == "SETUPVAL":
            p.upv[b] = R.get(a)
        elif op == "GETGLOBAL":
            R[a] = Global(self.f.consts[bx])
        elif op == "GETTABLE":
            t, k = R.get(b), self.rk(p, c)
            R[a] = (
                t.items.get(k)
                if isinstance(t, Table)
                else Global(f"{show(t)}[{show(k)}]")
            )
        elif op == "SETTABLE":
            t = R.get(a)
            if isinstance(t, Table):  # copy on write: paths share their tables
                new = Table(t.tid)
                new.items = dict(t.items)
                new.items[self.rk(p, b)] = self.rk(p, c)
                for r, v in list(R.items()):  # every register holding the same table
                    if isinstance(v, Table) and v.tid == t.tid:
                        R[r] = new
        elif op == "NEWTABLE":
            R[a] = Table(p.pc)
        elif op == "SELF":
            R[a + 1] = R.get(b)
            R[a] = Method(self.rk(p, c))
        elif op in ("ADD", "SUB", "MUL", "DIV", "POW"):
            x, y = self.rk(p, b), self.rk(p, c)
            if isinstance(x, float) and isinstance(y, float):
                try:
                    if op == "ADD":
                        R[a] = x + y
                    elif op == "SUB":
                        R[a] = x - y
                    elif op == "MUL":
                        R[a] = x * y
                    elif op == "DIV":
                        R[a] = x / y if y else 0.0
                    else:
                        R[a] = float(x**y)
                except (OverflowError, ValueError, TypeError):
                    R[a] = self.sym(f"({show(x)} {op} {show(y)})")
            else:
                R[a] = self.sym(f"({show(x)} {op} {show(y)})")
        elif op in ("UNM", "NOT", "CONCAT"):
            R[a] = self.sym(f"{op}({show(R.get(b))})")
        elif op == "JMP":
            p.pc += sbx
            if sbx < 0:
                raise RuntimeError("backward jump: a loop")
        elif op in ("EQ", "LT", "LE"):
            return self.compare(p, op, self.rk(p, b), self.rk(p, c), bool(a))
        elif op == "TEST":
            v = R.get(b)
            # 5.0: if isfalse(R(B)) == C, skip the JMP; else R(A) = R(B) and take it
            if isinstance(v, Sym) and not v.rand:
                self.seen[v.text].add(True)
                v = self.value(v)
            if isinstance(v, Sym):
                q = p.copy()
                p.cons.append(
                    (v, "EQ", (bool(c), "L"), False)
                )  # truthiness differs from C
                p.pc += 2
                q.cons.append((v, "EQ", (bool(c), "L"), True))
                q.regs[a] = v
                q.pc += 1
                return q
            truthy = v is not None and v is not False
            if (not truthy) == bool(c):
                p.pc += 2
            else:
                R[a] = v
                p.pc += 1
            return None
        elif op == "CALL":
            fn = R.get(a)
            args = [R.get(r) for r in range(a + 1, a + b)] if b else []
            name = fn.name if isinstance(fn, (Method, Global)) else show(fn)
            if isinstance(fn, Method) and args:
                args = args[1:]  # self
            p.calls.append((name, args))
            if name == "Common_Battle_Activate" and len(args) >= 3:
                # Common_Battle_Activate(ai, goal, odds, acts, after, params)
                odds = args[2] if isinstance(args[2], Table) else None
                p.result = {"odds": dict(odds.items) if odds else {}}
            if c != 1:
                if name == "GetRandam_Int":
                    lo, hi = (int(x) if isinstance(x, float) else 0 for x in args[:2])
                    self.rolls += 1
                    R[a] = Sym(f"roll{self.rolls}({lo}-{hi})", rand=(lo, hi))
                else:
                    R[a] = self.sym(f"{name}({', '.join(show(x) for x in args)})")
        elif op in ("TAILCALL", "RETURN"):
            p.done = True
            return None
        elif op == "CLOSURE":
            R[a] = Global("closure")
            p.pc += self.f.protos[bx].nups
        elif op in ("FORLOOP", "TFORLOOP", "TFORPREP"):
            raise RuntimeError(f"{op}: a loop")
        # SETGLOBAL, SETUPVAL, SETLIST, CLOSE: nothing to track
        p.pc += 1
        return None


def probability(p: Path) -> float:
    """The chance of a path's random rolls, given its other conditions."""
    prob = 1.0
    for s in {s for s, *_ in p.cons if s.rand}:
        if s.rand is None:
            continue
        lo, hi = s.rand
        vals = range(lo, hi + 1)
        ok = sum(
            1
            for v in vals
            if all(
                (holds(op, v, k) if side == "L" else holds(op, k, v)) == t
                for s2, op, (k, side), t in p.cons
                if s2 is s
            )
        )
        prob *= ok / len(vals) if len(vals) else 0
    return prob


def default(sym: Sym):
    """The state assumed when nothing says otherwise: timers finished, flags clear, and the
    player in front (inside an AI_DIR_TYPE_F cone, outside the back and side ones)."""
    if sym.text.startswith("IsFinishTimer"):
        return True
    if sym.text.startswith("IsInsideTarget"):
        return "AI_DIR_TYPE_F" in sym.text
    if sym.text.startswith("HasSpecialEffectId"):
        return False
    return 0.0


def outcome(
    act: lua50.Func, env: dict, seen: dict | None = None, depth: int = 0
) -> dict[str, float]:
    """{action or forced sequence: chance} for one state of the world. A path that ends
    without a choice but called another function of the same script (Gascoigne's Activate
    hands over to Gascoyne271000Battle) is followed into it."""
    r = Runner(act, env, upvalues=getattr(act, "start_values", None))
    paths = r.run()
    if seen is not None:
        for k, v in r.seen.items():
            seen.setdefault(k, set()).update(v)
    out: dict[str, float] = collections.defaultdict(float)
    for p in paths:
        pr = probability(p)
        if not pr:
            continue
        odds = (p.result or {}).get("odds", {})
        weights = {
            int(k): v
            for k, v in odds.items()
            if isinstance(k, float) and isinstance(v, float) and v > 0
        }
        total = sum(weights.values())
        if total > 0:
            for k, v in weights.items():
                out[f"Act{k:02d}"] += pr * v / total
        else:
            forced = []
            for name, args in p.calls:
                if name == "AddSubGoal" and len(args) > 2:
                    forced.append(
                        show(args[2])
                        if isinstance(args[2], float)
                        else show(args[0]).replace("GOAL_COMMON_", "")
                    )
                elif "Approach" in name:  # Approach_Act and the like, from aicommon
                    forced.append("approach")
            local = getattr(act, "chunk_funcs", {})
            inner = next((n for n, _ in reversed(p.calls) if n in local), None)
            if not forced and inner and depth < 3:
                sub = local[inner]
                sub.chunk_funcs = local
                for k, v in outcome(sub, env, seen, depth + 1).items():
                    out[k] += pr * v
                continue
            if forced:
                out["forced " + " → ".join(forced)] += pr
            else:
                out[
                    "all weights zero" if p.result is not None else "nothing chosen"
                ] += pr
    return dict(out)


def explore(
    act: lua50.Func, state: dict | None = None
) -> tuple[dict[str, list], dict[str, set]]:
    """Run until the distance and health thresholds stop growing: ({dimension: bands},
    {game value: constants compared with it})."""
    seen: dict[str, set] = {}
    cuts: dict[str, list[float]] = {d: [] for d in DIMENSIONS}
    for _ in range(12):
        dims = [(d, bands(cuts[d])) for d in DIMENSIONS]
        for combo in itertools.product(*[b for _, b in dims]):
            env = {d: v for (d, _), (_, v) in zip(dims, combo)}
            env.update(state or {})
            outcome(act, env, seen)
        new = {
            d: sorted({float(k) for k in seen.get(d, ()) if isinstance(k, float)})
            for d in DIMENSIONS
        }
        if new == cuts:
            break
        cuts = new
    return {d: bands(cuts[d]) for d in DIMENSIONS if cuts[d]}, seen


def bands(cuts: list[float]) -> list[tuple[str, float]]:
    """(label, a value inside) for each band the thresholds make."""
    if not cuts:
        return [("any", 0.0)]
    out = [(f"< {show(cuts[0])}", cuts[0] - 0.5)]
    for lo, hi in itertools.pairwise(cuts):
        out.append((f"{show(lo)}–{show(hi)}", (lo + hi) / 2))
    out.append((f"≥ {show(cuts[-1])}", cuts[-1] + 0.5))
    return out


def set_numbers(f: lua50.Func) -> list[tuple[int, float]]:
    """[(number, value)] an action sets: SetNumber(n, value) with constant arguments."""
    lines = lua50.render(f)
    out = []
    for i, ln in enumerate(lines):
        if "['SetNumber']" in ln:
            ks = [
                m.group(1)
                for m in (
                    re.match(r"\s*LOADK\s+r\d+ (\S+)$", x) for x in lines[i + 1 : i + 3]
                )
                if m
            ]
            if len(ks) == 2:
                try:
                    out.append((int(float(ks[0])), float(ks[1])))
                except ValueError:
                    pass
    return out


def timers(f: lua50.Func) -> list[tuple[int, float]]:
    """[(timer, seconds)] an action starts: SetTimer(n, seconds) with constant arguments."""
    lines = lua50.render(f)
    out = []
    for i, ln in enumerate(lines):
        if "['SetTimer']" in ln:
            ks = [
                m.group(1)
                for m in (
                    re.match(r"\s*LOADK\s+r\d+ (\S+)$", x) for x in lines[i + 1 : i + 3]
                )
                if m
            ]
            if len(ks) == 2:
                try:
                    out.append((int(float(ks[0])), float(ks[1])))
                except ValueError:
                    pass
    return out


def load(en: enemylib.Enemies, nid: int) -> tuple[str, lua50.Func | None, dict]:
    """(script name, Activate function, {action number: [attack animations]})."""
    t = en.think.get(en.w.npcs[nid]["aiThinkId"])
    if not t or t["battleGoalID"] <= 0:
        return "", None, {}
    fname = f"{t['battleGoalID']}_battle.lua"
    path = en.script_index().get(fname)
    chunk = lua50.chunks(path).get(fname) if path else None
    if chunk is None:
        return fname, None, {}
    main = lua50.Reader(chunk).function()
    names = {}
    pending = {}
    consts: dict[
        int, object
    ] = {}  # register: the constant the chunk last loaded into it
    code = main.code
    i = 0
    while i < len(code):
        ins = code[i]
        op, a, b, bx = ins & 0x3F, ins >> 24, (ins >> 15) & 0x1FF, ins >> 6 & 0x3FFFF
        if OPS[op] == "LOADK":
            consts[a] = main.consts[bx]
        elif OPS[op] == "LOADBOOL":
            consts[a] = bool(b)
        elif OPS[op] in ("ADD", "SUB", "MUL", "DIV"):
            # the chunk works out distances: reach minus the body, 4.2 - 1.1
            c = (ins >> 6) & 0x1FF
            x = main.consts[b - MAXSTACK] if b >= MAXSTACK else consts.get(b)
            y = main.consts[c - MAXSTACK] if c >= MAXSTACK else consts.get(c)
            if isinstance(x, float) and isinstance(y, float):
                consts[a] = {
                    "ADD": x + y,
                    "SUB": x - y,
                    "MUL": x * y,
                    "DIV": x / y if y else 0.0,
                }[OPS[op]]
            else:
                consts.pop(a, None)
        elif OPS[op] == "CLOSURE":
            proto = main.protos[bx]
            start = []
            for j in range(
                proto.nups
            ):  # pseudo-instructions: MOVE 0 <register> for each upvalue
                pseudo = code[i + 1 + j]
                start.append(
                    consts.get((pseudo >> 15) & 0x1FF, 0.0)
                    if OPS[pseudo & 0x3F] == "MOVE"
                    else 0.0
                )
            proto.start_values = start
            pending[a] = bx
            i += proto.nups
        elif OPS[op] == "SETGLOBAL" and a in pending:
            names[main.consts[bx]] = main.protos[pending.pop(a)]
        i += 1
    act = next(
        (f for g, f in names.items() if str(g).endswith("Battle_Activate")), None
    )
    if act is not None:
        act.script_funcs = {str(g): f for g, f in names.items()}
    if act is not None:  # the script's other functions that Activate may hand over to
        act.chunk_funcs = {
            str(g): f
            for g, f in names.items()
            if not re.search(
                r"(_Act\d+|Battle_Activate|_ActAfter\w*|Battle_(Update|Terminate|Interupt))$",
                str(g),
            )
        }
    model = en.model(nid)
    _, acts = en.actions(nid, set(en.animations(model)))
    by_num = {
        int(m.group(1)): {
            "anims": v,
            "timers": timers(names[g]) if g in names else [],
            "numbers": set_numbers(names[g]) if g in names else [],
        }
        for g, v in acts.items()
        if (m := re.search(r"_Act(\d+)$", g))
    }
    return fname, act, by_num


def requested(
    act: lua50.Func,
    own: dict,
    reqs: list[dict],
    labels: dict[int, str],
    dims: list | None = None,
    grid: list | None = None,
) -> list[str]:
    """The map scripts' requests to this enemy, and what each makes its AI do."""
    out = [
        "\n## What the map scripts request\n",
        (
            "Event scripts send AI requests (AI命令要求: an entity, a command, a slot). "
            "GetEventRequest(slot) returns the command until another request replaces it; "
            "-1 clears it. Conditions are the ones the event waits for before the request, in "
            "order (alternatives included).\n"
        ),
    ]
    dims = dims or []
    grid = grid or [()]
    if not reqs:
        return out + ["None found.\n"]
    seen = set()
    for q in reqs:
        key = (q["map"], q["event"], q["slot"], q["command"], tuple(q["conditions"]))
        if key in seen:
            continue
        seen.add(key)
        head = (
            f"* **{q['map']} event {q['event']}** {q['name']}: "
            + (
                ("after " + "; ".join(q["conditions"]) + ", ")
                if q["conditions"]
                else ""
            )
            + f"command {q['command']} on slot {q['slot']}"
        )
        if q["command"] == -1:
            out.append(head + ": clears the request.")
            continue
        ask = {f"GetEventRequest({q['slot']})": float(q["command"])}
        if q["slot"] == 0:
            ask["GetEventRequest()"] = float(q["command"])
        # the request's effect, averaged over the main table's distance and health bands
        before: dict[str, float] = collections.defaultdict(float)
        after: dict[str, float] = collections.defaultdict(float)
        try:
            for combo in grid:
                env = {d: v for (d, _), (_, v) in zip(dims, combo)}
                env.update(own)
                for k, v in outcome(act, env).items():
                    before[k] += v / len(grid)
                env.update(ask)
                for k, v in outcome(act, env).items():
                    after[k] += v / len(grid)
        except RuntimeError:
            out.append(head + ".")
            continue
        raised = sorted(
            (k for k in after if after[k] - before.get(k, 0) > 0.05),
            key=lambda k: -(after[k] - before.get(k, 0)),
        )
        name = lambda k: sequence(k, labels) if k.startswith("forced") else k
        if not raised:
            out.append(head + ": no change to the choice.")
        else:
            out.append(
                head
                + ": raises "
                + ", ".join(
                    f"{name(k)} from {before.get(k, 0) * 100:.0f}% to {after[k] * 100:.0f}%"
                    for k in raised[:4]
                )
                + " (on average over the bands above)."
            )
    return out


def report(
    en: enemylib.Enemies,
    nid: int,
    rows: list[int] | None = None,
    reqs: dict | None = None,
) -> list[str]:
    fname, act, acts = load(en, nid)
    name = en.name(nid)
    out = [f"# How {name} chooses\n"]
    if act is None:
        return out + [
            f"No battle script's Activate function found ({fname or 'no script'}).\n"
        ]
    try:
        own = {
            "GetNpcThinkParamID()": float(en.w.npcs[nid]["aiThinkId"])
        }  # which placement
        dimbands, seen = explore(act, own)
    except RuntimeError as e:
        return out + [f"{fname}: not followed ({e}).\n"]
    var = en.w.npcs[nid]["behaviorVariationId"]
    labels = {}
    for aid, hits in en.animations(en.model(nid)).items():
        for _, judge in hits:
            a_id = en.attack_id(var, judge)
            if a_id is not None and aid not in labels:
                labels[aid] = en.attack_labels.get(a_id, "").split("_")[-1]
    out.append(
        f"From `{fname}`'s Activate function. Each cell is the chance of that action when "
        "Activate runs, with its timers finished, its state numbers and effect checks clear, "
        "and the player in front of it.\n"
    )
    dims = list(dimbands.items())
    grid: list[tuple] = list(itertools.product(*[b for _, b in dims])) if dims else [()]

    def table(state: dict) -> list:
        res = []
        for combo in grid:
            env = {d: v for (d, _), (_, v) in zip(dims, combo)}
            env.update(state)
            res.append((combo, outcome(act, env)))
        return res

    base_state: dict = dict(own)
    results = table(base_state)
    if all(max(r.values(), default=0) > 0.99 for _, r in results):
        # one choice in every band: an opening move, until a state number changes
        only = {max(r, key=r.get) for _, r in results if r}
        best = None
        for fl in sorted(k for k in seen if k.startswith("GetNumber(")):
            for v in sorted({k for k in seen[fl] if isinstance(k, float)} | {1.0}):
                if v == default(Sym(fl)):
                    continue
                res = table({fl: v})
                score = len({k for _, r in res for k in r})
                if best is None or score > best[0]:
                    best = (score, fl, v, res)
        if best and best[0] > 1:
            _, fl, v, res = best
            setters = [
                f"Act{k:02d}"
                for k in sorted(acts)
                if (int(fl[10:-1]), v) in acts[k]["numbers"]
            ]
            out.append(
                f"At first ({fl} = 0) it chooses {', '.join(sorted(only))} every time. Once "
                f"{fl} = {show(v)}"
                + (f" (which {', '.join(setters)} sets)" if setters else "")
                + ", it chooses like this:\n"
            )
            base_state = {**own, fl: v}
            dimbands, seen2 = explore(act, base_state)
            for k, vs in seen2.items():
                seen.setdefault(k, set()).update(vs)
            dims = list(dimbands.items())
            grid = list(itertools.product(*[b for _, b in dims])) if dims else [()]
            results = table(base_state)
    keys = sorted({k for _, r in results for k in r})
    head = [("Distance" if d.startswith("GetDist") else "Health %") for d, _ in dims]
    out.append("| " + " | ".join(head + keys) + " |")
    out.append("|" + "---|" * len(head) + "---:|" * len(keys))
    for combo, r in results:
        out.append(
            "| "
            + " | ".join(
                [lab for lab, _ in combo]
                + [f"{r[k] * 100:.0f}%" if r.get(k) else "" for k in keys]
            )
            + " |"
        )
    out.append("\n## The actions\n")
    out.append(
        "| Action | Attack animations (with their attack labels) | Starts or sets |"
    )
    out.append("|---|---|---|")
    for k in sorted(acts):
        out.append(
            f"| Act{k:02d} | "
            + ", ".join(f"{a} {labels.get(a, '')}".strip() for a in acts[k]["anims"])
            + " | "
            + ", ".join(
                [f"timer {n} ({s:g} s)" for n, s in acts[k]["timers"]]
                + [f"number {n} = {show(v)}" for n, v in acts[k]["numbers"]]
            )
            + " |"
        )
    # what each state flag changes
    flags = sorted(k for k in seen if k not in DIMENSIONS and k not in base_state)
    out.append("\n## What the state changes\n")
    any_change = False
    for fl in flags:
        vals = sorted(
            {k for k in seen[fl] if not isinstance(k, (Table, Global, Sym))}, key=show
        )
        base = default(Sym(fl))
        alts = [v for v in vals if v != base and v is not None]
        if not alts:
            alts = [not base] if isinstance(base, bool) else [1.0]
        tried = set()
        for v in alts:
            if show(v) in tried:
                continue
            tried.add(show(v))
            diffs = []
            for combo, r0 in results:
                env = {d: x for (d, _), (_, x) in zip(dims, combo)}
                env.update(base_state)
                env[fl] = v
                r1 = outcome(act, env)
                if any(
                    abs(r1.get(k, 0) - r0.get(k, 0)) > 0.005 for k in set(r1) | set(r0)
                ):
                    where = ", ".join(lab for lab, _ in combo) or "always"
                    diffs.append(
                        f"{where}: "
                        + ", ".join(
                            f"{k} {r1.get(k, 0) * 100:.0f}%"
                            for k in sorted(r1)
                            if r1.get(k)
                        )
                    )
            if diffs:
                any_change = True
                out.append(
                    f"* **{fl} = {show(v)}** (normally {show(base)}): "
                    + "; ".join(diffs)
                )
    if not any_change:
        out.append("Nothing found.")
    if reqs is not None:
        mine = [q for r in (rows or [nid]) for q in reqs.get(r, [])]
        out += requested(act, base_state, mine, labels, dims, grid)
    out += inside(act, own, labels)
    return out


# The event instruction that sends an AI request, and the conditions worth naming before it.
AI_REQUEST = {
    (2004, 17),
    (2004, 32),
}  # AI命令要求 (entity, command, slot) and the flag variant
COMPARE = {
    0: "=",
    1: "≠",
    2: ">",
    3: "<",
    4: "≥",
    5: "≤",
}  # 比較タイプ, as the uses bear out


def _args(iset: evlib.InstructionSet, key, raw: bytes) -> list:
    layout, _ = iset.layout(key)
    return [
        struct.unpack_from("<" + fmt, raw, off)[0]
        if off + struct.calcsize(fmt) <= len(raw)
        else None
        for _, fmt, off in layout
    ]


def condition(iset: evlib.InstructionSet, key, raw: bytes) -> str | None:
    """An event instruction before a request, in words; None for ones not worth showing."""
    a = _args(iset, key, raw)
    if key == (4, 2):  # キャラのHP比率で判定
        return f"its health {COMPARE.get(a[2], a[2])} {a[3] * 100:g}%"
    if key == (4, 8):  # イベントメッセージIDが一致判定
        return f"its AI sends event message {a[2]}"
    if key == (4, 6):  # イベント部位＿HP
        return f"its part {a[2]} at HP {COMPARE.get(a[4], a[4])} {a[3]}"
    if key == (1001, 0):  # 経過秒数で待機
        return f"after {a[0]:g} s"
    if key == (3, 3):  # 対象同士の一定距離内外状態で判定
        return f"{'within' if a[1] == 1 else 'beyond'} {a[4]:g} m of {a[3]}"
    if key == (4, 9):  # 対象のAI状態で判定
        return f"entity {a[1]} in AI state {a[2]}"
    if key == (3, 0):  # イベントフラグ状態で判定
        return f"flag {a[3]} {flaglib.TEST_STATE.get(a[1], a[1])}"
    if key == (4, 5):  # 特殊効果IDがあるか
        return f"entity {a[1]} {'has' if a[3] else 'lacks'} special effect {a[2]}"
    if key[0] in (3, 4):
        return iset.instr[key][0]
    return None


def requests(w: itemlib.World) -> dict[int, list[dict]]:
    """{NpcParam row: [request]} from every running copy of every event: the map, the event
    and its name, the conditions met before the request, the slot and the command."""
    iset, evs, names, copies, concrete = flaglib.event_copies(w)
    npc_of: dict[int, set[int]] = collections.defaultdict(set)
    for m in w.maps():
        for part in w.layout(m).parts:
            if part.get("entity", -1) > 0 and part.get("npc", 0) > 0:
                npc_of[part["entity"]].add(part["npc"])
    out: dict[int, list[dict]] = collections.defaultdict(list)
    seen = set()
    for k, e in evs.items():
        f, eid = k
        for _, args in copies.get(k, {}):
            conds: list[str] = []
            for n, (c, i, raw) in enumerate(e.instructions):
                key = (c, i)
                if key not in iset.instr:
                    continue
                raw2, _ = concrete(e, n, raw, args)
                if key in AI_REQUEST:
                    ent, command, slot = _args(iset, key, raw2)[:3]
                    for npc in npc_of.get(ent, ()):
                        sig = (npc, f, eid, slot, command, tuple(conds))
                        if sig in seen:
                            continue
                        seen.add(sig)
                        out[npc].append(
                            {
                                "map": f,
                                "event": eid,
                                "name": names.get(k, ""),
                                "conditions": list(conds),
                                "slot": slot,
                                "command": command,
                            }
                        )
                    conds = []
                    continue
                cond = condition(iset, key, raw2)
                if cond:
                    conds.append(cond)
    return out


def sequence(key: str, labels: dict[int, str]) -> str:
    """'forced 701 → 3000 → 3001' as '701 → 3000 右フック → 3001 左フック'."""
    if key == "nothing chosen":
        return "nothing"
    steps = key.removeprefix("forced ").split(" → ")
    return " → ".join(
        f"{s} {labels.get(int(s), '')}".strip() if s.isdigit() else s for s in steps
    )


def inside(act: lua50.Func, own: dict, labels: dict[int, str]) -> list[str]:
    """Each action's own choices: by distance, what it queues and how often, and what its
    state numbers and flags change."""
    out = [
        "\n## Inside the actions\n",
        (
            "Each action is run the same way: by distance band, the animations it queues "
            "(AddSubGoal) in order, with the chance of each sequence, in the same default state. "
            '"approach" is a call to the common approach routine (Approach_Act).\n'
        ),
    ]
    funcs = getattr(act, "script_funcs", {})
    for g in sorted(funcs, key=lambda x: (len(x), x)):
        m = re.search(r"_Act(\d+)$", g)
        if not m:
            continue
        fn = funcs[g]
        fn.chunk_funcs = getattr(act, "chunk_funcs", {})
        try:
            out += one_action(fn, own, labels, int(m.group(1)))
        except RuntimeError as e:  # a loop, or too many paths
            out.append(f"### Act{int(m.group(1)):02d}\n\nNot followed ({e}).\n")
    return out


def one_action(
    fn: lua50.Func, own: dict, labels: dict[int, str], num: int
) -> list[str]:
    """One action's table: by band, the sequences it queues; then what the state changes."""
    out: list[str] = []
    if True:
        bands_, seen = explore(fn, own)
        dims = list(bands_.items())
        grid: list[tuple] = (
            list(itertools.product(*[b for _, b in dims])) if dims else [()]
        )
        rows = []
        for combo in grid:
            env = {d: v for (d, _), (_, v) in zip(dims, combo)}
            env.update(own)
            rows.append((combo, outcome(fn, env)))
        out.append(f"### Act{num:02d}\n")
        head = [
            ("Distance" if d.startswith("GetDist") else "Health %") for d, _ in dims
        ]
        out.append("| " + " | ".join(head + ["Queues"]) + " |")
        out.append("|" + "---|" * (len(head) + 1))
        for combo, r in rows:
            cell = "; ".join(
                f"{v * 100:.0f}% {sequence(k, labels)}"
                if v < 0.995
                else sequence(k, labels)
                for k, v in sorted(r.items(), key=lambda kv: -kv[1])
            )
            out.append(
                "| "
                + " | ".join([lab for lab, _ in combo] + [cell or "nothing"])
                + " |"
            )
        notes = []
        for fl in sorted(k for k in seen if k not in DIMENSIONS and k not in own):
            base = default(Sym(fl))
            vals = [
                v
                for v in sorted(
                    {k for k in seen[fl] if isinstance(k, (float, bool))}, key=show
                )
                if v != base
            ] or ([not base] if isinstance(base, bool) else [1.0])
            for v in vals[:3]:
                changed = []
                for combo, r0 in rows:
                    env = {d: x for (d, _), (_, x) in zip(dims, combo)}
                    env.update(own)
                    env[fl] = v
                    r1 = outcome(fn, env)
                    if any(
                        abs(r1.get(k, 0) - r0.get(k, 0)) > 0.005
                        for k in set(r1) | set(r0)
                    ):
                        where = ", ".join(lab for lab, _ in combo)
                        changed.append(
                            (f"{where}: " if where else "")
                            + "; ".join(
                                f"{x * 100:.0f}% {sequence(k, labels)}"
                                if x < 0.995
                                else sequence(k, labels)
                                for k, x in sorted(r1.items(), key=lambda kv: -kv[1])
                            )
                        )
                if changed:
                    notes.append(f"* {fl} = {show(v)}: " + " | ".join(changed))
        if notes:
            out.append("")
            out += notes
        out.append("")
    return out


def main() -> int:
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    a = sys.argv
    if len(a) < 5 or a[1] not in ("enemy", "paths", "tables"):
        print(__doc__)
        return 2
    en = enemylib.Enemies(itemlib.World(a[2], a[3]))
    if a[1] == "tables":
        os.makedirs(os.path.join(a[4], "ai"), exist_ok=True)
        index = [
            "# Enemy AI\n",
            (
                "Generated by `tools/ai.py tables`. One line per enemy with a battle script: its "
                "script, its distance and health bands, and whether it opens with a fixed move.\n"
            ),
            "| Enemy | Row | Script | Distance bands | Health bands | Opening | Actions it can choose |",
            "|---|---:|---|---:|---:|---|---:|",
        ]
        reqs = requests(en.w)
        done = 0
        for (model, _), rows in sorted(
            en.groups().items(), key=lambda kv: (en.name(kv[1][0]), kv[0])
        ):
            if not en.animations(model):
                continue
            lines = report(en, rows[0], rows, reqs)
            text = "\n".join(lines)
            if "No battle script" in text:
                continue
            name = f"{en.name(rows[0])} {rows[0]}"
            with open(
                os.path.join(a[4], "ai", enemylib.safe(name) + ".md"),
                "w",
                encoding="utf-8",
                newline="\n",
            ) as fo:
                fo.write(text + "\n")
            head = text.split("## The actions")[0]
            header = next((ln for ln in lines if ln.startswith("| ")), "")
            acts_chosen = len(re.findall(r"Act\d\d", header))
            opening = "yes" if "At first" in head else ""
            index.append(
                f"| {en.name(rows[0])} | {rows[0]} | "
                + (m.group(1) if (m := re.search(r"`([^`]+)`", text)) else "")
                + f" | {'yes' if 'Distance' in header else ''} | {'yes' if 'Health' in header else ''}"
                f" | {opening} | {acts_chosen} |"
            )
            done += 1
        with open(
            os.path.join(a[4], "ai.md"), "w", encoding="utf-8", newline="\n"
        ) as fo:
            fo.write("\n".join(index) + "\n")
        print(f"{done} enemies -> {a[4]}")
        return 0
    want = " ".join(a[4:])
    nid = int(want) if want.isdigit() else None
    if nid is None:
        nid = next(
            (
                rows[0]
                for rows in en.groups().values()
                if en.name(rows[0]).lower() == want.lower()
            ),
            None,
        )
    if nid is None or nid not in en.w.npcs:
        print(f"no enemy {want!r}")
        return 1
    if a[1] == "paths":
        _, act, _ = load(en, nid)
        if act is None:
            print("no Activate function")
            return 1
        for p in Runner(
            act, {"GetDist(TARGET_ENE_0)": float(a[5]) if len(a) > 5 else 0.0}
        ).run():
            conds = " and ".join(
                f"{s} {op} {show(k)}" if side == "L" else f"{show(k)} {op} {s}"
                for s, op, (k, side), t in p.cons
                if t
            ) + "".join(
                f" and not ({s} {op} {show(k)})"
                if side == "L"
                else f" and not ({show(k)} {op} {s})"
                for s, op, (k, side), t in p.cons
                if not t
            )
            odds = (p.result or {}).get("odds", {})
            print(
                f"p={probability(p):.2f} | {conds or 'always'} | "
                + ", ".join(
                    f"{show(k)}:{show(v)}"
                    for k, v in odds.items()
                    if isinstance(v, float) and v
                )
            )
        return 0
    group = next((rows for rows in en.groups().values() if nid in rows), [nid])
    print("\n".join(report(en, nid, group, requests(en.w))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
