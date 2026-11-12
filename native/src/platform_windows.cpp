// SPDX-License-Identifier: GPL-3.0-or-later
//
// Windows: 64-bit Windows doesn't let a program set fs, and gs points at each thread's own
// information block (TEB). The game's thread-pointer loads are rewritten to read gs:[0x1480 +
// 8 * slot]: the TEB's thread-local slots start at 0x1480, so this reads Windows TLS slot `slot`
// (from TlsAlloc), where each game thread keeps its thread pointer. No stack, flag or other
// register is touched.
//
// A fault is caught by a vectored exception handler, which records it and resumes the thread in
// mensis_restore, back on the loader's stack.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <format>

#include "loader.hpp"

using namespace mensis;

namespace {

DWORD g_slot = TLS_OUT_OF_INDEXES;

LONG CALLBACK on_exception(EXCEPTION_POINTERS* e) {
    GameThread* t = current_thread();
    if (!t || !t->enter.saved_rsp) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    std::string what;
    switch (r->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION: {
            u64 kind = r->NumberParameters > 0 ? r->ExceptionInformation[0] : 0;
            u64 at = r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0;
            what = std::format("memory fault ({}) at {:#x}",
                               kind == 0 ? "read" : kind == 1 ? "write" : "execute", at);
            break;
        }
        case EXCEPTION_IN_PAGE_ERROR: what = "memory fault (in-page error)"; break;
        case EXCEPTION_DATATYPE_MISALIGNMENT: what = "misaligned access"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION: what = "invalid instruction"; break;
        case EXCEPTION_PRIV_INSTRUCTION: what = "privileged instruction"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO: what = "integer divide by zero"; break;
        case EXCEPTION_INT_OVERFLOW: what = "integer overflow"; break;
        case EXCEPTION_BREAKPOINT: what = "breakpoint (int3)"; break;
        case EXCEPTION_STACK_OVERFLOW: what = "stack overflow"; break;
        default: return EXCEPTION_CONTINUE_SEARCH;  // not a fault of the game's (a C++ throw)
    }
    const CONTEXT& c = *e->ContextRecord;
    t->fault = {true, c.Rip, c.Rsp, c.Rbp, c.Rax, c.Rdi, c.Rsi};
    t->fault_reason = what;
    t->faulted = true;
    e->ContextRecord->Rip = reinterpret_cast<DWORD64>(&mensis_restore);
    e->ContextRecord->Rcx = t->enter.saved_rsp;
    return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

namespace mensis::platform {

void reserve(u64 addr, u64 size) {
    void* p = VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE);
    if (p != reinterpret_cast<void*>(addr))
        die(std::format("can't reserve {:#x} bytes at {:#x} (already in use?): error {}", size,
                        addr, GetLastError()));
}

void commit(u64 addr, u64 size) {
    if (!VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_COMMIT, PAGE_EXECUTE_READWRITE))
        die(std::format("can't commit {:#x} bytes at {:#x}: error {}", size, addr,
                        GetLastError()));
}

u32 prepare() {
    g_slot = TlsAlloc();
    if (g_slot == TLS_OUT_OF_INDEXES || g_slot >= 64)
        die("no Windows TLS slot below 64 is free (the TEB holds only the first 64)");
    return 0x1480 + 8 * g_slot;
}

void install_fault_handler() { AddVectoredExceptionHandler(1, on_exception); }

void enter_thread(u64 tp) { TlsSetValue(g_slot, reinterpret_cast<void*>(tp)); }

// The bytes at rip (Windows has no objdump to hand): look the address up in Ghidra.
std::vector<std::string> disassemble(u64 rip) {
    unsigned char buf[16];
    SIZE_T n = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(rip), buf, sizeof buf,
                           &n) ||
        n == 0)
        return {"  (rip is not in mapped memory)"};
    std::string s = "  bytes:";
    for (SIZE_T i = 0; i < n; i++) s += std::format(" {:02x}", buf[i]);
    return {s};
}

}  // namespace mensis::platform
