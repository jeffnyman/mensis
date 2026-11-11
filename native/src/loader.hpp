// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mensis {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

// The same fixed addresses as tools/loader.py, so both runs hand the game the same pointers
// and their call logs can be compared line by line. Each is 64 KiB aligned, as Windows needs.
constexpr u64 PAGE = 0x1000;
constexpr u64 STACK_TOP = 0x7F0000000000, STACK_SIZE = 16 << 20;
constexpr u64 HEAP_BASE = 0x200000000, HEAP_RESERVE = 64ull << 30;  // reserved, committed as used
constexpr u64 HEAP_STEP = 64 << 20;  // how much more is committed at a time
constexpr u64 TLS_BASE = 0x7E0000000000;
constexpr u64 PARAMS = 0x7D0000000000;  // the entry parameters and their strings
constexpr u64 EXIT_STUB = PARAMS + 0x800;  // the address _start is given to call on exit
constexpr u64 LIBC_DATA = PARAMS + 0x1000;  // the C library's character tables
constexpr u64 THUNK_SIZE = 32;

inline u64 up(u64 n, u64 a = PAGE) { return (n + a - 1) / a * a; }

// Built on the game's stack by mensis_import_common (enter_*.S); the layout is fixed there.
struct Frame {
    u64 args[6];
    u64 index;
    u64 caller;
    u64 game_fs;  // Linux only: the game's fs while the loader runs
    u64 ret2;  // returned in rdx: the second half of a 16-byte result (ldiv)
    alignas(16) unsigned char xmm0[16], xmm1[16];  // the first two floating-point arguments
    alignas(16) unsigned char fret[16];  // returned in xmm0
};
static_assert(sizeof(Frame) == 128, "enter_*.S lays the frame out by hand");

struct Import {
    std::string name, library;
    u64 stub = 0;
};

// A stand-in gets the six argument registers and may set a note for the log.
using Standin = i64 (*)(const u64* a, std::string& note);

// Where a fault or the time limit found the CPU.
struct Fault {
    bool valid = false;
    u64 rip = 0, rsp = 0, rbp = 0, rax = 0, rdi = 0, rsi = 0;
};

struct Machine {
    std::vector<Import> imports;  // by index; index imports.size() is the exit handler
    std::vector<Standin> standin;  // by index; null = none
    u64 entry = 0;
    u64 stubs_lo = 0, stubs_hi = 0, thunks = 0;
    std::vector<std::pair<u64, u64>> code;  // the game's executable ranges
    struct { u64 vaddr = 0, filesz = 0, memsz = 0, align = 16; } tls;

    std::vector<std::string> calls;
    std::deque<std::string> last_calls;
    std::unordered_map<std::string, u64> counts, unhandled;
    std::vector<std::string> first_use;
    std::vector<std::pair<u64, u64>> threads;  // (entry, argument) of each thread asked for
    std::vector<std::string> notes;  // how the platform prepared the run, for the summary

    u64 heap_top = HEAP_BASE, heap_committed = HEAP_BASE;
    std::unordered_map<u64, u64> allocs;  // address: size

    std::string stdout_text;  // what the game printed
    Frame* frame = nullptr;  // the call in progress, for stand-ins that need more than a[]

    u64 max_calls = 0;
    unsigned seconds = 10;
    std::string stop_reason;
    Fault fault;
};

extern Machine M;
extern volatile int g_in_loader;  // set while a stand-in runs
extern volatile int g_alarm;  // the time limit passed during a stand-in: stop after it

[[noreturn]] void die(const std::string& what);
u64 alloc(u64 size, u64 align = 16);
[[noreturn]] void stop(const std::string& reason);
const std::unordered_map<std::string, Standin>& standins();
bool is_submit(const std::string& name);
void init_libc_data();  // after LIBC_DATA is mapped

// What differs between Linux and Windows (platform_linux.cpp, platform_windows.cpp).
namespace platform {
void reserve(u64 addr, u64 size);  // address space at exactly addr; dies if it's taken
void commit(u64 addr, u64 size);  // make reserved memory usable (read, write, execute)
void prepare();  // after loading: whatever the CPU needs before the game's code runs
// Runs the game until it stops; on a fault or the time limit, fills M.fault and M.stop_reason.
void run(u64 entry, u64 params, u64 exit, u64 sp, u64 tp);
[[noreturn]] void leave();  // from inside a stand-in: back out of run()
std::vector<std::string> disassemble(u64 rip);  // the instructions at rip, for the summary
}  // namespace platform

}  // namespace mensis

extern "C" {
mensis::i64 mensis_dispatch(mensis::Frame* f);
void mensis_import_common();
}
