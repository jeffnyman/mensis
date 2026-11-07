# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn a PS4 (Orbis) executable into an ELF that a stock Ghidra loads well, plus a symbol list.

    python -I tools/orbis_prep.py <in.elf> <nid-db> <out.elf> <out-symbols.tsv> [--base 0x400000]

<in.elf> is the output of unfself.py. <nid-db> is shadPS4's aerolib.inl (NID -> name).

Why this is needed. A PS4 executable is a FreeBSD x86-64 ELF, but:
  * its e_type is 0xFE10 (ET_SCE_DYNEXEC), which generic loaders don't know;
  * it is linked at address 0, which makes every small integer look like a code pointer;
  * its dynamic linking information is not in the normal DT_* tables but in Sony's own
    (DT_SCE_SYMTAB, DT_SCE_RELA, DT_SCE_JMPREL, ...) pointing into the PT_SCE_DYNLIBDATA
    segment, which is not loaded into memory, so Ghidra never applies a single relocation;
  * imported functions are named by NID (a hash), e.g. "Mk11X2E0Bs8#L#M".

What this script does, all on a copy:
  1. rebases every loadable segment to <base> (default 0x400000);
  2. applies all relocations from DT_SCE_RELA and DT_SCE_JMPREL (R_X86_64_RELATIVE,
     R_X86_64_64, GLOB_DAT, JUMP_SLOT) so data pointers, vtables and the GOT hold real addresses;
  3. adds a small extra segment of one-byte stubs (a RET each), one per imported symbol, and
     points each GOT slot / data import at its stub, so that "jmp [GOT]" lands on a named thing;
  4. marks the PS4-only program headers as PT_NULL and sets e_type to ET_EXEC;
  5. writes a TSV of address/kind/name/library for the Ghidra script ApplyOrbisSymbols.java:
       IMPORT  the stub for an imported function or object
       PLT     the "jmp [rip+GOT]" trampoline that the game's code actually calls
       GOT     the GOT slot itself
       ENTRY / PROCPARAM   the entry point and the SCE process-parameter block
       FUNC    an unnamed function start taken from the .eh_frame_hdr search table
