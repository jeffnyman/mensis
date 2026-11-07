# SPDX-License-Identifier: GPL-3.0-or-later
"""Diff the decompiled C of functions paired between two builds, with address noise removed.

    python -I tools/c_diff.py <pairs.tsv> <old.functions.tsv> <new.functions.tsv> \
                              <old-c-dir> <new-c-dir> <out.md> [--buckets changed,replaced,...]

<pairs.tsv> comes from build_diff.py --pairs; the C directories from Decompile.java in directory
mode (one <address>.c per function). Before diffing, each function's text is normalized:

  * FUN_<addr> in the old build becomes FUN_<new addr> when the two builds' alignment pairs that
    function, so a call to the same callee reads the same on both sides;
  * other address-bearing names (DAT_, PTR_, LAB_, switchD_, s_, u_, ...) become placeholders,
    since data moves between builds, and so do raw hex literals inside the data segment
    (--data-range);
  * local variable declarations are dropped, and decompiler variable names (local_38, iVar3,
    uStack_c4, ...) are renumbered in order of first use, so a shifted stack frame or a different
    register allocation does not show as a change.

Pairs whose normalized text is identical are reported as "code generation only": the binary
differs (register choice, instruction order, a moved constant) but the decompiled logic does not.
The rest are written as unified diffs, largest first.
"""

from __future__ import annotations

import argparse
import csv
import difflib
import io
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_diff import read, token

VAR = re.compile(
    r"\b(?:local_[0-9a-f]+|[a-z]{1,6}Var\d+|[a-z]*Stack_[0-9a-f]+|in_[A-Z0-9]+|extraout_[A-Za-z0-9_]+|unaff_[A-Za-z0-9_]+)\b"
)
# FUN_ also appears inside thunk_FUN_..., so no word boundary in front.
FUNC = re.compile(r"(?<![0-9A-Za-z])FUN_([0-9a-f]{8})\b|(?<=thunk_)FUN_([0-9a-f]{8})\b")
# PTR_<anything>_<addr> names a pointer by its target, e.g. PTR_FUN_057a88e0 or PTR_thunk_FUN_..._0576...
PTRNAME = re.compile(r"\bPTR_\w+")
ADDRNAME = re.compile(
    r"\b(_?(?:DAT|LAB|switchD|caseD|UNK|uRam|s|u)_)[0-9a-fA-F]{6,16}(?:_[0-9a-f]+)?\b"
)
HEADER = re.compile(r"^// ===== .*$", re.MULTILINE)


def alignment(old_rows, new_rows):
    """old address (int) -> new address (str) for every function paired by the alignment."""
    sm = difflib.SequenceMatcher(
        None, [token(r) for r in old_rows], [token(r) for r in new_rows], autojunk=False
    )
    m = {}
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal" or (i2 - i1) == (j2 - j1):
            for o, n in zip(old_rows[i1:i2], new_rows[j1:j2]):
                m[int(o["address"], 16)] = n["address"]
    return m


HEX = re.compile(r"\b0x[0-9a-f]{6,10}\b")
DATA_RANGE = None  # (lo, hi): raw hex literals inside the data segment are addresses, not numbers
DECL = re.compile(r"^\s+[A-Za-z_][\w ]*?[\s\*]+[A-Za-z_]\w*(?: \[[^\]]*\])?;\s*$")


def normalize(text, fmap=None):
    text = HEADER.sub("", text)
    # Drop the local-variable declarations: their order and types shift with register allocation
    # and would otherwise renumber every variable after them.
    text = "\n".join(line for line in text.splitlines() if not DECL.match(line))
    text = PTRNAME.sub("PTR_xxx", text)
    if fmap is not None:

        def remap(m):
            addr = m.group(1) or m.group(2)
            return "FUN_" + fmap.get(int(addr, 16), "?" + addr)

        text = FUNC.sub(remap, text)
    text = ADDRNAME.sub(lambda m: m.group(1) + "xxx", text)
    if DATA_RANGE:
        lo, hi = DATA_RANGE
        text = HEX.sub(
            lambda m: "0xDATA" if lo <= int(m.group(0), 16) < hi else m.group(0), text
        )
    seen = {}
    text = VAR.sub(lambda m: seen.setdefault(m.group(0), f"v{len(seen) + 1}"), text)
    return [line.rstrip() for line in text.strip().splitlines()]


def main():
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    for name in ("pairs", "old_funcs", "new_funcs", "old_c", "new_c", "out"):
        ap.add_argument(name)
    ap.add_argument("--buckets", default="changed,replaced,constants,moved+constants")
    ap.add_argument(
        "--data-range",
        default="0x54dc000-0x5ae1000",
        help="address range of the data segment (as rebased by orbis_prep.py); hex "
        "literals inside it are normalized. Default: Bloodborne's eboot.",
    )
    a = ap.parse_args()
    global DATA_RANGE
    lo, hi = (int(x, 16) for x in a.data_range.split("-"))
    DATA_RANGE = (lo, hi)
    buckets = set(a.buckets.split(","))

    fmap = alignment(read(a.old_funcs), read(a.new_funcs))
    with open(a.pairs, encoding="utf-8") as f:
        pairs = [
            r
            for r in csv.DictReader(f, delimiter="\t")
            if r["bucket"] in buckets and r["old"] and r["new"]
        ]

    same, diffs = [], []
    for p in pairs:
        try:
            with open(os.path.join(a.old_c, p["old"] + ".c"), encoding="utf-8") as f:
                old = f.read()
            with open(os.path.join(a.new_c, p["new"] + ".c"), encoding="utf-8") as f:
                new = f.read()
        except FileNotFoundError:
            continue
        header = re.search(r"// ===== (\S+)", new)
        name = header.group(1) if header else p["new"]
        lo, ln = normalize(old, fmap), normalize(new)
        if lo == ln:
            same.append((p, name))
            continue
        d = list(
            difflib.unified_diff(
                lo, ln, f"1.00 {p['old']}", f"1.09 {p['new']}", lineterm="", n=2
            )
        )
        changed = sum(
            1 for x in d if x[:1] in "+-" and not x.startswith(("+++", "---"))
        )
        diffs.append((changed, p, name, d))

    diffs.sort(key=lambda t: -t[0])
    out = [
        f"# Decompiled diff: {len(pairs)} paired functions\n",
        f"* {len(same)} differ only in code generation (identical after normalization)",
        f"* {len(diffs)} differ in decompiled logic\n",
        "| Old | New | Name | Bucket | Changed lines |",
        "|---|---|---|---|---:|",
    ]
    for c, p, name, _ in diffs:
        out.append(f"| {p['old']} | {p['new']} | {name} | {p['bucket']} | {c} |")
    out.append("\n## Code generation only\n")
    out.append(", ".join(f"{p['new']} ({name})" for p, name in same) or "none")
    for c, p, name, d in diffs:
        out.append(f"\n## {name}: {p['old']} → {p['new']} ({p['bucket']}, {c} lines)\n")
        out.append("```diff")
        out.extend(d)
        out.append("```")
    with open(a.out, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print("\n".join(out[:3]))


if __name__ == "__main__":
    main()
