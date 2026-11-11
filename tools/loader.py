# SPDX-License-Identifier: GPL-3.0-or-later
"""Run Bloodborne's start-up under a CPU emulator and log every call it makes into the system.

    python -I tools/loader.py <prepared.elf> <symbols.tsv> <out dir> [--steps N]

<prepared.elf> and <symbols.tsv> are orbis_prep.py's outputs (work/ghidra/eboot-1.09.ghidra.elf
and .symbols.tsv). Writes <out dir>/calls.log (every import call in order, with its caller and
arguments) and <out dir>/summary.md (each import's count, and which were left unhandled).

This is an exploration tool, not an emulator (docs/the-loader.md). The game's own code runs
instruction by instruction in Unicorn (an x86-64 CPU emulator); the system it calls into is
replaced by Python:

* orbis_prep has already relocated the executable to 0x400000 and pointed every import's GOT
  slot at a one-byte stub (a RET) in its own segment. A hook on that segment sees each call:
  it reads the arguments (System V order: rdi, rsi, rdx, rcx, r8, r9), runs a Python stand-in
  if there is one, sets rax, and lets the RET return to the game.
* The stand-ins are the least the start-up needs: an allocator, memory and string functions,
  direct-memory mapping. Every other import returns 0 and is counted as unhandled.
* Thread-local storage follows x86-64's variant II: the TLS block sits just below the thread
  pointer, fs holds the thread pointer, and fs:[0] holds its own address.
* Threads the game creates are recorded, not run. Everything happens on the main thread.

It stops at the first GPU submission (sceGnmSubmitCommandBuffers or
sceGnmSubmitAndFlipCommandBuffers), at a memory fault (with the registers and the
instructions around it), at the exit handler, or after --steps instructions.
"""

from __future__ import annotations

import argparse
import collections
import io
import os
import struct
import sys

import capstone
from elftools.elf.elffile import ELFFile
from unicorn import (
    UC_ARCH_X86,
    UC_HOOK_CODE,
    UC_HOOK_MEM_FETCH_UNMAPPED,
    UC_HOOK_MEM_READ_UNMAPPED,
    UC_HOOK_MEM_WRITE_UNMAPPED,
    UC_MODE_64,
    UC_PROT_ALL,
)

# the package's own module for the classes: its __init__ re-exports them in a form a type checker
# doesn't count as public
from unicorn.unicorn import Uc, UcError
from unicorn.x86_const import (
    UC_X86_REG_FS_BASE,
    UC_X86_REG_R8,
    UC_X86_REG_R9,
    UC_X86_REG_RAX,
    UC_X86_REG_RBP,
    UC_X86_REG_RCX,
    UC_X86_REG_RDI,
    UC_X86_REG_RDX,
    UC_X86_REG_RIP,
    UC_X86_REG_RSI,
    UC_X86_REG_RSP,
)

PAGE = 0x1000
STACK_TOP, STACK_SIZE = 0x7F000000_0000, 16 << 20
HEAP_BASE, HEAP_STEP = 0x2_0000_0000, 64 << 20
TLS_BASE = 0x7E000000_0000
PARAMS = 0x7D000000_0000  # the entry parameters and their strings
EXIT_STUB = PARAMS + 0x800  # the address _start is given to call on exit
ARGS = (
    UC_X86_REG_RDI,
    UC_X86_REG_RSI,
    UC_X86_REG_RDX,
    UC_X86_REG_RCX,
    UC_X86_REG_R8,
    UC_X86_REG_R9,
)
SUBMIT = ("sceGnmSubmitCommandBuffers", "sceGnmSubmitAndFlipCommandBuffers")


def up(n: int, a: int = PAGE) -> int:
    return (n + a - 1) // a * a


class Stop(Exception):
    pass


