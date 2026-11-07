# Textures and item pictures

Can the game's artwork be pulled out as ordinary image files? For 2D textures, yes, all of it.
This page covers the item pictures (the large artwork on the item-description screen) and the
textures of characters, equipment, objects and maps: the containers, the PS4 GPU's tiled layout,
and the compression.

```
python -I tools/textures.py items  work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/pictures/items
python -I tools/textures.py models work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/pictures/models [chr,parts,obj,map] [--color] [--low]
python -I tools/textures.py tpf    <a .tpf, .chrbnd/.partsbnd/.objbnd or .tpfbhd> <out dir>
```

The output is the game's own artwork, so like everything else the tools produce it goes under
`work/`, which the repository ignores.

## Where the pictures are

`menu/` holds **1,056 files `menu_knowledge_NNNNN.tpf.dcx`**, numbered 00001 to 05022, each with
a single texture named `MENU_Knowledge_NNNNN`. The number is an **icon ID**: the
`iconId` field of `EquipParamGoods` and `EquipParamWeapon`, and `iconIdM`/`iconIdF` of
`EquipParamProtector`. Picture 1 is iconId 1, Quicksilver Bullets, and it does show a bullet.
Weapon upgrade levels share their weapon's picture (all of Beast Claw +0 to +10 use one).

## The container: TPF

A TPF ("texture pack file") is a header and one 0x24-byte entry per texture. On the PS4:

| Offset | Field |
|---|---|
| 0x00 | `TPF\0`, total data size, texture count, platform (**4** = PS4), a flag, name encoding (1 = UTF-16) |
| 0x10 + 0x24 *i* | data offset, data size, format byte, type (0 = 2D), mip count, flags, width, height, 1, 0x0d, name offset, 0, **DXGI format** |

The DXGI format is Direct3D's standard texture-format number. Across all 1,061 TPFs in `menu/`
there are 1,180 textures in just two formats, all single 2D images with one mip level:

| DXGI | Format | Bytes per 4×4 block | Textures |
|---:|---|---:|---:|
| 98 | BC7 | 16 | 1,078 (including almost all item pictures) |
| 71 | BC1 | 8 | 102 (some item pictures, the title screen) |

Both are **block compression**: the image is cut into 4×4-pixel blocks and each block is stored
in a fixed 8 or 16 bytes. Decoding them is standard; Pillow does it.

## The tiling

The first item picture is 400×400 pixels: 100×100 blocks of 16 bytes. That would be 160,000
bytes, but the file stores **173,056**, which is exactly 104×104 blocks. The extra is padding to
a multiple of 8 blocks each way, the sign of a **tiled** layout. The PS4's GPU doesn't store
textures row by row. It stores them in small square tiles, so that neighboring pixels are close
together in memory:

1. the image, in blocks, is padded to a multiple of 8 each way;
2. it is cut into **tiles of 8×8 blocks**, stored left to right, top to bottom;
3. inside a tile the 64 blocks are in **Morton (Z) order**. Block (*x*, *y*) is at the index made
   by interleaving the bits of *x* and *y*, *x* first: (1, 0) is 1, (0, 1) is 2, (1, 1) is 3,
   (2, 0) is 4, and so on.

Decoded *without* this step, the picture is unrecognizable: horizontal streaks of the right
colors, because each 8-block-wide row of the output is really one tile's blocks in Z order.
Untiled, it is the item's artwork. The same rule holds for BC1's 8-byte blocks: the title
screen's 2048×1024 texture (the hunter in a Yharnam street, with the logo) decodes cleanly.

`tools/textures.py` untiles (`untile()`), then decodes with Pillow's `bcn` decoder and crops
away the padding.

## What the pictures are

| Group | Pictures | Named after |
|---|---:|---|
| armor | 221 | `EquipParamProtector` |
| goods (consumables, key items, bells, …) | 219 | `EquipParamGoods` |
| weapons | 41 | `EquipParamWeapon` |
| blood gems (3000–4199) | 490 | — |
| rune glyphs (4200–4254) | 55 | — |
| other, unnamed | 30 | — |

Each picture is written as `NNNNN <item name>.png`, and `index.md` beside them lists every
picture with every item that uses it.

