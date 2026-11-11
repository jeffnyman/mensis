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
  "eboot.bin") and an exit handler in `rsi`.

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
    (Linux's). Windows uses another, so on Windows every stand-in must be declared System V
    (`sysv_abi` in clang, `extern "sysv64"` in Rust).
  * **Thread-local storage.** The game reads per-thread data through the `fs` register. Linux
    lets a program set `fs`; 64-bit Windows doesn't, so on Windows those instructions have to
    be found and rewritten before they run.
* **The call log is the to-do list.** Start-up needs about thirty functions before anything
  else: an allocator, string and memory functions, locks, semaphores, static-init guards,
  direct memory and a clock. Those are the first stand-ins a native loader needs, and the log
  shows the arguments each is called with.

## Files

| Path | |
|---|---|
| `tools/loader.py` | runs the start-up under Unicorn and logs every system call |
| `work/loader/calls.log` | every call, in order, with its caller and arguments |
| `work/loader/summary.md` | where the run stopped, and every import it reached |
