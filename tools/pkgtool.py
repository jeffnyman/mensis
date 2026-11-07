# SPDX-License-Identifier: GPL-3.0-or-later
"""Read a fake-signed (fpkg) PS4 PKG: header, sce_sys entries, and the game filesystem.

    python -I tools/pkgtool.py info    <pkg>
    python -I tools/pkgtool.py list    <pkg> [glob]
    python -I tools/pkgtool.py extract <pkg> <out-dir> [glob ...]

A PS4 PKG is laid out as:

    PKG header (big-endian, magic 7F 43 4E 54 "\\x7FCNT")
    entry table  -> sce_sys files (param.sfo, icon0.png, ...) plus key material
    outer PFS    -> XTS-AES encrypted, signed filesystem image
        pfs_image.dat (PFSC container: 64 KiB blocks, each zlib-compressed or stored)
            inner PFS -> the game's own files (eboot.bin, dvdbnd*.bdt, ...)

For a fake package the RSA keys that wrap the PFS key are public, so the chain is
    entry_keys[3]  --RSA(PkgDerivedKey3)-->  dk3
    sha256(image_key entry header || dk3)  -> AES-CBC iv/key
    image_key data --AES-CBC--> --RSA(FakeKeyset)--> EKPFS
    HMAC-SHA256(EKPFS, 1 || seed) -> tweak key || data key  (XTS-AES-128, 0x1000 sectors)
Ported from the PKG reader in shadPS4 v0.7.0 (GPL-2.0-or-later), restructured to walk
the inner filesystem as a tree rather than by scanning blocks.
"""

from __future__ import annotations

import fnmatch
import hashlib
import hmac
import io
import os
import struct
import sys
import time
import zlib
from dataclasses import dataclass

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pkgkeys

SECTOR = 0x1000


def rsa_decrypt(cipher: bytes, n: int, d: int) -> bytes:
    """Raw RSA then strip PKCS#1 v1.5 type-2 padding."""
    m = pow(int.from_bytes(cipher, "big"), d, n).to_bytes(256, "big")
    if m[0:2] != b"\x00\x02":
        raise ValueError("bad PKCS#1 padding - not a fake-signed package?")
    return m[m.index(b"\x00", 2) + 1 :]


@dataclass
class Entry:
    id: int
    name_off: int
    flags1: int
    flags2: int
    offset: int
    size: int
    raw: bytes
    name: str = ""

    @property
    def encrypted(self) -> bool:
        return bool(self.flags1 & 0x80000000)


