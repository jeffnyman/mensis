# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode Bloodborne's gameplay parameter tables (PARAM) using the game's own layouts (PARAMDEF).

    python -I tools/params.py check <parambnd> <paramdefbnd>
    python -I tools/params.py dump  <parambnd> <paramdefbnd> <ParamName> [row-id ...]
    python -I tools/params.py diff  <old parambnd> <new parambnd> <paramdefbnd> <out.md> [--msg item.msgbnd]

A PARAM is a table of fixed-size rows. Its layout is not in the file: it is in a PARAMDEF with
the same type name (EQUIP_PARAM_WEAPON_ST, ...), and the game ships all of them in
paramdef/paramdef.paramdefbnd.dcx. Each PARAMDEF field has a Japanese display name, an internal
type and an internal name:

    internal type   internal name            display name
    s32             behaviorVariationId      行動バリエーションID
    u8              isEnhance:1              (a 1-bit flag packed with its neighbors)
    dummy8          pad[3]                   (padding)

Formats (as found in this game; little-endian, 64-bit offsets):

  PARAMDEF  0x00 u32 file size, 0x04 u16 0xFF, 0x06 u16 data version, 0x08 u16 field count,
            0x0A u16 field record size (0xD0), 0x0C char[32] param type, 0x2C u8 big-endian,
            0x2D u8 unicode, 0x2E u16 format version, 0x30 i64 offset of field records.
            Field record: 0x00 wchar[32] display name, 0x40 char[8] display type, 0x48 char[8]
            format, 0x50 f32 default/min/max/increment, 0x60 i32 edit flags, 0x64 i32 byte size,
            0x68 i64 description offset, 0x70 char[32] internal type (often an enum name; the
            storage type is the display type at 0x40), 0x90 char[32] internal name,
            0xB0 i32 sort id.
  PARAM     0x00 u32 names offset, 0x0A u16 row count, 0x0C char[32] param type, 0x30 i64 data
            start; from 0x40, rows of { i32 id, pad, i64 data offset, i64 name offset }.