"""

from __future__ import annotations

import argparse
import collections
import re
import string
import struct
import sys

PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_TLS = 1, 2, 3, 7
PT_GNU_EH_FRAME = 0x6474E550
PT_SCE_DYNLIBDATA, PT_SCE_PROCPARAM = 0x61000000, 0x61000001
ET_EXEC, ET_SCE_DYNEXEC = 2, 0xFE10

DT_SCE = {
    "NEEDED_MODULE": 0x6100000F,
    "IMPORT_LIB": 0x61000015,
    "JMPREL": 0x61000029,
    "PLTRELSZ": 0x6100002D,
    "RELA": 0x6100002F,
    "RELASZ": 0x61000031,
    "STRTAB": 0x61000035,
    "STRSZ": 0x61000037,
    "SYMTAB": 0x61000039,
    "SYMTABSZ": 0x6100003F,
}
R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT, R_X86_64_RELATIVE = 1, 6, 7, 8

NID_ALPHABET = string.ascii_uppercase + string.ascii_lowercase + string.digits + "+-"


def decode_id(s: str) -> int:
    """Library/module ids after '#' in a NID name use the same base64 alphabet."""
    v = 0
    for ch in s:
        v = v * 64 + NID_ALPHABET.index(ch)
    return v


def load_nid_db(path: str) -> dict[str, str]:
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    return {
        m.group(1): m.group(2)
        for m in re.finditer(r'STUB\("([A-Za-z0-9+\-]{11})",\s*([^)\s]+)\)', text)
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("nid_db")
    ap.add_argument("out_elf")
    ap.add_argument("out_tsv")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x400000)
    a = ap.parse_args()
    base = a.base

    with open(a.elf, "rb") as f:
        data = bytearray(f.read())
    if data[:4] != b"\x7fELF":
        sys.exit("not an ELF (run unfself.py first)")
    (e_type,) = struct.unpack_from("<H", data, 0x10)
    e_entry, e_phoff = struct.unpack_from("<QQ", data, 0x18)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x36)
    if e_type != ET_SCE_DYNEXEC:
        print(f"note: e_type {e_type:#x}, expected 0xFE10; continuing")

    ph = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        ph.append([off, *struct.unpack_from("<IIQQQQQQ", data, off)])
    # ph row: [hdr_off, type, flags, offset, vaddr, paddr, filesz, memsz, align]

    def find(t):
        return next(p for p in ph if p[1] == t)

    loads = [p for p in ph if p[1] == PT_LOAD]

    def va2off(va):
        for p in loads:
            if p[4] <= va < p[4] + p[6]:
                return p[3] + (va - p[4])
        return None

    # --- Sony dynamic info ---------------------------------------------------------------
    dynp, dlp = find(PT_DYNAMIC), find(PT_SCE_DYNLIBDATA)
    dl = dlp[3]
    tags = [struct.unpack_from("<QQ", data, dynp[3] + i) for i in range(0, dynp[6], 16)]
    T: dict[int, int] = {}
    for k, v in tags:
        T.setdefault(k, v)
    strtab = dl + T[DT_SCE["STRTAB"]]

    def cstr(o):
        return data[strtab + o : data.index(b"\0", strtab + o)].decode()

    libs = {v >> 48: cstr(v & 0xFFFFFFFF) for k, v in tags if k == DT_SCE["IMPORT_LIB"]}
    mods = {
        v >> 48: cstr(v & 0xFFFFFFFF) for k, v in tags if k == DT_SCE["NEEDED_MODULE"]
    }

    nids = load_nid_db(a.nid_db)
    symtab, nsym = dl + T[DT_SCE["SYMTAB"]], T[DT_SCE["SYMTABSZ"]] // 24
    syms = []  # (name, lib, is_func, defined_value or None)
    unresolved = 0
    for i in range(nsym):
        st_name, info, _, shndx, value, _ = struct.unpack_from(
            "<IBBHQQ", data, symtab + i * 24
        )
        raw = cstr(st_name)
        lib = ""
        name = raw
        if raw.count("#") == 2:
            nid, l, _m = raw.split("#")
            lib = libs.get(decode_id(l), "")
            name = nids.get(nid)
            if name is None:
                unresolved += 1
                name = f"nid_{nid.replace('+', '_').replace('-', '_')}"
        syms.append((name, lib, (info & 0xF) == 2, value if shndx else None))

    # --- import stub segment -------------------------------------------------------------
    used = sorted({i for i, s in enumerate(syms) if s[3] is None and s[0]})
    stub_slot = 8
    top = max(p[4] + p[7] for p in loads)
    stub_va = (top + 0xFFFF) & ~0xFFFF  # pre-rebase vaddr
    stub_addr = {si: base + stub_va + k * stub_slot for k, si in enumerate(used)}
    stub_off = (len(data) + 0xFFF) & ~0xFFF
    stub_size = len(used) * stub_slot
    data.extend(b"\0" * (stub_off - len(data)))
    data.extend((b"\xc3" + b"\xcc" * (stub_slot - 1)) * len(used))

    # --- relocations ---------------------------------------------------------------------
    counts = collections.Counter()
    got = {}  # slot vaddr (rebased) -> symbol index
    for tag_off, tag_sz in (("RELA", "RELASZ"), ("JMPREL", "PLTRELSZ")):
        start, size = dl + T[DT_SCE[tag_off]], T[DT_SCE[tag_sz]]
        for i in range(start, start + size, 24):
            r_off, r_info, r_add = struct.unpack_from("<QQq", data, i)
            rtype, rsym = r_info & 0xFFFFFFFF, r_info >> 32
            fo = va2off(r_off)
            if fo is None:
                counts[f"type {rtype} outside file"] += 1
                continue
            if rtype == R_X86_64_RELATIVE:
                val = base + r_add
            elif rtype in (R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT):
                s = syms[rsym]
                if s[3] is not None:
                    val = base + s[3]
                elif rsym in stub_addr:
                    val = stub_addr[rsym]
                    if (
                        rtype != R_X86_64_64
                    ):  # R_X86_64_64 sites are data pointers, not GOT
                        got[base + r_off] = rsym
                    else:
                        counts["R_X86_64_64 to an import"] += 1
                else:
                    counts[f"type {rtype} unresolved symbol"] += 1
                    continue
                if rtype == R_X86_64_64:
                    val += r_add
            else:
                counts[f"type {rtype} skipped"] += 1
                continue
            struct.pack_into("<Q", data, fo, val & 0xFFFFFFFFFFFFFFFF)
            counts[
                {1: "R_X86_64_64", 6: "GLOB_DAT", 7: "JUMP_SLOT", 8: "RELATIVE"}[rtype]
            ] += 1

    # --- PLT trampolines: FF 25 disp32 = jmp qword ptr [rip+disp32] into a GOT slot ------
    plt = {}
    text = next(p for p in loads if p[2] & 1)
    tbytes = bytes(data[text[3] : text[3] + text[6]])
    for m in re.finditer(rb"\xff\x25", tbytes):
        at = text[4] + m.start()
        (disp,) = struct.unpack_from("<i", tbytes, m.start() + 2)
        target = base + at + 6 + disp
        if target in got:
            plt[base + at] = got[target]

    # --- function starts from .eh_frame_hdr -----------------------------------------------
    # The binary search table holds (initial_location, fde) pairs; with the encoding the
    # PS4 toolchain uses (0x3B = DW_EH_PE_datarel | sdata4) both are relative to the header.
    func_starts = []
    eh = next((p for p in ph if p[1] == PT_GNU_EH_FRAME), None)
    if eh is not None:
        h = eh[3]
        version, _ptr_enc, count_enc, table_enc = data[h : h + 4]
        if version == 1 and count_enc == 0x03 and table_enc == 0x3B:
            (fde_count,) = struct.unpack_from("<I", data, h + 8)
            for k in range(fde_count):
                loc, _ = struct.unpack_from("<ii", data, h + 12 + k * 8)
                func_starts.append(base + eh[4] + loc)
        else:
            print(
                f"note: unexpected .eh_frame_hdr encoding {bytes(data[h : h + 4]).hex()}"
            )

    # --- rewrite headers -----------------------------------------------------------------
    struct.pack_into("<H", data, 0x10, ET_EXEC)
    struct.pack_into("<Q", data, 0x18, base + e_entry)
    free = [p for p in ph if p[1] in (PT_INTERP, PT_DYNAMIC)]
    for p in ph:
        hdr = p[0]
        if p[1] in (PT_LOAD, PT_TLS, PT_GNU_EH_FRAME):
            struct.pack_into("<QQ", data, hdr + 0x10, base + p[4], base + p[5])
        elif p[1] not in (PT_LOAD,):
            struct.pack_into(
                "<I", data, hdr, 0
            )  # PT_NULL: Sony-only or not meaningful here
    hdr = free[0][0]
    struct.pack_into(
        "<IIQQQQQQ",
        data,
        hdr,
        PT_LOAD,
        5,
        stub_off,
        base + stub_va,
        base + stub_va,
        stub_size,
        stub_size,
        0x1000,
    )

    with open(a.out_elf, "wb") as f:
        f.write(data)

    # --- symbols file --------------------------------------------------------------------
    rows = [(base + e_entry, "ENTRY", "_start", "")]
    pp = find(PT_SCE_PROCPARAM)
    rows.append((base + pp[4], "PROCPARAM", "sceProcParam", ""))
    for si, addr in stub_addr.items():
        n, lib, is_func, _ = syms[si]
        rows.append((addr, "IMPORT" if is_func else "IMPORT_DATA", n, lib))
    for slot, si in got.items():
        rows.append((slot, "GOT", "__got_" + syms[si][0], syms[si][1]))
    for at, si in plt.items():
        rows.append((at, "PLT", syms[si][0], syms[si][1]))
    named = {r[0] for r in rows}
    rows += [(fs, "FUNC", "", "") for fs in sorted(set(func_starts)) if fs not in named]
    with open(a.out_tsv, "w", encoding="utf-8", newline="\n") as f:
        f.write("# address\tkind\tname\tlibrary\n")
        for addr, kind, n, lib in sorted(rows):
            f.write(f"{addr:#x}\t{kind}\t{n}\t{lib}\n")

    print(f"base {base:#x}, entry {base + e_entry:#x}")
    print(
        f"modules {len(mods)}, libraries {len(libs)}, symbols {nsym}, "
        f"NIDs unresolved {unresolved}"
    )
    print(
        f"import stubs {len(used)} at {base + stub_va:#x}, GOT slots named {len(got)}, "
        f"PLT trampolines {len(plt)}, function starts from .eh_frame_hdr {len(set(func_starts))}"
    )
    for k, v in sorted(counts.items()):
        print(f"  {v:8}  {k}")


if __name__ == "__main__":
    main()