class Pkg:
    def __init__(self, path: str):
        self.path = path
        self.f = open(path, "rb")  # noqa: SIM115  (kept open: entries are read on demand)
        h = self.f.read(0x1000)
        self.header = h
        if h[:4] != b"\x7fCNT":
            raise ValueError("not a PS4 PKG")
        (
            self.pkg_type,
            _,
            self.file_count,
            self.entry_count,
            self.sc_entry_count,
            _,
            self.entry_table_offset,
            self.sc_data_size,
            self.body_offset,
            self.body_size,
            self.content_offset,
            self.content_size,
        ) = struct.unpack_from(">IIIIHHIIQQQQ", h, 4)
        self.content_id = h[0x40:0x64].decode()
        (
            self.drm_type,
            self.content_type,
            self.content_flags,
            self.promote_size,
            self.version_date,
            self.version_hash,
        ) = struct.unpack_from(">IIIIII", h, 0x70)
        (
            self.pfs_image_count,
            self.pfs_flags,
            self.pfs_offset,
            self.pfs_size,
            self.mount_offset,
            self.mount_size,
            self.pkg_size,
            self.pfs_signed_size,
            self.pfs_cache_size,
        ) = struct.unpack_from(">IQQQQQQII", h, 0x404)
        self.entries: list[Entry] = []
        self.f.seek(self.entry_table_offset)
        for _ in range(self.entry_count):
            raw = self.f.read(32)
            e = Entry(*struct.unpack(">IIIIII", raw[:24]), raw=raw)
            self.entries.append(e)
        names = self.entry_data(0x200)
        for e in self.entries:
            if names is not None and e.name_off:
                e.name = names[e.name_off : names.index(b"\0", e.name_off)].decode()
            else:
                e.name = {
                    0x1: ".digests",
                    0x10: ".entry_keys",
                    0x20: ".image_key",
                    0x80: ".general_digests",
                    0x100: ".metas",
                    0x200: ".entry_names",
                    0x400: "license.dat",
                    0x401: "license.info",
                    0x402: "nptitle.dat",
                    0x403: "npbind.dat",
                    0x404: "selfinfo.dat",
                    0x406: "imageinfo.dat",
                    0x407: "target-deltainfo.dat",
                    0x408: "origin-deltainfo.dat",
                    0x409: "psreserved.dat",
                }.get(e.id, f"entry_{e.id:04x}")
        self._keys_ready = False

    def entry(self, eid: int) -> Entry | None:
        return next((e for e in self.entries if e.id == eid), None)

    def entry_data(self, eid: int) -> bytes | None:
        e = self.entry(eid)
        if e is None:
            return None
        self.f.seek(e.offset)
        return self.f.read(e.size)

    # --- keys -----------------------------------------------------------------------
    def derive_keys(self):
        if self._keys_ready:
            return
        ek = self.entry_data(0x10)
        ie = self.entry(0x20)
        ie_data = self.entry_data(0x20)
        if ek is None or ie is None or ie_data is None:
            raise ValueError("package has no key entries (0x10, 0x20)")
        key1 = [ek[0x20 + 7 * 32 + i * 256 :][:256] for i in range(7)]
        self.dk3 = rsa_decrypt(
            key1[3],
            pkgkeys.PKGDERIVEDKEY3KEYSET_MODULUS,
            pkgkeys.PKGDERIVEDKEY3KEYSET_PRIVATEEXPONENT,
        )[:32]
        ivkey = hashlib.sha256(ie.raw + self.dk3).digest()
        dec = Cipher(algorithms.AES(ivkey[16:]), modes.CBC(ivkey[:16])).decryptor()
        img_key = dec.update(ie_data[:256]) + dec.finalize()
        self.ekpfs = rsa_decrypt(
            img_key, pkgkeys.FAKEKEYSET_MODULUS, pkgkeys.FAKEKEYSET_PRIVATEEXPONENT
        )[:32]
        self.f.seek(self.pfs_offset + 0x370)
        self.seed = self.f.read(16)
        dt = hmac.new(
            self.ekpfs, struct.pack("<I", 1) + self.seed, hashlib.sha256
        ).digest()
        self.tweak_key, self.data_key = dt[:16], dt[16:]
        self._xts_key = self.data_key + self.tweak_key
        self._keys_ready = True

    def decrypt_entry(self, e: Entry) -> bytes:
        """sce_sys entries flagged encrypted (license.dat, nptitle.dat, ...)."""
        self.derive_keys()
        ivkey = hashlib.sha256(e.raw + self.dk3).digest()
        self.f.seek(e.offset)
        data = self.f.read((e.size + 15) & ~15)
        dec = Cipher(algorithms.AES(ivkey[16:]), modes.CBC(ivkey[:16])).decryptor()
        return (dec.update(data) + dec.finalize())[: e.size]

    # --- outer PFS (encrypted) -------------------------------------------------------
    def outer_read(self, off: int, n: int) -> bytes:
        """Read decrypted bytes from the outer PFS image."""
        self.derive_keys()
        start = off & ~(SECTOR - 1)
        end = (off + n + SECTOR - 1) & ~(SECTOR - 1)
        self.f.seek(self.pfs_offset + start)
        enc = self.f.read(end - start)
        out = bytearray()
        for i in range(0, len(enc), SECTOR):
            sector = (start + i) // SECTOR
            d = Cipher(
                algorithms.AES(self._xts_key), modes.XTS(sector.to_bytes(16, "little"))
            ).decryptor()
            out += d.update(enc[i : i + SECTOR])
        rel = off - start
        return bytes(out[rel : rel + n])

    def open_pfsc(self):
        if hasattr(self, "pfsc_off"):
            return
        for off in range(0x20000, self.pfs_size, 0x10000):
            if self.outer_read(off, 4) == b"PFSC":
                self.pfsc_off = off
                break
        else:
            raise ValueError("PFSC container not found in outer PFS")
        hdr = self.outer_read(self.pfsc_off, 0x30)
        (
            _,
            _,
            _,
            self.pfsc_block,
            self.pfsc_block2,
            map_off,
            self.pfsc_data_start,
            self.inner_size,
        ) = struct.unpack_from("<4sIIIQQQQ", hdr)
        nblocks = (self.inner_size + self.pfsc_block2 - 1) // self.pfsc_block2
        m = self.outer_read(self.pfsc_off + map_off, (nblocks + 1) * 8)
        self.sector_map = struct.unpack(f"<{nblocks + 1}Q", m)

    # --- inner PFS (PFSC-compressed) -------------------------------------------------
    def inner_blocks(self, first: int, count: int):
        """Yield decompressed 64 KiB blocks of the inner PFS image."""
        self.open_pfsc()
        bs = self.pfsc_block2
        if count <= 0:
            return
        lo = self.sector_map[first]
        hi = self.sector_map[first + count]
        chunk = self.outer_read(self.pfsc_off + lo, hi - lo)
        for b in range(first, first + count):
            a, z = self.sector_map[b] - lo, self.sector_map[b + 1] - lo
            if z - a == bs:
                yield chunk[a:z]
            elif z == a:
                yield bytes(bs)
            else:
                yield zlib.decompress(chunk[a:z]).ljust(bs, b"\0")

    def inner_read_blocks(self, first: int, count: int) -> bytes:
        return b"".join(self.inner_blocks(first, count))


