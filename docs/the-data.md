# The data

The inner filesystem has three top-level items: `eboot.bin`, `sce_module/` and `sce_discmap.plt`,
plus **`dvdroot_ps4/`**, which holds everything else. Bloodborne on PS4 ships its assets as
**loose files**: 28,808 of them. The PC Souls games pack everything into big encrypted
`dvdbnd*.bdt` archives. Here nothing is encrypted past the PKG layer, and nearly everything is a
FromSoftware format that the Souls modding community has already documented (SoulsFormats,
Paramdex, DSMapStudio, Smithbox, Yabber, …).

## The tree

| Directory | Size | Files | Holds |
|---|---:|---:|---|
| `map/` | 20.0 GiB | 17,211 | level geometry, textures, collision, navmesh, layouts |
| `sound/` | 3.5 GiB | 1,014 | FMOD banks (`sprj_<id>.fsb` + `.fev` + `.itl`), voices per language |
| `parts/` | 2.4 GiB | 1,190 | player equipment models (`am_`/`bd_`/`hd_`/`lg_` armour, `wp_` weapons) |
| `chr/` | 1.8 GiB | 479 | characters: 245 `cXXXX.chrbnd.dcx` (model + texture + skeleton + cloth), `anibnd` (animation), `esd` |
| `obj/` | 1.1 GiB | 1,880 | map objects `oXXXXXX.objbnd.dcx` |
| `movie/` | 340 MiB | 4 | `sprj_opening`, `sprj_staffroll`, `sprj_staffroll_dlc`, `sprj_advertise` (`.mp4`) |
| `menu/` | 185 MiB | 1,190 | Scaleform `.gfx` screens and their textures, per-language/region subfolders |
| `remo/` | 116 MiB | 42 | in-engine cutscenes `sXX_XX_XXXX.remobnd.dcx` |
| `sfx/` | 84 MiB | 37 | particle effects `frpg_sfxbnd_*.ffxbnd.dcx` |
| `shader/` | 16 MiB | 8 | `gxflvershader` (14,571 compiled `.vpo`/`.fpo` programs), decal, FFX |
| `event/` | 14 MiB | 5,337 | event scripts `*.emevd.dcx` + `*.emeld.dcx` per map |
| `msg/` | 7 MiB | 49 | text, 23 languages × `item` + `menu` msgbnds |
| `param/` | 2.6 MiB | 117 | `gameparam.parambnd.dcx` (all gameplay tables) + `drawparam/` (lighting, fog per area) |
| `script/` | 2 MiB | 38 | AI Lua per map (`luabnd`), NPC dialogue state machines (`talk/*.talkesdbnd`) |
| `action/` | 1.8 MiB | 182 | `action/script/cXXXX.hks`, per-character action scripts (Havok Script) |
| `paramdef/`, `mtd/`, `facegen/`, `other/` | small | | param layouts, material definitions, face generator, decals/rumble/system textures |

The `N:\SPRJ\data\INTERROOT_ps4\…` paths stored inside the archives are the studio's own source
tree. The talk scripts were built from `INTERROOT_win64`, because FromSoftware's tools ran on PC.

## Formats

Counts are files on disk. "In" means what a DCX holds once decompressed.

