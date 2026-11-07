# SPDX-License-Identifier: GPL-3.0-or-later
"""Carry a hand-written name list from one build to another through the function alignment.

    python -I tools/port_names.py <names.tsv> <from.functions.tsv> <to.functions.tsv> <out.tsv>

<from/to.functions.tsv> come from tools/ghidra/ExportFunctionHashes.java. The two function
lists are aligned exactly as tools/build_diff.py aligns them. A function name moves across only
when its function is paired with an *identical* one (same full and specific hash), so the code the
name was judged on is the code it lands on. Globals and anything not paired that way are listed
as skipped, so they can be ported by hand.
"""

from __future__ import annotations

import difflib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_diff import read, token  # noqa: E402


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    names_path, src_path, dst_path, out_path = sys.argv[1:]
    src, dst = read(src_path), read(dst_path)

    sm = difflib.SequenceMatcher(None, [token(r) for r in src], [token(r) for r in dst], autojunk=False)
    mapping = {}
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            for o, n in zip(src[i1:i2], dst[j1:j2]):
                if o["specific_hash"] == n["specific_hash"]:
                    mapping[int(o["address"], 16)] = n["address"]

    header, rows, ported, skipped = [], [], 0, []
    with open(names_path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#") or not line.strip():
                header.append(line)
                continue
            c = line.split("\t")
            if c[0] == "address":
                rows.append(line)
                continue
            new = mapping.get(int(c[0], 16)) if c[1] == "function" else None
            if new is None:
                skipped.append(f"{c[0]} {c[2]} ({c[1]})")
                continue
            c[4] = f"{c[4]} [ported from {os.path.basename(names_path)} {c[0]}]"
            c[0] = new
            rows.append("\t".join(c))
            ported += 1

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(header) + "\n")
        f.write(f"# Ported by tools/port_names.py from {os.path.basename(names_path)}; "
                f"only functions paired with identical code are carried across.\n")
        f.write("\n".join(rows) + "\n")
    print(f"ported {ported}, skipped {len(skipped)}")
    for s in skipped:
        print("  skipped", s)


if __name__ == "__main__":
    sys.exit(main())
