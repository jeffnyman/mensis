# Rebuilding `work/`

Everything the documentation reads from lives in `work/`, which isn't in the repository because
it is the game's own data or derived from it. This page rebuilds it from scratch, in order. Each
step names the page that explains it. Every command is run from the repository root, and
`python` means the one in `.venv` (`.venv/Scripts/python` on Windows).

## What you need

**The game, as PS4 package files.** Mensis starts from two PKG files of *Bloodborne: Game of the
Year Edition* (CUSA03173, the European release):

| Package | Content ID | Size |
|---|---|---:|
| the game, 1.00 | `EP9000-CUSA03173_00-BLOODBORNE0000EU` | about 29 GiB |
| the 1.09 update | the same content ID, as a patch package | about 173 MiB |

Use your own copy of the game. One thing matters: `pkgtool.py` opens **fake** packages, the
re-signed kind used for backups and homebrew, whose keys are public
([the keys, and why this works at all](the-package.md#the-keys-and-why-this-works-at-all)). A retail package straight from the PlayStation
Store, or the contents of a disc, is encrypted with keys only a console holds, and the tool can't
open it. Making a backup package of a game you own happens on a console and is outside this
project.

**Software.**

| | For | |
|---|---|---|
| Python 3.10 or later | every tool | with the project's dependencies: `uv venv .venv && uv pip install --python .venv -r pyproject.toml` |
| Ghidra 12.2 and a JDK 25 | steps 3–4 only | located through `GHIDRA_INSTALL_DIR` and `JAVA_HOME` ([what you need](ghidra.md#what-you-need)) |
| PowerShell 7 (`pwsh`) | step 3 | runs `tools/ghidra/analyze.ps1` |
| a POSIX shell with `awk` | the loops in steps 2, 4 and 5 | Git Bash on Windows |
| about 50 GB of disk | | about 34 GB for the extracted game and analysis, the rest for pictures |

The Ghidra steps need Windows (`analyze.ps1` runs Ghidra's `analyzeHeadless.bat` and the JDK's
`java.exe`) and plenty of memory: the analyzer uses up to 20 GB of heap. Steps 5–7 don't need
Ghidra at all.

## 1. Extract the packages ([the package](the-package.md))

```
python -I tools/pkgtool.py extract <game.pkg>   work/base
python -I tools/pkgtool.py extract <update.pkg> work/update
python -I tools/pkgtool.py list    <game.pkg>   > work/base-files.txt
python -I tools/pkgtool.py list    <update.pkg> > work/update-files.txt
```

`work/base` gets the game's filesystem: `dvdroot_ps4/` (all the data), `eboot.bin` (the
executable), `sce_module/` and `sce_sys/`. `work/update` gets the files 1.09 replaces, in the same
layout. The game is 28,808 files. Extraction runs at about 250 MiB/s, and an interrupted run can
be restarted, since files already there at the right size are skipped. Throughout, the game "as
patched" means `work/update` laid over `work/base`; the tools take both and prefer the update's
copy of a file.

## 2. Turn the executables back into ELFs ([the executable](the-executable.md))

`analyze.ps1` (next step) does this for the two `eboot.bin`s itself. The bundled modules are
converted by hand:

```
mkdir -p work/elf
for m in work/base/sce_module/*.prx; do
    python -I tools/unfself.py "$m" "work/elf/$(basename "$m" .prx).elf"
done
```

## 3. Load both executables into Ghidra ([loading it into Ghidra](ghidra.md))

```
pwsh tools/ghidra/analyze.ps1 -Self work/update/eboot.bin -Name eboot-1.09
pwsh tools/ghidra/analyze.ps1 -Self work/base/eboot.bin   -Name eboot-1.00
```

Each run:
1. downloads the NID name database to `work/ref/aerolib.inl`, on first use;
2. converts the SELF to an ELF (`work/elf/`);
3. prepares it for Ghidra and writes its symbols (`work/ghidra/<name>.ghidra.elf` and
   `.symbols.tsv`, whose `IMPORT` rows are the 701 resolved system imports);
4. imports and analyzes the program into the project `work/ghidra/project/Bloodborne.gpr`;
5. writes a report.

About 24 minutes each.

Then the names: rule-made names from the strings, and the hand-assigned lists. For each build
(`eboot-1.09` shown; repeat with `eboot-1.00` and its own list):

```
mkdir -p work/ghidra/naming
analyzeHeadless work/ghidra/project Bloodborne -process eboot-1.09.ghidra.elf -noanalysis \
    -scriptPath tools/ghidra -postScript NameFromStrings.java work/ghidra/naming/eboot-1.09.tsv
analyzeHeadless work/ghidra/project Bloodborne -process eboot-1.09.ghidra.elf -noanalysis \
    -scriptPath tools/ghidra -postScript ApplyNames.java tools/names/eboot-1.09.hand.tsv
```

(`analyzeHeadless` here is `$GHIDRA_INSTALL_DIR/support/analyzeHeadless.bat`.)

## 4. Compare the two builds' code ([the patch](the-patch.md#the-code))

```
analyzeHeadless work/ghidra/project Bloodborne -process "eboot-1.*.ghidra.elf" -noanalysis -readOnly \
    -scriptPath tools/ghidra -postScript ExportFunctionHashes.java work/ghidra/diff
python -I tools/build_diff.py work/ghidra/diff/eboot-1.00.functions.tsv work/ghidra/diff/eboot-1.09.functions.tsv \
    work/ghidra/diff/diff-1.00-to-1.09.md --pairs work/ghidra/diff/pairs-1.00-to-1.09.tsv
```

The functions to decompile are the pairs whose code differs. These two commands list their
addresses in each build:

```
awk -F'\t' 'NR>1 && $1~/^(constants|changed|replaced|moved\+constants|removed)$/ && $2!="" {print $2}' \
    work/ghidra/diff/pairs-1.00-to-1.09.tsv > work/ghidra/diff/old.list
awk -F'\t' 'NR>1 && $1~/^(constants|changed|replaced|moved\+constants|added)$/ && $3!="" {print $3}' \
    work/ghidra/diff/pairs-1.00-to-1.09.tsv > work/ghidra/diff/new.list
```

Then decompile both sides and diff them:

```
analyzeHeadless work/ghidra/project Bloodborne -process eboot-1.00.ghidra.elf -noanalysis -readOnly \
    -scriptPath tools/ghidra -postScript Decompile.java work/ghidra/diff/c-1.00/ @work/ghidra/diff/old.list
analyzeHeadless work/ghidra/project Bloodborne -process eboot-1.09.ghidra.elf -noanalysis -readOnly \
    -scriptPath tools/ghidra -postScript Decompile.java work/ghidra/diff/c-1.09/ @work/ghidra/diff/new.list
python -I tools/c_diff.py work/ghidra/diff/pairs-1.00-to-1.09.tsv work/ghidra/diff/eboot-1.00.functions.tsv \
    work/ghidra/diff/eboot-1.09.functions.tsv work/ghidra/diff/c-1.00 work/ghidra/diff/c-1.09 \
    work/ghidra/diff/c-diff-1.00-to-1.09.md
```

## 5. Compare the two builds' data ([the patch](the-patch.md))

One diff per kind of file the update replaces: one parameter table, 10 event scripts, 7 AI
scripts, 3 talk scripts and 2 map layouts.

```
mkdir -p work/params work/events work/ai work/talk work/msb
B=work/base/dvdroot_ps4; U=work/update/dvdroot_ps4
python -I tools/params.py diff $B/param/gameparam/gameparam.parambnd.dcx $U/param/gameparam/gameparam.parambnd.dcx \
    $B/paramdef/paramdef.paramdefbnd.dcx work/params/gameparam-1.00-to-1.09.md --msg $U/msg/engus/item.msgbnd.dcx
for f in $U/event/*.emevd.dcx; do m=$(basename $f .emevd.dcx)
    python -I tools/events.py diff $B/event/common.emedf.dcx $B/event/$m.emevd.dcx $f work/events/$m.md --eld $U/event/$m.emeld.dcx; done
for f in $U/script/*.luabnd.dcx; do m=$(basename $f .luabnd.dcx)
    python -I tools/lua50.py diff $B/script/$m.luabnd.dcx $f work/ai/$m.md; done
for f in $U/script/talk/*.talkesdbnd.dcx; do m=$(basename $f .talkesdbnd.dcx)
    python -I tools/esd.py diff $B/script/talk/$m.talkesdbnd.dcx $f work/talk/$m.md --talk $U/msg/engus/menu.msgbnd.dcx; done
for f in $U/map/mapstudio/*.msb.dcx; do m=$(basename $f .msb.dcx)
    python -I tools/msb.py diff $B/map/mapstudio/$m.msb.dcx $f work/msb/$m.md; done
```

## 6. The joined results ([where the items are](the-items.md), [the Chalice Dungeons](the-chalice-dungeons.md), [event flags](the-flags.md), [the questlines](the-questlines.md), [combat](the-combat.md))

```
mkdir -p work/items work/chalice work/story
python -I tools/items.py   world work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/items/items-1.09.md
python -I tools/chalice.py       work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/chalice/chalice-1.09.md
python -I tools/chalice_events.py work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/chalice/events-1.09.md
python -I tools/flags.py   index work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/story/flags-1.09.tsv
python -I tools/story.py   sheets work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/story/npcs
python -I tools/combat.py  weapons work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/combat
python -I tools/effects.py tables  work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/combat
python -I tools/enemies.py tables work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/combat
python -I tools/status.py tables  work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/combat
python -I tools/ai.py tables      work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/combat
python -I tools/leftovers.py      work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/leftovers.md
```

About 25 seconds each for the first three, which read every Chalice layout. The flag index, the
questline sheets, the combat tables, the effect tables, the enemy sheets, the status tables, the AI sheets and the leftovers list
take a few seconds each.

## 7. The artwork ([textures](the-textures.md), [models](the-models.md))

```
python -I tools/textures.py items  work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/pictures/items
python -I tools/textures.py models work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/pictures/models chr,parts
python -I tools/flver.py    all    work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/pictures/renders
python -I tools/flver.py    parts  work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/pictures/equipment --size 768
python -I tools/flver.py    hunters work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/pictures/hunters --size 768
```

| Step | Takes | Writes |
|---|---|---:|
| item pictures | 13 seconds | 1,056 PNG |
| character and equipment textures | 7 minutes | 5.4 GB |
| character renders | 10 minutes | 217 PNG, 74 MB |
| equipment renders | about as long | 568 PNG, 109 MB |

Map and object textures are optional (`models … map,obj`); all 56 map packs come to roughly 7–8
GB. Like everything in `work/`, the pictures stay out of the repository.

## 8. Watch the start-up ([the loader](the-loader.md))

```
python -I tools/loader.py work/ghidra/eboot-1.09.ghidra.elf work/ghidra/eboot-1.09.symbols.tsv work/loader
```

About 5 seconds. Writes `work/loader/calls.log`, every system call the start-up makes, and
`work/loader/summary.md`. Needs step 3's prepared executable.

## Checking the result

**The regression check** reruns the tools and compares every output with the result this
repository was built from:

```
python -I tools/check.py run
```

```
SAME  params-diff
SAME  event-diffs
…
SAME  renders
```

The outputs themselves are the game's, so the repository holds only their SHA-256 hashes
(`tools/check-hashes.tsv`). A hash proves a match without containing anything. Each check
covers one tool, or one tool's output for every file of a kind:
- the five patch diffs;
- the readers' `check` commands;
- the item index, the two Chalice outputs, the flag index, the questline sheets, the combat tables, the effect tables, the enemy sheets, the status tables, the AI sheets and the leftovers list;
- `unfself` and `orbis_prep` on the 1.09 executable;
- the build and decompiled-code diffs;
- the item pictures;
- four renders, one of them a whole hunter.
- every link in `README.md` and `docs/`, which needs no game data at all (`tools/doclinks.py`;
  run it on its own to see which link broke).

The checks take about four minutes. A check whose inputs you haven't built is reported as
`SKIP`; for example, the code diffs need step 4, and so Ghidra. Text is compared after
normalizing line endings and path separators, and images on their pixels rather than their PNG
bytes, so the result is the same on any machine. Renders come from floating-point arithmetic, so
a different numpy build could in principle move a pixel; if only `renders` differs after a
library upgrade, look at the pictures first.

`check.py` is also how the tools are kept honest. After changing a tool, `run` shows whether its
output moved. After an intended change, `record` updates the hashes, and the commit shows which
outputs changed.

Several pages also give numbers to compare against:
- 28,808 files in the game;
- 167,054 functions in each analyzed build, and 701 resolved imports;
- 1,056 item pictures, 217 character renders and 568 equipment renders;
- 2,685 Chalice layouts;
- 29,290 references to event flags, 30 questline sheets and 95 weapons.

The data readers also have `check` commands that decode every file of their kind and report any
that don't:

```
python -I tools/params.py check work/update/dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx \
    work/base/dvdroot_ps4/paramdef/paramdef.paramdefbnd.dcx
python -I tools/events.py check work/base/dvdroot_ps4/event/common.emedf.dcx work/base/dvdroot_ps4/event/*.emevd.dcx
```
