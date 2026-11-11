// SPDX-License-Identifier: GPL-3.0-or-later
//
// Windows: 64-bit Windows doesn't let a program set fs, so the game's thread pointer can't go
// there. Instead, every place the game's code reads it is rewritten before the run. In this
// executable that is one instruction, 17,127 times:
//
//     64 48 8B 04 25 00 00 00 00      mov rax, qword ptr fs:[0]
//
// which, under TLS variant II, loads the thread pointer (fs:[0] holds the thread pointer
// itself). It becomes, at the same length:
//
//     65 48 8B 04 25 <0x1480 + 8*slot>  mov rax, qword ptr gs:[0x1480 + 8*slot]
//
// gs points at the thread's TEB, whose TlsSlots array starts at 0x1480: the instruction reads
// Windows thread-local slot `slot` (from TlsAlloc), where each thread keeps its game thread
// pointer. Nothing else changes: no stack is touched, no flags, no other register.
//
// Faults are caught by a vectored exception handler, which records the registers and resumes
// at mensis_bail (enter_windows.S), back on the loader's stack. The time limit is a watchdog
// thread that does the same to the main thread from outside.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <condition_variable>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>

#include "loader.hpp"

using namespace mensis;

extern "C" {
u64 mensis_host_rsp = 0;  // enter_windows.S
u64 mensis_game_stack_base = 0, mensis_game_stack_limit = 0;
void mensis_enter_game(u64 entry, u64 params, u64 exit, u64 stack);
[[noreturn]] void mensis_bail();
}

namespace {

DWORD g_slot = TLS_OUT_OF_INDEXES;
volatile LONG g_running = 0;

void record(const CONTEXT& c) {
    M.fault = {true, c.Rip, c.Rsp, c.Rbp, c.Rax, c.Rdi, c.Rsi};
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* e) {
    if (!g_running) return EXCEPTION_CONTINUE_SEARCH;
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
    g_running = 0;
    record(*e->ContextRecord);
    M.stop_reason = what;
    e->ContextRecord->Rip = reinterpret_cast<DWORD64>(&mensis_bail);
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

void prepare() {
    g_slot = TlsAlloc();
    if (g_slot == TLS_OUT_OF_INDEXES || g_slot >= 64)
        die("no Windows TLS slot below 64 is free (the TEB holds only the first 64)");
    static const unsigned char load_fs[9] = {0x64, 0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0};
    u32 disp = 0x1480 + 8 * g_slot;
    u64 patched = 0;
    for (auto [lo, hi] : M.code) {
        auto* p = reinterpret_cast<unsigned char*>(lo);
        auto* end = reinterpret_cast<unsigned char*>(hi) - sizeof load_fs;
        while (p <= end) {
            p = static_cast<unsigned char*>(std::memchr(p, 0x64, size_t(end - p) + 1));
            if (!p) break;
            if (std::memcmp(p, load_fs, sizeof load_fs) == 0) {
                p[0] = 0x65;  // fs -> gs
                std::memcpy(p + 5, &disp, 4);
                patched++;
                p += sizeof load_fs;
            } else {
                p++;
            }
        }
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    M.notes.push_back(std::format(
        "Thread pointer: {} loads of fs:[0] rewritten to read Windows TLS slot {} "
        "(gs:[{:#x}]).",
        patched, g_slot, disp));
}

void run(u64 entry, u64 params, u64 exit, u64 sp, u64 tp) {
    TlsSetValue(g_slot, reinterpret_cast<void*>(tp));
    mensis_game_stack_base = STACK_TOP;
    mensis_game_stack_limit = STACK_TOP - STACK_SIZE;
    PVOID handler = AddVectoredExceptionHandler(1, on_exception);

    // The watchdog: after M.seconds, stop the main thread wherever it is.
    HANDLE main_thread = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread,
                    0, FALSE, DUPLICATE_SAME_ACCESS);
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    std::thread watchdog([&] {
        std::unique_lock lock(mu);
        if (cv.wait_for(lock, std::chrono::seconds(M.seconds), [&] { return done; })) return;
        if (!g_running) return;
        if (g_in_loader) {  // inside a stand-in: stop at the end of the call
            g_alarm = 1;
            return;
        }
        SuspendThread(main_thread);
        if (g_running && !g_in_loader) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_FULL;
            GetThreadContext(main_thread, &c);
            g_running = 0;
            record(c);
            M.stop_reason = std::format("time limit ({} s) reached", M.seconds);
            c.Rip = reinterpret_cast<DWORD64>(&mensis_bail);
            SetThreadContext(main_thread, &c);
        } else if (g_running) {
            g_alarm = 1;
        }
        ResumeThread(main_thread);
    });

    g_running = 1;
    mensis_enter_game(entry, params, exit, sp);
    g_running = 0;

    {
        std::lock_guard lock(mu);
        done = true;
    }
    cv.notify_one();
    watchdog.join();
    CloseHandle(main_thread);
    RemoveVectoredExceptionHandler(handler);
}

void leave() {
    g_running = 0;
    mensis_bail();
}

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
