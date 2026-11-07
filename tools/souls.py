# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal readers for the FromSoftware container formats Bloodborne ships.

    python -I tools/souls.py dcx   <file.dcx> <out>          # decompress a DCX
    python -I tools/souls.py bnd   <file[.dcx]> [out-dir]    # list (or unpack) a BND4
    python -I tools/souls.py fmg   <file.fmg | msgbnd> [id]  # dump text
    python -I tools/souls.py param <parambnd>                # list params and row counts

Layouts follow SoulsFormats (JKAnderson et al.); only the Bloodborne/DS3-era variants
(DCX_DFLT, BND4, FMG v2, PARAM with 64-bit offsets) are handled.
"""

from __future__ import annotations

import io
import os
import struct
import sys
import zlib
from dataclasses import dataclass


# --- DCX ---------------------------------------------------------------------------------
def dcx_decompress(data: bytes) -> bytes:
    """DCX: 'DCX\\0' header, 'DCS\\0' sizes, 'DCP\\0' + 'DFLT' params, 'DCA\\0', zlib stream."""
    if data[:4] != b"DCX\0":
        return data
    usize, csize = struct.unpack_from(">II", data, 0x1C)
    fmt = data[0x28:0x2C]
    if fmt != b"DFLT":
        raise ValueError(f"DCX compressor {fmt!r} not handled")
    start = 0x4C
    out = zlib.decompress(data[start : start + csize])
    if len(out) != usize:
        raise ValueError("DCX size mismatch")
    return out


def load(path: str) -> bytes:
    with open(path, "rb") as f:
        return dcx_decompress(f.read())


# --- BND4 --------------------------------------------------------------------------------
@dataclass
class BndFile:
    flags: int
    id: int
    name: str
    data: bytes


def _rev8(b: int) -> int:
    return int(f"{b:08b}"[::-1], 2)


def bnd4_read(data: bytes, blob: bytes | None = None) -> list[BndFile]:
    """A BND4, or a split BXF4: pass the BHF4 header as `data` and its BDF4 data file as `blob`.

    BHF4 has the same header and entry layout as BND4; only the files' bytes live in the
    separate .bdt, at the offsets the entries give (the maps' .tpfbhd/.tpfbdt pairs).
    """
    if data[:4] not in (b"BND4", b"BHF4"):
        raise ValueError("not BND4 or BHF4")
    body = blob if blob is not None else data
    big = data[9] == 1
    _bit_big = (
        data[10] == 1
    )  # bit order of the format byte; handled below by trying both
    e = ">" if big else "<"
    count = struct.unpack_from(e + "i", data, 0x0C)[0]
    unicode = data[0x30] == 1
    raw_fmt = data[0x31]
    entry_size = struct.unpack_from(e + "q", data, 0x20)[0]
    # Binder.Format: 1 BigEndian, 2 IDs, 4 Names1, 8 Names2, 0x10 LongOffsets, 0x20 Compression.
    # The byte may be stored bit-reversed; pick whichever reading matches the entry size.
    for fmt in (_rev8(raw_fmt), raw_fmt):
        has_ids, has_names = bool(fmt & 2), bool(fmt & 0xC)
        long_off, compressed = bool(fmt & 0x10), bool(fmt & 0x20)
        size = (
            16
            + (8 if compressed else 0)
            + (8 if long_off else 4)
            + 4 * has_ids
            + 4 * has_names
        )
        if size == entry_size:
            break
    else:
        raise ValueError(
            f"BND4 format {raw_fmt:#x} does not match entry size {entry_size:#x}"
        )
    out = []
    for i in range(count):
        p = 0x40 + i * entry_size
        flags = data[p]
        p += 8  # flags, 3 pad, i32 -1
        csize = struct.unpack_from(e + "q", data, p)[0]
        p += 8
        if compressed:
            p += 8  # uncompressed size
        if long_off:
            off = struct.unpack_from(e + "q", data, p)[0]
            p += 8
        else:
            off = struct.unpack_from(e + "I", data, p)[0]
            p += 4
        fid = -1
        if has_ids:
            fid = struct.unpack_from(e + "i", data, p)[0]
            p += 4
        name = ""
        if has_names:
            noff = struct.unpack_from(e + "I", data, p)[0]
            p += 4
            if unicode:
                end = noff
                while data[end : end + 2] != b"\0\0":
                    end += 2
                name = data[noff:end].decode("utf-16-be" if big else "utf-16-le")
            else:
                name = data[noff : data.index(b"\0", noff)].decode("shift_jis")
        out.append(BndFile(flags, fid, name, dcx_decompress(body[off : off + csize])))
    return out


# --- FMG (text) --------------------------------------------------------------------------
def fmg_read(data: bytes) -> dict[int, str]:
    big = data[1] == 0xFF
    e = ">" if big else "<"
    version = data[2]
    groups, _strings = struct.unpack_from(e + "ii", data, 0x0C)
    if version == 2:
        str_off = struct.unpack_from(e + "q", data, 0x18)[0]
        p, gsz, osz = 0x28, 16, 8
    else:
        str_off = struct.unpack_from(e + "i", data, 0x14)[0]
        p, gsz, osz = 0x1C, 12, 4
    out = {}
    for g in range(groups):
        idx, first, last = struct.unpack_from(e + "iii", data, p + g * gsz)
        for k in range(last - first + 1):
            fmt = e + ("q" if osz == 8 else "i")
            so = struct.unpack_from(fmt, data, str_off + (idx + k) * osz)[0]
            if so <= 0:
                continue
            end = so
            while data[end : end + 2] != b"\0\0":
                end += 2
            out[first + k] = data[so:end].decode("utf-16-be" if big else "utf-16-le")
    return out


# --- PARAM -------------------------------------------------------------------------------
def param_info(data: bytes) -> tuple[str, int, int]:
    """Return (param type, row count, format flags) for a DS3/BB-era PARAM."""
    rows = struct.unpack_from("<H", data, 0x0A)[0]
    f2d, _f2e = data[0x2D], data[0x2E]
    if f2d & 0x80:  # OffsetParamType: name stored elsewhere
        off = struct.unpack_from("<q", data, 0x10)[0]
        ptype = data[off : data.index(b"\0", off)].decode("ascii")
    else:
        ptype = data[0x0C:0x2C].split(b"\0")[0].decode("ascii", "replace").strip()
    return ptype, rows, f2d


def main(argv):
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, path = argv[1], argv[2]
    data = load(path)
    if cmd == "dcx":
        with open(argv[3], "wb") as f:
            f.write(data)
    elif cmd == "bnd":
        files = bnd4_read(data)
        for bf in files:
            print(f"{bf.id:>10} {len(bf.data):>10}  {bf.data[:4]!r:12} {bf.name}")
        if len(argv) > 3:
            for bf in files:
                rel = (
                    bf.name.replace("\\", "/").lstrip("/").split(":", 1)[-1].lstrip("/")
                )
                dst = os.path.join(argv[3], *rel.split("/"))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "wb") as f:
                    f.write(bf.data)
    elif cmd == "fmg":
        sources = (
            [(path, data)]
            if data[:4] != b"BND4"
            else [
                (bf.name, bf.data)
                for bf in bnd4_read(data)
                if bf.name.lower().endswith(".fmg")
            ]
        )
        for name, d in sources:
            texts = fmg_read(d)
            if len(argv) > 3:
                t = texts.get(int(argv[3]))
                if t:
                    print(f"{os.path.basename(name)}: {t}")
                continue
            print(f"== {name}  ({len(texts)} strings)")
            for k, v in list(texts.items())[:5]:
                print(f"  {k}: {v[:100]!r}")
    elif cmd == "param":
        for bf in bnd4_read(data):
            ptype, rows, _flags = param_info(bf.data)
            print(
                f"{rows:6} rows  {ptype:32} {os.path.basename(bf.name.replace(chr(92), '/'))}"
            )
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