class Machine:
    def __init__(self, elf_path: str, symbols: str):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_64)
        self.imports: dict[int, tuple[str, str]] = {}  # stub address: (name, library)
        self.entry = 0
        with open(symbols, encoding="utf-8") as f:
            for line in f:
                p = line.rstrip("\n").split("\t")
                if len(p) < 3 or p[0].startswith("#"):
                    continue
                if p[1] == "IMPORT":
                    self.imports[int(p[0], 16)] = (p[2], p[3] if len(p) > 3 else "")
                elif p[1] == "ENTRY":
                    self.entry = int(p[0], 16)
        self.calls: list[str] = []
        self.counts: collections.Counter[str] = collections.Counter()
        self.unhandled: collections.Counter[str] = collections.Counter()
        self.threads: list[
            tuple[int, int]
        ] = []  # (entry, argument) of each thread asked for
        self.heap_top = self.heap_end = HEAP_BASE
        self.allocs: dict[int, int] = {}  # address: size
        self.last_calls: collections.deque[str] = collections.deque(maxlen=12)
        self.stop_reason = ""
        self._load(elf_path)
        self._setup()

    # ---- memory -------------------------------------------------------------------------
    def _load(self, path: str):
        with open(path, "rb") as f:
            e = ELFFile(f)
            for s in e.iter_segments():
                if s["p_type"] == "PT_LOAD":
                    lo = s["p_vaddr"] // PAGE * PAGE
                    hi = up(s["p_vaddr"] + s["p_memsz"])
                    self.uc.mem_map(lo, hi - lo, UC_PROT_ALL)
                    self.uc.mem_write(s["p_vaddr"], s.data())
                elif s["p_type"] == "PT_TLS":
                    self.tls = (
                        s["p_vaddr"],
                        s["p_filesz"],
                        s["p_memsz"],
                        max(s["p_align"], 16),
                    )
        self.stubs = (min(self.imports), max(self.imports) + 1)

    def _setup(self):
        uc = self.uc
        uc.mem_map(STACK_TOP - STACK_SIZE, STACK_SIZE, UC_PROT_ALL)
        # TLS, variant II: [image][thread pointer -> self]
        _, filesz, memsz, align = self.tls
        uc.mem_map(TLS_BASE, 0x10000, UC_PROT_ALL)
        tp = TLS_BASE + up(memsz, align) + 0x100
        if filesz:
            uc.mem_write(
                tp - up(memsz, align), bytes(self.uc.mem_read(self.tls[0], filesz))
            )
        uc.mem_write(tp, struct.pack("<Q", tp))
        uc.reg_write(UC_X86_REG_FS_BASE, tp)
        # entry parameters: { int argc; int pad; const char *argv[3]; } and the exit stub (RET)
        uc.mem_map(PARAMS, PAGE, UC_PROT_ALL)
        uc.mem_write(PARAMS + 0x100, b"eboot.bin\0")
        uc.mem_write(PARAMS, struct.pack("<iiQQQ", 1, 0, PARAMS + 0x100, 0, 0))
        uc.mem_write(EXIT_STUB, b"\xc3")
        sp = STACK_TOP - 0x1000
        uc.reg_write(UC_X86_REG_RSP, sp)
        uc.reg_write(UC_X86_REG_RDI, PARAMS)
        uc.reg_write(UC_X86_REG_RSI, EXIT_STUB)
        uc.hook_add(
            UC_HOOK_CODE, self._on_import, begin=self.stubs[0], end=self.stubs[1]
        )
        uc.hook_add(UC_HOOK_CODE, self._on_exit, begin=EXIT_STUB, end=EXIT_STUB)
        uc.hook_add(
            UC_HOOK_MEM_READ_UNMAPPED
            | UC_HOOK_MEM_WRITE_UNMAPPED
            | UC_HOOK_MEM_FETCH_UNMAPPED,
            self._on_fault,
        )

    def alloc(self, size: int, align: int = 16) -> int:
        size = max(size, 1)
        addr = up(self.heap_top, max(align, 16))
        while addr + size > self.heap_end:
            self.uc.mem_map(self.heap_end, HEAP_STEP, UC_PROT_ALL)
            self.heap_end += HEAP_STEP
        self.heap_top = addr + size
        self.allocs[addr] = size
        return addr

    def read(self, a: int, n: int) -> bytes:
        return bytes(self.uc.mem_read(a, n))

    def cstr(self, a: int, limit: int = 512) -> str:
        if not a:
            return ""
        out = bytearray()
        while len(out) < limit:
            c = self.uc.mem_read(a + len(out), 1)[0]
            if c == 0:
                break
            out.append(c)
        return out.decode("latin-1")

    # ---- hooks --------------------------------------------------------------------------
    def _on_import(self, uc, addr, size, _):
        if addr not in self.imports:
            return
        name, _ = self.imports[addr]
        args = [uc.reg_read(r) for r in ARGS]
        caller = struct.unpack("<Q", self.read(uc.reg_read(UC_X86_REG_RSP), 8))[0]
        self.counts[name] += 1
        if name in SUBMIT:
            self.stop_reason = f"first GPU submission: {name}"
            self._log(caller, name, args, "stop")
            uc.emu_stop()
            return
        fn = STANDINS.get(name)
        if fn is None:
            self.unhandled[name] += 1
            ret = 0
            note = "unhandled"
        else:
            r = fn(self, args)
            ret, note = r if isinstance(r, tuple) else (r, "")
        uc.reg_write(UC_X86_REG_RAX, ret & 0xFFFFFFFFFFFFFFFF)
        self._log(caller, name, args, f"-> {ret:#x}" + (f" {note}" if note else ""))

    def _log(self, caller, name, args, tail):
        line = f"{len(self.calls):6} {caller:#010x} {name}({', '.join(f'{a:#x}' for a in args[:4])}) {tail}"
        self.calls.append(line)
        self.last_calls.append(line)

    def _on_exit(self, uc, addr, size, _):
        self.stop_reason = f"exit handler called with {uc.reg_read(UC_X86_REG_RDI):#x}"
        uc.emu_stop()

    def _on_fault(self, uc, access, addr, size, value, _):
        self.stop_reason = f"memory fault: access {access} at {addr:#x}"
        return False

    # ---- running ------------------------------------------------------------------------
    def run(self, steps: int):
        try:
            self.uc.emu_start(self.entry, 0, count=steps)
            if not self.stop_reason:
                self.stop_reason = f"instruction budget ({steps:,}) used up"
        except UcError as e:
            if not self.stop_reason:
                self.stop_reason = f"CPU error: {e}"
        except Stop as e:
            self.stop_reason = str(e)

    def context(self) -> list[str]:
        uc = self.uc
        rip = uc.reg_read(UC_X86_REG_RIP)
        regs = ", ".join(
            f"{n} {uc.reg_read(r):#x}"
            for n, r in (
                ("rip", UC_X86_REG_RIP),
                ("rsp", UC_X86_REG_RSP),
                ("rbp", UC_X86_REG_RBP),
                ("rax", UC_X86_REG_RAX),
                ("rdi", UC_X86_REG_RDI),
                ("rsi", UC_X86_REG_RSI),
            )
        )
        out = [regs]
        try:
            md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
            for i in md.disasm(self.read(rip, 48), rip):
                out.append(f"  {i.address:#x} {i.mnemonic} {i.op_str}")
                if len(out) > 6:
                    break
        except UcError:
            out.append("  (rip is not in mapped memory)")
        return out


