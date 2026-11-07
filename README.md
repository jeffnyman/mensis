# Mensis

Taking apart the PS4 *Bloodborne: Game of the Year Edition* (CUSA03173, EU) to see how it is built,
in the same spirit as *Confederation*, a sibling project, does for *Wing Commander*.

## Short answer

**Yes, it can be opened all the way down.**

* The PKG is a *fake* (homebrew re-signed) package, so its encryption keys are public. The
  extractor in `tools/` decrypts it and produces the game's real filesystem: 28,808 files,
  29.3 GiB.
* The executable is a *fake* SELF with plaintext segments. It converts back to an ordinary
  x86-64 ELF that Ghidra or IDA can load, and all 701 of its system imports resolve to names.
  Decompiling it is possible but large: about 160k functions in an 81 MiB link-time-optimised
  build with no symbols.
* The data is FromSoftware's well-documented Souls formats, stored as loose zlib-compressed files.
  Gameplay numbers, text, event scripts, AI (Lua 5.0) and action scripts (Havok Script) are all
  readable without touching the executable.

## Notes

* [The package](docs/the-package.md): PKG → encrypted PFS → PFSC → inner PFS, the key chain,
  PlayGo, and what the 1.09 update replaces.
* [The executable](docs/the-executable.md): SELF → ELF, imports, the engine and middleware it was
  built from, and what decompiling it involves.
* [Loading it into Ghidra](docs/ghidra.md): the step-by-step procedure from `eboot.bin` to an
  analysed Ghidra project, why each step is needed, what the result looks like, how the 1.00
  and 1.09 builds were compared, and how functions get named: by rule from the strings around
  them, and by hand for the most-called helpers.
* [What the 1.09 patch changed](docs/the-patch.md): the last update read function by function
  and table by table: developer tooling switched off, an SOS-sign limit read live, the balance
  changes (Boom Hammer, Whirligig Saw, Valtr, the DLC enemies, Blood Rock in the shop), and the
  event-script fixes (elevators, missing DLC, NPCs, bosses), the AI changes (NPC hunters on
  ladders, Lady Maria), the talk-script changes, and the map-layout changes that tie them
  together.
* [Where the items are](docs/the-items.md): every treasure spot, enemy drop and event-given item,
  traced from the map layouts through the item tables to their names.
* [The data](docs/the-data.md): the `dvdroot_ps4` tree, every file format in it, map IDs,
  params and text.

## Setup

The tools are Python scripts; their dependencies (`cryptography`, `pyelftools`, `capstone`) are in
`pyproject.toml`:

```
uv venv .venv && uv pip install --python .venv -r pyproject.toml
```

or the equivalent with `python -m venv .venv` and `pip install cryptography pyelftools capstone`.

