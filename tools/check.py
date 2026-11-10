# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression check: rerun the tools on the game and compare their outputs with recorded hashes.

    python -I tools/check.py run    [--only NAME,...]   # SAME / DIFF / SKIP per check; exit 1 on any DIFF
    python -I tools/check.py record [--only NAME,...]   # write tools/check-hashes.tsv from this run
    python -I tools/check.py list                       # the checks and what each needs

Run from the repository root, after `work/` has been built (docs/rebuilding-work.md). Outputs go
to `work/check/` (gitignored, recreated each run).

The outputs are derived from the game, so they stay out of the repository; their SHA-256
hashes are in `tools/check-hashes.tsv`, which proves a match without holding any content. So
that the hashes match on any machine:
  - text is hashed after normalizing line endings (CRLF -> LF) and path separators (\\ -> /),
    since one output embeds a path and Windows writes it with backslashes;
  - images are hashed on their decoded pixels (size, mode, RGBA bytes), not the PNG file,
    whose bytes depend on the compression library's version;
  - a check whose inputs aren't present (no Ghidra outputs, say) is skipped, not failed.

Renders come from a floating-point rasterizer; a different numpy build could, in principle, move
a pixel. If only render checks differ after a library upgrade, look at the pictures before
assuming the code changed.
"""

from __future__ import annotations

import glob
import hashlib
import io
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HASHES = os.path.join(ROOT, "tools", "check-hashes.tsv")
OUT = "work/check"
B, U = "work/base/dvdroot_ps4", "work/update/dvdroot_ps4"
PY = [sys.executable, "-I"]


def _text_hash(paths: list[str]) -> str:
    h = hashlib.sha256()
    for p in sorted(paths):
        with open(p, "rb") as f:
            data = f.read().replace(b"\r\n", b"\n").replace(b"\\", b"/")
        h.update(os.path.basename(p).encode() + b"\0" + data + b"\0")
    return h.hexdigest()


def _image_hash(paths: list[str]) -> str:
    from PIL import Image

    h = hashlib.sha256()
    for p in sorted(paths):
        with Image.open(p) as im:
            rgba = im.convert("RGBA")
            h.update(os.path.basename(p).encode() + b"\0")
            h.update(f"{rgba.size}".encode() + rgba.tobytes())
    return h.hexdigest()


def _run(args: list[str], any_status: bool = False) -> str:
    """Run a command from the repository root; return its stdout.

    any_status: the command's exit status carries a finding, not a failure. The readers'
    `check` commands exit 1 when something doesn't decode (events.py always does: 121
    instructions in the shipped scripts aren't in the EMEDF), and what they print is what's
    compared. A crash (a traceback) still fails."""
    r = subprocess.run(
        args, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", check=False
    )
    if r.returncode != 0 and (not any_status or "Traceback" in r.stderr):
        raise RuntimeError(f"{' '.join(args[2:4])} failed:\n{r.stderr.strip()[-800:]}")
    return r.stdout


def _per_file(kind: str, pattern: str, make) -> tuple[list[str], list[str]]:
    """One diff per file the update replaces: (commands, outputs)."""
    cmds, outs = [], []
    for f in sorted(glob.glob(os.path.join(ROOT, U, pattern))):
        name = os.path.basename(f).split(".")[0]
        out = f"{OUT}/{kind}/{name}.md"
        cmds.append(
            make(
                name,
                f"{U}/{os.path.relpath(f, os.path.join(ROOT, U))}".replace("\\", "/"),
                out,
            )
        )
        outs.append(out)
    return cmds, outs


def checks() -> list[dict]:
    """Each check: name, what it needs, how to produce its output, and how to hash it."""
    ev, ev_out = _per_file(
        "events",
        "event/*.emevd.dcx",
        lambda m, f, o: (
            PY
            + [
                "tools/events.py",
                "diff",
                f"{B}/event/common.emedf.dcx",
                f"{B}/event/{m}.emevd.dcx",
                f,
                o,
                "--eld",
                f"{U}/event/{m}.emeld.dcx",
            ]
        ),
    )
    ai, ai_out = _per_file(
        "ai",
        "script/*.luabnd.dcx",
        lambda m, f, o: (
            PY + ["tools/lua50.py", "diff", f"{B}/script/{m}.luabnd.dcx", f, o]
        ),
    )
    tk, tk_out = _per_file(
        "talk",
        "script/talk/*.talkesdbnd.dcx",
        lambda m, f, o: (
            PY
            + [
                "tools/esd.py",
                "diff",
                f"{B}/script/talk/{m}.talkesdbnd.dcx",
                f,
                o,
                "--talk",
                f"{U}/msg/engus/menu.msgbnd.dcx",
            ]
        ),
    )
    ms, ms_out = _per_file(
        "msb",
        "map/mapstudio/*.msb.dcx",
        lambda m, f, o: (
            PY + ["tools/msb.py", "diff", f"{B}/map/mapstudio/{m}.msb.dcx", f, o]
        ),
    )
    gd = "work/ghidra/diff"
    return [
        {
            "name": "params-diff",
            "needs": [B, U],
            "text": [f"{OUT}/params.md"],
            "cmds": [
                PY
                + [
                    "tools/params.py",
                    "diff",
                    f"{B}/param/gameparam/gameparam.parambnd.dcx",
                    f"{U}/param/gameparam/gameparam.parambnd.dcx",
                    f"{B}/paramdef/paramdef.paramdefbnd.dcx",
                    f"{OUT}/params.md",
                    "--msg",
                    f"{U}/msg/engus/item.msgbnd.dcx",
                ]
            ],
        },
        {
            "name": "event-diffs",
            "needs": [B, U],
            "text": ev_out,
            "cmds": ev,
            "dirs": ["events"],
        },
        {
            "name": "ai-diffs",
            "needs": [B, U],
            "text": ai_out,
            "cmds": ai,
            "dirs": ["ai"],
        },
        {
            "name": "talk-diffs",
            "needs": [B, U],
            "text": tk_out,
            "cmds": tk,
            "dirs": ["talk"],
        },
        {
            "name": "map-diffs",
            "needs": [B, U],
            "text": ms_out,
            "cmds": ms,
            "dirs": ["msb"],
        },
        {
            "name": "readers-check",
            "needs": [B, U],
            "stdout": True,
            "cmds": [
                PY
                + ["tools/events.py", "check", f"{B}/event/common.emedf.dcx"]
                + sorted(glob.glob(f"{B}/event/*.emevd.dcx"))
                + sorted(glob.glob(f"{U}/event/*.emevd.dcx")),
                PY
                + [
                    "tools/params.py",
                    "check",
                    f"{U}/param/gameparam/gameparam.parambnd.dcx",
                    f"{B}/paramdef/paramdef.paramdefbnd.dcx",
                ],
                PY
                + ["tools/msb.py", "check"]
                + sorted(glob.glob(f"{B}/map/mapstudio/*.msb.dcx")),
                PY
                + ["tools/esd.py", "check"]
                + sorted(glob.glob(f"{B}/script/talk/*.talkesdbnd.dcx")),
            ],
        },
        {
            "name": "items",
            "needs": [B, U],
            "text": [f"{OUT}/items.md"],
            "cmds": [PY + ["tools/items.py", "world", B, U, f"{OUT}/items.md"]],
        },
        {
            "name": "chalice",
            "needs": [B, U],
            "text": [f"{OUT}/chalice.md"],
            "cmds": [PY + ["tools/chalice.py", B, U, f"{OUT}/chalice.md"]],
        },
        {
            "name": "chalice-events",
            "needs": [B, U],
            "text": [f"{OUT}/chalice-events.md"],
            "cmds": [
                PY + ["tools/chalice_events.py", B, U, f"{OUT}/chalice-events.md"]
            ],
        },
        {
            "name": "flags",
            "needs": [B, U],
            "text": [f"{OUT}/flags.tsv"],
            "cmds": [PY + ["tools/flags.py", "index", B, U, f"{OUT}/flags.tsv"]],
        },
        {
            "name": "story",
            "needs": [B, U],
            "texts": f"{OUT}/story/*.md",
            "cmds": [PY + ["tools/story.py", "sheets", B, U, f"{OUT}/story"]],
        },
        {
            "name": "unfself",
            "needs": ["work/update/eboot.bin"],
            "binary": [f"{OUT}/eboot-1.09.elf"],
            "cmds": [
                PY
                + ["tools/unfself.py", "work/update/eboot.bin", f"{OUT}/eboot-1.09.elf"]
            ],
        },
        {
            "name": "orbis-prep",
            "needs": ["work/elf/eboot-1.09.elf", "work/ref/aerolib.inl"],
            "binary": [f"{OUT}/eboot-1.09.ghidra.elf"],
            "text": [f"{OUT}/eboot-1.09.symbols.tsv"],
            "cmds": [
                PY
                + [
                    "tools/orbis_prep.py",
                    "work/elf/eboot-1.09.elf",
                    "work/ref/aerolib.inl",
                    f"{OUT}/eboot-1.09.ghidra.elf",
                    f"{OUT}/eboot-1.09.symbols.tsv",
                ]
            ],
        },
        {
            "name": "build-diff",
            "needs": [
                f"{gd}/eboot-1.00.functions.tsv",
                f"{gd}/eboot-1.09.functions.tsv",
            ],
            "text": [f"{OUT}/build-diff.md", f"{OUT}/pairs.tsv"],
            "cmds": [
                PY
                + [
                    "tools/build_diff.py",
                    f"{gd}/eboot-1.00.functions.tsv",
                    f"{gd}/eboot-1.09.functions.tsv",
                    f"{OUT}/build-diff.md",
                    "--pairs",
                    f"{OUT}/pairs.tsv",
                ]
            ],
        },
        {
            "name": "c-diff",
            "needs": [f"{gd}/pairs-1.00-to-1.09.tsv", f"{gd}/c-1.00", f"{gd}/c-1.09"],
            "text": [f"{OUT}/c-diff.md"],
            "cmds": [
                PY
                + [
                    "tools/c_diff.py",
                    f"{gd}/pairs-1.00-to-1.09.tsv",
                    f"{gd}/eboot-1.00.functions.tsv",
                    f"{gd}/eboot-1.09.functions.tsv",
                    f"{gd}/c-1.00",
                    f"{gd}/c-1.09",
                    f"{OUT}/c-diff.md",
                ]
            ],
        },
        {
            "name": "item-pictures",
            "needs": [B, U],
            "text": [f"{OUT}/items-pictures/index.md"],
            "images": f"{OUT}/items-pictures/*.png",
            "cmds": [
                PY + ["tools/textures.py", "items", B, U, f"{OUT}/items-pictures"]
            ],
        },
        {
            "name": "renders",
            "needs": [B, U],
            "images": f"{OUT}/render-*.png",
            "cmds": [
                PY
                + [
                    "tools/flver.py",
                    "render",
                    f"{B}/chr/c2010.chrbnd.dcx",
                    f"{OUT}/render-c2010.png",
                    "--size",
                    "512",
                ],
                PY
                + [
                    "tools/flver.py",
                    "render",
                    f"{B}/chr/c4020.chrbnd.dcx",
                    f"{OUT}/render-c4020.png",
                    "--size",
                    "512",
                ],
                PY
                + [
                    "tools/flver.py",
                    "render",
                    f"{B}/parts/wp_a_1063.partsbnd.dcx",
                    f"{OUT}/render-wp_a_1063.png",
                    "--size",
                    "512",
                    "--yaw",
                    "0",
                ],
                PY
                + [
                    "tools/flver.py",
                    "hunter",
                    B,
                    U,
                    f"{OUT}/render-hunter-3200-f.png",
                    "3200",
                    "--sex",
                    "f",
                    "--size",
                    "512",
                ],
            ],
        },
    ]


def run_check(c: dict) -> str | None:
    """Produce a check's outputs; return their combined hash, or None to skip."""
    if not all(os.path.exists(os.path.join(ROOT, n)) for n in c["needs"]):
        return None
    for d in c.get("dirs", []):
        os.makedirs(os.path.join(ROOT, OUT, d), exist_ok=True)
    stdout = "".join(_run(cmd, bool(c.get("stdout"))) for cmd in c["cmds"])
    parts = []
    if c.get("stdout"):
        parts.append(hashlib.sha256(stdout.replace("\r\n", "\n").encode()).hexdigest())
    if c.get("text"):
        parts.append(_text_hash([os.path.join(ROOT, p) for p in c["text"]]))
    if c.get(
        "texts"
    ):  # a glob: a folder of text outputs whose names come from the data
        parts.append(_text_hash(sorted(glob.glob(os.path.join(ROOT, c["texts"])))))
    if c.get("binary"):
        h = hashlib.sha256()
        for p in c["binary"]:
            with open(os.path.join(ROOT, p), "rb") as f:
                h.update(f.read())
        parts.append(h.hexdigest())
    if c.get("images"):
        parts.append(_image_hash(glob.glob(os.path.join(ROOT, c["images"]))))
    return hashlib.sha256("".join(parts).encode()).hexdigest()


