# Models and character renders

The textures on [the previous page](the-textures.md) are flat sheets. To see a character as a
character you need its 3D model: FromSoftware's **FLVER** format, which sits in every character,
equipment and object archive beside the TPF. This page reads FLVER far enough to export a model
to a standard 3D file and to render a picture of it.

```
python -I tools/flver.py info   work/base/dvdroot_ps4/chr/c2010.chrbnd.dcx
python -I tools/flver.py render work/base/dvdroot_ps4/chr/c2010.chrbnd.dcx work/pictures/c2010.png
python -I tools/flver.py obj    work/base/dvdroot_ps4/chr/c5100.chrbnd.dcx work/pictures/obj/c5100
python -I tools/flver.py all    work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/pictures/renders
python -I tools/flver.py parts  work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/pictures/equipment
```

`obj` writes a Wavefront `.obj` with an `.mtl` and the colour textures as PNG. Most 3D programs
open that, including Windows' 3D Viewer and Blender, and you can turn the model around in them.
`render` draws a still picture. `all` renders every character and `parts` every piece of
equipment, each with an index. As with the
textures, everything goes under `work/`, which the repository ignores.

## The format

A character archive (`chr/cNNNN.chrbnd.dcx`, a BND4) holds the TPF, the model (`cNNNN.flver`),
and Havok physics and cloth data. The FLVER is little-endian, version `0x20014`, and is a header
followed by packed tables:

| Table | Entry | Holds |
|---|---:|---|
| header | 0x80 | version, where the geometry starts, the count of everything below, a bounding box, the index size (0 = per face set) |
| dummies | 0x40 | attachment points |
| materials | 0x20 | a name, the shader definition (an `.mtd` path), and a range of texture slots |
| bones | 0x80 | name, translation, rotation, scale, parent, child and sibling links, bounding box |
| meshes | 0x30 | a *dynamic* flag, its material, its face sets and vertex buffers |
| face sets | 0x20 | flags (detail level), strip or list, an index count and offset, the index size |
| vertex buffers | 0x20 | which layout, vertex size and count, offset |
| buffer layouts | 0x10 | the members of a vertex: offset, data type and meaning |
| textures | 0x20 | a texture path and its slot type (`g_Diffuse`, …) |

The lantern-bearer `c2010`, for example, has 134 bones, 11 meshes (lantern, hook, pick, weapon,
body, robe and five cloth pieces) and 66 face sets.

**Vertices are described, not fixed.** Each mesh's buffers follow a layout that lists its
members, and the six layouts in `c2010` mix position (three floats), normal, tangents, bone
indices and weights, a colour and UVs, in different orders. Some meshes split them across two
buffers: the cloth pieces keep position and normal in one and the UVs in another. The reader
merges them. The data types used are a handful: floats, four bytes (normals as (*b* − 127) / 127,
or bone indices), and UVs as signed 16-bit numbers divided by 2048. That scale was confirmed by
rendering: with it, the face texture lands exactly on the face.

**Detail levels.** A mesh has several face sets, and their flags mark the lower-detail copies the
game swaps in at a distance. The reader takes the one with no detail flag set. All the meshes
looked at use plain triangle lists with 16-bit indices. The reader also handles strips, with
their restart index skipped, in case others don't.

**Rest pose.** Every mesh looked at is *dynamic*: skinned to the skeleton, with its vertices
already in the model's rest pose. So no bone arithmetic is needed for a still picture, and every
character comes out in that pose: arms out, the A-pose it was modelled in.

**Characters face −Z.** The renderer looks from +Z, so unturned it shows a character's back;
renders default to a turn of 180° to show the front.

## The renderer

`render` is a small software rasteriser, about 70 lines with numpy, so it needs nothing beyond
the project's dependencies:

1. rotate the model (yaw, pitch) and scale it to fit the picture (an orthographic view, no
   perspective);
2. for each triangle, find the pixels it covers and, through a depth buffer, which of them it is
   in front at;
3. colour each pixel from the material's colour texture (`g_Diffuse`, the `_a` texture), using
   the interpolated UV, and drop pixels whose alpha is below a half (hair and ragged cloth edges);
4. light it with one light, through the interpolated normal.

It is deliberately plain: no normal maps, no shadows, no shaders. It shows the model and its
colours faithfully, not the game's lighting.

Some characters keep part of their texture set in a separate `chr/cNNNN_N.tpf.dcx` (Rom's is
`c5100_2`); the loader picks those up.

