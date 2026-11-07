# SPDX-License-Identifier: GPL-3.0-or-later
"""Textures out of the game as PNG files: the TPF container, the PS4's tiled layout, BCn decoding.

    python -I tools/textures.py tpf   <file> <out dir>
        # every texture in a .tpf[.dcx], in a BND4 that holds TPFs (.chrbnd/.partsbnd/.objbnd), or
        # in a map texture archive (.tpfbhd, with its .tpfbdt beside it)
    python -I tools/textures.py items <base dvdroot> <update dvdroot> <out dir>
        # the item pictures (menu/menu_knowledge_NNNNN.tpf.dcx), named after the items whose
        # iconId is NNNNN, plus an index.md listing every picture and every item that uses it
    python -I tools/textures.py models <base dvdroot> <update dvdroot> <out dir> [kinds] [--colour] [--low]
        # characters, equipment, objects and maps: one folder per model or map pack, named where
        # the data names it. kinds: any of chr,parts,obj,map joined by commas (default chr,parts).
        # --colour keeps only the colour textures (names ending _a); --low adds the _l parts.

Output belongs under work/ (gitignored): the pictures are the game's own artwork.

TPF (PS4), as found in this game:
    0x00 "TPF\\0", u32 total data size, u32 texture count, u8 platform (4 = PS4), u8 flag,
         u8 name encoding (1 = UTF-16)
    0x10 one 0x24-byte entry per texture:
         u32 data offset, u32 data size, u8 format, u8 type (0 = 2D), u8 mip count, u8 flags,
         u16 width, u16 height, u32 1, u32 0x0d, u32 name offset, u32 0, u32 DXGI format
    The DXGI format is the standard Direct3D enum: 71/72 BC1, 74 BC2, 77/78 BC3, 80 BC4, 83 BC5,
    98/99 BC7 (the even/odd pairs are plain and sRGB). The item pictures are BC7, 400 x 400;
    model textures are mostly BC1 sRGB and BC4, with mip levels (the full-size level first).
Containers: BND4 archives (.chrbnd, .partsbnd, .objbnd) hold TPFs; map texture packs are split
BXF4 archives, a .tpfbhd header (BHF4, BND4's layout) and a .tpfbdt holding the files.

The PS4 tiling. The GPU stores textures tiled, not row by row. For block-compressed formats the
unit is the 4x4-pixel block (8 or 16 bytes); the image is padded to a multiple of 8 blocks each
way, cut into tiles of 8 x 8 blocks laid out left to right, top to bottom, and inside a tile the
64 blocks are in Morton (Z) order: block (x, y) is at index interleave(x, y), the bits of x and y
alternating, x first. A 400 x 400 BC7 picture is 100 x 100 blocks, padded to 104 x 104: exactly
the 173,056 bytes stored. Read without untiling, the picture comes out as horizontal streaks.
Only the first mip level is decoded.
"""

from __future__ import annotations

import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls  # noqa: E402

DXGI = {  # DXGI format -> (Pillow "bcn" decoder number, bytes per 4x4 block)
    70: (1, 8), 71: (1, 8), 72: (1, 8),       # BC1
    73: (2, 16), 74: (2, 16), 75: (2, 16),    # BC2
    76: (3, 16), 77: (3, 16), 78: (3, 16),    # BC3
    79: (4, 8), 80: (4, 8), 81: (4, 8),       # BC4
    82: (5, 16), 83: (5, 16), 84: (5, 16),    # BC5
    97: (7, 16), 98: (7, 16), 99: (7, 16),    # BC7
}


def _morton8(x: int, y: int) -> int:
    """Index of block (x, y), 0 <= x, y < 8, inside an 8 x 8 tile: bits of x and y interleaved."""
    r = 0
    for i in range(3):
        r |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1)
    return r


_ORDER = [[_morton8(x, y) for x in range(8)] for y in range(8)]


