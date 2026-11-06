# The executable

`eboot.bin` is the game. Everything under `dvdroot_ps4/` is data it loads.

## SELF to ELF

PS4 executables are **SELF** files (magic `4F 15 3D 1D`): a header and segment table, then the
original ELF header and program headers, then the segment payloads. Retail SELFs have encrypted
payloads. A fake package carries **fake SELFs** whose payloads are plaintext (segment flags `0x800`
"blocked" and `0x4` signed, but not `0x2` encrypted or `0x8` compressed). Rebuilding the ELF only
means putting each payload back at its program header's `p_offset`:

```
python -I tools/unfself.py work/update/eboot.bin work/elf/eboot-1.09.elf
```

`work/elf/` holds the 1.00 and 1.09 executables plus the seven bundled `sce_module/*.prx`
(libc, Fios2, and the camera and motion libraries Face, Hand, HeadTracker, Smart,
S3DConversion).

## Shape

| | |
|---|---|
| Type | `0xFE10` ET_SCE_DYNEXEC, a position-independent executable based at 0 |
| Machine | x86-64 (AMD Jaguar), FreeBSD OS ABI |
| Code (`R-X` PT_LOAD) | 0x50D96DC bytes ≈ **81 MiB** |
| Data (`RW-` PT_LOAD) | 3.0 MiB file / 6.0 MiB in memory |
| `.eh_frame_hdr` entries | **162,959**, about one per function, so roughly 160k functions |
| Build path (PT_SCE_COMMENT) | `E:/AB/SPRJ/Source/SPRJ/dist_ps4/Master_LTO/SPRJ_ps4_Master_LTO.elf` |
| Symbols | none: a link-time-optimised ("Master_LTO") release build |
| C++ RTTI | only FMOD's 201 classes; the engine was built with RTTI off |

**SPRJ** is FromSoftware's internal project code for Bloodborne. It appears in the build path, in
every packed asset path (`N:\SPRJ\data\INTERROOT_ps4\…`) and in the asset names (`sprj_c1000.fsb`,
`sprj_opening.mp4`).

Besides the two PT_LOADs there are the usual PS4 extras: PT_SCE_DYNLIBDATA (`0x61000000`,
5.4 MiB, holding the symbol, string and relocation tables), PT_SCE_PROCPARAM (`0x61000001`),
PT_SCE_COMMENT (`0x6FFFFF00`), PT_SCE_LIBVERSION (`0x6FFFFF01`), PT_TLS and PT_GNU_EH_FRAME.

## Imports

A PS4 binary links against system modules by **NID**, not by name. A NID is the first 8 bytes of
`SHA1(name ‖ 518D64A635DED8C1E6B039B1C3E55230)`, byte-reversed and written in a URL-safe base64
(`+` and `-`), with the library and module appended as `NID#L#M`. Hashing candidate names against
shadPS4's database resolves **all 701** imports. The list is in `work/elf/eboot-imports-resolved.txt`.

It needs 42 modules. By import count:

| Library | Imports | What it says about the game |
|---|---:|---|
| libc / libkernel / libScePosix | 216 / 80 / 32 | pthreads, direct/flexible memory, semaphores, event queues |
| libSceGnmDriver | 50 | renders through **Gnm** directly: LS/HS/ES/GS/VS/PS/CS stages, compute queues, Razor capture hooks |
| libSceNet, Http, Ssl, NpMatching2, NpSignaling, NpWebApi, NpScore, … | ~140 | online play: co-op/invasions (Matching2 rooms, P2P signaling), leaderboards for Chalice Dungeons |
| libSceFios2 | 17 | file I/O through FIOS2 (its own copy ships in `sce_module/`) |
| libSceAvPlayer | 16 | plays the four `movie/*.mp4` |
| libSceAjm | 12 | hardware audio decoding (for FMOD's compressed banks) |
| libSceVoice, AudioIn/Out | 11 + 9 | voice chat in multiplayer, audio output |
| libSceSaveData (+Dialog) | 15 | saves |
| libScePlayGo | 5 | streaming install (see [the package](the-package.md)) |
| libSceNpTrophy | 6 | trophies |
| libScePad, Mouse, ImeDialog, MsgDialog, WebBrowserDialog, … | | input and system UI |

## What it was built from

Assert strings and `__FILE__` paths left in the binary give a clear picture of the layers:

| Layer | Evidence |
|---|---|
| **dantelion2** (`DL*` classes): FromSoftware's core library for heaps, containers, reflection, threads | `…/Include/dantelion2/Core/Kernel/DLBiHeapStrategy.inl`, `N:/FD4/Library/dantelion2_sprj/` |
| **FD4**: FromSoftware's game engine, the generation shared by Dark Souls II/III | `N:/FD4/GameEngine/Source/…`, `FD4*` and `*::FD4STEPTEMPLATEBASE_CHILD_CLASS` step/task types |
| **GS / GraphicsFramework**: FromSoftware's renderer (`GX*` shaders) | `N:/GS/Source/GraphicsFramework/`, `shader/gx*.shaderbnd.dcx` |
| **Game code** (`CS*`, `Sprj*`, `Frpg*`) | 263 `Sprj*`, 174 `Frpg*`, 45 `CS*` class-name strings. `Frpg` ("FromSoftware RPG") is the Dark Souls lineage |
| **Havok 2014.1.0**: Physics2012 + hknp, Animation, Behavior, AI pathfinding, Cloth, **Havok Script** | `N:/HavokCompleteClient3/SDK_LocalRelease/SP/Havok_2014_1_0/hk2014_1_0_r1/…` |
| **FMOD Ex 4.44.50** with FMOD Designer events | `N:/FMOD/Source/fmodexsrc44450/…`, `.fev`/`.fsb` data |
| **Scaleform GFx** (Flash UI) | `SprjScaleform`, `GFX 8,0,0,0`, 128 `menu/*.gfx` |
| **YEBIS** (Silicon Studio post-processing: bloom, DoF, tone mapping) | `G:/workspace/yebis_git/yebis_draft/src/ppfx.cpp` asserts |
| zlib 1.2.7 (and 1.2.3), libjpeg 9, libpng | copyright strings |

## Decompiling it

This has now been done for 1.09; [Loading it into Ghidra](ghidra.md) gives the procedure and results.

It is feasible. The obstacles are size and missing names, not protection:

* **Loading.** The rebuilt ELF loads in Ghidra or IDA as x86-64. A PS4 loader (for Ghidra, e.g.
  the *GhidraOrbis* family) parses `PT_SCE_DYNLIBDATA` and applies the NID names so imports show
  as `sceGnmSubmitCommandBuffers` and not `ZG+ghi…`. `tools/` already has the NID hash, so a small
  Ghidra script could do the same.
* **Size.** About 160k functions in 81 MiB of LTO-inlined code. Expect hours for the first
  auto-analysis, and treat it as a project, not a weekend.
* **Names.** There are no symbols and almost no RTTI. The anchors are the thousands of class-name
  strings (`CSDungeonRitualHelper::THIS_CLASS`, …), assert strings with file and line, Havok's
  class-reflection tables, the system imports, and the community knowledge of the same engine in
  Dark Souls III and Sekiro, whose PC builds are much better mapped.
* **Two builds.** Both 1.00 and 1.09 are in `work/elf/`, so diffing them shows what the balance
  patch touched in code, not only in params.

Much of the *game logic* is not in the executable at all. It is data the engine interprets: the
params, the EMEVD event scripts, the Lua AI, the ESD state machines and the HKS action scripts
(see [the data](the-data.md)). For understanding how Bloodborne plays, those are the cheaper place
to start.