**Textures from elsewhere.** On the first full run, 43 models came out plain grey: none of their
colour textures were in their own archive. The names their materials ask for point elsewhere: to
**texture-only character archives** (`c4029`, `c4059`, …: 17 `chrbnd` files hold a TPF and no
model at all), to equipment (`BD_…`) and to objects (`o0211…`). So `flver.py` keeps a game-wide
index, texture name to archive, over `chr/`, `parts/`, `obj/` and `other/`, including standalone
texture files such as `parts/common_body.tpf.dcx`. That is 10,578 textures, built in about 25
seconds the first time a texture is missing, with each archive decoded only when needed. Names
are matched regardless of case, because materials and textures often disagree (`c2080_body_a`
against `c2080_Body_a`).

## What comes out

`all` over the 245 character archives (about 10 minutes, 74 MB of PNG at 1024×1024):

| | Models |
|---|---:|
| rendered | **217** |
| no model in the archive: texture-only, supplying other models | 17 |
| a model with no meshes: a skeleton only | 11 |

The 11 skeleton-only models include `c0000`, the human (below), and `c9030`. `c9030`'s
`NpcParam` row 903000 is the stand-in that sits in Chalice special-enemy spots
([the event scripts](the-chalice-events.md#special-enemies-what-rotted-offering-adds)), and here
it has nothing to draw.

With the index, 2 models are still entirely grey and 10 more have a grey part, for three reasons:

* **Faces are generated.** `FC_M_face` and `FG_eye_a` are in no file. `facegen/facegen.fgbnd`
  holds FaceGen data instead: a base face model, morph targets (`.tri`, `.egm`) and
  texture-generation data (`.egt`, with a base `face.bmp`). Human faces are made at run time,
  which is also how the character creator works.
* **Four textures don't ship.** `c2710_Axe_a`, `c2800_bow_a`, `c2810_Body_a` and
  `c5500_Body_NoLight_a` are named by materials but are in none of `chr/`, `parts/`, `obj/`,
  `other/`, the map texture packs, the effect archives or the patch.
* **Some materials have no colour slot** (12 meshes; effects, presumably).

An earlier version of this page reported 8 fully grey and 21 partly grey. Searching `other/` and
`parts/`'s standalone textures (where `BD_Ghost_Scroll_1_a`, used by 38 meshes, turned out to
be) and matching names regardless of case closed most of the gap.

The renders also answer a question the data couldn't. Model `c1110`, the "shop character" of
[the Chalice Dungeons' event scripts](the-chalice-events.md#special-characters) whose entity runs
the `クモ男パッチ` ("Patches the spider-man") events, is a giant spider with a man's head. The
same renderer handles equipment and objects: `wp_a_1063` is the Chikage's curved blade, and
`o240611`, the treasure corpse the Saw Spear lies on in Central Yharnam
([where the items are](the-items.md#what-it-finds)), is a dead hunter hanging from a 7-metre
chain.

## Equipment

`parts` renders every piece of player equipment (not the `_l` low-detail twins), named from the
item tables as on [the textures page](the-textures.md#characters-equipment-objects-and-maps).
Weapons are drawn side-on, armour from the front:

| Prefix | Is | Rendered |
|---|---|---:|
| `wp_a` | weapons and shields | 91 |
| `hd`, `bd`, `am`, `lg` | head, body, arms, legs (`_m`, `_f`, `_a` = male, female, both) | 386 |
| `fg_a`, `fc`, `hr_a` | face parts, faces and hair | 91 |

**568** pieces render (109 MB at 768×768); the other 27 have no meshes of their own, such as
`wp_a_1000`, Bare Fists.

Every weapon is recognisable from its name: Saw Cleaver, Logarius' Wheel, Stake Driver,
Whirligig Saw, the Wooden Shield. The armour comes out as garments: the Charred Hunter coat,
the Noble Dress, the Crowfeather Garb. Each piece is drawn on its own, in the pose it is fitted
to the body in. An arms piece is a pair of separate gloves, and a body piece a coat with no one in
it.

## What this doesn't cover

* **Poses and animation.** The renders are all in the rest pose. Posing a character means reading
  its skeleton's animations (`cNNNN.anibnd`, Havok data) and skinning the vertices to them.
* **The player character.** `c0000`, the human model every hunter uses, has no geometry of its
  own. It is assembled from equipment parts (`parts/hd_…`, `bd_…`, `am_…`, `lg_…`, `wp_…`), each a
  separate FLVER the renderer can draw on its own. Fitting them together onto one figure isn't
  done.
* **Materials beyond colour.** Normal, reflectance and shininess maps, and the game's shaders
  (`.mtd`), aren't applied.
* **Maps.** Map pieces are FLVERs too (`map/mNN_…/*.flver.dcx`, 11,033 of them), placed by the
  map layouts. Assembling a whole area is a different job.

## Files

| Path | |
|---|---|
| `work/pictures/renders/` | every character rendered from the front, with `index.md` (generated; not in the repository) |
| `work/pictures/equipment/` | every piece of equipment rendered, named, with `index.md` |
| `work/pictures/obj/` | models exported with `flver.py obj` |
