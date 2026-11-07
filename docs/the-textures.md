# Textures and item pictures

Can the game's artwork be pulled out as ordinary image files? For 2D textures, yes. This page
covers the item pictures (the large artwork on the item-description screen) and, with the same
tool, any texture in a menu TPF: the container, the PS4 GPU's tiled layout, and the compression.

```
python -I tools/textures.py items work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/pictures/items
python -I tools/textures.py tpf   work/base/dvdroot_ps4/menu/title.tpf.dcx work/pictures/title
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
textures row by row. It stores them in small square tiles, so that neighbouring pixels are close
together in memory:

1. the image, in blocks, is padded to a multiple of 8 each way;
2. it is cut into **tiles of 8×8 blocks**, stored left to right, top to bottom;
3. inside a tile the 64 blocks are in **Morton (Z) order**. Block (*x*, *y*) is at the index made
   by interleaving the bits of *x* and *y*, *x* first: (1, 0) is 1, (0, 1) is 2, (1, 1) is 3,
   (2, 0) is 4, and so on.

Decoded *without* this step, the picture is unrecognisable: horizontal streaks of the right
colours, because each 8-block-wide row of the output is really one tile's blocks in Z order.
Untiled, it is the item's artwork. The same rule holds for BC1's 8-byte blocks: the title
screen's 2048×1024 texture (the hunter in a Yharnam street, with the logo) decodes cleanly.

`tools/textures.py` untiles (`untile()`), then decodes with Pillow's `bcn` decoder and crops
away the padding.

## What the pictures are

| Group | Pictures | Named after |
|---|---:|---|
| armour | 221 | `EquipParamProtector` |
| goods (consumables, key items, bells, …) | 219 | `EquipParamGoods` |
| weapons | 41 | `EquipParamWeapon` |
| blood gems (3000–4199) | 490 | — |
| rune glyphs (4200–4245) | 55 | — |
| other, unnamed | 30 | — |

Each picture is written as `NNNNN <item name>.png`, and `index.md` beside them lists every
picture with every item that uses it.

**Gems and runes have no item rows to match.** A blood gem is generated when it drops
([where the items are](the-items.md#what-it-doesnt-cover-yet)), so it gets its picture from its
effects, not from a fixed item. Looking at the pictures, the numbering is systematic:
- the hundreds digit is the gem's shape (a radial star at 31xx, a triangle at 32xx, a ring at
  37xx, round "droplets" at 40xx–41xx);
- the last digit is its size and glow (3100 is plain, 3106 large and glowing);
- the tens digit is a colour variant (3110 is blue rather than red).

Part of the mapping is in the data: for 101 pictures, the number is 3000 plus a
`GemeffectParam` row's `gemIconIdOffset`. The covenant rune effect "Cont. heal near death"
(offset 1014) has picture 4014, for example. How the rest are chosen hasn't been worked out. The
55 pictures at 4200–4245 are rune glyphs.

The **30 other unnamed pictures** belong to item rows with no name in the text, such as hoods
(2116–2131), a ring (2142), a chest of vials (2) and a lantern (33). Whether they're cut
content or simply unnamed hasn't been checked.

## What this doesn't cover

* **Textures inside other containers.** Characters, maps and objects keep their textures in TPFs
  packed inside BND4 archives (`chr/*.chrbnd`, `map/…`). The decoder handles the textures; the
  tool doesn't yet open those archives. They also hold flattened texture sheets (skin, normal
  maps), not pictures of the character.
* **Mip levels beyond the first, cube maps and arrays**, and formats other than BC1–BC5 and BC7.
  None occur in `menu/`.
* **The blood gem picture rule** beyond the `gemIconIdOffset` cases.
* **Pictures of 3D models.** That needs the model format (FLVER) and a renderer, a project of
  its own.

## Files

| Path | |
|---|---|
| `work/pictures/items/` | the 1,056 item pictures as PNG, and `index.md` (generated; not in the repository) |
