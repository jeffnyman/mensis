# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare two builds of a program function by function, from ExportFunctionHashes.java output.

    python -I tools/build_diff.py <old.functions.tsv> <new.functions.tsv> <out.md> [--top N] [--pairs out.tsv]

How functions are paired. A linker lays functions out in the order of the object files and of
the functions within them, and a patch build of the same game keeps that order. So both lists,
sorted by address, are the same sequence with a few edits. Each function becomes a token (its
FID full hash: the instructions with address-dependent operands masked out, or its size and
instruction count when it is too small to hash), and difflib aligns the two token sequences.

Buckets:
  identical   aligned, same full hash and same specific hash
  constants   aligned, same full hash but a different specific hash: the same instructions
              with different constant operands (a tuned number, a changed buffer size)
  changed     aligned against each other inside an edited run of equal length on both sides:
              the same function, edited
  replaced    inside an edited run whose lengths differ, paired by position as far as both
              sides go: probably the same function, less certain
  moved       left over from an edited run on one side, but the same code (same full and
              specific hash) turns up left over on the other side: relocated by the linker
  moved+constants   the same, with different constants
  removed     left over on the old side of an uneven edited run
  added       left over on the new side of an uneven edited run
"""

from __future__ import annotations

import argparse
import collections
import csv
import difflib
import io
import sys


def read(path):
    with open(path, encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE))
    rows.sort(key=lambda r: int(r["address"], 16))
    return rows


def token(r):
    return r["full_hash"] or f"tiny:{r['size']}:{r['instructions']}"


def main():
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("out")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument(
        "--pairs",
        help="also write every pairing as TSV: bucket, old address, new address",
    )
    a = ap.parse_args()
    old, new = read(a.old), read(a.new)

    sm = difflib.SequenceMatcher(
        None, [token(r) for r in old], [token(r) for r in new], autojunk=False
    )
    buckets = collections.Counter()
    pairs = collections.defaultdict(list)  # bucket -> [(old, new)]
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            for o, n in zip(old[i1:i2], new[j1:j2]):
                b = (
                    "identical"
                    if o["specific_hash"] == n["specific_hash"]
                    else "constants"
                )
                buckets[b] += 1
                pairs[b].append((o, n))
            continue
        lo, ln = i2 - i1, j2 - j1
        b = "changed" if lo == ln else "replaced"
        k = min(lo, ln)
        for o, n in zip(old[i1 : i1 + k], new[j1 : j1 + k]):
            buckets[b] += 1
            pairs[b].append((o, n))
        for o in old[i1 + k : i2]:
            buckets["removed"] += 1
            pairs["removed"].append((o, None))
        for n in new[j1 + k : j2]:
            buckets["added"] += 1
            pairs["added"].append((None, n))

    # Moved code: whole object files can land elsewhere in the new link, which an alignment sees
    # as a delete here and an insert there. Pair leftovers whose code hash matches, in order.
    pool = collections.defaultdict(collections.deque)
    for _, n in pairs["added"]:
        pool[token(n)].append(n)
    still_removed, moved_new = [], set()
    for o, _ in pairs["removed"]:
        q = pool.get(token(o))
        if q:
            n = q.popleft()
            moved_new.add(id(n))
            b = (
                "moved"
                if o["specific_hash"] == n["specific_hash"]
                else "moved+constants"
            )
            buckets[b] += 1
            pairs[b].append((o, n))
        else:
            still_removed.append((o, None))
    pairs["removed"] = still_removed
    pairs["added"] = [(o, n) for o, n in pairs["added"] if id(n) not in moved_new]
    buckets["removed"], buckets["added"] = len(pairs["removed"]), len(pairs["added"])

    def strs(r):
        return (r or {}).get("strings", "").replace("|", "/")

    lines = [
        f"# Build diff: `{a.old}` → `{a.new}`\n",
        f"{len(old):,} functions in the old build, {len(new):,} in the new (thunks excluded).\n",
        "| Bucket | Functions |",
        "|---|---:|",
    ]
    for b in (
        "identical",
        "constants",
        "moved",
        "moved+constants",
        "changed",
        "replaced",
        "removed",
        "added",
    ):
        lines.append(f"| {b} | {buckets[b]:,} |")

    def table(title, rows, key):
        rows = sorted(rows, key=key)
        lines.append(f"\n## {title} ({len(rows):,})\n")
        lines.append(
            "| Old | New | Bytes old → new | Instr. old → new | Strings referenced |"
        )
        lines.append("|---|---|---|---|---|")
        for o, n in rows[: a.top]:
            lines.append(
                "| {} | {} | {} → {} | {} → {} | {} |".format(
                    o["address"] if o else "",
                    n["address"] if n else "",
                    o["size"] if o else "–",
                    n["size"] if n else "–",
                    o["instructions"] if o else "–",
                    n["instructions"] if n else "–",
                    strs(n) or strs(o),
                )
            )
        if len(rows) > a.top:
            lines.append(f"\n…and {len(rows) - a.top:,} more.")

    delta = lambda p: -abs(int(p[1]["size"]) - int(p[0]["size"]))
    table(
        "Changed functions, largest size change first",
        pairs["changed"] + pairs["replaced"],
        delta,
    )
    table(
        "Constant-only changes",
        pairs["constants"] + pairs["moved+constants"],
        lambda p: -int(p[1]["size"]),
    )
    table("Removed functions", pairs["removed"], lambda p: -int(p[0]["size"]))
    table("Added functions", pairs["added"], lambda p: -int(p[1]["size"]))

    if a.pairs:
        with open(a.pairs, "w", encoding="utf-8", newline="\n") as f:
            f.write("bucket\told\tnew\n")
            for b, ps in pairs.items():
                if b == "identical":
                    continue
                for o, n in ps:
                    old = o["address"] if o else ""
                    new = n["address"] if n else ""
                    f.write(f"{b}\t{old}\t{new}\n")

    with open(a.out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines[:10]))


if __name__ == "__main__":
    main()