# ---- the stand-ins: the least of the system the start-up needs --------------------------
def _malloc(m: Machine, a):
    return m.alloc(a[0])


def _calloc(m: Machine, a):
    p = m.alloc(a[0] * a[1])
    m.uc.mem_write(p, bytes(a[0] * a[1]))
    return p


def _realloc(m: Machine, a):
    p = m.alloc(a[1])
    if a[0]:
        old = m.allocs.get(a[0], 0)
        m.uc.mem_write(p, m.read(a[0], min(old, a[1])))
    return p


def _memalign(m: Machine, a):  # memalign(align, size), aligned_alloc(align, size)
    return m.alloc(a[1], a[0])


def _posix_memalign(m: Machine, a):  # posix_memalign(void **out, align, size)
    p = m.alloc(a[2], a[1])
    m.uc.mem_write(a[0], struct.pack("<Q", p))
    return 0


def _memcpy(m: Machine, a):
    if a[2]:
        m.uc.mem_write(a[0], m.read(a[1], a[2]))
    return a[0]


def _memset(m: Machine, a):
    if a[2]:
        m.uc.mem_write(a[0], bytes([a[1] & 0xFF]) * a[2])
    return a[0]


def _memcmp(m: Machine, a):
    x, y = m.read(a[0], a[2]), m.read(a[1], a[2])
    return 0 if x == y else (-1 if x < y else 1)


def _strlen(m: Machine, a):
    return len(m.cstr(a[0], 1 << 16))


def _strcmp(m: Machine, a):
    x, y = m.cstr(a[0]), m.cstr(a[1])
    return 0 if x == y else (-1 if x < y else 1)


def _strncmp(m: Machine, a):
    x, y = m.cstr(a[0])[: a[2]], m.cstr(a[1])[: a[2]]
    return 0 if x == y else (-1 if x < y else 1)


def _strcpy(m: Machine, a):
    m.uc.mem_write(a[0], m.cstr(a[1], 1 << 16).encode("latin-1") + b"\0")
    return a[0]


def _alloc_direct(m: Machine, a):
    # sceKernelAllocateDirectMemory(start, end, len, align, type, *physOut): hand out heap
    p = m.alloc(a[2], max(a[3], 0x10000))
    m.uc.mem_write(a[5], struct.pack("<Q", p))
    return 0


def _map_direct(m: Machine, a):
    # sceKernelMapDirectMemory(void **addr, len, prot, flags, phys, align): map it 1:1
    m.uc.mem_write(a[0], struct.pack("<Q", a[4]))
    return 0


def _map_flexible(m: Machine, a):
    # sceKernelMapFlexibleMemory / MapNamedFlexibleMemory(void **addr, len, prot, flags, ...)
    p = m.alloc(a[1], 0x4000)
    m.uc.mem_write(a[0], struct.pack("<Q", p))
    return 0