| Format | Magic | Count | What it is |
|---|---|---:|---|
| **DCX** | `DCX\0` | 26,913 | compressed wrapper. **Every one is `DFLT` (zlib)**; no Oodle, unlike Sekiro or Elden Ring |
| **BND4** | `BND4` | ~3,750 | archive of named, numbered files (`*bnd.dcx`) |
| **BXF4** | `BHF4` + `BDF4` | 108 pairs | split archive: header `.tpfbhd`/`.hkxbhd`, data `.tpfbdt`/`.hkxbdt` (map textures, collision); `.patch` pairs override |
| **FLVER** | `FLVER\0` v`0x20014` | 11,033 + in bnds | 3D models: meshes, materials, bones (DS3-era version) |
| **TPF** | `TPF\0`, platform byte 4 | 1,131 + in bnds | texture packs. PS4 platform, so GNF-tiled texture data, not DDS |
| **MSB** | `MSB ` | 2,709 | map layout: parts, enemies, regions, events. m29 has hundreds (Chalice Dungeon pieces) |
| **NVMA / NVM / ONAV** | `NVMA` | 2,701 | navmesh |
| **EMEVD / EMELD** | `EVD\0` / `ELD\0` | 2,668 each | event scripts (bytecode-like instruction lists) and their event names |
| **EMEDF** | `EDF` + NUL | 1 | the event instruction set itself: 25 classes, 287 instructions, Japanese names, typed arguments (`event/common.emedf.dcx`) |
| **PARAM** | | 65 in gameparam | gameplay tables (below) |
| **FMG** | | in msgbnds | UTF-16 string tables, named in Japanese (`武器名` weapon names, `地名` place names, …) |
| **ESD** | `fsSL` | 20 bnds + 2 | NPC dialogue/menu state machines (`enemycommon.esd`, `c0000.esd`) |
| **TAE** | `TAE ` | in anibnds | animation events (hitboxes, sounds, i-frames tied to animation frames) |
| **HKX** | `57 E0 E0 57` | in bnds / bxf | Havok 2014 packfiles: skeletons, animations, ragdolls, collision, cloth |
| **CLM2**, `.hkxpwv` | | in chrbnds | cloth and physics-weight data |
| AI Lua | `\x1bLuaP` | in 18 luabnds | **Lua 5.0** bytecode (`goal_list.lua`, `common_battle_func.lua`, `NNNNNN_battle.lua`/`_logic.lua`) |
| HKS | `\x1bLuaQ`, format `0x0E` | 179 | **Havok Script** bytecode (Lua 5.1 derivative) for character action logic |
| FSB5 / FEV1 / RIFF | | 504 / 254 | FMOD sample banks and FMOD Designer event projects |
| GFX | `GFX\x0b` | 128 | Scaleform Flash UI |
| GPARAM | | 86 | per-area graphics parameters (UTF-16 field names) |
| `entryfilelist` | `ENFL` | 268 | which files a map/character pulls in (loading lists) |
| `breakobj`, `btab`/`btl`/`btpb`, `bhdrinfo` | | | breakable objects, baked lighting (atlas, lights, probes) |