Bit fields of the same base type are packed least-significant bit first into one unit of that
type (padding bits, dummy8 pad:N, counting as u8), until the type changes, the unit is full or a
non-bit field comes along.
"""

from __future__ import annotations

import argparse
import io
import os
import re
import struct
import sys
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls

TYPES = {
    "s8": ("b", 1),
    "u8": ("B", 1),
    "s16": ("h", 2),
    "u16": ("H", 2),
    "s32": ("i", 4),
    "u32": ("I", 4),
    "f32": ("f", 4),
    "dummy8": ("B", 1),
}


@dataclass
class Field:
    name: str  # internal name without bit/array suffix
    display: str  # Japanese display name
    type: str
    offset: int  # byte offset in the row
    size: int  # bytes (for a bit field: the unit it lives in)
    bits: int = 0  # >0 for a bit field
    bit: int = 0  # first bit within the unit
    count: int = 1  # array length (dummy8 pad[3], fixstr name[16])


class ParamDef:
    def __init__(self, data: bytes):
        self.type = data[0x0C:0x2C].split(b"\0")[0].decode("ascii")
        n, rec = struct.unpack_from("<HH", data, 0x08)
        unicode = data[0x2D] == 1
        start = struct.unpack_from("<q", data, 0x30)[0]
        self.fields: list[Field] = []
        off = 0
        # The bit-field unit being filled: its type, offset and bits used (None: no unit open).
        unit_type: str | None = None
        unit_off = unit_used = 0
        for i in range(n):
            r = data[start + i * rec : start + (i + 1) * rec]
            disp = (
                r[:0x40].decode("utf-16-le").split("\0")[0]
                if unicode
                else r[:0x40].split(b"\0")[0].decode("shift_jis")
            )
            # Storage type is the display type at 0x40 (s32, u8, dummy8, fixstr...); the internal
            # type at 0x70 is often an enum name such as ACTION_BUTTON_REGION_TYPE.
            itype = r[0x40:0x48].split(b"\0")[0].decode("ascii")
            iname = r[0x90:0xB0].split(b"\0")[0].decode("shift_jis", "replace")
            m = re.match(
                r"^(\w+)\s*(?::\s*(\d+))?(?:\[(\d+)\])?$", iname.strip()
            )  # also "hasTarget : 1"
            if m is None:
                raise ValueError(f"field name {iname!r} not understood")
            name, bits, count = m.group(1), int(m.group(2) or 0), int(m.group(3) or 1)
            if bits:
                size = TYPES.get(itype, ("B", 1))[1]
                # Padding bits (dummy8 pad:5) share a unit with u8 flags, so pack by storage
                # type with dummy8 counted as u8.
                base = "u8" if itype == "dummy8" else itype
                if unit_type != base or unit_used + bits > size * 8:
                    unit_type, unit_off, unit_used = base, off, 0
                    off += size
                self.fields.append(
                    Field(name, disp, itype, unit_off, size, bits, unit_used)
                )
                unit_used += bits
                continue
            unit_type = None
            if itype in TYPES:
                size = TYPES[itype][1] * count
            elif itype == "fixstr":
                size = count
            elif itype == "fixstrW":
                size = 2 * count
            else:
                raise ValueError(f"{self.type}: unknown field type {itype} ({iname})")
            self.fields.append(Field(name, disp, itype, off, size, 0, 0, count))
            off += size
        self.row_size = off

    def decode(self, row: bytes) -> dict:
        out = {}
        for f in self.fields:
            if f.type == "dummy8":
                continue
            if f.bits:
                fmt = TYPES[f.type][0]
                unit = struct.unpack_from(
                    "<" + fmt.upper() if fmt.islower() else "<" + fmt, row, f.offset
                )[0]
                out[f.name] = (unit >> f.bit) & ((1 << f.bits) - 1)
            elif f.type == "fixstr":
                out[f.name] = (
                    row[f.offset : f.offset + f.size]
                    .split(b"\0")[0]
                    .decode("shift_jis", "replace")
                )
            elif f.type == "fixstrW":
                out[f.name] = (
                    row[f.offset : f.offset + f.size].decode("utf-16-le").split("\0")[0]
                )
            else:
                fmt, _sz = TYPES[f.type]
                vals = struct.unpack_from(f"<{f.count}{fmt}", row, f.offset)
                v = vals[0] if f.count == 1 else list(vals)
                out[f.name] = round(v, 6) if isinstance(v, float) else v
        return out


class Param:
    def __init__(self, name: str, data: bytes):
        self.name = name
        self.type = data[0x0C:0x2C].split(b"\0")[0].decode("ascii")
        count = struct.unpack_from("<H", data, 0x0A)[0]
        self.rows = []  # (id, data offset)
        for i in range(count):
            rid, off = struct.unpack_from("<i4xq", data, 0x40 + i * 24)
            self.rows.append((rid, off))
        self.data = data
        offs = sorted(o for _, o in self.rows)
        self.stride = offs[1] - offs[0] if len(offs) > 1 else None

    def row(self, i: int, size: int) -> bytes:
        off = self.rows[i][1]
        return self.data[off : off + size]

    def row_names(self) -> dict[int, str]:
        """{row id: the row's working name}. Each row header (24 bytes at 0x40) is the id, the
        data offset and the offset of a UTF-16 name, often the developers' Japanese label."""
        out = {}
        for i, (rid, _) in enumerate(self.rows):
            off = struct.unpack_from("<q", self.data, 0x40 + i * 24 + 16)[0]
            end = off
            while (
                0 < off
                and end + 1 < len(self.data)
                and self.data[end : end + 2] != b"\0\0"
            ):
                end += 2
            out[rid] = self.data[off:end].decode("utf-16-le", "replace") if off else ""
        return out


def load(parambnd: str, paramdefbnd: str):
    defs = {}
    for bf in souls.bnd4_read(souls.load(paramdefbnd)):
        d = ParamDef(bf.data)
        defs[d.type] = d
    params = {}
    for bf in souls.bnd4_read(souls.load(parambnd)):
        name = os.path.basename(bf.name.replace("\\", "/")).rsplit(".", 1)[0]
        params[name] = Param(name, bf.data)
    return params, defs


def rows(p: Param, d: ParamDef) -> dict[int, dict]:
    return {rid: d.decode(p.row(i, d.row_size)) for i, (rid, _) in enumerate(p.rows)}


def item_names(msgbnd: str) -> dict[str, dict[int, str]]:
    """Row labels for the equipment and goods params, from the item text tables."""
    which = {
        "武器名": "EquipParamWeapon",
        "防具名": "EquipParamProtector",
        "アイテム名": "EquipParamGoods",
        "アクセサリ名": "EquipParamAccessory",
        "魔法名": "Magic",
    }
    out = {}
    for bf in souls.bnd4_read(souls.load(msgbnd)):
        stem = os.path.basename(bf.name.replace("\\", "/")).rsplit(".", 1)[0]
        if stem in which:
            out[which[stem]] = {
                k: v for k, v in souls.fmg_read(bf.data).items() if v and v != "*"
            }
    return out


def fmt(v):
    if isinstance(v, float):
        return f"{v:g}"
    return str(v)


def main():
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("parambnd")
    c.add_argument("paramdefbnd")
    d = sub.add_parser("dump")
    d.add_argument("parambnd")
    d.add_argument("paramdefbnd")
    d.add_argument("param")
    d.add_argument("ids", nargs="*", type=int)
    f = sub.add_parser("diff")
    f.add_argument("old")
    f.add_argument("new")
    f.add_argument("paramdefbnd")
    f.add_argument("out")
    f.add_argument(
        "--msg",
        help="item.msgbnd.dcx, to label weapon/armor/goods rows with their names",
    )
    a = ap.parse_args()

    if a.cmd == "check":
        params, defs = load(a.parambnd, a.paramdefbnd)
        bad = 0
        for name, p in sorted(params.items()):
            dfn = defs.get(p.type)
            ok = dfn is not None and (p.stride is None or p.stride == dfn.row_size)
            bad += not ok
            print(
                f"{'ok ' if ok else 'BAD'} {name:32} {p.type:32} rows {len(p.rows):6}  "
                f"stride {p.stride}  paramdef {dfn.row_size if dfn else '-'}  fields {len(dfn.fields) if dfn else '-'}"
            )
        print(f"{len(params) - bad}/{len(params)} params match their paramdef")
        return 0 if bad == 0 else 1

    if a.cmd == "dump":
        params, defs = load(a.parambnd, a.paramdefbnd)
        p = params[a.param]
        dfn = defs[p.type]
        disp = {f.name: f.display for f in dfn.fields}
        for rid, vals in rows(p, dfn).items():
            if a.ids and rid not in a.ids:
                continue
            print(f"== row {rid}")
            for k, v in vals.items():
                print(f"  {k:36} {fmt(v):>14}   {disp.get(k, '')}")
        return 0

    old, defs = load(a.old, a.paramdefbnd)
    new, _ = load(a.new, a.paramdefbnd)
    labels = item_names(a.msg) if a.msg else {}
    lines = [
        "# Param diff\n",
        f"`{a.old}` → `{a.new}`\n",
        "| Param | Rows changed | Added | Removed | Fields changed |",
        "|---|---:|---:|---:|---:|",
    ]
    detail = []
    for name in sorted(set(old) | set(new)):
        if name not in old or name not in new:
            lines.append(
                f"| {name} | (param {'added' if name in new else 'removed'}) | | | |"
            )
            continue
        dfn = defs[new[name].type]
        disp = {f.name: f.display for f in dfn.fields}
        ro, rn = rows(old[name], dfn), rows(new[name], dfn)
        added = sorted(set(rn) - set(ro))
        removed = sorted(set(ro) - set(rn))
        changed = [
            (
                rid,
                [
                    (k, ro[rid][k], rn[rid][k])
                    for k in rn[rid]
                    if ro[rid][k] != rn[rid][k]
                ],
            )
            for rid in sorted(set(ro) & set(rn))
            if ro[rid] != rn[rid]
        ]
        if not (added or removed or changed):
            continue
        nfields = sum(len(c) for _, c in changed)
        lines.append(
            f"| {name} | {len(changed)} | {len(added)} | {len(removed)} | {nfields} |"
        )
        lab = labels.get(name, {})
        detail.append(f"\n## {name} ({new[name].type})\n")
        if added:
            detail.append(
                "Added rows: "
                + ", ".join(
                    f"{r}" + (f" ({lab[r]})" if r in lab else "") for r in added
                )
                + "\n"
            )
        if removed:
            detail.append(
                "Removed rows: "
                + ", ".join(
                    f"{r}" + (f" ({lab[r]})" if r in lab else "") for r in removed
                )
                + "\n"
            )
        if changed:
            detail.append("| Row | Field | Old | New | Field (display name) |")
            detail.append("|---|---|---:|---:|---|")
            for rid, cs in changed:
                label = f"{rid}" + (f" {lab[rid]}" if rid in lab else "")
                for k, o, n in cs:
                    detail.append(
                        f"| {label} | `{k}` | {fmt(o)} | {fmt(n)} | {disp.get(k, '')} |"
                    )
                    label = ""
    with open(a.out, "w", encoding="utf-8") as fo:
        fo.write("\n".join(lines + detail) + "\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