INODE_SIZE = 0xA8
DT_FILE, DT_DIR, DT_DOT, DT_DOTDOT = 2, 3, 4, 5


@dataclass
class Inode:
    ino: int
    mode: int
    flags: int
    size: int
    size_compressed: int
    blocks: int
    loc: int


class InnerFs:
    def __init__(self, pkg: Pkg):
        self.pkg = pkg
        sb = pkg.inner_read_blocks(0, 1)
        (
            self.version,
            self.magic,
            self.id,
            self.fmode,
            self.clean,
            self.readonly,
            _,
            self.mode,
            _,
            self.block_size,
            self.nbackup,
            self.nblock,
            self.dinode_count,
            self.ndblock,
            self.dinode_block_count,
            self.superroot_ino,
        ) = struct.unpack_from("<QQQBBBBHHIIQQQQQ", sb)
        if self.magic != 20130315:
            raise ValueError(f"inner PFS magic {self.magic} unexpected")
        if self.mode & 0x3:
            raise ValueError(
                f"inner PFS mode {self.mode:#x}: signed/64-bit inodes unsupported"
            )
        per_block = self.block_size // INODE_SIZE
        self.inodes: list[Inode] = []
        for bi, blk in enumerate(pkg.inner_blocks(1, self.dinode_block_count)):
            for k in range(per_block):
                ino = bi * per_block + k
                if ino >= self.dinode_count:
                    break
                p = k * INODE_SIZE
                mode, _, flags, size, sizec = struct.unpack_from("<HHIQQ", blk, p)
                blocks, loc = struct.unpack_from("<II", blk, p + 0x60)
                self.inodes.append(Inode(ino, mode, flags, size, sizec, blocks, loc))

    def dirents(self, ino: int):
        node = self.inodes[ino]
        data = self.pkg.inner_read_blocks(node.loc, node.blocks)
        p = 0
        while p < min(len(data), max(node.size, len(data))):
            d_ino, d_type, namelen, entsize = struct.unpack_from("<iiii", data, p)
            if entsize == 0:
                # directories pad each block; skip to the next block boundary
                nxt = (p // self.block_size + 1) * self.block_size
                if nxt >= len(data):
                    break
                p = nxt
                continue
            yield (
                d_ino,
                d_type,
                data[p + 16 : p + 16 + namelen].decode("utf-8", "replace"),
            )
            p += entsize

    def walk(self):
        """Yield (path, Inode, is_dir) for everything under uroot."""
        root = next(
            i
            for i, t, n in self.dirents(self.superroot_ino)
            if n == "uroot" and t == DT_DIR
        )
        stack = [("", root)]
        while stack:
            prefix, ino = stack.pop()
            for d_ino, d_type, name in self.dirents(ino):
                if d_type in (DT_DOT, DT_DOTDOT):
                    continue
                path = f"{prefix}{name}"
                if d_type == DT_DIR:
                    yield path + "/", self.inodes[d_ino], True
                    stack.append((path + "/", d_ino))
                else:
                    yield path, self.inodes[d_ino], False

    def read_file(self, node: Inode, out):
        bs = self.block_size
        remaining = node.size
        step = 256  # blocks per batch (16 MiB)
        for b in range(0, node.blocks, step):
            n = min(step, node.blocks - b)
            for blk in self.pkg.inner_blocks(node.loc + b, n):
                w = min(remaining, bs)
                out.write(blk[:w])
                remaining -= w


def human(n: float) -> str:
    for u in ("B", "KiB", "MiB", "GiB"):
        if n < 1024:
            return f"{n:.1f} {u}" if u != "B" else f"{int(n)} B"
        n /= 1024
    return f"{n:.2f} TiB"


def cmd_info(pkg: Pkg):
    print(f"content id      {pkg.content_id}")
    print(
        f"pkg type        {pkg.pkg_type:#x}   drm {pkg.drm_type:#x}   content type {pkg.content_type:#x}"
        f"   content flags {pkg.content_flags:#010x}"
    )
    print(f"version date    {pkg.version_date:#x}  hash {pkg.version_hash:#x}")
    print(f"entries         {pkg.entry_count} at {pkg.entry_table_offset:#x}")
    print(
        f"pfs image       offset {pkg.pfs_offset:#x}  size {pkg.pfs_size:#x} ({human(pkg.pfs_size)})"
        f"  flags {pkg.pfs_flags:#x}"
    )
    print(f"pkg size        {pkg.pkg_size:#x} ({human(pkg.pkg_size)})")
    print()
    print("  id     flags1     flags2     offset     size  name")
    for e in pkg.entries:
        print(
            f"  {e.id:04x}  {e.flags1:08x}  {e.flags2:08x}  {e.offset:9x}  {e.size:7x}  {e.name}"
            + ("  [enc]" if e.encrypted else "")
        )
    pkg.derive_keys()
    print()
    print(f"EKPFS           {pkg.ekpfs.hex()}")
    print(f"seed            {pkg.seed.hex()}")
    pkg.open_pfsc()
    print(
        f"PFSC            at outer+{pkg.pfsc_off:#x}, block {pkg.pfsc_block2:#x}, "
        f"inner image {pkg.inner_size:#x} ({human(pkg.inner_size)}), {len(pkg.sector_map) - 1} blocks"
    )
    fs = InnerFs(pkg)
    print(
        f"inner PFS       mode {fs.mode:#x}, block {fs.block_size:#x}, {fs.dinode_count} inodes, "
        f"superroot ino {fs.superroot_ino}"
    )


def cmd_list(pkg: Pkg, patterns):
    fs = InnerFs(pkg)
    total = 0
    count = 0
    for path, node, is_dir in sorted(fs.walk(), key=lambda t: t[0]):
        if patterns and not any(fnmatch.fnmatch(path, p) for p in patterns):
            continue
        if not is_dir:
            total += node.size
            count += 1
        print(f"{'' if is_dir else node.size:>12}  {path}")
    print(f"{count} files, {human(total)}", file=sys.stderr)


def cmd_extract(pkg: Pkg, out_dir, patterns):
    os.makedirs(os.path.join(out_dir, "sce_sys"), exist_ok=True)
    for e in pkg.entries:
        if e.id < 0x400:
            continue  # key material and tables, not files
        data = pkg.decrypt_entry(e) if e.encrypted else pkg.entry_data(e.id)
        if data is None:
            continue
        p = os.path.join(out_dir, "sce_sys", e.name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as fo:
            fo.write(data)
    fs = InnerFs(pkg)
    items = [t for t in fs.walk() if not t[2]]
    if patterns:
        items = [t for t in items if any(fnmatch.fnmatch(t[0], p) for p in patterns)]
    total = sum(n.size for _, n, _ in items)
    done = 0
    t0 = time.time()
    for path, node, _ in items:
        dst = os.path.join(out_dir, *path.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.exists(dst) and os.path.getsize(dst) == node.size:
            done += node.size
            continue
        with open(dst + ".part", "wb") as fo:
            fs.read_file(node, fo)
        os.replace(dst + ".part", dst)
        done += node.size
        rate = done / max(time.time() - t0, 1e-6)
        print(f"[{human(done)} / {human(total)}  {human(rate)}/s]  {path}", flush=True)
    print(f"extracted {len(items)} files, {human(total)}")


def main(argv):
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, path = argv[1], argv[2]
    pkg = Pkg(path)
    if cmd == "info":
        cmd_info(pkg)
    elif cmd == "list":
        cmd_list(pkg, argv[3:])
    elif cmd == "extract":
        cmd_extract(pkg, argv[3], argv[4:])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