def untile(data: bytes, blocks_w: int, blocks_h: int, block_bytes: int) -> tuple[bytes, int, int]:
    """PS4 tiled block data -> row-major block data. Returns (data, padded blocks w, padded blocks h)."""
    pw, ph = (blocks_w + 7) // 8 * 8, (blocks_h + 7) // 8 * 8
    need = pw * ph * block_bytes
    if len(data) < need:
        raise ValueError(f"{len(data)} bytes, tiled layout needs {need}")
    out = bytearray(need)
    tiles_x = pw // 8
    b = block_bytes
    for ty in range(ph // 8):
        for tx in range(tiles_x):
            tile = (ty * tiles_x + tx) * 64
            for y in range(8):
                row = ((ty * 8 + y) * pw + tx * 8) * b
                order = _ORDER[y]
                for x in range(8):
                    src = (tile + order[x]) * b
                    out[row + x * b: row + (x + 1) * b] = data[src: src + b]
    return bytes(out), pw, ph


class Texture:
    def __init__(self, name, width, height, dxgi, kind, mips, data):
        self.name, self.width, self.height = name, width, height
        self.dxgi, self.kind, self.mips, self.data = dxgi, kind, mips, data

    def image(self):
        """The first mip level as a Pillow RGBA image."""
        from PIL import Image
        if self.dxgi not in DXGI:
            raise ValueError(f"DXGI format {self.dxgi} not supported")
        if self.kind != 0:
            raise ValueError(f"texture type {self.kind} (not 2D) not supported")
        n, b = DXGI[self.dxgi]
        bw, bh = (self.width + 3) // 4, (self.height + 3) // 4
        flat, pw, ph = untile(self.data, bw, bh, b)
        im = Image.frombytes("RGBA" if n in (1, 2, 3, 7) else "L" if n == 4 else "RGB",
                             (pw * 4, ph * 4), flat, "bcn", n)
        return im.crop((0, 0, self.width, self.height))


def read_tpf(data: bytes) -> list[Texture]:
    if data[:4] != b"TPF\0":
        raise ValueError("not a TPF")
    count, platform = struct.unpack_from("<IB", data, 8)
    if platform != 4:
        raise ValueError(f"platform {platform}: only the PS4 layout is handled")
    out = []
    for i in range(count):
        (off, size, _fmt, kind, mips, _fl, w, h, _one, _unk, name_off, _z,
         dxgi) = struct.unpack_from("<IIBBBBHHIIIII", data, 0x10 + i * 0x24)
        end = data.index(b"\0\0", name_off)
        end += (end - name_off) % 2  # align to the UTF-16 terminator
        name = data[name_off:end].decode("utf-16-le")
        out.append(Texture(name, w, h, dxgi, kind, mips, data[off:off + size]))
    return out


def _safe(name: str) -> str:
    return re.sub(r'[<>:"/\\|?*]', "_", name).strip().rstrip(".")


def item_pictures(base: str, update: str, outdir: str) -> int:
    """menu_knowledge_NNNNN -> PNG named after the items with iconId NNNNN; writes index.md."""
    import collections
    import items as itemlib
    w = itemlib.World(base, update)
    users = collections.defaultdict(list)  # icon id -> [(table label, item name)]
    for table, fields, label in (("EquipParamWeapon", ("iconId",), "weapon"),
                                 ("EquipParamProtector", ("iconIdM", "iconIdF"), "armour"),
                                 ("EquipParamAccessory", ("iconId",), "accessory"),
                                 ("EquipParamGoods", ("iconId",), "item")):
        names = w.names.get(table, {})
        for rid, r in w.rows(table).items():
            name = (names.get(rid) or "").strip()
            if not name:
                continue
            name = re.sub(r" \+\d+$", "", name)  # weapon upgrade levels share one picture
            for f in fields:
                icon = r.get(f, -1)
                if icon > 0 and (label, name) not in users[icon]:
                    users[icon].append((label, name))
    os.makedirs(outdir, exist_ok=True)
    menu = os.path.join(base, "menu")
    pics = sorted(f for f in os.listdir(menu) if f.startswith("menu_knowledge_") and f.endswith(".tpf.dcx"))
    lines = ["# Item pictures\n", f"Generated by `tools/textures.py items`: {len(pics)} pictures.\n",
             "| Picture | File | Used by |", "|---:|---|---|"]
    written = 0
    for fn in pics:
        icon = int(fn[len("menu_knowledge_"):-len(".tpf.dcx")])
        path = w.path(f"menu/{fn}")
        tex = read_tpf(souls.load(path))[0]
        who = users.get(icon, [])
        stem = f"{icon:05d}" + (f" {_safe(who[0][1])}" if who else "")
        tex.image().save(os.path.join(outdir, stem + ".png"))
        written += 1
        lines.append(f"| {icon} | `{stem}.png` | " + (", ".join(f"{n} ({l})" for l, n in who) or "—") + " |")
    with open(os.path.join(outdir, "index.md"), "w", encoding="utf-8") as fo:
        fo.write("\n".join(lines) + "\n")
    return written


def textures_in(path: str) -> list[Texture]:
    """Every texture in a TPF, in the TPFs inside a BND4, or in a split .tpfbhd/.tpfbdt archive."""
    if path.endswith(".tpfbhd"):
        head = open(path, "rb").read()
        blob = open(path[:-len(".tpfbhd")] + ".tpfbdt", "rb").read()
        tpfs = [f.data for f in souls.bnd4_read(head, blob)]
    else:
        data = souls.load(path)
        tpfs = [data] if data[:4] == b"TPF\0" else [f.data for f in souls.bnd4_read(data)]
    return [t for tpf in tpfs if tpf[:4] == b"TPF\0" for t in read_tpf(tpf)]


def _model_names(w) -> dict[str, list[str]]:
    """Folder labels from the data: characters from the map layouts (a part's model and NpcParam
    row give the name the game shows, for bosses and named characters), equipment parts from the
    item tables' equipModelId, map texture packs from the place-name table."""
    import collections
    import glob
    import msb as msblib
    names = collections.defaultdict(collections.Counter)
    layouts = [w.path(f"map/mapstudio/{m}.msb.dcx") for m in w.maps()]
    layouts += glob.glob(os.path.join(w.base, "map", "mapstudio", "m29_*", "*.msb.dcx"))
    for path in layouts:
        for p in msblib.Msb(souls.load(path)).parts:
            if p.get("npc", -1) > 0 and p.get("model"):
                r = w.npcs.get(p["npc"])
                n = w.npc_names.get(r["nameId"], "").strip() if r else ""
                if n:
                    names[p["model"]][n] += 1
    out = {m: [n for n, _ in c.most_common(3)] for m, c in names.items()}
    wn = w.names.get("EquipParamWeapon", {})
    for rid, r in w.rows("EquipParamWeapon").items():
        n = re.sub(r" \+\d+$", "", (wn.get(rid) or "").strip())
        if n and n != "*" and r["equipModelId"] > 0:
            lst = out.setdefault(f"wp_a_{r['equipModelId']:04d}", [])
            if n not in lst:
                lst.append(n)
    pn = w.names.get("EquipParamProtector", {})
    for rid, r in w.rows("EquipParamProtector").items():
        n = (pn.get(rid) or "").strip()
        if not n or n == "NOT NEEDED":
            continue
        for slot, flag in (("hd", "headEquip"), ("bd", "bodyEquip"), ("am", "armEquip"), ("lg", "legEquip")):
            if r.get(flag):
                for sex in "mfa":
                    lst = out.setdefault(f"{slot}_{sex}_{r['equipModelId']:04d}", [])
                    if n not in lst:
                        lst.append(n)
    return out


def models(base: str, update: str, outdir: str, kinds: set[str], colour_only: bool, low: bool) -> None:
    import glob
    import items as itemlib
    w = itemlib.World(base, update)
    labels = _model_names(w)
    jobs = []  # (category, folder label, source path)
    root = lambda *p: os.path.join(base, *p)  # noqa: E731
    if "chr" in kinds:
        for p in sorted(glob.glob(root("chr", "c*.chrbnd.dcx")) + glob.glob(root("chr", "c*_*.tpf.dcx"))):
            model = os.path.basename(p).split(".")[0].split("_")[0]
            jobs.append(("chr", model, p))
    if "parts" in kinds:
        for p in sorted(glob.glob(root("parts", "*.partsbnd.dcx"))):
            stem = os.path.basename(p).split(".")[0]
            if stem.endswith("_l") and not low:
                continue
            jobs.append(("parts", stem, p))
    if "obj" in kinds:
        for p in sorted(glob.glob(root("obj", "o*.objbnd.dcx"))):
            jobs.append(("obj", os.path.basename(p).split(".")[0], p))
    if "map" in kinds:
        for p in sorted(glob.glob(root("map", "m*", "*.tpfbhd"))):
            area = os.path.basename(os.path.dirname(p))
            place = w.places.get(int(area[1:3]) * 100, "").strip() if area[1:3].isdigit() else ""
            jobs.append(("map", f"{area}{' ' + place if place else ''}/{os.path.basename(p)[:-7]}", p))
    written = skipped = 0
    lines = ["# Model and map textures\n", "Generated by `tools/textures.py models`.\n",
             "Character folders are named by model only. *Placed as* lists names the game shows for "
             "characters placed with that model (map layout part -> NpcParam -> name). That is not the "
             "model's identity: c0000 is the human model every hunter uses, and the bell-ringers in "
             "The One Reborn's arena carry that boss's name.\n",
             "| Folder | Textures | Source | Placed as |", "|---|---:|---|---|"]
    for cat, label, path in jobs:
        key = label.split("/")[0].split(" ")[0]
        extra = ", ".join(labels.get(key, []))
        named = extra if cat == "parts" else ""  # equipment names come from the item tables
        folder = os.path.join(outdir, cat, _safe(label.split("/")[0] + (f" {named}" if named else "")),
                              *([label.split("/")[1]] if "/" in label else []))
        try:
            texs = textures_in(path)
        except ValueError as e:
            print(f"{path}: {e}")
            continue
        if colour_only:
            texs = [t for t in texs if t.name.lower().endswith("_a")]
        if not texs:
            continue
        os.makedirs(folder, exist_ok=True)
        n = 0
        for t in texs:
            try:
                t.image().save(os.path.join(folder, _safe(t.name) + ".png"))
                n += 1
            except ValueError:
                skipped += 1
        written += n
        lines.append(f"| `{os.path.relpath(folder, outdir)}` | {n} | `{os.path.relpath(path, base)}` | "
                     f"{extra if cat == 'chr' else ''} |")
        print(f"{cat:5} {os.path.relpath(folder, outdir)}: {n}")
    with open(os.path.join(outdir, "index.md"), "w", encoding="utf-8") as fo:
        fo.write("\n".join(lines) + "\n")
    print(f"{written} textures written, {skipped} skipped -> {outdir}")


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    if len(sys.argv) < 2 or sys.argv[1] not in ("tpf", "items", "models"):
        print(__doc__)
        return 2
    if sys.argv[1] == "models" and len(sys.argv) >= 5:
        rest = sys.argv[5:]
        kinds = next((a for a in rest if not a.startswith("--")), "chr,parts")
        models(sys.argv[2], sys.argv[3], sys.argv[4], set(kinds.split(",")), "--colour" in rest, "--low" in rest)
        return 0
    if sys.argv[1] == "tpf" and len(sys.argv) == 4:
        os.makedirs(sys.argv[3], exist_ok=True)
        ok = bad = 0
        for t in textures_in(sys.argv[2]):
            try:
                t.image().save(os.path.join(sys.argv[3], _safe(t.name) + ".png"))
                ok += 1
            except ValueError as e:
                print(f"{t.name}: {e}")
                bad += 1
        print(f"{ok} textures written, {bad} skipped -> {sys.argv[3]}")
        return 0
    if sys.argv[1] == "items" and len(sys.argv) == 5:
        n = item_pictures(sys.argv[2], sys.argv[3], sys.argv[4])
        print(f"{n} item pictures -> {sys.argv[4]}")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