For the Ghidra work you also need Ghidra 12.2 and a JDK 25, located through two environment
variables (see [Loading it into Ghidra](docs/ghidra.md#what-you-need)):

| Variable | Points at |
|---|---|
| `GHIDRA_INSTALL_DIR` | the Ghidra install folder |
| `JAVA_HOME` | the JDK |
| `MENSIS_PYTHON` | *(optional)* a Python other than `.venv`'s |
| `GHIDRA_HEADLESS_MAXMEM` | *(optional)* heap for headless analysis, default 20G |

## Tools

```
python -I tools/pkgtool.py info|list|extract <pkg> …               # PKG/PFS extractor
python -I tools/unfself.py <eboot.bin> <out.elf>                     # fake SELF → ELF
python -I tools/souls.py dcx|bnd|fmg|param <file> …                  # DCX/BND4/FMG/PARAM
python -I tools/params.py check|dump|diff <parambnd> <paramdefbnd> …  # gameplay tables, decoded
python -I tools/events.py check|dump|diff <emedf> <emevd> …           # event scripts, decoded
python -I tools/lua50.py dis|diff <luabnd> …                            # AI scripts (Lua 5.0), disassembled
python -I tools/esd.py check|dump|diff <talkesdbnd> …                   # talk scripts (ESD), decoded
python -I tools/msb.py check|dump|diff <msb> …                          # map layouts (MSB)
python -I tools/items.py world|find <base dvdroot> <update dvdroot> …   # where every item is
python -I tools/orbis_prep.py <elf> <nid-db> <out.elf> <out.tsv>     # ELF → Ghidra-ready
pwsh tools/ghidra/analyze.ps1 -Self <eboot.bin> -Name <name>         # whole Ghidra pipeline
python -I tools/build_diff.py <old.tsv> <new.tsv> <out.md>             # compare two builds
python -I tools/port_names.py <names.tsv> <from.tsv> <to.tsv> <out.tsv>  # carry names across builds
python -I tools/c_diff.py <pairs.tsv> <old.tsv> <new.tsv> <old-c> <new-c> <out.md>  # diff decompiled C
```

(`python` meaning the one in `.venv`, e.g. `.venv/Scripts/python` on Windows.)

`tools/ghidra/` also holds the Ghidra scripts: `ApplyOrbisSymbols.java` (names and function
starts before analysis), `OrbisReport.java` (summary and landmark decompilations),
`ExportFunctionHashes.java` (per-function hashes for `build_diff.py`), `FindStringUses.java`
(who uses which strings), `NameFromStrings.java` (names functions and globals from step tables,
profiler markers and singleton asserts), `MostCalled.java` (the most-called unnamed functions,
decompiled), `Decompile.java` (decompile any addresses, optionally with callers),
`ApplyNames.java` (apply a hand-written name list) and `DeleteProjectFile.java`.

`tools/names/` holds the hand-assigned names for each build, one per line with a confidence
level and the evidence for it.
`tools/pkgkeys.py` holds the public fake-package RSA keys.

## Working files (`work/`, ~30 GiB, not source)

| Path | |
|---|---|
| `work/base/` | full extracted 1.00 base game (`eboot.bin`, `dvdroot_ps4/`, `sce_sys/`, …) |
| `work/update/` | extracted 1.09 patch: lay over `base/` for the current game |
| `work/elf/` | `eboot-1.00.elf`, `eboot-1.09.elf`, the bundled `.prx` modules as ELFs, resolved import list |
| `work/ghidra/` | Ghidra-ready ELFs + symbols, the Ghidra project with both builds (`project/Bloodborne.gpr`), logs, `report-eboot-1.0x/`, `diff/` (function hashes and the 1.00 → 1.09 diff) |
| `work/ref/aerolib.inl` | NID → name database (shadPS4) |
| `work/base-files.txt`, `work/update-files.txt` | full file listings with sizes |

## Not looked at yet

`Bloodborne [FitGirl Repack]` is a compressed installer (`setup.exe` + 15 GiB of `fg-*.bin`). Its
contents can't be seen without running it. If it is what the name suggests, it holds the shadPS4
emulator plus an already-extracted `CUSA03173` folder, the same tree as `work/base` with
`work/update` laid over it, probably with community patches. Nothing here depends on it, since the
PKG gives the same files directly. shadPS4 is the obvious way to *run* what's in `work/` if that
becomes useful.

## License

**Mensis**, Notes and Tools for Taking Apart *Bloodborne*Copyright (C) 2026 [Jeff Nyman][owner]

[![GNU General Public License version 3](https://raw.githubusercontent.com/jeffnyman/mensis/main/assets/gplv3-127x51.png)][GNU General Public License]

> This program is free software: you can redistribute it and/or modify
> it under the terms of the [GNU General Public License] as published by
> the Free Software Foundation, either version 3 of the License, or
> (at your option) any later version.
>
> This program is distributed in the hope that it will be useful,
> but WITHOUT ANY WARRANTY; without even the implied warranty of
> MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
> [GNU General Public License] for more details.

[owner]: https://github.com/jeffnyman
[GNU General Public License]: https://www.gnu.org/licenses/gpl-3.0.en.html

The PKG decryption in `tools/pkgtool.py` and the keys in `tools/pkgkeys.py` are ported from
[shadPS4](https://github.com/shadps4-emu/shadPS4) v0.7.0, licensed GPL-2.0-or-later, which is
used here under version 3. The NID name database the Ghidra pipeline downloads also comes from
shadPS4 and is not part of this repository.

`tools/esd.py` and `tools/msb.py` follow the ESD and MSB layouts documented by
[SoulsFormats](https://github.com/JKAnderson/SoulsFormats), and `esd.py` the expression bytecode
documented by [ESDLang](https://github.com/thefifthmatt/ESDLang), both MIT-licensed.