Formats with a reader in `tools/`: DCX, BND4 and FMG (`souls.py`), PARAM (`params.py`),
EMEVD/EMELD/EMEDF (`events.py`), AI Lua (`lua50.py`), ESD (`esd.py`) and MSB (`msb.py`); see
[Tools](#tools) below. HKS, FLVER, TPF, HKX, TAE and the sound formats have none here.

## Maps

Map IDs are `mAA_BB_CC_DD`. AA is the area, and the place-name table (`地名.fmg`) uses the same
numbering:

| ID | Area |
|---|---|
| m21 | Hunter's Dream (2110: Abandoned Old Workshop) |
| m22 | Hemwick Charnel Lane |
| m23 | Old Yharnam |
| m24 | Cathedral Ward (2400), Central Yharnam (2410), Upper Cathedral Ward (2420): `m24_00/01/02` |
| m25 | Forsaken Cainhurst Castle |
| m26 | Nightmare of Mensis |
| m27 | Forbidden Woods |
| m28 | Yahar'gul, Unseen Village |
| m29 | **Chalice Dungeons**: 68 `m29_AB_CD_00` map blocks and 2,685 layout files (`map/mapstudio/m29_*/`), mostly the 100 or 200 pre-built layouts of each Root chalice; see [the Chalice Dungeons](the-chalice-dungeons.md) |
| m32 | Byrgenwerth / Lecture Building / Moonside Lake |
| m33–m36 | *The Old Hunters*: Nightmare Frontier, Hunter's Nightmare, Research Hall, Fishing Hamlet |

There is no m30 or m31 in the data.

A normal map folder such as `map/m24_01_00_00/` holds about 960 `m24_01_00_00_NNNNNN.flver.dcx`
pieces. Beside them are the collision BXF pairs (`h…` hit, `l…` low), the navmesh
(`.nva.dcx`, `.nvmhktbnd.dcx`) and baked lighting (`btab`/`btl`/`btpb`/`bhdrinfo`). The
area-shared textures sit in `map/m24/*.tpfbhd/bdt`, and the layout itself is in
`map/mapstudio/m24_01_00_00.msb.dcx`.

## Params

`param/gameparam/gameparam.parambnd.dcx` holds 65 tables. These are the numbers the game plays by.
The largest:

| Rows | Param |
|---:|---|
| 16,101 | `GemeffectParam` (blood gem effects) |
| 9,720 | `ItemLotParam` (drops) |
| 7,854 | `SpEffectParam` (buffs/debuffs/status) |
| 7,173 + 1,695 | `BehaviorParam`, `BehaviorParam_PC` |
| 6,545 + 1,877 | `AtkParam_Npc`, `AtkParam_Pc` |
| 6,064 | `NpcParam` |
| 4,536 | `TalkParam` |
| 2,191 | `ObjectParam` |
| 2,136 | `Bullet` |
| 1,700 | `CharaInitParam` |
| 1,337 + 1,228 | `GemGenParam`, `WeaponGenParam` (random gem/weapon generation for dungeons) |
| 1,090 | `EquipParamWeapon` |

Bloodborne-specific systems show up as their own tables: `DungeonFeatureParam`,
`DungeonSubFeatLotParam`, `RitualRequiredMatParam`, `HolygrailExParam` (Chalice Dungeons),
`GemCategoryParam`, `GemDropDopingParam` (blood gems), and `QwcChange`/`QwcJudge` (the world
tendency system inherited from Demon's Souls; Bloodborne reuses a `qwcId` field in the shop table
for unlock flags, see [where the items are](the-items.md#unlock-flags)). Row layouts, with every
field's internal name and Japanese display name, are in the game's own
`paramdef/paramdef.paramdefbnd.dcx`, which is what `tools/params.py` decodes them with.

## Text

`msg/<lang>/item.msgbnd.dcx` and `menu.msgbnd.dcx` hold FMG tables for 23 language folders.
For example, `python -I tools/souls.py fmg work/base/dvdroot_ps4/msg/engus/item.msgbnd.dcx` gives
1,245 weapon names, 1,330 weapon descriptions, 87 place names, 87 NPC names, and so on. String IDs
match param row IDs: weapon `2000000` is *Chikage*, `2000100` is *Chikage +1*.

## Tools

`tools/souls.py` reads enough of these formats to survey them:

```
python -I tools/souls.py dcx   <file.dcx> <out>          # decompress
python -I tools/souls.py bnd   <file[.dcx]> [out-dir]    # list / unpack a BND4
python -I tools/souls.py fmg   <fmg | msgbnd> [id]       # dump text
python -I tools/souls.py param <parambnd>                # tables and row counts
```

`tools/params.py` decodes the gameplay tables field by field, using the game's own layouts in
`paramdef/paramdef.paramdefbnd.dcx`:

```
python -I tools/params.py check <parambnd> <paramdefbnd>                    # every table matches its layout?
python -I tools/params.py dump  <parambnd> <paramdefbnd> EquipParamWeapon 2000000
python -I tools/params.py diff  <old> <new> <paramdefbnd> <out.md> [--msg item.msgbnd.dcx]
```

For example, `dump` on row 2000000 of `EquipParamWeapon` gives the Chikage's fields
(`weight 5`, `sellValue 30000`, `correctStrength 15`, `correctAgility 45`, …) with each field's
Japanese display name. Several names are Dark Souls' (`correctAgility`, `correctFaith`) carried
over into Bloodborne. [What the 1.09 patch changed](the-patch.md#the-balance-changes-in-the-data-not-the-code)
uses `diff` to read the update's balance changes.

`tools/events.py` decodes the event scripts (EMEVD) with the instruction set the game describes in
`event/common.emedf.dcx` (25 classes, 287 instructions, Japanese names, typed arguments) and the
event names in each map's `.emeld.dcx`:

```
python -I tools/events.py check <emedf> <emevd ...>                    # every instruction decodes?
python -I tools/events.py dump  <emedf> <emevd> [--eld <emeld>] [--event ID]
python -I tools/events.py diff  <emedf> <old.emevd> <new.emevd> <out.md> [--eld <emeld>]
```

`tools/lua50.py` disassembles the AI scripts (Lua 5.0 bytecode) in `script/*.luabnd.dcx`, with
constants and globals resolved, and diffs two banks function by function:

```
python -I tools/lua50.py dis  <luabnd | .lua> [name-filter]
python -I tools/lua50.py diff <old.luabnd> <new.luabnd> <out.md>
```

`tools/esd.py` decodes the talk scripts (ESD state machines) in `script/talk/*.talkesdbnd.dcx`,
folds the arithmetic their compiler leaves in, and labels talk IDs with their English subtitle
line:

```
python -I tools/esd.py check <talkesdbnd ...>
python -I tools/esd.py dump  <talkesdbnd> [name-filter] [--talk menu.msgbnd.dcx]
python -I tools/esd.py diff  <old> <new> <out.md> [--talk menu.msgbnd.dcx]
```

`tools/msb.py` reads the map layouts (`map/mapstudio/*.msb.dcx`): models, parts (with each
enemy's NPC, think, talk and chara-init IDs), regions and map events (with each treasure's item
lots):

```
python -I tools/msb.py check <msb ...>
python -I tools/msb.py dump  <msb> [parts|enemies|regions|events|treasure|models]
python -I tools/msb.py diff  <old> <new> <out.md>
```

`tools/items.py` joins these readers to answer where every item comes from (treasure, enemy
drops, event awards, shops); see [where the items are](the-items.md).

For anything deeper (FLVER meshes, MSB editing, TAE, EMEVD decompilation), the mature community
tools already handle Bloodborne's formats. The one PS4-specific gap is textures: TPF platform 4
holds GNF-tiled data that has to be de-tiled before it becomes a viewable DDS.