def _thread_create(m: Machine, a):
    # scePthreadCreate(*thread, attr, entry, arg, name): record it, don't run it
    m.threads.append((a[2], a[3]))
    m.uc.mem_write(a[0], struct.pack("<Q", 0x1000 + len(m.threads)))
    return 0, f"thread {len(m.threads)} not run: {m.cstr(a[4])!r}"


def _guard_acquire(m: Machine, a):
    # __cxa_guard_acquire(guard): 1 = "construct it now" while the guard's first byte is 0
    return 1 if m.read(a[0], 1)[0] == 0 else 0


def _guard_release(m: Machine, a):
    m.uc.mem_write(a[0], b"")
    return 0


def _direct_size(m: Machine, a):
    return 5 << 30  # sceKernelGetDirectMemorySize: the 5 GiB a game is given


def _stack_chk_fail(m: Machine, a):
    raise Stop("__stack_chk_fail: the game's stack check failed")


STANDINS = {
    "malloc": _malloc,
    "calloc": _calloc,
    "realloc": _realloc,
    "memalign": _memalign,
    "aligned_alloc": _memalign,
    "posix_memalign": _posix_memalign,
    "_Znwm": _malloc,
    "_Znam": _malloc,
    "memcpy": _memcpy,
    "memmove": _memcpy,
    "memset": _memset,
    "memcmp": _memcmp,
    "strlen": _strlen,
    "strcmp": _strcmp,
    "strncmp": _strncmp,
    "strcpy": _strcpy,
    "sceKernelAllocateDirectMemory": _alloc_direct,
    "sceKernelMapDirectMemory": _map_direct,
    "sceKernelMapFlexibleMemory": _map_flexible,
    "sceKernelMapNamedFlexibleMemory": _map_flexible,
    "scePthreadCreate": _thread_create,
    "__stack_chk_fail": _stack_chk_fail,
    "__cxa_guard_acquire": _guard_acquire,
    "__cxa_guard_release": _guard_release,
    "sceKernelGetDirectMemorySize": _direct_size,
}
# Imports for which 0 ("success", "nothing to do") is the right answer in a run with one thread:
# locks never contend, and exit handlers never run.
ZERO_OK = {
    "_init_env",
    "atexit",
    "__cxa_atexit",
    "__cxa_guard_abort",
    "scePthreadMutexattrInit",
    "scePthreadMutexattrSettype",
    "scePthreadMutexattrDestroy",
    "scePthreadMutexInit",
    "scePthreadMutexDestroy",
    "scePthreadMutexLock",
    "scePthreadMutexUnlock",
    "scePthreadMutexTrylock",
    "scePthreadRwlockInit",
    "scePthreadRwlockRdlock",
    "scePthreadRwlockWrlock",
    "scePthreadRwlockUnlock",
    "scePthreadCondInit",
    "scePthreadCondSignal",
    "scePthreadCondBroadcast",
    "sceKernelCreateSema",
    "sceKernelSignalSema",
    "sceKernelPollSema",
}
for _n in ZERO_OK:
    STANDINS.setdefault(_n, lambda m, a: 0)


def main() -> int:
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("elf")
    ap.add_argument("symbols")
    ap.add_argument("out")
    ap.add_argument("--steps", type=int, default=50_000_000)
    a = ap.parse_args()
    m = Machine(a.elf, a.symbols)
    m.run(a.steps)
    os.makedirs(a.out, exist_ok=True)
    with open(
        os.path.join(a.out, "calls.log"), "w", encoding="utf-8", newline="\n"
    ) as f:
        f.write("\n".join(m.calls) + "\n")
    summary = [
        "# Loader run\n",
        (
            f"Stopped: {m.stop_reason}. {len(m.calls):,} import calls, {len(m.counts)} "
            f"distinct; {sum(m.unhandled.values()):,} calls to {len(m.unhandled)} imports "
            f"without a stand-in; {len(m.threads)} threads asked for, not run.\n"
        ),
        "## Where it stopped\n",
        "```",
        *m.context(),
        "```\n",
        "Last calls:\n",
        "```",
        *m.last_calls,
        "```\n",
        "## Imports by first use\n",
        "| Import | Calls | Stand-in |",
        "|---|---:|---|",
    ]
    seen = []
    for line in m.calls:
        n = line.split()[2].split("(")[0]
        if n not in seen:
            seen.append(n)
    for n in seen:
        summary.append(f"| {n} | {m.counts[n]} | {'yes' if n in STANDINS else ''} |")
    with open(
        os.path.join(a.out, "summary.md"), "w", encoding="utf-8", newline="\n"
    ) as f:
        f.write("\n".join(summary) + "\n")
    print("\n".join(summary[:6]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
