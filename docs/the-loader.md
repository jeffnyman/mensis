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
tools for Windows" component, and the Vulkan SDK (free, from vulkan.lunarg.com; its "Core"
is enough), which the window draws through:

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
whatever the game printed, `files/`, the folders the game may write to, `gpu/`, what the
game asked the GPU to do, `shaders/`, the GPU programs its draws and dispatches used,
disassembled, `draws/`, what the first draws of each pixel shader drew, `frames/` (with `--snapshots`), and `frame.bmp`, the
frame on screen when it stopped. Add `--frames 60` to stop after 60 frames, or `--seconds 20`
to run longer than the default 10. On Windows, `--window` opens a window on the display, whose
title counts the frames as they are shown; the run then goes on until the window is closed.
On Windows, the draws are drawn on the PC's GPU (all of them, so far), and the game runs at
its own 30 frames a second to its title screen, which comes within 30 seconds (`--seconds
30`). `--cpu` draws them on the CPU instead, as the Linux build does, and slower: the title
screen then comes within 70 seconds (`--seconds 75`). `--gpu-check` draws each draw saved to
`draws/` on both, and compares the two (below, and `draws/index.txt`). `--no-draw` leaves the
drawing out, and the game runs at its own 30 frames a second, to a black screen. The two
builds run the same code: up to the point where the game's threads started running, they
made the same calls, from the same places, with the same results.

The game has a controller. In the window, the keyboard is one, and so is an Xbox-style
(XInput) gamepad, if one is plugged in. This release confirms with Circle and goes back with
Cross, so Enter is Circle:

| Keys | Buttons |
|---|---|
| arrow keys | the directional buttons |
| Enter, Space | Circle (confirm) |
| Backspace | Cross (back) |
| Q, E | Square, Triangle |
| Z, X, C, V | L1, R1, L2, R2 |
| B, N | L3, R3 (the sticks pressed) |
| Escape, T | Options, the touch pad |
| W A S D, I J K L | the left stick, the right stick |

`--press` presses buttons for a run, from a given frame, for 6 frames, the same way each
time: `--press 1000:down,1060:circle` picks "Play Offline" from the title menu, and with
`,1400:circle,1500:circle,1650:circle` added (and `--seconds 110`), "New Game" on the main
menu, the message about playing offline, and "New Game" again (the buttons
by name: cross, circle, square, triangle, up, down, left, right, l1, r1, l2, r2, l3, r3,
options, touchpad; `down+circle` presses two at once). The summary lists the presses the
game saw. `--snapshots 50` saves the screen every 50 frames, at half size, to `frames/`, which
shows where a long run was when, and so when to press what. `--save-from 2000` saves draws
to `draws/` (and, with `--gpu-check`, compares them) only from frame 2000 on, and
`--save-ps 2d3355600,...` only the draws of those pixel shaders (eight of each), for draws
well into a run. These presses, with `--seconds
260`, go on through the brightness and control settings to character creation:

```
--press 1000:down,1060:circle,1400:circle,1500:circle,1650:circle,1750:down,1800:circle,1900:down,1915:down,1930:down,1945:down,1960:down,1975:down,2000:circle
```

and `,2100:circle,2150:up,2190:circle,2240:left,2270:circle` added (`--seconds 500`) name
the character (the system's keyboard isn't shown: what the player types is `--name`,
"Hunter" if not given), choose Finish, and answer YES to "Finalize contract?". The game
then saves the character; from then on the main menu starts at "Continue". A run stopped
in the game (as one stopped by `--seconds` is) leaves the game a note that it wasn't left by
Exit Game, which it shows first next time, so after the first such run
`--press 1000:circle,1480:down,1530:circle,1640:circle,1760:circle,1900:circle` answers that,
picks "Play Offline", the two messages about the network, and "Continue", and the game
loads where it was left. (Delete `files/savedata` to start over.)

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
  folders in the run's output (`files/download0/`, ...); and its saves, mounted at
  `/savedata0/` and so on, which are folders too, `files/savedata/<user>/<save's name>/`,
  kept from run to run (delete one to start again without it). Any other path doesn't exist. Three
  families of calls reach them, sharing one table of open files: the kernel's (`sceKernelOpen`,
  `Read`, `Lseek`, `Stat`, `Getdirentries`, ...), POSIX's (`stat`, `mkdir`, with `errno`
  through `__error`), and the C library's streams (`fopen`, `fread`, `fgets`, ...). Each logged
  open and stat shows which file answered. The `printf` family writes to `stdout.txt`; the
  variadic ones read their arguments the way the called function's own `va_start` would, from
  the registers and the caller's stack.
* **The graphics driver** answers the questions a driver answers at set-up: where the
  tessellation ring goes, a compute queue's id, that validation isn't built in (as on a retail
  console). Most of its other calls write the GPU's commands (PM4 packets, AMD's format) into
  a command buffer the game is filling: a shader's registers, the default state, a debugging
  marker. The game reserves the space first and moves on by exactly that much, so each call
  fills it all: the registers from the structure the game hands over, then a `NOP` to the end.
  What the real driver writes besides isn't reproduced; the buffer stays well-formed and the
  same length.
* **The command processor** (`gpu.cpp`) is what the GPU's front end does with what the game
  submits, short of drawing. The graphics queue (`sceGnmSubmitCommandBuffers`) and each compute
  queue (a ring of packets; `sceGnmDingDong` says how far it is written, and the read pointer
  tells the game how far the GPU has got) has a thread of its own that walks the packets in
  order, and hands each draw and dispatch to the CPU's stand-in for the GPU's cores
  (`draw.cpp`, below). What a packet
  writes to memory is written (`WRITE_DATA`, `EVENT_WRITE_EOP`, `RELEASE_MEM`, `DMA_DATA`), and
  a wait on memory (`WAIT_REG_MEM`) waits, because the game and the GPU keep in step through
  memory: a label written when some work is done, which the CPU or another queue waits to
  see. The first six submissions of each queue are written to `gpu/`, decoded.
* **The shaders.** The command processor keeps the shader registers each queue sets, so each
  draw and dispatch names the programs it would run (in `gpu/`, `[vs ..., ps ...]`). At the
  end, each program is written to `shaders/`: its code (`.bin`) and, disassembled by `gcn.cpp`,
  a listing (`.txt`) with its register counts and the user data it started with. The GPU is
  AMD's GCN of the second generation (Sea Islands), and the disassembler follows AMD's
  reference for its instruction set. A program's length comes from the footer the PS4's
  compiler puts after its code ("OrbShdr"). A vertex shader doesn't load its vertices itself:
  it calls a fetch shader (`s_swappc_b64`), whose address is in its user data, and those are
  written out too. LLVM's disassembler (`llvm-mc`) only reads later GCN generations, so the
  listings were checked by hand against the reference, not against another tool.
