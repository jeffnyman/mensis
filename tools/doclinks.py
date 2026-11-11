# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that every link in the documentation resolves.

    python -I tools/doclinks.py [<repository root>]

Reads README.md and docs/*.md. Each relative link must name a file that exists, and a link
with a #fragment must match a heading of its page, by GitHub's rule for heading anchors:
lowercase, punctuation dropped (letters of any script, digits, "_" and "-" kept), spaces
turned to "-", and "-1", "-2" added to repeats. HTML <img src="..."> paths are checked as files.
Links to web addresses aren't checked.

Prints "ok" when everything resolves, so the regression check's hash only changes when a link
breaks. Otherwise prints each broken link and exits 1.
"""

from __future__ import annotations

import io
import os
import re
import sys

LINK = re.compile(r"\]\(([^)\s]+)\)")
IMG = re.compile(r"<img[^>]*\bsrc=\"([^\"]+)\"")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
FENCE = re.compile(r"^\s*(```|~~~)")


def slug(text: str) -> str:
    s = re.sub(r"`", "", text).strip().lower()
    s = re.sub(r"[^\w\- ]", "", s)
    return s.replace(" ", "-")


def anchors(path: str) -> set[str]:
    """The heading anchors of a markdown file (headings inside code blocks don't count)."""
    out: set[str] = set()
    seen: dict[str, int] = {}
    fenced = False
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if FENCE.match(line):
                fenced = not fenced
                continue
            m = None if fenced else HEADING.match(line)
            if m:
                s = slug(m.group(2))
                n = seen.get(s, 0)
                seen[s] = n + 1
                out.add(s if n == 0 else f"{s}-{n}")
    return out


def links(path: str) -> list[tuple[int, str]]:
    """(line, target) for every link and image outside code blocks."""
    out = []
    fenced = False
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            if FENCE.match(line):
                fenced = not fenced
                continue
            if fenced:
                continue
            out += [(n, t) for t in LINK.findall(line) + IMG.findall(line)]
    return out


def broken(root: str) -> list[str]:
    pages = [os.path.join(root, "README.md")] + sorted(
        os.path.join(root, "docs", f)
        for f in os.listdir(os.path.join(root, "docs"))
        if f.endswith(".md")
    )
    cache: dict[str, set[str]] = {}
    bad = []
    for page in pages:
        here = os.path.dirname(page)
        for line, target in links(page):
            if re.match(r"[a-z]+:", target):  # https:, mailto:
                continue
            file, _, frag = target.partition("#")
            dest = os.path.normpath(os.path.join(here, file)) if file else page
            where = (
                f"{os.path.relpath(page, root).replace(os.sep, '/')}:{line}: {target}"
            )
            if not os.path.exists(dest):
                bad.append(f"{where}  (no such file)")
                continue
            if frag and dest.endswith(".md"):
                if dest not in cache:
                    cache[dest] = anchors(dest)
                if frag not in cache[dest]:
                    bad.append(f"{where}  (no such heading)")
    return bad


def main() -> int:
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    root = (
        sys.argv[1]
        if len(sys.argv) > 1
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    )
    bad = broken(root)
    if bad:
        print("\n".join(bad))
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
