# The loader: watching the game start

An emulator's first job is to stand in for the PS4's system: the 39 libraries and about 700
functions Bloodborne imports ([the executable](the-executable.md)). Before writing any of that
for real, `tools/loader.py` asks a smaller question: **what does the game call, in what order,
when it starts?** It runs the game's own code under a CPU emulator, replaces every system call
with a Python function that logs it, and stops when the game gets stuck.

```
python -I tools/loader.py work/ghidra/eboot-1.09.ghidra.elf work/ghidra/eboot-1.09.symbols.tsv work/loader
```

It writes `work/loader/calls.log`, every system call in order with its caller and arguments,
and `work/loader/summary.md`, where the run stopped and every import it reached.

## How it works

* **The executable** is `orbis_prep.py`'s output ([loading it into Ghidra](ghidra.md)), already
  relocated to 0x400000 with every import's GOT slot pointing at a one-byte stub (a `RET`) in a
  segment of its own. The loader maps its loadable segments, a 16 MiB stack and a heap.
* **The CPU** is [Unicorn](https://www.unicorn-engine.org/), an x86-64 emulator, so the game's
  code runs instruction by instruction, under the loader's control.
* **System calls** are caught by a hook on the stub segment. The hook reads the arguments in the
  System V order the PS4 uses (`rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`), runs a Python stand-in,
  puts the result in `rax`, and lets the stub's `RET` return to the game.
* **The stand-ins** are the least start-up needs: an allocator, memory and string functions,
  direct-memory mapping, C++ static-initialization guards, and a direct-memory size of 5 GiB.
  Lock and semaphore calls return 0, which is right when only one thread runs. Anything else
  returns 0 and is counted as unhandled.
* **Thread-local storage** follows x86-64's variant II: the TLS block sits just below the thread
  pointer, `fs` holds the thread pointer, and `fs:[0]` holds its own address.
* **The entry point**, `_start`, gets the PS4's entry parameters in `rdi` (`argc` 1, `argv`
  "eboot.bin") and an exit handler in `rsi`. The exit handler is also pushed as its return
  address ([why](#what-it-showed)).

## What the game does first

The run reaches 26,631 system calls, to 30 different functions, before it stops. In order:

1. **The C runtime.** `_init_env`, then `atexit` registers the exit handler.
2. **Its own allocator.** One `__cxa_guard_acquire` in, the game builds a mutex, asks for the
   direct-memory size (`sceKernelGetDirectMemorySize`), then takes one block of **0x7400000
   bytes (116 MiB)**, aligned to 2 MiB (`sceKernelAllocateDirectMemory`), and maps it
   (`sceKernelMapDirectMemory`). Everything after this allocates from that block, so the game
   never calls `malloc` during start-up.
3. **Static objects.** Then come the C++ static constructors: 1,106 guarded initializations,
   1,320 mutexes (each made with `scePthreadMutexattrInit`, `Settype`, `Init`, and the
   attribute destroyed), 2,813 lock/unlock pairs, 211 `__cxa_atexit` registrations, 17
   semaphores and 56 C++ stream initializations (`ios_base::Init`). Many objects register
   themselves by name, which is why `strlen` (3,372 calls) and `memcmp` (9,661) are the most
   frequent calls of all.
4. **A clock.** One `gettimeofday`, and a few calls into the PS4's own pthread functions
   (`pthread_mutex_*`, not `scePthread*`).

No thread is created, no file is opened and nothing touches the GPU before the run stops.

## Where it stops: AVX

The run ends while static initialization is still going, at 0x143279b, on `vblendvps xmm3, xmm3, xmm4,
xmm6`: an **AVX** instruction. The PS4's CPU (AMD Jaguar) has AVX, and Bloodborne uses it.
Unicorn is built on an older QEMU whose x86 emulation has no AVX at all, so the first AVX
instruction is the end of the road.

AVX isn't rare in this game. Of the executable's 26.8 million instructions, **1.03 million
(3.8%) are AVX**, of 219 kinds. They're nearly all 128-bit (only about 9,000 use the 256-bit
`ymm` registers). The commonest are the everyday vector-math ones: `vmovaps`, `vmovss`,
`vmulps`, `vpshufd`, `vaddps`. Sixty kinds cover 95% of uses, and eighty cover 98%.

## What it tells us

* **An emulator for this game has to run its code natively.** Emulating the CPU in software
  means emulating AVX, and with a million AVX instructions spread through the game, that is
  too slow for anything past start-up. The PS4's CPU is x86-64 like a PC's, so native
  execution is possible. That is why PS4 emulators work at all.
* **Native execution means C, C++ or Rust**, which can map the game's code into memory and jump
  into it. It also means meeting two problems the loader avoided by emulating:
  * **The calling convention.** The game calls the system with the System V convention
    (Linux's). Windows uses another, so on Windows the game's calls have to be translated
    before they reach the loader's own code.
  * **Thread-local storage.** The game reads per-thread data through the `fs` register. Linux
    lets a program set `fs`; 64-bit Windows doesn't, so on Windows those instructions have to
    be found and rewritten before they run.

  [Running it natively](#running-it-natively) shows how the native loader meets both, on
  Windows and on Linux.
* **The call log is the to-do list.** Start-up needs about thirty functions before anything
  else: an allocator, string and memory functions, locks, semaphores, static-init guards,
  direct memory and a clock. Those are the first stand-ins a native loader needs, and the log
  shows the arguments each is called with. [Running it natively](#running-it-natively) works
  through that list.

## Running it natively

`native/` is the next step: the same loader in C++, running the game's code directly on the
CPU. It runs on Windows and on Linux, on any x86-64 machine. On Windows it needs the Visual
Studio 2022 Build Tools with the "Desktop development with C++" workload and its "C++ Clang
tools for Windows" component:

```
pwsh native/build.ps1
work/native/windows/mensis-loader.exe work/ghidra/eboot-1.09.ghidra.elf work/ghidra/eboot-1.09.symbols.tsv work/loader-native
```

On Linux (or WSL2), with clang or g++, CMake and Ninja:

```
cmake -S native -B work/native/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build work/native/linux
work/native/linux/mensis-loader work/ghidra/eboot-1.09.ghidra.elf work/ghidra/eboot-1.09.symbols.tsv work/loader-native
```

It writes the same `calls.log` and `summary.md` into `work/loader-native/`, plus `stdout.txt`,
whatever the game printed, and `files/`, the folders the game may write to. The two builds run
the same code: up to the point where the game's threads started running, they made the same
calls, from the same places, with the same results.

The game's own files (`/app0/`) are read from `work/update`, then `work/base`: the 1.09
update's files over the base game's, as [the package](the-package.md) extracts them. Run from
the repository's root, or name the folders with `--app0` (more than once, first match wins).

### How it works

* **The game's memory** is reserved at the addresses it was linked for (0x400000 up), and the
  stack, heap, thread-local storage and entry parameters at the same fixed addresses as
  `loader.py`'s. Both loaders hand the game the same pointers, so their logs can be compared.
  The heap is the one exception: Windows places some memory of its own at random addresses
  before the loader starts, now and then inside the heap's range. Then the heap moves to the
  next of a few fixed places, and the summary says so.
* **Each import stub** (eight bytes) is overwritten with a jump to a small thunk, which loads
  the import's number and jumps to one shared routine (`enter_windows.S`, `enter_linux.S`).
  That routine gathers the argument registers into a frame, calls the C++ dispatcher, and puts
  the result in `rax` (and `rdx` and `xmm0`, for the few calls that return there).
* **The calling convention.** The game passes arguments System V's way, Linux's. On Linux a
  stand-in could take them directly; on Windows, whose convention differs, it couldn't. Neither
  build needs to: the assembly routine hands the dispatcher one pointer to the frame, and a
  pointer is passed the same way everywhere. `vprintf` reads the game's System V `va_list`
  itself, one conversion at a time, rather than handing it to the host's C library.
* **The thread pointer.** The game reads it with one instruction, `mov rax, qword ptr fs:[0]`,
  which appears 17,127 times, and in no other way. The PS4 keeps it in `fs`; 64-bit Windows
  doesn't let a program set `fs`, and on Linux the loader's own C library uses it. So before
  the run each is rewritten in place, at the same nine bytes, to read through `gs` instead,
  which neither loader uses for anything else. No stack, flag or other register is touched:
  * on Windows, to `mov rax, qword ptr gs:[0x1480 + 8 × slot]`. There `gs` points at the
    thread's own information block, whose thread-local slots start at 0x1480, so this reads a
    slot (from `TlsAlloc`) where each thread keeps its game thread pointer;
  * on Linux, to `mov rax, qword ptr gs:[0]`, with each thread's `gs` base set to its thread
    pointer (`wrgsbase`, which Linux allows since 5.9): `[tp]` holds the thread pointer, as on
    the PS4.
* **Threads.** Each thread the game creates runs on a thread of the loader's own, on a stack
  of the size it asked for (in a 64 MiB slot of its own, with a megabyte more for the loader's
  code, which runs on the game's stack during a call) and with a TLS block of its own. Mutexes,
  condition variables, read-write locks and semaphores are real. Each object the game declares
  is a pointer-sized handle, as in FreeBSD, and points at the loader's own object, kept in an
  area of its own, so the game's heap is laid out as in a run without threads. Function-level
  statics (`__cxa_guard_acquire`) are constructed once, with a second thread waiting for the
  first.
* **Files.** The PS4 gives a game a few places: `/app0/`, its own package, read-only, here
  the folders named above; and `/download0/`, `/temp0/` and `/data/`, which it may write, here
  folders in the run's output (`files/download0/`, ...). Any other path doesn't exist. Three
  families of calls reach them, sharing one table of open files: the kernel's (`sceKernelOpen`,
  `Read`, `Lseek`, `Stat`, `Getdirentries`, ...), POSIX's (`stat`, `mkdir`, with `errno`
  through `__error`), and the C library's streams (`fopen`, `fread`, `fgets`, ...). Each logged
  open and stat shows which file answered. The `printf` family writes to `stdout.txt`; the
  variadic ones read their arguments the way the called function's own `va_start` would, from
  the registers and the caller's stack.
* **The graphics driver and the display** answer the questions a driver answers at set-up:
  where the tessellation ring goes, a compute queue's id, that validation isn't built in (as
  on a retail console), and the display's handle, buffer attributes and event queue. There is
  no GPU behind them yet. The run stops at the game's first GPU submission
  (`sceGnmSubmitCommandBuffers`), and saves the command buffers it submits to `gpu/`.
* **Sound** is taken and discarded, at the pace of a real audio port: `sceAudioOutOutput`
  waits until the port's next buffer is due (256 samples at 48 kHz, every 5.3 ms), as the
  sound library's audio thread expects.
* **The C library's** mathematics, strings and number parsing are the host's, including
  Dinkumware's internal names (`_FLog`, `_Sin`, `_Stoul`, ...). The game's assertions
  (`_Assert`) go to `stdout.txt`, and the run carries on, as a release build's would.
* **Stopping**: a fault is caught (Linux: a signal, on a stack of the thread's own; Windows: a
  vectored exception handler), recorded, and the thread resumes on its own stack, from where
  the outputs are written and the process ends. The summary shows the instructions at the fault
  (Linux, through `objdump`) or their bytes (Windows), the game-code addresses on the
  faulting thread's stack (mostly return addresses: the way back), and every thread with its
  state and last call. Any thread can stop the run; so can the main thread's `_start`
  returning, a time limit (`--seconds`, 10 by default) and a call limit (`--calls`).
* **Repeatable, as far as threads allow.** Anything the stand-ins hand the game lives at a
  fixed address. Once the game's threads run, the order in which they make their calls, and
  how often a thread polling the clock does so, varies from run to run.

### What it showed

1. **The same start-up.** Over the 26,631 calls `loader.py` reached, the native run makes the
   same calls, from the same places, with the same results.
2. **The stack was misaligned.** The first native run faulted after 2,402 calls on
   `vmovaps [rbp-0x70]`, an instruction that requires a 16-byte-aligned address, eight bytes
   off. `_start` begins `push rbp; mov rbp, rsp`: it is an ordinary function and expects to be
   *called*, with a return address on the stack. Both loaders now push the exit handler as
   that return address. Unicorn doesn't check alignment, so `loader.py` never noticed.
3. **Past AVX.** The `vblendvps` that stopped `loader.py` just runs.
4. **The engine's own error report.** Next the game printed, through `vprintf`:

   ```
   [Dantelion2 Panic]
   E:\AB\SPRJ\Source\SPRJ\Source\Sys\Mem\SprjFastBiHeap.cpp(1570)
   SprjFastBiHeap already initialized
   DL_PANIC is invoking Debugger...
   DL_PANIC is firing core dump...
   ```

   and then wrote to address 0 on purpose (`mov dword [0], 0xdeadba`). Dantelion2 is
   FromSoftware's engine, and SPRJ is Bloodborne's project name. The cause was the stand-ins
   that answered 0: `wcscmp` saying every pair of strings is equal, `wcstol` reading every
   number as 0, `ldiv` returning no remainder. The game compares wide-string names (build
   configurations such as `Release_win64`, `Master_ps4_GFXMIX`, `Master_ps4_flexible`, and
   heap names `MAIN`, `GFX`) and parses its settings from wide text. With real answers, the
   panic went away.
5. **The PS4's `wchar_t` is 16 bits**, where Linux's is 32, so the wide-string functions are
   written out over 16-bit characters rather than taken from the host's C library. The
   character-class tables (`_Getpctype`) are Dinkumware's, the C library the PS4's is built
   on.
6. **The game claims almost all of its memory up front**: 13 direct-memory blocks totalling
   4.48 GiB and six flexible-memory blocks totalling 322 MiB, nearly all within a thousand
   calls of each other and before any thread but the logger's. It then carves its own heaps
   out of them, so the system's allocator is barely used.
7. **Then threads.** Without them, the run made 112,777 calls to 83 functions, asked for 13
   threads (a background logger, a job worker, nine work pools `EzWorkPool_TypeA0` to
   `TypeD1`, `MOMainThread` and `GXRenderThread`), started the network, SSL and HTTP
   libraries, and faulted in the main thread, reading through a null pointer just after a
   `scePthreadCondWait`. The obvious reading, that the wait was for a thread that never ran,
   turned out to be wrong: with the threads running, the fault is the same (below).
8. **With the threads running**, the game starts 16 of them, and they behave as threads
   should: the work pools wait on their condition variables for work, the logger and the job
   worker on their semaphores. Three more belong to FMOD, the sound library: its mixer, its
   audio-out thread and its stream thread. About 290,000 calls are made in a fifth of a
   second.
9. **Files are the next wall.** `MOMainThread` is the sound system's start-up. It starts FMOD,
   asks for the sound project, `/app0/dvdroot_ps4/sound/sprj_mixer.fev`
   (`sceKernelStat`), is told it doesn't exist (there is no file system yet), shuts FMOD down
   cleanly (waits for its three threads, deletes their semaphores), wakes the main thread
   and exits. The main thread goes on without sound into graphics set-up (the function that
   creates the default render target and depth stencil) and faults on the same null pointer
   as before, in a call made with a null object. `/app0/` is the game's own directory, and
   its files are those [the package](the-package.md) extracts into `work/`.
10. **With its files, sound starts.** The sound start-up finds `sprj_mixer.fev` and its
    companions (`sprj_multirpc.rpc`, `sprj_multimix.mix`, `sprj_multich.mch`) in the base
    game, opens and reads them, reads its settings as wide text ("0.60", "0.02", ...), and
    stays up: FMOD's mixer and audio-out threads run, handing sound to `sceAudioOutOutput`
    (which has no stand-in, so nothing is heard).
11. **Graphics are the next wall.** The main thread still faults on the same null object, so
    the missing sound wasn't its cause. Meanwhile the render thread has made its first calls
    into the graphics driver (`sceGnmMapComputeQueue`, `sceGnmValidateCommandBuffers`, the
    tessellation ring's address), all without stand-ins. Which thread faults first varies
    from run to run; both are setting up graphics.
12. **The graphics set-up was failing, and cleaning up after itself.** The render thread's
    set-up (read through Ghidra's decompiler) allocates and maps the tessellation ring, then
    maps a compute queue, and checks the queue's id: 0 means failure. With no stand-in, the
    driver call returned 0, so the render thread released the ring, woke the main thread as
    if set-up were done, and the main thread went on to read the objects that were never
    made: the null pointer. The PS4's driver numbers queues from 1. With that one answer the
    set-up completes, and the run goes on to the first GPU submission.
13. **The game starts the rest of itself**: five graphics workers (`GXWorker:1` to `5`), the
    resource system (`Core.Res.FileLoader`, `CacheableFileLoader`, `TaskManager` and three
    `PostProcessor`s), 28 threads in all. It opens the display (one 1920×1080 output, 8-bit
    ARGB in sRGB, three frame buffers to flip between, and a `FlipEventQueue`). YEBIS, the
    post-processing middleware, reports a run of assertions while creating its effects
    (`hfx.tech_BokehBlur_Sn[...]`, `hfx.fParam_FocusDistance`, ...): techniques and parameters
    it looks for and doesn't find. Why is still open; the run carries on as a release build
    would.
14. **The first GPU submission** comes after about 4.2 million calls and two seconds, from
    the render thread, on Windows and Linux alike. It is one command buffer of 28 bytes, a
    single PM4 packet (the GPU's command format): `WAIT_REG_MEM`, "wait until the 32-bit
    value at 0x2_4140_0030 equals 0x29", a label next to the compute queue's ring. It sits
    where the game had asked `sceGnmDrawInitDefaultHardwareState200` to write the GPU's
    default state, which has no stand-in yet, so the wait is all there is. Reading what the
    game submits, and giving the GPU something to run it on, is the next step.

## Files

| Path | |
|---|---|
| `tools/loader.py` | runs the start-up under Unicorn and logs every system call |
| `native/` | the native loader: `main.cpp` (loading, dispatch, output); the stand-ins, in `standins.cpp`, `threads.cpp`, `files.cpp`, `libc.cpp`, `gnm.cpp` (graphics and display) and `audio.cpp`; and for each platform `platform_*.cpp` (memory, the thread pointer, stopping) and `enter_*.S` (the crossings between the game and the loader); `build.ps1` builds it on Windows |
| `work/loader/calls.log` | every call, in order, with its caller and arguments |
| `work/loader/summary.md` | where the run stopped, and every import it reached |
| `work/loader-native/` | the native run's `calls.log`, `summary.md`, `stdout.txt`, `files/` and `gpu/` (the submitted command buffers) |