* **Drawing**, on the CPU. `shade.cpp` runs the shaders: an interpreter for the same
  instruction set, a wave of 64 threads at a time, as the GPU runs them, with each
  instruction's lanes switched on and off by its exec mask; with the buffer, image and sampler
  descriptors the shaders read their memory through (formats, filtering, wrapping), and the
  local memory a compute group shares. `draw.cpp` does what the GPU's fixed-function parts do
  around them: a draw's vertices through the vertex shader; triangles (lists, strips, fans and
  the "rectangles" a full-screen pass uses) through the viewport and the scissor; their
  pixels, in 2×2 quads (a shader compares its neighbours in a quad to pick a texture's level
  of detail), through the pixel shader; and its colour to the render target, in the target's
  format and blended. A dispatch runs each group's waves in turn, from barrier to barrier. The
  pixels are shared out by rows among the CPU's threads (up to 16). Most of the game's
  textures are compressed as BC7 (`bc7.cpp`). Textures in the micro-tiled modes, the ones the
  game's files hold, are read in the PS4's arrangement, as [the textures](the-textures.md) are
  extracted; surfaces in the macro-tiled modes (render targets, the display) are only ever
  written and read by the GPU, and the loader keeps those in rows instead, the console's
  arrangement of them not being reproduced; depth buffers too (32-bit floats, or 16-bit), and
  stencil buffers (8 bits a pixel, beside them). A pixel's depth, its triangle's z
  interpolated and clamped to the viewport's range, is tested against the depth buffer, and
  its stencil value against the draw's reference (front and back faces each their own), as
  Vulkan does it: the pixel shader runs for every pixel covered, colour is written where both
  tests pass, and for the pixels the shader didn't kill the depth is written (where they
  passed) and the stencil operation for how they fared is done. A triangle reaching past the
  guard band (the game sets how far past the viewport that is) or behind the eye is clipped
  first, as the hardware does: cut to the band's planes in clip space, before the divide by
  w, its new corners new vertices, the polygon left drawn as a fan; depth isn't clipped, but
  clamped. A depth buffer is cleared the
  console's fast way: each 8x8 tile marked cleared in its HTILE buffer, the console reading a
  cleared tile as the clear value; when every tile is marked so, the loader fills the depth
  buffer with that value (on the GPU, if it is kept there), once for each time the HTILE
  buffer is written. Not done yet: a depth buffer cleared in part, mip levels past the first,
  multisampling; the summary counts whatever a run
  met that wasn't done.
