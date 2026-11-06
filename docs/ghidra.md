# Loading the executable into Ghidra

How `eboot.bin` goes from a file inside a PS4 package to a program you can browse and decompile
in Ghidra. Each step explains what is done and why.

[The executable](the-executable.md) covers what the binary *is*. This page covers getting it into
a disassembler.

## What you need

| | Version used | Found through |
|---|---|---|
| Ghidra | 12.2 (`ghidra_12.2_PUBLIC_20261016`, from the NSA's GitHub releases) | `GHIDRA_INSTALL_DIR` |
| JDK | Temurin **25** (Ghidra 12.2 refuses 21: `application.java.min=25`) | `JAVA_HOME` |
| Python | 3.12+ with the packages in `pyproject.toml` | `MENSIS_PYTHON`, else `.venv`, else `python` |
| NID database | shadPS4 v0.7.0 `src/core/aerolib/aerolib.inl` (59,280 names) | downloaded to `work/ref/aerolib.inl` on first run |

Ghidra and the JDK are plain zips. Unpack both anywhere and set the two variables, for example in
PowerShell:

```
$env:GHIDRA_INSTALL_DIR = "C:\Tools\ghidra_12.2_PUBLIC"
$env:JAVA_HOME          = "C:\Tools\jdk-25.0.5+8"
```

Then run `%GHIDRA_INSTALL_DIR%\ghidraRun.bat` for the GUI, or `support\analyzeHeadless.bat` for
scripted use. The headless analyser gets 2 GB of heap by default, which is not enough for an
81 MiB program. `analyze.ps1` sets `GHIDRA_HEADLESS_MAXMEM` to 20G unless you set it yourself; for
the GUI, raise `MAXMEM` in `support\launch.properties`.

## The short version

```
pwsh tools/ghidra/analyze.ps1 -Self work/update/eboot.bin -Name eboot-1.09
```

This runs the four steps below and leaves an analysed program `eboot-1.09.ghidra.elf` in the
Ghidra project `work/ghidra/project/Bloodborne.gpr`, plus a report and sample decompilations in
`work/ghidra/report-eboot-1.09/`. Open the project in the Ghidra GUI to browse it.

## Step 1: fake SELF → ELF (`tools/unfself.py`)

`eboot.bin` is a SELF: Sony's signed wrapper around an ELF. In this package the segments are
stored in plain text (a *fake* SELF), so unwrapping means copying each segment payload back to the
file offset its ELF program header names. The result is `work/elf/eboot-1.09.elf`, byte for byte
the ELF the PS4 linker produced. See [the executable](the-executable.md#self-to-elf).

## Step 2: make the ELF digestible (`tools/orbis_prep.py`)

Ghidra loads the plain ELF, but it comes out nearly useless. Four PS4 peculiarities get in the
way:

| Problem | Effect in Ghidra if left alone | What the prep does |
|---|---|---|
| `e_type` is `0xFE10` (ET_SCE_DYNEXEC) | unknown type, treated cautiously | set to `ET_EXEC` (2) |
| Linked at address **0** | every small constant (0x10, 0x200, …) looks like a pointer into code; references everywhere | **rebase** every loadable segment to `0x400000`, the address the PS4 kernel itself used for non-ASLR executables |
| Dynamic-link tables are Sony's (`DT_SCE_RELA`, `DT_SCE_JMPREL`, `DT_SCE_SYMTAB`, …) and live in `PT_SCE_DYNLIBDATA`, a segment that is never mapped | Ghidra applies **no relocations**: vtables, string tables and function-pointer tables hold link-time offsets, not addresses | apply all 234,339 relocations in the file itself (231,478 `R_X86_64_RELATIVE`, 2,838 `R_X86_64_64`, 661 `JUMP_SLOT`, 23 `GLOB_DAT`) |
| Imports are named by **NID**, e.g. `Mk11X2E0Bs8#S#T` | calls go through unnamed GOT slots to nowhere | resolve each NID to its name, add a small segment of one-byte `RET` stubs (one per import), point every GOT slot at its stub, and write the names to a symbols file |

It also turns off the program headers that only mean something to the PS4 loader (`PT_DYNAMIC`,
`PT_INTERP`, `PT_SCE_*`) by setting them to `PT_NULL`, and reuses one of those slots to describe
the stub segment.

Finally it reads the **`.eh_frame_hdr` binary-search table**. The C++ exception unwinder needs to
find any function from an address, so the compiler leaves a sorted list of every function's start
address (except a few with no unwind info). That table is a free, exact function list,
**162,959 entries**, and it saves Ghidra from guessing where functions begin in 81 MiB of code.

```
python -I tools/orbis_prep.py work/elf/eboot-1.09.elf work/ref/aerolib.inl \
       work/ghidra/eboot-1.09.ghidra.elf work/ghidra/eboot-1.09.symbols.tsv
```

```
base 0x400000, entry 0x4000a0
modules 42, libraries 43, symbols 703, NIDs unresolved 0
import stubs 701 at 0x5ae0000, GOT slots named 684, PLT trampolines 661, function starts from .eh_frame_hdr 162959
```

The symbols file is a plain TSV: `address  kind  name  library`.

| Kind | Count | Meaning |
|---|---:|---|
| `IMPORT` | 681 | the `RET` stub that stands in for a system function |
| `IMPORT_DATA` | 20 | stand-in for a system variable (e.g. `__stack_chk_guard`) |
| `PLT` | 661 | the game's own `jmp qword ptr [rip+GOT]` trampoline, found by scanning the code for `FF 25` whose target is a known GOT slot |
| `GOT` | 684 | the GOT slot itself |
| `ENTRY`, `PROCPARAM` | 1 + 1 | entry point (`0x4000a0`) and the SCE process-parameter block |
| `FUNC` | 162,958 | unnamed function starts from `.eh_frame_hdr` |

### How a NID becomes a name

A NID is `base64(reverse(SHA1(name ‖ 518D64A635DED8C1E6B039B1C3E55230)[0:8]))` using the
alphabet `A–Z a–z 0–9 + -`, which gives 11 characters. It can't be inverted, but candidate names
can be hashed and compared. `malloc` hashes to `gQX+4GDQjpM`, exactly what the binary contains.
The emulator projects keep databases of tens of thousands of known names. The `#S#T` after the
NID are the library and module IDs, in the same base64, indexing the `DT_SCE_IMPORT_LIB` and
`DT_SCE_NEEDED_MODULE` tables.

## Step 3: import and analyse (`analyzeHeadless` + `ApplyOrbisSymbols.java`)

```
set GHIDRA_HEADLESS_MAXMEM=20G
%GHIDRA_INSTALL_DIR%\support\analyzeHeadless work\ghidra\project Bloodborne ^
    -import work\ghidra\eboot-1.09.ghidra.elf ^
    -scriptPath tools\ghidra ^
    -preScript ApplyOrbisSymbols.java work\ghidra\eboot-1.09.symbols.tsv ^
    -max-cpu 12
```

* Ghidra picks its standard **ELF loader** with x86:LE:64 and the gcc compiler spec. The PS4
  compiler is clang targeting the System V AMD64 ABI, which the gcc spec describes correctly.
* `-preScript` runs `tools/ghidra/ApplyOrbisSymbols.java` **after import and before
  auto-analysis**, so analysis starts with names and function boundaries in place:
  * imports become functions in a namespace per library (`libkernel::sceKernelOpen`);
  * each PLT trampoline becomes a **thunk** of its import, so a call shows in the decompiler
    as `sceKernelOpen(...)` and not `FUN_02fbe4b8(...)`;
  * GOT slots are labelled and typed as pointers;
  * all 162,958 `.eh_frame_hdr` starts are disassembled and made functions.
* Auto-analysis then runs with Ghidra's defaults. It follows flow, finds references and strings,
  recovers stacks, propagates constants and demangles the C++ names that some imports carry
  (libc++ in libc).

The same script works from the GUI: import the `.ghidra.elf`, answer **No** to "analyze now?",
run `ApplyOrbisSymbols.java` from the Script Manager (category *PS4*), pick the TSV, then
*Analysis → Auto Analyze*.

## Step 4: report (`OrbisReport.java`)

```
analyzeHeadless work\ghidra\project Bloodborne -process eboot-1.09.ghidra.elf -noanalysis -readOnly ^
    -scriptPath tools\ghidra -postScript OrbisReport.java work\ghidra\report-eboot-1.09
```

It writes `report.txt` (function counts, most-called system imports) and decompiles a set of
landmark functions to `.c` files: the entry point, the callers of a few telling system calls
(frame submission, pad input, movie player, save data, trophies), and the users of a few virtual
file paths.

## Results

On a 16-thread machine with a 20 GB heap:

| Stage | Time | Outcome |
|---|---:|---|
| `unfself.py` + `orbis_prep.py` | seconds | 234,339 relocations applied, 701 imports, 661 PLT trampolines, 162,959 function starts |
| Import + `ApplyOrbisSymbols.java` | ~2 min | 681 imports, 20 data imports, 661 thunks, 684 GOT slots; **161,798** of the 162,958 starts became functions (the rest fell inside data or overlapped) |
| Auto-analysis | **24 min** (1,428 s) | `Analysis succeeded`, project saved (828 MB) |
| `OrbisReport.java` | ~1 min | report + 19 decompiled landmark functions |

Most of the analysis time goes to a few analyzers: *Non-Returning Functions – Discovered* (700 s),
*x86 Constant Reference* (160 s), *Decompiler Switch Analysis* (211 s across both passes),
*Stack* (120 s) and *ELF Scalar Operand References* (86 s). Everything else takes seconds.
Without the `.eh_frame_hdr` function list and the pre-applied relocations, analysis is slower and
finds fewer, worse-bounded functions.

**Final program:** 167,054 functions, of which 8,603 are thunks (the 661 PLT trampolines plus
small `jmp` wrappers inside the game). Only 690 have real names: the imports and the entry point.
**157,761 are still `FUN_xxxxxxxx`**, and naming them is the real reverse-engineering work.

The most-called system functions show what the code is like: `__stack_chk_fail` (35,054
functions, so stack protection is on everywhere), `operator delete`/`new` (6,407 and 2,296),
C++ static-init guards (5,148), `__cxa_atexit`, the MSVC-style `_Xbad_alloc`/`_Xlength_error`
helpers from Sony's Dinkumware-based C++ library, and **wide-string** functions (`wcslen` 1,869,
`wcscmp` 271). FromSoftware's engine handles every path and name as UTF-16.

### What the decompiler shows

`_start` (`0x4000a0`) is the standard PS4 C runtime start, and it leads straight to `main`:

```c
void _start(undefined4 *param_1, __func *param_2)
{
  uVar1 = *param_1;
  libc::_init_env();
  libc::atexit(param_2);
  libc::atexit(FUN_02fa8220);
  FUN_00400020();                                   // static constructors
  __status = FUN_0224a6c0(uVar1, param_1 + 2, 0);   // main(argc, argv, envp)
  libc::catchReturnFromMain(__status);
  libc::exit(__status);
}
```

`FUN_02310150` loads the game parameters. Three things that make it readable come from the prep
step: the import names, the relocated vtable pointers (`PTR_FUN_056f0700`), and the wide strings
the relocations now point at:

```c
  FUN_02bc0e00(local_78, L"param:/GameParam/GameParam.parambnd");
  lVar4 = (*pcVar1)(plVar6, &local_80);                // SprjFile vtable +0x18: open
  ...
  if (DAT_0593b118 == (long *)0x0) {
    FUN_024b55b0("E:\\AB\\SPRJ\\Source\\Library\\FD4\\dist_ps4_vc2012\\Include\\Core/Singleton/FD4DerivedSingleton.h",
                 0xaa, &DAT_04d3b3bd, "SprjFile");
  }
  FUN_02bc0e00(local_c0, L"parampatch:/GameParam/GameParamPatch.parambnd");
  ...
  FUN_02bc0e00(local_108, L"paramdef:/paramdef.paramdefbnd");
```

This one function shows how naming proceeds. The assert gives the source file of a singleton
template and the class name `SprjFile`, so `DAT_0593b118` is *the `SprjFile` singleton* and
`FUN_024b55b0` is *the fatal-assert handler* (called from thousands of places, so naming it pays
off everywhere). It also shows the game's virtual file system: `param:`, `parampatch:` and
`paramdef:` are **mount names**. The executable holds about 70 of them (`map:`, `chr:`,
`chranibnd:`, `sound:`, `movie:`, `msg:`, `talkscript:`, `luascriptpatch:`, …) that map onto the
`dvdroot_ps4/` tree, along with debug/capture mounts (`cap_param:`, `cap_screenshot:`, `dbgai:`,
`capture:/win64savedata/`) that are dead in the release build. A `…patch:` mount exists for
params, Lua and talk scripts, a built-in way to overlay those files without replacing the
originals.

The other landmarks in `work/ghidra/report-eboot-1.09/`: eight frame-submission functions
(`sceGnmSubmitAndFlipCommandBuffers`), pad polling, the movie player set-up, two save-data
routines, trophy unlocking, draw-param loading, and the AI Lua loader that still knows the
developers' `dbgai:/ai/out/%06d_battle.lua` path.

## Where to go from here

Ordered by payoff for effort:

1. **Name from strings.** Asserts carry file, line and class names (`FD4DerivedSingleton.h`,
   `"SprjFile"`), and ~16,600 UTF-16 strings hold paths and class names
   (`SprjEmkSystemUpdateTask::FD4STEPTEMPLATEBASE_CHILD_CLASS`). A script that names a function
   after the class string it references would name thousands of functions at once.
2. **Name the hot helpers by hand.** The assert handler, the wide-string constructor
   (`FUN_02bc0e00`), the heap allocators and the singleton getters are each called thousands of
   times.
3. **Bring in Havok and FMOD.** Their code makes up much of the binary and their headers are
   documented. Ghidra *Function ID* databases or BSim signatures built from matching SDK versions
   (Havok 2014.1, FMOD Ex 4.44.50) would name them in bulk.
4. **Port names from better-mapped relatives.** Dark Souls III (PC) runs on the same FD4 and
   dantelion2 engine, and community work has mapped a great deal of it. BSim similarity search
   can carry those names across.
5. **Diff 1.00 against 1.09.** Run `analyze.ps1 -Self work/base/eboot.bin -Name eboot-1.00` and
   compare with Ghidra's *Version Tracking* to see the code side of the last patch.
6. **Map the data interpreters.** The EMEVD event VM, the ESD state machines, the Lua AI bindings
   (`SprjAiLua`) and the param readers are the places where code and data meet, and the community
   documentation of the data formats tells you what each one should be doing.

## Opening it in the GUI

Run `%GHIDRA_INSTALL_DIR%\ghidraRun.bat` with `JAVA_HOME` set as above, *File → Open
Project…* `work\ghidra\project\Bloodborne.gpr`, and double-click `eboot-1.09.ghidra.elf`. Raise
`MAXMEM` in `support\launch.properties` to about 16G first, because the defaults struggle with a
program this size. The *PS4* script category holds `ApplyOrbisSymbols` and `OrbisReport`.