def read_hashes() -> dict[str, str]:
    if not os.path.exists(HASHES):
        return {}
    with open(HASHES, encoding="utf-8") as f:
        rows = [
            ln.rstrip("\n").split("\t")
            for ln in f
            if ln.strip() and not ln.startswith("#")
        ]
    return {r[0]: r[1] for r in rows}


def main() -> int:
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    a = sys.argv[1:]
    if not a or a[0] not in ("run", "record", "list"):
        print(__doc__)
        return 2
    only = set(a[a.index("--only") + 1].split(",")) if "--only" in a else None
    todo = [c for c in checks() if only is None or c["name"] in only]
    if a[0] == "list":
        for c in todo:
            print(f"{c['name']:15} needs {', '.join(c['needs'])}")
        return 0
    shutil.rmtree(os.path.join(ROOT, OUT), ignore_errors=True)
    os.makedirs(os.path.join(ROOT, OUT), exist_ok=True)
    expected = read_hashes()
    got, bad = {}, 0
    for c in todo:
        try:
            h = run_check(c)
        except RuntimeError as e:
            print(f"FAIL  {c['name']}: {e}")
            bad += 1
            continue
        if h is None:
            print(f"SKIP  {c['name']} (needs {', '.join(c['needs'])})")
            continue
        got[c["name"]] = h
        if a[0] == "run":
            want = expected.get(c["name"])
            state = "SAME" if h == want else "NEW " if want is None else "DIFF"
            bad += state == "DIFF"
            print(f"{state}  {c['name']}", flush=True)
        else:
            print(f"      {c['name']}  {h[:16]}…", flush=True)
    if a[0] == "record":
        merged = {**expected, **got}
        with open(HASHES, "w", encoding="utf-8", newline="\n") as f:
            f.write(
                "# tools/check.py: SHA-256 of each check's normalized outputs (see the docstring).\n"
            )
            f.write(
                "# Regenerate with `python -I tools/check.py record` after an intended change.\n"
            )
            f.writelines(
                f"{name}\t{merged[name]}\n"
                for name in [c["name"] for c in checks() if c["name"] in merged]
            )
        print(f"{len(got)} hashes recorded -> {os.path.relpath(HASHES, ROOT)}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
