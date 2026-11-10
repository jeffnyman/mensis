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
| Python | 3.10+ (3.12 used) with the packages in `pyproject.toml` | `MENSIS_PYTHON`, else `.venv`, else `python` |
| NID database | shadPS4 v0.7.0 `src/core/aerolib/aerolib.inl` (59,280 names) | downloaded to `work/ref/aerolib.inl` on first run |

Ghidra and the JDK are plain zips. Unpack both anywhere and set the two variables, for example in
PowerShell:

```
$env:GHIDRA_INSTALL_DIR = "C:\Tools\ghidra_12.2_PUBLIC"
$env:JAVA_HOME          = "C:\Tools\jdk-25.0.5+8"
```

Then run `%GHIDRA_INSTALL_DIR%\ghidraRun.bat` for the GUI, or `support\analyzeHeadless.bat` for
scripted use. The headless analyzer gets 2 GB of heap by default, which is not enough for an
81 MiB program. `analyze.ps1` sets `GHIDRA_HEADLESS_MAXMEM` to 20G unless you set it yourself; for
the GUI, raise `MAXMEM` in `support\launch.properties`.

## The short version

```
pwsh tools/ghidra/analyze.ps1 -Self work/update/eboot.bin -Name eboot-1.09
```

This runs the four steps below and leaves an analyzed program `eboot-1.09.ghidra.elf` in the
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

## Step 3: import and analyze (`analyzeHeadless` + `ApplyOrbisSymbols.java`)

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
  * GOT slots are labeled and typed as pointers;
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

