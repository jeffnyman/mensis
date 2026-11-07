# The package

What is inside the game's PKG file (and its 1.09 update), and how `tools/pkgtool.py` gets the
game's files out of it.

## Identity

| | Base PKG | Update PKG |
|---|---|---|
| Content ID | `EP9000-CUSA03173_00-BLOODBORNE0000EU` | same |
| Category (`param.sfo`) | `gd` (game data / full app) | `gp` (game patch) |
| `APP_VER` | 01.00 | 01.09 |
| Content flags | `0x0A000000` | `0x62100000` (first patch + cumulative patch) |
| Package size | 29.2 GiB | 172.9 MiB |
| Inner filesystem | 28,808 files, 29.3 GiB | 405 files, 167 MiB |
| `PUBTOOLINFO` | `c_date=20181010, sdk_ver=02000000` | `c_date=20181010` |

The *Game of the Year* edition is the game with *The Old Hunters* already in it:
areas m33–m36 (Nightmare Frontier, Hunter's Nightmare, Research Hall, Fishing Hamlet) are in the
base package itself, not a separate add-on. `SYSTEM_VER` is 4.50.

## Layers

```
PKG file (big-endian header, magic 7F 43 4E 54 = "\x7FCNT")
├── header 0x000–0xFFF   content id, entry table offset, PFS image offset/size, digests
├── entry table @0x2A80  23 entries (37 in the update), 32 bytes each
│   ├── 0x0001 digests, 0x0010 entry_keys, 0x0020 image_key, 0x0080 general_digests,
│   │   0x0100 metas, 0x0200 entry_names                      ← key material and tables
│   ├── 0x0400 license.dat, 0x0401 license.info, 0x0402 nptitle.dat, 0x0403 npbind.dat
│   │                                                          ← AES-CBC encrypted
│   └── 0x1000 param.sfo, 0x1001 playgo-chunk.dat, 0x1200 icon0.png, 0x1400 trophy00.trp, …
│                                                              ← plain → sce_sys/
└── outer PFS image @0xC00000, 0x74BEC0000 bytes   XTS-AES-128, 0x1000-byte sectors
    └── pfs_image.dat (a "PFSC" container) @ outer+0x10E0000
        ├── header: block size 0x10000, offset of block map, data length
        ├── block map: 499,519 × u64 offsets   (block i = [map[i], map[i+1]))
        └── 499,518 blocks of 64 KiB, each zlib-compressed, or stored if exactly 0x10000
            └── inner PFS (30.5 GiB uncompressed)
                ├── superblock: magic 20130315, block 0x10000, mode 0x8, 29,128 inodes
                ├── inode table (blocks 1…): 0xA8 bytes each, files are one contiguous run
                └── superroot (inode 0) → flat_path_table, uroot/ ← the game's root dir
```

The outer PFS is signed, encrypted and holds one file. The inner PFS is unsigned and
unencrypted but compressed, and it is the filesystem the game sees as `/app0`.

## The keys, and why this works at all

A retail PKG cannot be opened off the console. Its PFS key (EKPFS) is wrapped with keys that live
in the console's secure processor and are not public. These packages are **fake packages**
(fpkg): they were re-signed for consoles running homebrew, and that toolchain wraps the keys with
RSA keysets that are public (they ship in every fpkg tool and in shadPS4). Unwrapping takes four
steps:

1. `entry_keys` (0x0010) holds seven 256-byte RSA blocks. Block 3 decrypts with the
   **PkgDerivedKey3** keyset to give `dk3` (32 bytes).
2. `sha256(raw 32-byte image_key entry header ‖ dk3)` gives an IV (first 16 bytes) and an AES key
   (last 16 bytes).
3. The 256-byte `image_key` (0x0020) payload, AES-CBC-decrypted with those, then RSA-decrypted
   with the **FakeKeyset**, gives `EKPFS`.
4. `HMAC-SHA256(EKPFS, u32le(1) ‖ seed)`, where `seed` is the 16 bytes at outer PFS + 0x370, splits
   into the **tweak key** (first 16 bytes) and the **data key** (last 16 bytes). Sector *n* of the
   outer image is XTS-AES-128 with tweak *n*.

The encrypted `sce_sys` entries (license, NP title/bind) use the same `sha256(entry ‖ dk3)` trick
with their own entry header. `license.dat` decrypts to an `RIF\0` license and `nptitle.dat` to
`NPTD`, which shows the derivation is right.

This only works because the package is a fake one. The same code on a retail PKG fails at step 1
with "bad PKCS#1 padding", and nothing short of the console's keys gets past that.

## PlayGo

`playgo-chunk.dat` and `playgo-manifest.xml` describe three install chunks in one scenario
(`initial_chunk_count=1`), so the game can start before the whole disc is copied.
`sce_discmap.plt` (3.1 MiB) maps files to chunks. The executable imports `scePlayGoGetLocus` and
`scePlayGoGetChunkId` to ask whether a chunk has landed yet.

## The update is file replacement

The 1.09 update is a cumulative patch. Its inner PFS carries whole replacement files, not binary
deltas. It also carries `sce_discmap_patch.plt` and `origin-deltainfo.dat`. To get the 1.09 game
tree, lay `work/update/` over `work/base/`. It replaces:

* `eboot.bin` (the whole executable, 89.5 MiB)
* `gameparam.parambnd.dcx` (balance: "Improved game balance" in `changeinfo.xml`)
* 268 `map/entryfilelist/*` and 26 `map/breakobj/*` files
* event scripts (`*.emevd.dcx`, `*.emeld.dcx`): the common script and nine maps; AI Lua for seven
  maps; talk scripts for three
* the map layouts (`*.msb.dcx`) of the Research Hall (m35) and the Fishing Hamlet (m36)
* a handful of models, animation banks, navmeshes, cutscenes (`remo`) and sounds, mostly in the
  DLC areas
* `msg/*/` for every language

`changeinfo/` keeps release notes back to 1.07 ("Various bug fixes"): 16 files, one per
language. What the replaced files actually change, function by function and table by table, is
in [what the 1.09 patch changed](the-patch.md).

## Using the tool

```
python -I tools/pkgtool.py info    <pkg>                  # header, entries, keys, PFS geometry
python -I tools/pkgtool.py list    <pkg> [glob …]         # inner file tree with sizes
python -I tools/pkgtool.py extract <pkg> <out> [glob …]   # sce_sys/ + matching files
```

Extracting the whole base package runs at about 250 MiB/s and takes about two minutes. Files
already present at the right size are skipped, so an interrupted run can be restarted. It needs
`cryptography`; the project's `.venv` has it.