* **Translating the shaders**, for the PC's GPU (`translate.cpp`). A pixel, vertex or compute shader
  becomes GLSL (`shaders/`, beside its disassembly), which the Vulkan SDK's compiler (shaderc) turns into SPIR-V, the form a GPU takes
  through Vulkan. On the console a wave runs 64 pixels in step; on the PC each pixel runs on
  its own, so each plays one lane of the wave: the vector registers become its variables, and
  the scalar ones too, the same in every pixel. A lane mask (`exec`, `vcc`) is kept as the
  pixel's own bit, all ones or all zeros, so the bitwise instructions that combine masks give
  each pixel its own bit of the result, as they would on the console. What the shader reads
  through its scalar loads (constants, descriptors) is the same for all a draw's pixels; it is
  found before the draw, by following those instructions with the draw's values, and handed to
  the shader in a buffer, with a texture and sampler bound for each texture the image
  instructions read (those that read the same one share it). The interpolation (`v_interp`)
  is done as the console does it, P0 + i (P1 − P0) + j (P2 − P0), from the triangle's three
  corners and the barycentrics the GPU hands the shader (`VK_KHR_fragment_shader_barycentric`),
  perspective or not: an attribute the same at all three corners then comes out exactly that,
  as a shader that turns it into a whole number (an index) needs. On a GPU without that, the
  GPU's own interpolation is used. Branches, forward ones so far: `s_cbranch_execz` skips code
  when no lane is on, which then does nothing a lane can see, so it is simply gone through; so
  is `s_cbranch_scc0` on a lane mask (taken when no pixel is left, after the shader has killed
  some); the others (`s_branch`, `s_cbranch_scc0` and `_scc1` on scalar values, the same in
  every lane) are taken, a variable saying which label the code is skipping to. So are
  `s_cbranch_vccz` and `_vccnz` when vcc comes from comparing values the same in every lane
  that is on (constants, SGPRs, and VGPRs worked out from them since exec last changed): such
  a value is also kept, unmasked, in a shadow variable every invocation holds, so that every
  invocation takes the branch the same way, as the console's wave does. Loops become GLSL
  loops (at most 65,536 times round): one on uniform values goes round the same in every
  invocation; in one each lane leaves on its own (`s_cbranch_execnz` back, or `s_cbranch_scc0`
  out of it on lane masks), each invocation leaves when its own bit is gone, as long as the
  loop changes no SGPR but lane masks. An `s_buffer_load` by an offset in an SGPR reads the
  buffer's dwords as the draw found them (all of it, up to 64 KiB, when the offset isn't
  known before the draw), by the offset the shader works out. A depth-compare sample
  (`image_sample_c_lz`, a shadow map's) reads the 2x2 texels a bilinear read would use
  (`textureGather`), compares each with the reference by the sampler's function, and weighs
  the results as bilinear filtering does (or takes the nearest), as the CPU does it. A vertex shader's call to its
  fetch shader is followed, and the fetch shader's instructions put in its place; their loads,
  each vertex's data by its index, read the buffer's elements as the CPU unpacked them. Its
  position goes through the console's viewport, recast as Vulkan's, and its parameters become
  the pixel shader's attributes, as `SPI_PS_INPUT_CNTL` pairs them. Vulkan has no rectangles,
  so for a rectangle list a geometry shader adds each one's fourth corner. A translation is
  kept, by the program and what it depends on, and so are a buffer's elements once unpacked,
  by its descriptor and a hash of its memory. `vkdraw.cpp` draws with them, recorded into one
  batch of commands, which goes to the GPU, to be waited for, only when something needs what
  it did (a target read back into memory, `--gpu-check`'s copies, the end of one of the game's
  command buffers), into the render targets kept on the GPU (below), up to 8 at
  once, in 8-bit RGBA (linear or sRGB), 16-bit floats (two or four) or 11- and 10-bit floats;
  or into copies (`--gpu-check`, which compares every target, a channel's values as whole
  steps of its own encoding). A draw tries the GPU first, the vertex shader included; only if
  that can't be translated does the CPU run it, for the GPU to be given its vertices. Kept on the GPU between draws: the compiled shaders and pipelines, the
  samplers, the render targets, and the textures, by their descriptor and a hash of the
  memory they read, so a texture that hasn't changed isn't made again. One in 8-bit RGBA or
  in one of those float formats, in rows, goes up as it is in memory, or, if it is a render
  target kept there (the frame's picture, read by the next pass; a G-buffer, read by the
  lighting), is copied on the GPU; any other is decoded by the CPU, as the CPU's interpreter
  reads it (so the shaders, the triangles, the filtering and the blending are the GPU's), an
  integer one as 32-bit integers, read without filtering; one whose format the CPU can't read
  (an unused descriptor's) reads the same everywhere, and is a single texel. A cube is read as
  the CPU reads it: six slices of a 2D array, its faces' coordinates coming as 1 to 2. A compute shader runs a thread to an invocation, in groups as the
  dispatch's, its group number and thread number where the console puts them. Its buffer
  loads and stores by index of memory kept on the GPU read and write that memory's image,
  viewed as uints, an element a texel, unpacked and packed exactly as the CPU's interpreter
  does it: 8-bit RGBA, 32-bit integers or floats (one, two or four), 16-bit floats (two or
  four), 11- and 10-bit floats. A store into memory not kept yet keeps it from then on (whole
  pages only: memory sharing a page with other memory would keep being let go), shaped as
  the image it was copied from, if it was; a load of other memory reads it as it is, if its
  elements are 32-bit values, else as the CPU unpacked it. A pixel shader's buffer loads read the buffer's elements as the CPU unpacked them, as a
  vertex shader's do. Not yet translated: loops (branches backwards), branches on `vcc` or on
  `scc1` from a lane mask, buffer loads other than by index, other stores, LDS instructions
  (but for `ds_swizzle_b32` in a pixel shader, swapping values within each 2x2 quad of
  pixels, which a subgroup's quad operations do), depth export, gathers, depth-compare sampling; a shader with any of them says so, and is
  left to the CPU (for a vertex shader, the GPU is given the CPU's vertices instead). A draw
  with depth has its depth buffer kept on the GPU too, as a depth image (its memory brought
  back and taken up as a render target's is), with its stencil buffer if it has one (one
  image of both, two pieces of memory), tested and written as the CPU does it (a stencil
  operation Vulkan has no match for leaves the draw to the CPU); a pass for depth or stencil
  alone is drawn with no colour target at all, an export to a target that isn't there going
  nowhere. A texture read from kept memory of the same size of texel but laid out another way
  (the depth buffer's depth, read by the lighting, say) is copied on the GPU through a buffer.
* **Render targets kept on the GPU.** A render target stays on the GPU from draw to draw, and
  its memory is guarded by the pages' protection: while the GPU's picture is the newer,
  nothing may touch the memory, and once the two are the same, it may be read. Whatever
  touches it (the window, the CPU's dispatches and draws, the game) faults, and the fault
  handler brings the picture back into the memory first; a write makes the memory the newer,
  and the next draw takes it up again. It is kept to a multiple of 8 rows, as the console lays
  a target out, since a compute shader may write the rows past its height.
* **The display** keeps the timing: one flip at each vertical blank, 60 a second or the rate
  the game asks for, each reported in `sceVideoOutGetFlipStatus` and as an event to the event
  queues that asked for one (`sceKernelWaitEqueue`). What it shows is the frame buffer the last
  flip chose: in the window (`--window`, Windows; a copy 30 times a second, scaled to the
  window on the PC's GPU, through Vulkan, `vulkan.cpp`) and, at the end, in `frame.bmp`. The
  buffers are in one of the GPU's macro-tiled modes, which the loader's drawing keeps in
  rows, so both show them as they are.
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
  state and last call (and, for a thread waiting in a call, what it waits on). Any thread can
  stop the run; so can the main thread's `_start` returning, a time limit (`--seconds`, 10 by
  default), a call limit (`--calls`) and a frame limit (`--frames`, flips shown).
* **The log.** `calls.log` keeps the first 5 million calls whole (`--log` changes how
  many), which covers the start-up and the first frames; after that, each function's first
  100 calls, so what the game starts doing late (its saves, say) is there too, and the last
  10,000. The
  summary still counts every call, and shows each thread's last. Most of a call takes no
  lock: the stand-in runs, and only the log entry, made once it is done, is shared.
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
    value at 0x2_4140_0030 is at least 0x29", a label next to the compute queue's ring. It sits
    where the game had asked `sceGnmDrawInitDefaultHardwareState200` to write the GPU's
    default state, which has no stand-in yet, so the wait is all there is. Reading what the
    game submits, and giving the GPU something to run it on, is the next step.
15. **The driver's calls, as the game makes them.** Each is wrapped in a function of the
    game's that reserves space and then advances (read through Ghidra's decompiler):
    `sceGnmSetVsShader` gets 29 dwords, `SetPsShader` 40, `SetCsShader` 25, and the default
    state needs 256 free and reports what it wrote. With them answered, the first submission
    is two buffers: the wait, then the 256 dwords of default state.
16. **The queues wait on each other.** The compute queue's ring holds indirect buffers of
    dispatches, each piece of work ending in a `RELEASE_MEM` that writes 0, 1, 2, ... to the
    label at 0x2_4140_0030. The graphics queue's first wait, for 0x29 or more, holds it until
    the compute queue's 42nd piece of work is done. Within a graphics buffer the pattern
    repeats: an `EVENT_WRITE_EOP` writes 1 to a label, a `WAIT_REG_MEM` waits for it, caches
    are flushed (`ACQUIRE_MEM`), and the next dispatch goes. The first draw is in the third
    submission: `DRAW_INDEX_AUTO` of 3 vertices, most likely one triangle over the whole
    screen, the usual way to run a pixel shader once per pixel.
17. **Two flips, then stuck.** The main thread signalled five semaphores that no thread was
    waiting on, then waited itself, for ever. Their five threads had never started: they were
    created with
    `pthread_create_name_np`, FreeBSD's way of creating a named thread, which had no stand-in.
    They are `HavokWorkerThread`s, the physics library's workers. With them running, the game
    goes on.
18. **Spinning on things that never come.** Two threads polled without a pause:
    `DLUserPreferenceNotifyService` called `sceUserServiceGetEvent` 8 million times, because
    the call answered 0, success, rather than "no event"; and the socket thread, one of four
    `NexusRevolution` threads that handle the game's networking, called `sceNetEpollWait` 5.6
    million times, because it returned at once rather than waiting out its 33 ms timeout for
    network events. With both answering as a console with no user events and no network would, the
    game starts 48 threads.
19. **The game runs frame after frame.** In 20 seconds (Windows) it shows 335 frames: 2,044
    graphics submissions with 1,697 draws and 2,356 dispatches, 29.6 million calls. After
    about 14 million calls it hides the system's splash screen
    (`sceSystemServiceHideSplashScreen`), so what it draws now is its own. It asks for 60
    flips a second and gets about 20 here: the loader logs every call, and the log alone
    (`calls.log`) is 2.5 GB after 20 seconds. Linux runs the same way, more slowly, reading
    the game's files across WSL's file system.
20. **At the game's own pace.** With the log kept short, a call costs little more than the
    stand-in itself: the same 20 seconds make 260 million calls rather than 30 million, and
    show 534 frames. Between the tenth second and the twentieth the game shows 301, 30 a
    second, the rate Bloodborne runs at on a PS4, so the flips are now paced by the game rather
    than by the loader. The time between frames goes where it goes on the console: the main
    thread reads the clock in a loop (`gettimeofday`, 238 million of the calls) until its next
    frame is due. Linux shows 504 frames in the same time.
21. **The screen is black.** The game's three frame buffers are 1920×1080, 32 bits a pixel,
    tiled, 8,355,840 bytes apart (1,088 rows: the height rounded up to the GPU's tiles). The one
    on screen after 120 frames has 0 of its 2,073,600 pixels set. Nothing else would be
    expected: a GPU writes pixels by running shaders, the game's own programs for the GPU's
    cores, and here none run. The window, its title counting 30 frames a second over black, is
    where they will show. Running the shaders, translated from the GPU's instruction set (AMD's
    GCN) to something a PC's GPU runs, is the next part, and the largest.
22. **The game's shaders.** In 20 seconds the draws and dispatches use 36 programs: 11 vertex
    shaders, 9 pixel shaders, 6 compute shaders and 10 fetch shaders, 806 instructions in all,
    every one decoded. They are short, from 6 instructions to 76, and each but the fetch
    shaders starts the same way, with `s_mov_b32 vcc_hi, ...`, a mark of the PS4's compiler.
    The first draw (`DRAW_INDEX_AUTO`, 3 vertices) is a triangle that covers the screen: its
    vertex shader, 13 instructions, makes the corners from the vertex's number alone, with no
    vertex buffer. Its pixel shader reads the source image at the pixel and puts each of red,
    green and blue through a curve of its own, a layer of a 3-layer texture: a colour
    correction. The compute shader used most, 19 instructions, copies a buffer into an image,
    one element a thread, 64 threads a group. Most of the other vertex shaders call a fetch
    shader for two or three attributes and multiply the position by a 2×4 matrix from their
    constants: flat shapes, as a splash screen and its text would be. Linux, slower to reach
    the later screens, used 27 of them in the same time, with the same listings.
23. **The first pictures.** Run, the shaders drew the splash screen ("Sony Computer
    Entertainment presents", fading in, `draws/`), but the screen stayed black: the last pass
    of each frame, a triangle over the whole screen, copies that picture to the display
    through a colour correction, each of red, green and blue through a curve kept in a
    texture, and the curves were 0. Compute shaders fill them, from a buffer; they ran, and
    skipped every thread, because the count they compare with came from the wrong address. A
    buffer's descriptor holds its address in 44 bits, not 48: the 4 bits above are set
    (`0xb`) in every one of the game's, and read as part of the address they pointed at
    nothing. With 44 bits, the splash screen shows.
24. **BC7, checked.** Most of the game's textures are BC7, a block compression with eight
    modes and tables of how a block's 16 pixels divide among up to three colour ranges. The
    decoder was checked against Pillow's, which `tools/textures.py` uses: 69,615 blocks
    sampled from 170 textures, all modes 4, 5 and 6, came out the same, pixel for pixel. Of
    the game's 65,462 BC7 textures, only 763 blocks use another mode (0, 1 and 3, the ones
    with partition tables); those came out the same too. Modes 2 and 7 aren't used at all.
    The one difference is a block that isn't valid (no mode bit), all 0 in the game's padding:
    Pillow makes it opaque black, Direct3D's specification transparent black, and the loader
    follows the specification.
25. **The title screen.** In 90 seconds the game shows 534 frames, past the splash screen to
    its title: the Hunter, the logo, and "Play Online" and "Play Offline", the game's own
    menu, drawn by its own shaders (3,792 draws, 4 billion pixels, 3,825 dispatches). It
    runs at about 6 frames a second (5.8 in the window, at frame 596): the game waits on its
    GPU, which is now the CPU, and the time it took is the time the game sees. Nothing it
    used in that time was counted as not done. Linux draws the same splash screen (211
    frames in 30 seconds).
26. **Faster.** The summary now says how long the drawing took: of 31 seconds, the draws
    took 26.5 and the dispatches 1.8, so the pixels were what to speed up. Counting the
    processor's cycles by kind of instruction pointed at three things: reading textures (37%),
    writing pixels to the target (30%) and arithmetic (26%). Each now works out once what it
    can (a texture's or a target's layout, once an instruction or a draw), has a short way
    through for the usual formats (8-bit RGBA, 32-bit floats), decodes a compressed block once
    rather than for each pixel that reads it, and does the usual arithmetic in straight loops
    over the 64 lanes. The conversions to and from 16-bit floats, in nearly every pixel's
    export, are now done with bits; checked against the old ones over every 16-bit value and
    20 million others, they differ only where the old ones were wrong: infinity rounded toward
    zero, which stays infinity, and the values just below the smallest 16-bit number, which
    round to it rather than to 0. In 30 seconds the game now shows 287 frames rather than
    195, nearly 10 a second, and reaches its title screen within 70 seconds (668 frames);
    Linux shows 301 in 30 seconds rather than 211. All 16 of the CPU's threads (8 cores) is
    best: 12 gave 255 frames, 8 gave 185. What is left is the interpreting itself, each
    instruction taken apart again for each wave; much faster would mean translating the
    shaders, to the CPU's own instructions or to a PC GPU's.
27. **Vulkan.** The way chosen is the PC's GPU, through Vulkan, which is how PS4 emulators
    run their games: the game's shaders translated into programs for it, which would run
    thousands of pixels at once. The first step is the window: it is now drawn through
    Vulkan rather than GDI, on the GPU that can present to it (a discrete one first), the
    display's buffer copied up to the GPU and scaled into the window there. The drawing is
    still the CPU's. The validation layer, which comes with the Vulkan SDK and reports any
    wrong use of Vulkan, is on whenever it is installed, and the summary says what it found.
    In a 75-second run on an RTX 5090, to the title screen (760 frames), and in one with the
    window resized, minimised, restored and maximised, it found nothing. If Vulkan can't be
    used, the window falls back to GDI and the summary says why.
28. **The pixel shaders on the GPU.** Each of the 8 pixel shaders the game uses up to its
    title screen is translated into GLSL (`translate.cpp`) and drawn on the GPU
    (`--gpu-check`), and the result compared with the CPU's, pixel by pixel; with how many
    pixels the draw changed, so that "the same" can't only mean "left as they were". In a
    75-second run, 16 draws: 14 come out the same in every pixel (to 1 in 255), among them the
    colour correction that makes each frame's picture for the display, the splash screen's
    text and the title screen's backgrounds. The other 2, the menu's text, differ in 5 and 6
    of their 3,600 and 3,000 pixels, by 3 in 255 at most, at the faint edges of letters: the
    likeliest cause is the GPU's bilinear filtering, which weighs the four texels it blends
    with less precision than the CPU's floats. The validation layer reported nothing. The
    vertices are still the CPU's, and the game still sees only the CPU's drawing.
29. **The vertex shaders too.** The 8 vertex shaders those draws use, each with its fetch
    shader put in its place, are translated as well, and the GPU now runs both shaders: the
    triangles are its own, culled and clipped by it, with a geometry shader for the
    rectangles. The same 16 draws come out as before, 14 the same in every pixel and the
    menu's text with the same few pixels apart, which says the GPU's triangles cover the very
    pixels the CPU's do. The translations are written to `shaders/` as GLSL, next to the
    listings they came from.
30. **Drawn on the GPU.** With `--gpu` (an option at first), the GPU's picture is the one
    the game gets: each draw is drawn there, its render target sent up from memory and read
    back into it, so
    the display, the window and `frame.bmp` show the GPU's drawing, and the CPU's stays as
    the reference (`--gpu-check`). The compute dispatches are still the CPU's. The first try
    was no faster, 289 frames in 30 seconds, 21 ms a draw: the readback came through the
    GPU's own memory, which the CPU can see but reads very slowly. Read back through the
    CPU's cached memory instead, a draw takes about 2 ms, uploads and readbacks included, and
    in 30 seconds the game shows 808 frames rather than 287, all its draws (5,887) on the GPU,
    to its title screen. In the window, the title screen runs at 30.1 frames a second, the
    game's own pace. Nothing is left to the CPU so far, and the validation layer reported
    nothing. Watched in the window, it looked as it should, and plainly faster, and it is now
    the default on Windows; `--cpu` asks for the CPU's drawing instead.
31. **Render targets kept on the GPU.** Each draw still sent its render target up (8 MB for
    the 1920x1080 picture) and read it back, though the next draw was very often into the
    same one. A trace of the packets showed why the simple fix, reading back at the fences
    the CPU waits on, wouldn't do: the game writes one after almost every draw. Instead the
    targets stay on the GPU, their memory guarded (above), and come back only when
    something touches them. That showed two touches a frame: a compute shader, run on the
    CPU, writes over the main picture once a frame (a clear, it seems), so it is read back
    and taken up again once a frame rather than 7 times; and the next pass reads the
    picture as a texture, which is now copied on the GPU instead. In 30 seconds the GPU's
    draws (5,944) took 4.0 s rather than 11.2, and textures uploaded fell from 812 to 45;
    the title screen is the same, the window too, and the validation layer reported nothing.
    `--gpu-check` gives the same as before (16 draws compared, 14 the same in every pixel).
32. **Compute shaders on the GPU.** What was left was the once-a-frame clear: a compute
    shader writing one colour over the main picture, through buffer stores, some 16,000
    groups a frame, nearly all the CPU's compute work, and the reason the picture came back
    into memory and went up again each frame. Compute shaders are now translated too, and a
    dispatch runs on the GPU when its stores all go into render targets kept there, written
    straight into the image. The clear writes 1,088 rows of a 1,080-row target (the console
    lays targets out in 8-row tiles), so the kept image now has those rows too. In 30
    seconds: the 758 clears on the GPU took 0.3 s, the CPU's dispatches 1.6 s rather than 6.5,
    and the main picture went up from memory 4 times and came back 17, rather than some 770
    times each; the GPU's draws took 2.1 s. The title screen is the same, in the window too, `--gpu-check` gives the
    same as before, and the validation layer reported nothing. The other dispatches stay on the CPU:
    those with branches, and those that store elsewhere, or in other formats.
33. **A controller.** The game sat on its title menu: it opened the controller of a user
    it had never been told of (`sceUserServiceGetInitialUser` had no stand-in, so the id was
    whatever was on its stack), and nothing answered `scePadReadState`. Now there is one user,
    logged in from the start, and a DualShock 4 for them (`pad.cpp`), which the game reads
    once a frame: from the window's keyboard, an XInput gamepad, or `--press`. Pressing Cross
    on "Play Offline" did nothing; this release confirms with Circle. With `--press
    1000:down,1060:circle` the game goes past its title screen for the first time: it checks
    for save data, and says "Failed to create save data. Press OK to try again.", since there
    are no stand-ins for `sceSaveData*` yet.
34. **Save data.** The game's saves are now folders (`savedata.cpp`), which it mounts by name
    and then reads and writes with its ordinary file calls, at `/savedata0/`. Its first look
    finds no save "SPRJ0005", so it creates one, 1,136 blocks of 32 KiB, gives it a title
    ("Bloodborne SaveData") and an icon, and writes it: `userdata0000` to `userdata0009`,
    1.25 MiB each (a slot for each character, it seems), and `userdata0010`, 256 KiB, which
    it then copies to `backup0010`. Then comes its main menu: New Game, System, Log In. The
    next run finds the save and goes straight there. "New Game" says the game starts in
    offline mode; past that, "New Game" again leads to the first-time brightness screen.
    `calls.log` showed none of this at first, since it all happens after its first 5 million
    calls; it now keeps each function's first 100 calls as well.
35. **Character creation.** With `--snapshots` to see where a run was when, the presses went
    on: past the brightness screen and a screen of control settings (camera axes, its
    sensitivity, auto target switching, vibration), each with its own "Next", to character
    creation, "Contract": a name, gender, age, origin, appearance, and the character drawn
    in 3D. It is the first of the game's 3D drawing, and it shows what is missing. The game
    slows to a crawl, some 2,060 frames in 260 seconds, since its 3D passes are drawn on the
    CPU: 3,353 draws to more than one render target at once, 380 to targets of 16-bit floats,
    429 million pixels in all. And the panel of starting stats is a heap of icons where its
    numbers should be.
36. **The 3D passes on the GPU.** The character's passes write a G-buffer, 896x832, up to
    seven targets at once, in 8-bit RGBA (some sRGB), 16-bit floats and 11/10-bit floats; its
    lighting reads them back as textures. A draw now writes up to 8 targets on the GPU, in
    those formats, each kept there, and the textures in them are copied there. Then, one at a
    time, what the run showed it needed: the forward branches above, the clamped and legacy
    reciprocals (`v_rcp_*`, `v_rsq_*`), the cube map instructions (`v_cubeid`, `v_cubesc`,
    `v_cubetc`, `v_cubema`) and cube textures, integer textures, pixel shaders' buffer loads,
    up to 31 textures (one binding for each, not for each instruction), passes for depth alone
    left out, and the vertex shader no longer run on the CPU for a draw the GPU does whole.
    One cost was hidden: unused texture descriptors claim 8193x512 texels of a format that
    doesn't exist, and each was decoded, all four million of them, on every draw; it is one
    texel now. With all of it, every draw of character creation is drawn on the GPU (the
    CPU's pixels: none), and it runs at 2.5 frames a second rather than well under one; what
    remains is mostly each draw waiting for the GPU before the next (some 430 a frame). The
    check (`--gpu-check`, through character creation) compares the G-buffer's every target:
    of 56 draws, 32 the same as the CPU's in every pixel, most of the rest in a few pixels
    out of thousands (a triangle's edge falling the other way). Three shaders differ in more:
    they kill pixels whose texture alpha is under a threshold (hair), and the GPU's texture
    coordinates, a little different from the CPU's, tip many such pixels the other way.
37. **Batches.** Each draw on the GPU was sent on its own and waited for, some 430 times a
    frame in character creation. Now draws and dispatches are recorded into one command
    buffer, which goes to the GPU only when something needs what it did: a render target read
    back into memory (when something touches it), `--gpu-check`'s copies, the end of a
    command buffer of the game's, or a batch that is full. Until then each draw's uploads
    stay where they are, and each has a descriptor set of its own; images that go wait for
    the batch to be done. The time the GPU's draws took halved, but character creation ran
    no faster: most of the time went before the GPU. A profile showed where: each draw
    unpacked its vertex buffers again (15 s in a run) and made its shaders' GLSL again (11 s).
    Both are kept now, the GLSL by the program and everything it depends on, the buffers by
    their descriptor and a hash of their memory. Character creation went from 2.5 frames a
    second to 4.2. Most of what remains is the compute shaders the CPU still runs (64 million
    groups in a run): they store into memory in ways the GPU doesn't yet. The character's face
    is a speckle of noise, with or without batches; that is most likely the depth test still
    missing, the hair's layers and the face drawn over each other in the order they come.
38. **Compute shaders on the GPU, reading and writing memory.** The compute shaders the CPU
    still ran, 64 million groups in a run of character creation: the clear, writing the
    G-buffer's float targets too; a copy of the G-buffer's first target to memory elsewhere,
    25 million groups alone; a gather, a fill. Their stores now go on the GPU in all those
    formats, and a store into memory that isn't kept keeps it (shaped as the copy's source,
    so that a draw or a texture can use it as it is); their loads of kept memory read its
    image. Kept memory is now matched by its address, size and bytes a texel, not its format
    (an image is viewed in whatever format a draw wants), and memory kept in one shape (a
    clear sees a buffer) and wanted in another (a draw sees 896x832) is reshaped on the GPU,
    not let go: the first try let 694 go in a run, each read back and taken up again. A
    profile then showed the time going to unpacking the loads' buffers, mostly for dispatches
    that went to the CPU in the end: whether a dispatch can go to the GPU is now known before
    any of that, and a load of plain 32-bit values takes the memory as it is, the shader
    picking its elements out. The CPU's compute work fell from 64 million groups to 4 million,
    and character creation went from 4.2 frames a second to 6.7. Still the CPU's: shaders that
    store into images (`image_store`), and stores into memory sharing its pages.
39. **Depth.** The character's face was a speckle of noise, the hair's layers and the face
    drawn over each other in the order they came. Depth is now tested and written, on the
    CPU and the GPU alike: 32-bit float depth for the scene, 16-bit for what look like shadow
    maps, drawn a slice of an array at a time. Its first run drew no character at all: every
    pixel failed the test. The game never writes its depth buffer's clear: it uses the
    console's fast clear, marking each 8x8 tile cleared in a small HTILE buffer (its words
    `0xfffffff0`: the low four bits 0), and the console reads a cleared tile as the clear
    value, 1.0 here. The loader does that clear when it sees every tile so marked. Then the
    face came right, eyes and hair and all. The check (`--gpu-check`, through character
    creation) has 33 draws of 56 the same as the CPU's in every pixel, as before. Stencil is
    next: the game's lighting passes test it, and draw everywhere without it.
40. **Stencil.** Each depth buffer has an 8-bit stencil buffer beside it, which the game
    marks as it draws the character and tests in later passes. It is now tested and its
    operations done (keep, zero, replace, increment and decrement, invert, and the others),
    on the CPU and on the GPU, where the depth and stencil buffers are one image of both,
    kept as two pieces of memory. With it the panel of starting stats came right: the heap of
    icons where its numbers should be was the panel's art drawn everywhere a stencil test
    would have kept it out of; now it reads Level 10, Blood Echoes 300, Vitality 11, and so
    on. Then the cost: the fast clear filled each depth buffer from the CPU, each frame,
    bringing it back from the GPU and taking it up again; it is cleared on the GPU now, and
    the HTILE buffer is no longer written, only looked at when it changes. A texture reading
    the depth buffer (the lighting does) or other memory kept in another layout is copied on
    the GPU through a buffer rather than brought back. Character creation runs at about 6.2
    frames a second. The check (`--gpu-check`, through character creation) has 37 draws
    of 60 the same as the CPU's in every pixel (more draws compared than before: passes for
    stencil alone are drawn now), the lighting's stencil-tested passes among them, drawn
    into some 8,000 pixels each rather than all 614,656.
41. **Into the game.** Naming the character opens the system's on-screen keyboard
    (libSceImeDialog, titled "Please Enter Name", 16 characters at most); the loader has no
    keyboard to show, so the dialog finishes at once, the name (`--name`) written into the
    game's buffer. Choosing YES to "Finalize contract?" then crashed: the game calls
    `setjmp` and `longjmp` (the error handling of its scripts, likely), which work on the
    caller's own registers and stack, so they can't go through the loader's usual way into
    a stand-in; their stubs now jump straight to small pieces of assembly that save and
    restore those registers. The character is saved, and the game goes on to load its first
    real scene, showing an item's description while it loads. That scene is the first to
    use tessellation and geometry shaders: the driver calls that set those stages
    (`sceGnmSetLsShader`, `HsShader`, `EsShader`, `GsShader`) had no stand-ins, so the space
    the game kept for them in its command buffers held leftovers, which the GPU read as
    commands until it waited forever on one. They now write the stages' programs and fill
    the rest; draws that use those stages aren't drawn yet. The scene loads, slowly (the
    draws took 436 s of a 500-second run), and the summary lists what it needs: triangles
    with a vertex behind the eye (35 million skipped), LDS in vertex and pixel shaders,
    depth-compare sampling, BC6 textures, mip levels, and tessellation.
42. **Clipping.** A triangle with a corner behind the eye was skipped, which is fine for a
    menu but not for a world seen from inside it: the first scene skipped 35 million of
    them. The CPU now clips each triangle as the
    GPU does, in clip space before the divide by w, to the guard band's planes (where the
    game's registers put them; their planes leave out what is behind the eye too); the
    corners where its edges cross a plane are new vertices, their parameters worked out in
    proportion, and the polygon left is drawn as a fan of triangles. Depth isn't clipped:
    as on the GPU here, it is clamped. A draw done on the GPU, vertices and all, is clipped
    by Vulkan, as before; one given the CPU's vertices gets the clipped triangles. Loading
    the first scene, 203,531 triangles were clipped and none skipped. The scene is still
    slow to draw (the draws took 437 s of a 500-second run, still behind the loading
    screen); the 6,120 draws whose shaders use LDS, drawn on the CPU, were the most of any
    one kind.
43. **LDS, and the shadow maps.** The LDS instructions in those pixel shaders were all
    `ds_swizzle_b32` in its quad mode: each pixel taking a value from one of the 2x2 quad of
    pixels it is shaded with, here to work out how the depth changes from pixel to pixel.
    The shaders are the shadow casters, writing depth and depth squared into small variance
    shadow maps. The translation does it with GLSL's subgroup quad operations, whose quads
    are the same four pixels in the same order; the CPU's interpreter didn't do it at all
    (it left the register as it was), and now does, both its modes. Comparing those draws
    (`--save-ps`, new, and `--save-from`) showed they drew nothing at all, on the GPU or
    the CPU: the shadow maps' depth was 0, every caster failing its test. The game clears
    it with a draw that has no pixel shader, which writes depth all the same, and the
    loader had dropped every such draw; they are drawn now, depth and stencil alone, on the
    CPU and the GPU (an empty fragment shader). A clear by a draw (DB_RENDER_CONTROL's
    depth or stencil clear) is done too, over the part of the buffer its scissors leave,
    on the GPU if the buffer is kept there. With all that, of 80 draws of the 23 shaders
    with `ds_swizzle_b32`, 72 are the same on the GPU as on the CPU in every pixel, the
    rest but for 1 to 7 pixels on triangles' edges. The LDS draws are off the CPU (the
    triangles it draws went from 16.5 million to about 1 million), but the draws still
    take about 436 s of a 500-second run: the CPU still fills some 570 million pixels, for
    the draws left to it, those with branches, loops, depth export and depth-compare
    sampling.
44. **Branches on vcc.** The branches that stopped a translation were nearly all
    `s_cbranch_vccz`, and every one of them tested a value the same in every pixel: a
    constant from the draw's buffer compared with 0 (is this light on, say), through a VGPR
    just set to 0. In a wave that is the same branch for all its lanes; in an invocation
    per lane, as here, each lane holds only its own bit of vcc, and a lane that is off
    holds 0 and would go the other way. So the translation now follows which values are
    the same in every lane that is on: constants, SGPRs that aren't lane masks, and VGPRs
    a vector op wrote from those since exec last changed (lanes that come on then hold
    other values). Such an op also writes its result, unmasked, to a shadow variable,
    which every invocation holds whether its lane is on or not, and a comparison of them
    into vcc writes `vccu`, which the branch then goes on. At a label, what is the same
    in every lane is what was on every way there. A branch on vcc that differs from lane
    to lane still isn't translated, and says so. Loading the first scene, no draw is left to
    the CPU for its branches now (there were 280 to 1,037 a run). Of 32 draws of those
    shaders compared, all 32 are the same on the GPU as on the CPU, but none of them
    changed a pixel there (lights that add nothing, it seems), so the check shows the
    branches taken alike, not yet the lit side of them. The CPU still fills some 550
    million pixels, for the draws with loops, depth export and depth-compare sampling,
    and for vertex shaders with more than 31 buffers.
45. **Loops.** Three pixel shaders in the first scene have loops (260 draws). Two go round
    on scalar values, the same in every lane: one over the scene's lights, a counter in an
    SGPR, reading each light's four dwords with `s_buffer_load` at an offset worked out
    from it; the third goes round a different number of times in each lane, each leaving
    on its own (`s_cbranch_scc0` on what is left of a lane mask). A loop is now a GLSL
    loop, at most 65,536 times round so that a mistake can't hang the GPU: a branch back
    says to go round again, anything else leaves. In a loop each lane leaves on its own,
    each invocation leaves when its own bit is gone, as a lane whose bit is gone does
    nothing more in it on the console; that holds only if the loop changes nothing all
    the lanes see, so one that changes an SGPR other than a lane mask isn't translated.
    The light loop's load can't be found before the draw, the offset changing each time
    round, so a scalar buffer load by an offset in an SGPR now reads the buffer's dwords
    as the draw found them (all of it, up to 64 KiB, if the offset isn't known), by the
    offset the shader works out, 0 past the end. No draw is left to the CPU for loops now.
    Of 16 draws of the two scalar loops compared, all are the same on the GPU as on the
    CPU in every pixel, the lights' over all 518,400 pixels of their target and the
    other's over some 2 million; the third shader now stops at sampling with given
    derivatives, so its loop isn't drawn on the GPU yet. Dispatches with loops stop at
    other things now (scalar loads from addresses they work out, mostly).
46. **Shadows: depth-compare sampling.** The shaders that read the shadow maps sample them
    with `image_sample_c_lz`: each texel's depth compared with the pixel's (the reference,
    in the address registers after any bias), by the sampler's compare function, 1 where
    it passes and 0 where not, then filtered. The CPU's interpreter had sampled the depth
    and left the compare out, so every shadow read came back as a depth, not as lit or
    not. Both now do it the same way: the 2x2 texels a bilinear read would use, each
    compared, then weighed as bilinear filtering weighs them (percentage-closer filtering)
    or, if the sampler doesn't filter, the nearest alone; the reference clamped to 0-1 for
    a unorm texture. On the GPU the texels come from `textureGather`, which needs no
    depth format or compare sampler. Of 24 draws of those shaders compared, all are the
    same on the GPU as on the CPU, the main lighting pass's over all 2 million pixels of
    its target. The pixels the CPU fills, loading the first scene, went from about 450
    million to about 120 million; the draws still take about 430 s of a 500-second run,
    so the time is now going somewhere other than pixels.
47. **Where the time goes.** The summary now says where the draws' time went, stage by
    stage (a draw's time goes to one stage at a time), and some stages' parts: shaders
    translated and evaluated, buffers unpacked, textures' memory hashed, textures decoded
    texel by texel; and, on the GPU, the time spent waiting for batches, reading targets
    back, and copying a draw's data to upload. Into the first scene (a 300-second run, the
    draws 229 s of it), the pixels the CPU fills take 4 s, and the vertices it runs 1 s.
    Getting draws ready for the GPU takes 127 s, 102 of them decoding 9,852 textures texel
    by texel for the GPU (those it can't take as they are in memory) and 21 unpacking
    buffers; the GPU's draws themselves take 91 s, 26 of them waiting for 13,642 batches,
    7 reading targets back, 6 copying uploads: 271 GiB of textures, as the textures the
    GPU takes as they are in memory aren't kept from draw to draw but uploaded each time.
48. **Textures kept.** A texture the GPU takes as it is in memory (8-bit RGBA in rows) is
    now kept there from draw to draw too, under its T# and a hash of its memory, as the
    decoded ones were; not one in memory the GPU keeps itself (a target read as a texture),
    which it copies there, and which hashing on the CPU would only bring back. And the
    textures kept there may now take a quarter of the GPU's memory (at least 2 GiB), not
    2 GiB: decoded, a texture takes 16 bytes a texel, and those let go to stay within 2 GiB
    were decoded again when next used. Into the first scene, in 300 seconds, the loader
    now gets through 85,603 draws, 77,539 of them on the GPU (about 47,000 to 49,000 on the
    GPU before), the draws' time about the same: some 2.9 ms a GPU draw, down from 4.6.
    Uploads went from 271 GiB of textures to 57 GiB, and none were let go. What is left
    is decoding textures (49 s), unpacking buffers (58 s), hashing textures' memory
    (15 s), and the GPU's draws (88 s, 20 of them waiting).
49. **Buffers kept longer.** A buffer's elements, unpacked for the GPU, were kept by the
    descriptor, offset, format and a hash of the memory, up to 512 MiB, and then all let
    go at once: into the first scene that happened 89 times, and 111,824 buffers were
    unpacked (46 s), most of them again. They may now take 2 GiB, and past that the least
    lately used go, down to 1 GiB. The summary says how many were unpacked and found kept,
    and how long hashing and unpacking took. In the same 300 seconds, 61,907 were unpacked
    (4 s), 558,036 found kept, and only twice did any go; the buffers took 31 s, not 64,
    and the loader got through 98,907 draws, not 86,204. The rest of the buffers' time is
    hashing their memory (14 s) and copying the kept ones out for each draw.
50. **Descriptors that aren't images.** The summary now lists the textures decoded texel
    by texel by format, tiling and type, with how many times and how long. Into the first
    scene, 46 of their 68 seconds went to one kind, decoded 9,244 times: a T# of type 0,
    which isn't an image's type (8 to 15), its other fields as they happened to be
    (format 14, a size), and its memory not all in the game's; not all in memory, it
    wasn't kept from draw to draw, and was decoded again each time, mostly from memory
    that isn't there. The hardware reads such a descriptor as 0, and now so does the
    loader: on the CPU every read of one gives 0 (a store to it goes nowhere), and on the
    GPU it is one texel of 0, kept there. In the same 300 seconds the loader now gets
    through 144,880 draws, not 98,156; 1,157 textures are decoded (20 s), mostly
    block-compressed ones micro-tiled (tile mode 13) and a cube of 16-bit floats. The
    draws' time is now mostly hashing memory (buffers 30 s, textures 19 s), copying kept
    buffers out for each draw, and the GPU's draws (94 s).
51. **Memory hashed again only once written.** A kept texture or buffer was found again by
    a hash of its memory, hashed for each draw: 49 of the draws' seconds into the first
    scene. On Windows the game's heap is now reserved with the OS noting which pages are
    written (`MEM_WRITE_WATCH`), and before a range is hashed the loader asks which of its
    pages were written since it last asked (`GetWriteWatch`), giving those a new number;
    while the highest number of a range's pages is what it was when the range was last
    hashed, the hash is as it was, and the memory isn't read. (Keyed by those numbers
    alone, memory the game wrote again with what it held already was taken as changed,
    and a cube texture of 16-bit floats was decoded twice as often; hashed when written,
    it still matches.) In the same 300 seconds, memory was told unchanged without reading
    it 1,560,338 times and hashed 46,559 times; telling took 4.5 s, not 49, and the loader
    got through 198,237 draws, not 144,880. Linux still hashes every time. The summary
    counts both. The draws' time is now mostly the GPU's draws (109 s), copying kept
    buffers out for each draw (most of the 48 s of "unpacking buffers"), and decoding
    textures (23 s).
52. **Kept buffers shared, not copied.** A buffer found kept was copied whole into each
    draw that used it, and then copied again into the GPU's upload buffer. Now the draw
    shares the kept one (it is never changed once unpacked; a buffer let go stays for as
    long as a draw still holds it), and only the copy to the GPU is left. In the same 300
    seconds, "unpacking buffers" took 8.5 s, not 48, and the loader got through 308,050
    draws, not 198,237. The draws' time is now mostly the GPU's draws (125 s: of it 35 s
    copying draws' data to upload, 33 s reading targets back into memory when touched,
    34 s submitting batches and waiting for them), setting up (31 s) and decoding
    textures (30 s).
53. **Compressed textures sent compressed.** A block-compressed texture was decoded on the
    CPU, texel by texel, into four 32-bit floats a texel (16 bytes, where BC7 has 1 and
    BC1 half of one), and those were uploaded: most of 17.5 GB of textures into the first
    scene. Now a 2D texture in BC1 to BC5 or BC7 (UNORM, or SRGB where there is one: the
    ones the CPU decodes) is sent as its blocks, their micro tiles undone (each 8- or
    16-byte block copied to its place in rows), in Vulkan's format for it
    (`textureCompressionBC`), and the GPU decodes it; kept there like the rest, under the
    T# and a hash of its memory. Our BC7 decoder (`bc7.cpp`), which the GPU's now stands
    beside, gave the same bytes as Pillow's for 4,096 random blocks, 512 in each mode.
    Into the first scene, 803 textures went as their blocks (0.1 s), 928 were still
    decoded (16 s, nearly all one cube of 16-bit floats, decoded 614 times), and 13.5 GB
    of textures were uploaded, not 17.5. With `--gpu-check` into the first scene 74 or 75
    of 78 draws are the same as the CPU's, as before: one small draw (pixel shader
    0x313a0b400, of a 16x16 BC7 texture) differed in one run and not in the next, and
    differs from run to run in what it draws. The draws got no further in 300 seconds
    (303,312): preparing for the GPU took 35 s, not 48, but the GPU's stage took 136 s,
    not 125, most of it still copying uploads (39 s), reading targets back (34 s) and
    waiting for batches (36 s).
54. **What reads kept memory back.** The summary now counts why each batch went to the GPU
    and why each target was read back into memory, by what touched it: the loader says
    what it is doing on each thread (the stage of a draw, hashing, unpacking, a dispatch,
    the command processor, a call of the game's; the worker threads take on what gave them
    their work), and a touch is counted under that. Into the first scene, of 26,000
    batches 17,000 went to read a target back, and every one of the 10,000 read-backs was
    the loader's own doing, not the game's: dispatches run on the CPU, a draw's setting up
    (an index buffer a compute shader wrote), pixels drawn on the CPU, hashing, and a
    compute shader's array of kept memory, copied here. That last is now copied on the GPU
    instead, from the images the memory is kept in (a depth buffer's depth and stencil
    too), into the dispatch's upload, each part of the array from its own image and the
    rest from memory. But the same memory is then read by dispatches the CPU runs, and the
    read-backs only moved there: 10,089 in 300 seconds, as before, and as many draws.
    Most dispatches not on the GPU store into memory not kept there in whole pages (9,064
    in 200 seconds) or have no buffer stores (7,750).
55. **Compute shaders' image stores on the GPU.** Most of the dispatches with no buffer
    stores store into an image instead (`image_store`): one clears a 960x540 image of
    16-bit floats, 8,160 groups of 64. A 2D image in rows (not micro-tiled) is memory too,
    a texel an element and `pitch` of them to a row, so an image store is now taken as a
    store into that memory by index (y times the pitch, plus x, inside the image's width
    and height), and goes where buffer stores go: into the memory kept on the GPU. Into
    the first scene, 18,299 dispatches went to the GPU in 300 seconds; validation is
    clean, and `--gpu-check` is as before. But again the read-backs only moved (10,619,
    the most of them by dispatches still on the CPU), and the draws got no further
    (340,028, within a run's spread). Keeping the whole pages a store is in, when it isn't
    whole pages, was tried too, and undone: a copy shader then kept the vertex data it
    writes on the GPU, every draw unpacking those vertices on the CPU read it back, and
    the loader got through a third of the draws. The work to do is in what reads the
    GPU's memory on the CPU (draws' vertex and index buffers, dispatches the CPU runs),
    not in sending more to the GPU.
56. **Depth buffers' fast clears seen to on the GPU.** Labelling the parts of a draw's
    setting up showed what read back the most: not the index or vertex buffers, but the
    HTILE buffer of a depth buffer, which a compute shader marks cleared each frame on the
    GPU, and which the loader read on the CPU to see whether every tile was cleared (one
    48 KiB buffer, 2,516 times in 300 seconds, plus two smaller ones). When the HTILE buffer
    and the depth buffer are both kept on the GPU, a compute pass there now counts the
    tiles marked cleared, and a pass over the whole depth buffer writes the clear value
    only if that is all of them, the same rule as on the CPU, with nothing read back.
    Into the first scene in 300 seconds: 3,016 seen to there; the read-backs fell from
    9,828 to 7,222, but the time spent reading back did not (31.9 s, from 30.9: those
    buffers were small, and the large targets still come back), and the draws (327,283)
    are within a run's spread. Validation is clean, and `--gpu-check` is as before.
57. **Depth exported by a pixel shader.** The read-backs are now timed by why, and the
    summary lists them, the most time first. Into the first scene in 300 seconds, 772
    read-backs for draws whose pixels were done on the CPU took 24.9 of the 32 seconds;
    the dispatches the CPU runs took 4.1 (5,194 read-backs, but small ones). Of the CPU's
    draws, nearly all the time was one pixel shader's: a depth downsample, the least of
    four depth samples given as the pixel's depth (`exp mrtz`), a depth buffer of
    16384x183 read back for each. With DB_SHADER_CONTROL's Z_EXPORT_ENABLE on, the
    translation now writes that value to `gl_FragDepth`; the CPU, which used to ignore it
    and test the triangle's depth, now tests the shader's depth once the shader has run
    (colour is not held back by that test on the CPU: a note says so, should a shader
    export both). `--gpu-check` now compares depth too, for draws that write it: the
    GPU's depth read back against the CPU's, each pixel within 1 in 65535. In 300
    seconds, the time reading back went from 32.3 s to 7.7, the pixels on the CPU from
    29.3 s to 2.8, and the draws from 319,781 to 343,678 (a little past a run's spread).
    In-game `--gpu-check`: 77 of 80 the same (75 of 78 before; the two new are this
    shader's), their depth the same in every pixel; validation is clean.

## Files

| Path | |
|---|---|
| `tools/loader.py` | runs the start-up under Unicorn and logs every system call |
| `native/` | the native loader: `main.cpp` (loading, dispatch, output); the stand-ins, in `standins.cpp`, `threads.cpp`, `files.cpp`, `libc.cpp`, `gnm.cpp` (the graphics driver), `video.cpp` (the display and event queues), `audio.cpp`, `pad.cpp` (the user and the controller) and `savedata.cpp`; `gpu.cpp`, the command processor; `gcn.cpp`, the shader disassembler; `shade.cpp`, `draw.cpp` and `bc7.cpp`, drawing on the CPU; `vulkan.cpp`, the window through Vulkan (Windows); `translate.cpp`, the shaders (pixel, vertex, compute) translated into GLSL, and `vkdraw.cpp`, the draws and dispatches on the GPU through Vulkan (Windows; `--gpu-check`); and for each platform `platform_*.cpp` (memory and its guarding, the thread pointer, stopping) and `enter_*.S` (the crossings between the game and the loader); `build.ps1` builds it on Windows |
| `work/loader/calls.log` | every call, in order, with its caller and arguments |
| `work/loader/summary.md` | where the run stopped, and every import it reached |
| `work/loader-native/` | the native run's `calls.log`, `summary.md`, `stdout.txt`, `files/`, `gpu/` (the first submissions of each queue, decoded), `shaders/` (the GPU programs used, disassembled, and with `--gpu-check` translated into GLSL), `draws/` (the first draws of each pixel shader, as drawn, and with `--gpu-check` as the GPU drew them, `NNN-gpu.bmp`) and `frame.bmp` (the screen when it stopped) |
