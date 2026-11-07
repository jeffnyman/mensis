# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn a fake-signed PS4 SELF (eboot.bin / *.prx / *.sprx) back into a plain ELF.

    python -I tools/unfself.py <in.self> <out.elf>

A SELF is a wrapper: a SELF header and segment table, the original ELF header and program
headers, an extended-info block, and then the segment payloads. On a retail SELF those
payloads are encrypted; on a fake SELF (what a fake PKG carries) they are plaintext, so
rebuilding the ELF is just a matter of putting each payload back at its p_offset.

SELF segment flags: 0x2 encrypted, 0x8 compressed, 0x800 blocked; bits 20+ = phdr index.
"""

import struct
import sys


def unfself(data: bytes) -> bytes:
    if data[:4] != b"\x4f\x15\x3d\x1d":
        if data[:4] == b"\x7fELF":
            return data
        raise ValueError("not a SELF")
    nseg = struct.unpack_from("<H", data, 0x18)[0]
    segs = [struct.unpack_from("<QQQQ", data, 0x20 + i * 32) for i in range(nseg)]
    elf_off = 0x20 + nseg * 32
    if data[elf_off : elf_off + 4] != b"\x7fELF":
        raise ValueError("embedded ELF header not found")
    (e_phoff, _, _, _, e_phentsize, e_phnum) = struct.unpack_from(
        "<QQIHHH", data, elf_off + 0x20
    )
    phdrs = [
        struct.unpack_from("<IIQQQQQQ", data, elf_off + e_phoff + i * e_phentsize)
        for i in range(e_phnum)
    ]
    size = max([e_phoff + e_phnum * e_phentsize] + [p[2] + p[5] for p in phdrs if p[5]])
    out = bytearray(size)
    hdr_len = e_phoff + e_phnum * e_phentsize
    out[:hdr_len] = data[elf_off : elf_off + hdr_len]
    for flags, off, csize, dsize in segs:
        if not flags & 0x800:
            continue  # signature / info segments, not part of the ELF
        if flags & 0x2:
            raise ValueError(
                "segment is encrypted - this is a retail SELF, not a fake one"
            )
        if flags & 0x8:
            raise ValueError("compressed SELF segments not handled")
        idx = (flags >> 20) & 0xFFF
        p_offset, p_filesz = phdrs[idx][2], phdrs[idx][5]
        out[p_offset : p_offset + p_filesz] = data[off : off + min(csize, p_filesz)]
    return bytes(out)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    with open(sys.argv[1], "rb") as f:
        elf = unfself(f.read())
    with open(sys.argv[2], "wb") as f:
        f.write(elf)
    print(f"{sys.argv[2]}: {len(elf):#x} bytes")