**Gems and runes have no item rows to match.** A blood gem is generated when it drops
([where the items are](the-items.md#what-it-doesnt-cover-yet)), so it gets its picture from its
effects, not from a fixed item. Looking at the pictures, the numbering is systematic:
- the hundreds digit is the gem's shape (a radial star at 31xx, a triangle at 32xx, a ring at
  37xx, round "droplets" at 40xx–41xx);
- the last digit is its size and glow (3100 is plain, 3106 large and glowing);
- the tens digit is a color variant (3110 is blue rather than red).

Part of the mapping is in the data: for 101 pictures, the number is 3000 plus a
`GemeffectParam` row's `gemIconIdOffset`. That table holds rune effects as well as gem effects,
so the rule gives some rune effects a picture here too: the covenant rune effect "Cont. heal near
death" (offset 1014) has picture 4014, a round droplet. Which picture the game shows for a rune,
this or a glyph, hasn't been checked, and how the other gem pictures are chosen hasn't been
worked out. The 55 pictures at 4200–4254 are rune glyphs.

The **30 other unnamed pictures** belong to item rows with no name in the text, such as hoods
(2116–2131), a ring (2142), a chest of vials (2) and a lantern (33). Whether they're cut
content or simply unnamed hasn't been checked.

## Characters, equipment, objects and maps

Everything else with a texture keeps it in a TPF one layer down, inside an archive:

| Where | Container | Holds |
|---|---|---|
| `chr/cNNNN.chrbnd.dcx` (245), `chr/cNNNN_N.tpf.dcx` (24) | BND4 | a character's TPF, its model (`.flver`), Havok physics and cloth data |
| `parts/xx_y_NNNN.partsbnd.dcx` | BND4 | one piece of player equipment: a TPF and a model |
| `obj/oNNNNNN.objbnd.dcx` (1,880) | BND4 | an object's TPF, model and animations |
| `map/mNN/mNN_NNNN.tpfbhd` + `.tpfbdt` (56 pairs) | split BXF4 | a map area's texture packs: one DCX-compressed TPF per file, 493 in `m21_0000` |

A **split archive** keeps the directory and the data apart. The `.tpfbhd` header (`BHF4`) has
exactly BND4's header and entry layout, and its offsets point into the `.tpfbdt` (`BDF4`).
`souls.py`'s BND4 reader reads both, given the data file as a second argument.

The formats are the same block-compressed family, with sRGB variants and one-channel BC4 added:

| DXGI | Format | Textures (characters, equipment, a tenth of the objects) |
|---:|---|---:|
| 72 | BC1, sRGB | 3,942 |
| 80 | BC4 (one channel) | 2,123 |
| 71 | BC1 | 1,808 |
| 99 | BC7, sRGB | 348 |
| 98 | BC7 | 213 |
| 77, 78 | BC3 | 27 |

Unlike the menu pictures, nearly all have **mip levels**, smaller copies for distance. The full-
size level is stored first and the same tiling rule holds, so taking the start of the data and
untiling it works. 2048×2048 textures, BC4 and every format above decode cleanly.

A character's textures come in sets of four per part, by name suffix. `c2010`, for example, has
`Body`, `Hook`, `Lantern`, `Pickel`, `Robe` and a weapon, each with:

| Suffix | Looks like |
|---|---|
| `_a` | the color of the surface (the character's face and hands are recognizable in `c2010_Body_a`) |
| `_n` | a normal map, the familiar lilac-blue image of surface bumps |
| `_r`, `_s` | grayscale maps, probably reflectance and shininess or roughness; the letters are the game's, the reading is a guess from how they look |

These are **texture sheets**: the surfaces of a 3D model, cut apart and laid flat. They aren't
pictures of the character, which would need the model itself (below).

**Names.** Equipment folders are named from the item tables, which give each weapon and armor
piece an `equipModelId`. A weapon's parts file is `wp_a_<model>` (Chikage is `wp_a_1063`, shared
with Uncanny and Lost Chikage). Armor is `hd`/`bd`/`am`/`lg` (head, body, arms, legs), then `m`,
`f` or `a` (male, female, both), then the set's model number, so `hd_m_1100` is the Gray Wolf Cap.
Each piece also has an `_l` twin, almost certainly a lower-detail version, skipped unless `--low`
is given. Character folders are named by model only. The index lists the names the game shows
for characters placed with that model, but that isn't the model's identity. `c0000` is the human
model every hunter uses, and the bell-ringers in The One Reborn's arena carry the boss's name.

**Sizes.** Characters are 3,734 textures (2.2 GB as PNG, about 3 minutes) and equipment 2,184
(3.2 GB). One map pack is 137 MB, so all 56 come to roughly 7–8 GB. `--color` keeps only the
`_a` textures.

## What this doesn't cover

* **Cube maps, texture arrays and formats outside BC1–BC5 and BC7.** None turned up in the
  containers surveyed; the tool reports any it skips.
* **Mip levels beyond the first**, which are smaller copies of the same image.
* **The blood gem picture rule** beyond the `gemIconIdOffset` cases.
* **Pictures of 3D models**, which are on the next page,
  [models and character renders](the-models.md).

## Files

| Path | |
|---|---|
| `work/pictures/items/` | the 1,056 item pictures as PNG, and `index.md` (generated; not in the repository) |
| `work/pictures/models/` | `chr/`, `parts/` (and `obj/`, `map/` when asked for), one folder per model or map pack, and `index.md` |
