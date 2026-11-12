// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mensis {

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

// The same fixed addresses as tools/loader.py where it has them, so both runs hand the game the
// same pointers. Each is 64 KiB aligned, as Windows needs.
constexpr u64 PAGE = 0x1000;
constexpr u64 STACK_TOP = 0x7F0000000000, STACK_SIZE = 16 << 20;  // the main thread's stack
constexpr u64 HEAP_BASE = 0x200000000, HEAP_RESERVE = 64ull << 30;  // reserved, committed as used
constexpr u64 HEAP_STEP = 64 << 20;  // how much more is committed at a time
constexpr u64 TLS_BASE = 0x7E0000000000;  // each thread's TLS block, 64 KiB apart
constexpr u64 TLS_SLOT = 0x10000, MAX_THREADS = 256;
constexpr u64 PARAMS = 0x7D0000000000;  // the entry parameters and their strings
constexpr u64 EXIT_STUB = PARAMS + 0x800;  // the address _start is given to call on exit
constexpr u64 LIBC_DATA = PARAMS + 0x1000;  // the C library's character tables
constexpr u64 THREAD_STACKS = 0x7C0000000000, THREAD_STACK_SLOT = 64 << 20;  // other threads'
constexpr u64 SYNC_BASE = 0x7B0000000000, SYNC_RESERVE = 1ull << 30;  // mutexes, conditions...
constexpr u64 THUNK_SIZE = 32;

inline u64 up(u64 n, u64 a = PAGE) { return (n + a - 1) / a * a; }

// Built on the game's stack by mensis_import_common (enter_*.S); the layout is fixed there.
struct Frame {
    u64 args[6];
    u64 index;
    u64 caller;
    u64 unused;
    u64 ret2;  // returned in rdx: the second half of a 16-byte result (ldiv)
    alignas(16) unsigned char xmm0[16], xmm1[16];  // the first two floating-point arguments
    alignas(16) unsigned char fret[16];  // returned in xmm0
};
static_assert(sizeof(Frame) == 128, "enter_*.S lays the frame out by hand");

// mensis_enter_game's argument (enter_*.S lays it out by hand).
struct EnterArgs {
    u64 entry, arg0, arg1;  // the function, and its first two arguments (rdi, rsi)
    u64 stack_top, stack_limit;
    u64 saved_rsp;  // written: where the loader's own state is saved while the game runs
};
static_assert(sizeof(EnterArgs) == 48);

struct Import {
    std::string name, library;
    u64 stub = 0;
};

// A stand-in gets the six argument registers and may set a note for the log.
using Standin = i64 (*)(const u64* a, std::string& note);

// Where a fault found the CPU.
struct Fault {
    bool valid = false;
    u64 rip = 0, rsp = 0, rbp = 0, rax = 0, rdi = 0, rsi = 0;
};

// One of the game's threads, run on a thread of the loader's own.
struct GameThread {
    u32 id = 0;  // 0 is the main thread
    std::string name;
    u64 handle = 0;  // what the game knows it by (scePthreadCreate's *thread, scePthreadSelf)
    u64 tp = 0;  // its thread pointer
    EnterArgs enter{};
    std::thread host;

    // guarded by M.log_lock
    std::string state = "starting";
    std::string last_call;
    u64 calls = 0;

    // set by the thread itself on its way out
    bool faulted = false;
    Fault fault;
    std::string fault_reason;
    std::string stop_reason;  // a stand-in asked the whole run to stop
    bool exited = false;  // scePthreadExit
    u64 result = 0;

    // joining
    std::mutex m;
    std::condition_variable cv;
    bool finished = false;
    bool detached = false;
};

struct Machine {
    std::vector<Import> imports;  // by index; index imports.size() is the exit handler
    std::vector<Standin> standin;  // by index; null = none
    u64 entry = 0;
    u64 stubs_lo = 0, stubs_hi = 0, thunks = 0;
    std::vector<std::pair<u64, u64>> code;  // the game's executable ranges
    struct { u64 vaddr = 0, filesz = 0, memsz = 0, align = 16; } tls;

    // The log and everything below it, shared by all threads: under log_lock.
    std::mutex log_lock;
    std::vector<std::string> calls;
    std::deque<std::string> last_calls;
    std::unordered_map<std::string, u64> counts, unhandled;
    std::vector<std::string> first_use;
    std::vector<std::string> notes;  // how the platform prepared the run, for the summary
    std::string stdout_text;  // what the game printed
    std::deque<std::unique_ptr<GameThread>> threads;

    // The game's heap: under heap_lock.
    std::mutex heap_lock;
    u64 heap_top = HEAP_BASE, heap_committed = HEAP_BASE;
    std::unordered_map<u64, u64> allocs;  // address: size

    u64 max_calls = 0;
    unsigned seconds = 10;
    std::string out_dir;
    std::chrono::steady_clock::time_point t0;
    Fault fault;  // the first fault, if a fault stopped the run
};

extern Machine M;

[[noreturn]] void die(const std::string& what);
u64 alloc(u64 size, u64 align = 16);
// From inside a stand-in: stop the whole run (the outputs are written once this thread is back
// on its own stack).
[[noreturn]] void stop(const std::string& reason);
// Write the outputs and end the process. The first caller wins; any other waits for the end.
[[noreturn]] void finish(const std::string& reason);
const std::unordered_map<std::string, Standin>& standins();
void add_thread_standins(std::unordered_map<std::string, Standin>& t);  // threads.cpp
bool is_submit(const std::string& name);
void init_libc_data();  // after LIBC_DATA is mapped
Frame* current_frame();  // the call in progress on this thread, for stand-ins that need more
GameThread* current_thread();
// Make a game thread ready to run: its stack and TLS block. Returns it (owned by M.threads).
GameThread* new_thread(const std::string& name, u64 entry, u64 arg0, u64 arg1, u64 stack_size);
void run_thread(GameThread* t);  // on the calling host thread; returns when the thread ends
u64 sync_alloc(u64 size, u64 align);  // memory for the loader's own objects, at a fixed place

// What differs between Linux and Windows (platform_linux.cpp, platform_windows.cpp).
namespace platform {
void reserve(u64 addr, u64 size);  // address space at exactly addr; dies if it's taken
void commit(u64 addr, u64 size);  // make reserved memory usable (read, write, execute)
// After loading: whatever the CPU needs before the game's code runs. Returns the
// displacement the thread-pointer loads are rewritten to read through gs.
u32 prepare();
void install_fault_handler();
void enter_thread(u64 tp);  // on each host thread before it runs game code
std::vector<std::string> disassemble(u64 rip);  // the instructions at rip, for the summary
}  // namespace platform

}  // namespace mensis

extern "C" {
mensis::i64 mensis_dispatch(mensis::Frame* f);
void mensis_import_common();
// Saves the loader's state, switches to the game's stack and runs entry(arg0, arg1). Returns its
// result when it returns, or 0 when mensis_restore brings the thread back.
mensis::u64 mensis_enter_game(mensis::EnterArgs* a);
[[noreturn]] void mensis_restore(mensis::u64 saved_rsp);
void mensis_game_returned();
}