**The program after analysis:** 167,054 functions, of which 8,603 are thunks (the 661 PLT
trampolines plus small `jmp` wrappers inside the game). At this point only 690 have real names:
the imports and the entry point. **157,761 are still `FUN_xxxxxxxx`**, and naming them is the real
reverse-engineering work ([naming from strings](#naming-from-strings) and
[by hand](#naming-the-hot-helpers-by-hand), below).

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

## Comparing two builds

The package carries two executables: 1.00 in the base PKG and 1.09 in the update. Comparing them
shows the *code* side of the last patch, alongside the data files the update replaced (see
[the package](the-package.md#the-update-is-file-replacement)).

### Load the second build

```
pwsh tools/ghidra/analyze.ps1 -Self work/base/eboot.bin -Name eboot-1.00
```

This runs the same pipeline as above into the same project: 24 minutes of analysis, with the same
outcome as 1.09. Both builds have exactly **162,959** `.eh_frame_hdr` entries and 167,054
functions after analysis, which is the first finding. The patch did not add or remove functions
overall; it edited code inside them. A byte-level comparison is no use, though: the first
difference comes 45 bytes into the code, and from there everything is shifted, so 20,498 of
20,697 code pages differ.

### Why not Version Tracking

Ghidra's own tool for this is *Version Tracking*, and it runs headless through the bundled
`AutoVersionTrackingScript.java`:

```
analyzeHeadless work\ghidra\project Bloodborne -process eboot-1.09.ghidra.elf -noanalysis ^
    -postScript AutoVersionTrackingScript.java "/" "vt-1.00-to-1.09" "/eboot-1.00.ghidra.elf"
```

It keeps **both** programs in memory and runs every correlator across 160k × 160k functions. On a
32 GB machine with a 20 GB Ghidra heap it ran the system out of memory before finishing. It also
leaves a half-made session behind that blocks a rerun under the same name;
`DeleteProjectFile.java` removes it. Version Tracking is the right tool for programs that differ a
lot. Two builds of the same game a patch apart don't need it.

### The lighter way

Instead, each build is summarized on its own, one program in memory at a time, and the comparison
happens outside Ghidra.

1. **`ExportFunctionHashes.java`** writes one row per function: address, size, instruction count,
   strings it references, and two *Function ID* hashes. The **full hash** covers the instructions
   with every address-dependent operand masked out, so the same code at a different address
   hashes the same. The **specific hash** also covers constant operands, so it catches a changed
   number that leaves the instructions alone.

   ```
   analyzeHeadless work\ghidra\project Bloodborne -process "eboot-1.*.ghidra.elf" -noanalysis -readOnly ^
       -scriptPath tools\ghidra -postScript ExportFunctionHashes.java work\ghidra\diff
   ```

   Both builds take a few minutes with a 12 GB heap.

2. **`tools/build_diff.py`** sorts each list by address and aligns them as two token sequences
   (Python's `difflib`, one token per function's full hash). This works because a linker lays
   functions out in object-file order, and a patch build keeps that order. Runs that align are
   *identical*, or *constants* where only the specific hash differs. A run that differs but has
   the same length on both sides is the same functions *changed*. Whatever is left over is paired
   by hash where possible, because the linker sometimes places a whole object file elsewhere and
   an alignment sees that as a delete in one place and an insert in another.

   ```
   python -I tools/build_diff.py work/ghidra/diff/eboot-1.00.functions.tsv \
          work/ghidra/diff/eboot-1.09.functions.tsv work/ghidra/diff/diff-1.00-to-1.09.md
   ```

   It takes under a minute.

### 1.00 → 1.09

| Bucket | Functions |
|---|---:|
| identical | 157,840 |
| moved (same code, new place) | 484 |
| constants only | 34 (+3 moved) |
| **changed** | **80** (+2 less certain) |
| removed | 8 |
| added | 8 |

158,451 non-thunk functions per build. So **99.6% of the code is unchanged**, and the patch
touches about a hundred functions. Their referenced strings and class names (taken from the
singleton asserts) show where the changes are:

* **Developer tooling switched off.** The clearest pattern in the patch. Of the functions that
  read the `Game.IsUseLuaConsole` property, the console's constructor shrinks from 431 to 79
  bytes, two shrink to 3-byte stubs, one from 199 to 61 bytes, and one is removed. Functions
  touching `SprjSystemProperties`/`FsrmiWrapper.Port` (a remote debugging connection) shrink from
  1,139 to 750 and from 165 to 3 bytes. The `DbgRemote` singleton user drops from 143 to 47
  bytes. Three remote-probe functions (`TestRemoteProbe`, `ClientSendWithReply_ReqPos`, …) are
  removed outright. The release build had shipped with these hooks compiled in, and 1.09
  stubbed them out.
* **AI scripting:** the `AiLua` / `SprjAiLua` set-up and the `aiscript`/`luascriptpatch` mount
  logic changed. That fits the update replacing AI Lua for seven maps, and `luascriptpatch:` is how
  patched Lua is overlaid.
* **Online play:** several `FrpgNetMan`, `SprjNetworkClientMan`, `SprjSessionManager` and
  `CSMultiPlayMan` functions, plus one reading `ChrSync.FrameSkipCount`: co-op/PvP session and
  character-sync fixes.
* **Items and menus:** `SprjGaitem` (item instances), `MsgRepository` users that build weapon and
  gem lists (`forWepList`, `forGemList`, `?GemName?`), and save-slot text (`SLOT%d:`).
* **World and events:** `WorldChrMan`, `SprjEventMan`/`SprjLuaEventMan`, `SprjEventFlagMan` with
  `CSDlc`/`CSPlaygo`, a new `SprjBulletIns` function, and the hit-box `Weapon&Body` helper.
* **Graphics:** a graphics object's constructor grows from 117 to 469 bytes. Its
  `DLLinearHeap::FreeUntil()` assert string first suggested a heap change, but reading the code
  showed it now builds a default scale buffer and inlines the temporary-heap code that the assert
  belongs to ([the patch](the-patch.md#3-graphics-a-default-scale-buffer)). The system
  texture/shader loader also changed.

The 34 constant-only changes are mostly in third-party code at addresses that did not move
(`/libexec/ld-elf.so.1` and PCRE-style option strings appear among them). They look like
references to data that moved, not tuned numbers. The game's *balance* changes are not in code
at all: they are in the replaced `gameparam.parambnd.dcx`.

The full tables, with addresses in both builds for every changed, moved, added and removed
function, are in `work/ghidra/diff/diff-1.00-to-1.09.md`.

## Naming from strings

After analysis, 157k functions are `FUN_xxxxxxxx`. The binary has no symbols, but the engine and
its middleware leave strings that say what nearby code is. Turning those into names is the
cheapest naming there is, and it is mechanical enough to script.

### Exploring first: `FindStringUses.java`

Before writing a namer it helps to see *how* a kind of string is used. `FindStringUses.java`
lists every string matching some regular expressions, every instruction that references them,
and optionally decompiles the referencing functions:

```
analyzeHeadless work\ghidra\project Bloodborne -process eboot-1.09.ghidra.elf -noanalysis -readOnly ^
    -scriptPath tools\ghidra -postScript FindStringUses.java out.txt 3 "^SprjEmkSystemUpdateTask"
```

`analyzeHeadless.bat` passes its arguments through `cmd.exe`, which treats `| < > ^ &` as its
own. So `<regex>` arguments are OR'd together rather than written with `|`, and a pattern that
needs those characters goes in a file passed as `@file`.

Three things came out of exploring:

* About **740 UTF-16 strings of the form `Class::STEP_Name`** (`MapRes::STEP_LoTex_Finish`,
  `SprjSystemStep::STEP_Wait_forBootPhase5`, `CSMovieIns::STEP_Init`). FD4 runs much of the game
  as step-based state machines, and static initializers register each step as a 24-byte record
  `{ code*, 0, L"Class::STEP_Name" }`:

  ```c
  _DAT_05957950 = FUN_0198c2b0;  _DAT_05957958 = 0;  _DAT_05957960 = L"MapRes::STEP_Init";
  _DAT_05957968 = FUN_0198c2d0;  _DAT_05957970 = 0;  _DAT_05957978 = L"MapRes::STEP_NoData";
  ```

  So the function stored next to each name *is* that step. These are the developers' own method
  names.
* About **70 Havok profiler markers** (`TthkbWorld::step`, `LthkpWorld::getClosestPoints`) and
  Scaleform renderer markers (`HAL::BeginFrame`). Havok's `HK_TIMER_BEGIN` macros put these
  inside the function being timed, with a `Tt`/`Lt` prefix.
* About **5,660 singleton asserts**:
  `if (g == 0) assert("...FD4Singleton.h" or "...FD4DerivedSingleton.h", line, ..., "ClassName")`.
  The global `g` is the instance pointer of `ClassName`.

### `NameFromStrings.java`

```
analyzeHeadless work\ghidra\project Bloodborne -process eboot-1.09.ghidra.elf -noanalysis ^
    -scriptPath tools\ghidra -postScript NameFromStrings.java work\ghidra\naming\eboot-1.09.tsv [dry]
```

It applies three rules, most certain first, and writes every decision (and every refusal) to the
TSV. With `dry` it changes nothing.

| Rule | How | Named in 1.09 |
|---|---|---:|
| **step** | In each function that references a step name, track `LEA reg,[addr]` followed by `MOV [abs],reg`. A step-name string stored at `X` with a function stored at `X−0x10` names that function `Class::STEP_Name`, in a namespace for the class | 742 functions (744 entries; two functions serve two steps each) |
| **havok / scaleform** | A function referencing exactly **one** distinct profiler marker is named after it, minus the `Tt`/`Lt` prefix. With several markers the function is probably several functions inlined together, so it is left alone (4 such) | 74 + 18 functions |
| **singleton** | For each assert, find its class-name argument, then the null check that guards it (below). Name the global `g_<Class>` | 109 globals |

The singleton rule needed two corrections, both worth knowing for this kind of work:

1. **Two headers, arguments in any order.** Engine code asserts from `FD4Singleton.h` and game
   code from `FD4DerivedSingleton.h`. The class-name argument can be loaded before or after the
   path string, so the rule searches a window around the path string up to the `CALL`.
   Matching only `FD4DerivedSingleton.h` and searching forward found just 3 classes.
2. **The null check is not just before the assert.** The compiler moves each assert into a cold,
   out-of-line block, so the instruction physically before it belongs to unrelated code. Taking
   "the nearest global read before the assert" gave one class name on ten different globals. The
   rule now uses Ghidra's `BasicBlockModel`: it takes the block holding the assert, follows every
   branch into it (and the conditional branch that falls through into it), and reads the global
   loaded just before *that*. It also requires a clear majority (at least 2 votes and 90%) and
   allows each class name on only one global. Result: 109 globals, every name unique; 30
   ambiguous ones are logged and left unnamed.

The handler, called by all 5,659 asserts, becomes `AssertFailed`. (It was first named
`FD4_SingletonAssert`; [hand-naming](#naming-the-hot-helpers-by-hand) showed it is the engine-wide
assert handler, used by lock, mutex and heap code too.) That name and the `g_` prefix are
*descriptive*, chosen here; the step and marker names are *the binary's own*. Every name is applied with Ghidra's **analysis** source type, so a name a person
sets later always wins. Each named item also gets a comment saying which rule named it and on
what evidence (`[mensis:step] step table entry 05957960 written by FUN_0198d3d0`).

The results agree with everything identified by hand earlier: `DAT_0593b118` → `g_SprjFile`,
`FUN_024b55b0` → `AssertFailed`, `FUN_016f2510` →
`SprjEmkSystemUpdateTask::_STEP_Update`. The parameter loader now decompiles as:

```c
  if (g_SprjFile != (long *)0x0) {
    pcVar1 = *(code **)(*g_SprjFile + 0x18);
    FUN_02bc0e00(local_78, L"param:/GameParam/GameParam.parambnd");
    ...
      AssertFailed("E:\\AB\\SPRJ\\...\\FD4DerivedSingleton.h", 0xaa, &DAT_04d3b3bd, "SprjFile");
```

**Numbers:** 835 functions and 109 globals named in 1.09 (834 and 109 in 1.00). That's under 1%
of the functions, but the globals are the engine's managers (`g_WorldChrMan`, `g_FrpgNetMan`,
`g_SprjEventFlagMan`, `g_SprjGaitem`, `g_CSMovie`, `g_LuaConsoleManager`, …), and between them
they are null-checked at over 5,000 sites. Their names now show up in every function that uses a
manager, and those uses are the starting point for naming the managers' methods.

## Naming the hot helpers by hand

Rules name what the strings point at. The next most valuable names belong to the small helpers
every other function calls: allocators, locks, string routines. Naming one of those improves
thousands of decompiled functions at once. These need reading, not rules.

### Finding them: `MostCalled.java`

```
analyzeHeadless work\ghidra\project Bloodborne -process eboot-1.09.ghidra.elf -noanalysis -readOnly ^
    -scriptPath tools\ghidra -postScript MostCalled.java work\ghidra\hot 40
```

It counts, for every unnamed function, how many distinct functions call it (following thunks to
the real target), and writes the top N to `most-called.tsv` along with a decompilation of each.
The top of the list in 1.09:

| Rank | Address | Callers |
|---:|---|---:|
| 1 | `0247b720` | 4,057 |
| 2 | `0247fa50` | 3,827 |
| 3 | `0247ee80` | 2,744 |
| 4 | `02483fe0` | 2,718 |
| 5 | `0247bbf0` | 2,672 |

A companion script, `Decompile.java`, decompiles any list of addresses. An address written as
`addr:callers` also gets up to three of its callers, which is often how a function's meaning
becomes clear: #14 is a one-line call through a vtable, and only its callers (`#14(0xB0, 0x10,
heap)`, then an object built in the result) show it is an aligned allocation.

### What they turned out to be

Reading the top 40 gave names to 25 of them. Two more functions were named along the way
(`WString_FromCStr`, and `AssertFailed`, below), plus two globals. The other 15 are clear as code
but nothing says *what* they are, so they stay unnamed rather than get invented names. The named
ones fall into a few families:

* **Memory.** The engine's heaps (dantelion2's). `Heap_FindForPointer` (#1, 4,057 callers) is
  the core of every free: it takes a pointer and checks the thread's temporary heap range, then
  the heap list under a read/write lock, then falls back to the default heap. Around it:
  `Heap_GetDefault`, `MemorySystem_Get` (a 0x440-byte global holding the heap list, its lock and
  the container allocator), `Heap_AllocAligned`, the per-thread `TemporaryHeap_*`, and the
  `HeapContainer_*` constructor/destructor behind the ubiquitous "Tried to create container with
  incompatible heap." assert.
* **Locks.** `PthreadMutex_InitRecursive` (asserts from `PthreadMutex.cpp`; mutex type 2 is
  recursive), `Mutex_IsCreated`, `GlobalInitMutex_Get`, `AtomicDecrement`.
* **Strings.** The wide-string routines of Sony's Dinkumware C++ library: `WString_Assign`
  (found twice, identical 477-byte bodies, two template instantiations), `WString_AssignSubstr`,
  and `WString_FromCStr`, which builds every virtual path such as
  `L"param:/GameParam/GameParam.parambnd"`. The layout gives them away: an 8-character inline
  buffer at `+8`, length at `+0x18`, capacity at `+0x20`. Also `String_Format`.
* **Reference counting.** `RefCounted_Release` and `RefCounted_ReleaseAtomic` (decrement, and at
  zero call the destructor slot).
* **Middleware.** Havok's `hkArrayUtil::_reserve`/`_reserveMore` (the capacity word's
  `0x3FFFFFFF` mask and "don't deallocate" bit are `hkArray`'s), `HavokScript_PrintAssert`, and
  two Scaleform ActionScript 3 garbage-collector release functions.
* **Reflection.** `DLRuntimeClass_Register` (asserts from `DLRuntimeClass.cpp`), the call the
  class-registration initializers make right after writing `THIS_CLASS`.

Reading also corrected an earlier name. The function `NameFromStrings` had called
`FD4_SingletonAssert` is the engine-wide assert handler: the lock, mutex and heap code call it
with their own file names. It is now `AssertFailed`, in the hand list and in the script.

### Recording names: `tools/names/*.tsv` and `ApplyNames.java`

Unlike rule-made names, these are judgments, so each one is kept in a committed file with its
evidence and a confidence level:

| Confidence | Meaning | Count |
|---|---|---:|
| certain | the code does exactly this, or a source-file assert names it | 7 |
| likely | the behavior is clear; the exact name is ours | 18 |
| probable | attributing it to a particular library is an inference | 4 |

```
address   kind      name                   confidence  evidence
0247b720  function  Heap_FindForPointer    likely      rank 1, 4,057 callers: given a pointer, checks ...
02483fe0  function  PthreadMutex_InitRecursive  certain  rank 4: scePthreadMutexattrSettype(attr, 2 = recursive) ...
```

(The list has grown since this reading, for 1.09 only: `ChaliceGlyphEntry_OnInput`, found while
working out [Chalice Glyphs](the-chalice-dungeons.md#glyphs), 44 network functions (the HTTP
client, the `ss.info` fetch and one builder per game-service request) from
[the online boundary](the-online-boundary.md#the-requests), 12 for the Chalice ritual roll from
[how a ritual picks its layout](the-chalice-dungeons.md#how-a-ritual-picks-its-layout), and 7 for
the stat curve, attack rating, defense and level costs from [combat](the-combat.md#the-formulas).)

`ApplyNames.java <names.tsv>` applies a list as **user-defined** names, which outrank both
analysis and `NameFromStrings`. Each item gets a comment like `[mensis:hand/likely] rank 1, …`,
so the evidence is visible right in the listing.

### Carrying names to the other build: `tools/port_names.py`

The list is written against 1.09 addresses. `port_names.py` aligns the two builds' function lists
the same way `build_diff.py` does. It moves a function name across only when the function is
paired with **identical** code, so a name always lands on the code it was judged on. All 27 moved
to 1.00. Globals can't be paired that way, so the two were found by hand through the functions
that read them (`WString_FromCStr` and `HeapContainer_Construct` in 1.00) and added to
`tools/names/eboot-1.00.hand.tsv`.

The parameter loader, before any naming and after both passes:

```c
// before
  if (DAT_0593b118 != (long *)0x0) {
    FUN_02bc0e00(local_78, L"param:/GameParam/GameParam.parambnd");
      FUN_024b55b0("E:\\AB\\SPRJ\\...\\FD4DerivedSingleton.h", 0xaa, &DAT_04d3b3bd, "SprjFile");
// after
  if (g_SprjFile != (long *)0x0) {
    WString_FromCStr(local_78, L"param:/GameParam/GameParam.parambnd");
      AssertFailed("E:\\AB\\SPRJ\\...\\FD4DerivedSingleton.h", 0xaa, &DAT_04d3b3bd, "SprjFile");
```

## Where to go from here

Ordered by payoff for effort:

1. **More rules for `NameFromStrings`.** The three rules in
   [Naming from strings](#naming-from-strings) are a start. Other assert macros (the dantelion2
   `SingletonLifetimePolicies.inl` ones, `DLReferenceCountObject`, container heap checks) and the
   `THIS_CLASS` registrations (which tie a class name to its runtime class record) are the next
   candidates.
2. **Keep going down the most-called list.** [Naming the hot helpers by
   hand](#naming-the-hot-helpers-by-hand) covered the top 40; ranks 40–200 are still called
   hundreds of times each.
3. **Bring in Havok and FMOD.** Their code makes up much of the binary and their headers are
   documented. Ghidra *Function ID* databases or BSim signatures built from matching SDK versions
   (Havok 2014.1, FMOD Ex 4.44.50) would name them in bulk.
4. **Port names from better-mapped relatives.** Dark Souls III (PC) runs on the same FD4 and
   dantelion2 engine, and community work has mapped a great deal of it. BSim similarity search
   can carry those names across.
5. **Name the managers' methods.** [What the 1.09 patch changed](the-patch.md) shows where
   reading stops: changes inside `FrpgNetMan`, `SprjGaitem`, `WorldChrMan` and friends are clear
   as code but not yet as meaning.
6. **Map the data interpreters.** The EMEVD event VM, the ESD state machines, the Lua AI bindings
   (`SprjAiLua`) and the param readers are the places where code and data meet, and the community
   documentation of the data formats tells you what each one should be doing.

## Opening it in the GUI

Run `%GHIDRA_INSTALL_DIR%\ghidraRun.bat` with `JAVA_HOME` set as above, *File → Open
Project…* `work\ghidra\project\Bloodborne.gpr`, and double-click `eboot-1.09.ghidra.elf`. Raise
`MAXMEM` in `support\launch.properties` to about 16G first, because the defaults struggle with a
program this size. The *PS4* script category in the Script Manager holds all the scripts in
`tools/ghidra/`; `FindStringUses`, `RefsTo`, `Decompile` and `MostCalled` are the ones most useful
for exploring by hand.
