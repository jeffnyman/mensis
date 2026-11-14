// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
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
// The game's heap: reserved, committed as used. At HEAP_BASE (tools/loader.py's) if it's free;
// Windows sometimes has something of its own there already, and then the next of these.
constexpr u64 HEAP_BASES[] = {0x200000000, 0x6000000000, 0xA000000000, 0xE000000000};
constexpr u64 HEAP_BASE = HEAP_BASES[0], HEAP_RESERVE = 16ull << 30;
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
    alignas(16) unsigned char xmm2_7[6][16];  // the other floating-point arguments
    u64 stack_args;  // where the caller's arguments past the registers are
    u64 pad;
};
static_assert(sizeof(Frame) == 240, "enter_*.S lays the frame out by hand");

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

// One import call, as the log keeps it: formatted only when written out.
struct Call {
    u64 n = 0;  // its number, in the order calls finish
    u64 caller = 0;
    u64 args[4] = {};
    i64 ret = 0;
    u32 import = 0;
    u32 thread = 0;
    std::string note;  // the stand-in's, or "unhandled"
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
    Call last;  // its last finished call (last.n == 0 and no import: none yet)
    u64 calls = 0;
    // The call it's in, if any: the import's index + 1 (0 = none), and its frame. Set by the
    // thread without the lock; read for the summary.
    std::atomic<u32> in_call{0};
    std::atomic<const Frame*> frame{nullptr};

    // set by the thread itself on its way out
    bool faulted = false;
    Fault fault;
    std::string fault_reason;
    std::string stop_reason;  // a stand-in asked the whole run to stop
    bool exited = false;  // scePthreadExit
    u64 result = 0;
    u64 errno_at = 0;  // its errno (__error), in the loader's own area

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
    std::vector<std::pair<u64, u64>> image;  // all of its loaded segments
    struct { u64 vaddr = 0, filesz = 0, memsz = 0, align = 16; } tls;

    // The log and everything below it, shared by all threads: under log_lock. The first
    // log_limit calls are kept whole (formatted); after that only the last RECENT, the counts,
    // and each thread's last call, so a long run costs little more than the calls themselves.
    std::mutex log_lock;
    u64 log_limit = 5'000'000;
    static constexpr size_t RECENT = 10'000;
    std::vector<std::string> calls;  // the first log_limit, formatted
    std::vector<Call> recent;  // a ring of the last RECENT
    u64 total_calls = 0;  // finished
    std::vector<u64> unhandled;  // by import index
    std::vector<u32> first_use;  // import indexes, in the order first called
    // By import index, without the lock: calls started, and whether it has been called yet.
    std::unique_ptr<std::atomic<u64>[]> counts;
    std::unique_ptr<std::atomic<bool>[]> seen;
    std::atomic<u64> started{0};  // calls started (for --calls)
    std::vector<std::string> notes;  // how the platform prepared the run, for the summary
    std::string stdout_text;  // what the game printed
    std::deque<std::unique_ptr<GameThread>> threads;

    // The game's heap: under heap_lock.
    std::mutex heap_lock;
    u64 heap_base = HEAP_BASE, heap_top = HEAP_BASE, heap_committed = HEAP_BASE;
    std::unordered_map<u64, u64> allocs;  // address: size

    u64 max_calls = 0;
    u64 max_flips = 0;  // --frames: stop after this many flips
    u64 snapshots = 0;  // --snapshots N: the screen saved every N flips, to <out>/frames/
    unsigned seconds = 10;  // 0: no time limit
    bool draw = true;  // run the draws and dispatches (--no-draw: only follow the commands)
    bool gpu_check = false;  // draw the draws saved to draws/ on the PC's GPU too (--gpu-check)
    bool gpu = false;  // draw on the PC's GPU what it can (the default where there is Vulkan; --cpu: not)
    std::string out_dir;
    std::vector<std::string> app0;  // the folders /app0/ is read from, first match wins
    std::vector<std::pair<u64, u32>> presses;  // --press: from which flip, which buttons (pad.cpp)
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
void add_file_standins(std::unordered_map<std::string, Standin>& t);  // files.cpp
void add_gnm_standins(std::unordered_map<std::string, Standin>& t);  // gnm.cpp
void add_libc_standins(std::unordered_map<std::string, Standin>& t);  // libc.cpp
void add_audio_standins(std::unordered_map<std::string, Standin>& t);  // audio.cpp
void add_video_standins(std::unordered_map<std::string, Standin>& t);  // video.cpp
void add_pad_standins(std::unordered_map<std::string, Standin>& t);  // pad.cpp
void add_savedata_standins(std::unordered_map<std::string, Standin>& t);  // savedata.cpp
// The folder a save data mount point ("savedata0", ...) stands for, and whether it may be
// written; empty if nothing is mounted there (savedata.cpp)
std::string savedata_folder(const std::string& point, bool& writable);
u32 pad_button(const std::string& name);  // a button's bit, by its name (cross, up, ...); 0 if none
std::vector<std::string> pad_report();  // for the summary
u64 video_flips();  // the flips shown so far
// What the controller reports: its buttons (SCE_PAD_BUTTON_*), the sticks (0 to 255, 128 the
// middle, y growing downwards) and the triggers (0 to 255)
struct PadInput {
    u32 buttons = 0;
    std::uint8_t lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
};
// The command processor (gpu.cpp): a submission to the graphics queue, then `then` (a flip);
// a compute queue's ring; the ring written up to offset (sceGnmDingDong); the summary's lines.
void gpu_submit(u32 count, const u64* dcbs, const u32* dcb_bytes, const u64* ccbs,
                const u32* ccb_bytes, std::function<void()> then);
void gpu_map_compute(int id, u64 ring, u32 dwords, u64 rptr_addr);
void gpu_ding_dong(int id, u32 offset);
std::vector<std::string> gpu_report();  // and writes the shaders out, disassembled
// The GPU's shaders (gcn.cpp): the code at `code`, up to `dwords` long, disassembled to its
// end (the footer's length; with no footer, the first s_endpgm or return)
struct GcnListing {
    std::vector<std::string> lines;
    u32 bytes = 0, instructions = 0, unknown = 0;
    bool has_footer = false;
    u32 hash0 = 0, hash1 = 0, footer_at = 0;
    std::vector<int> calls;  // s_swappc_b64 / s_setpc_b64 to an SGPR pair: its first SGPR
};
GcnListing gcn_disassemble(const u32* code, u32 dwords);
// Drawing (draw.cpp): the registers a queue has set (SH: the shaders'; context: the rest of
// a draw's state; user config: the primitive type), and what the index packets said.
struct GpuRegs {
    std::array<u32, 0x400> sh{}, ctx{}, uc{};
    std::array<bool, 0x400> ctx_set{};  // set since the last CLEAR_STATE (else: the default)
    u32 index_size = 2;  // bytes
    u64 index_base = 0;
    u32 instances = 1;
};
// A draw of `count` vertices (indices: their address, or 0 for 0, 1, 2...) and a dispatch of
// x * y * z groups, run on the CPU; each says what it did, for the listing.
std::string gpu_draw(const GpuRegs& r, u32 count, u64 indices, u32 first);
std::string gpu_dispatch(const GpuRegs& r, u32 x, u32 y, u32 z);
std::vector<std::string> draw_report();
// Events (video.cpp): the kevent filters and idents the display and the GPU report
constexpr int EVENT_VIDEO_OUT = -13, EVENT_GRAPHICS = -14;
constexpr u64 VIDEO_OUT_FLIP = 6, GRAPHICS_EOP = 0x40;
void trigger_event(int filter, u64 ident, i64 data);  // to every queue that registered it
std::vector<std::string> video_report();  // and saves the buffer on screen to frame.bmp
u64 video_picture(std::vector<u32>& pixels, u32& width, u32& height);  // for the window
// printf's formatting, reading the arguments from a System V va_list at va (standins.cpp)
std::string format_sysv(const char* format, u64 va);
// Write to one of the game's FILE*s; false if it isn't one the loader opened (files.cpp)
bool stream_write(u64 file, const std::string& text);
void set_errno(int e);  // the calling game thread's errno (FreeBSD's values)
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
bool try_reserve(u64 addr, u64 size);  // the same, but says whether it could
void commit(u64 addr, u64 size);  // make reserved memory usable (read, write, execute)
// What committed memory may be used for: 0 nothing, 1 reading, 2 anything (as committed)
void protect(u64 addr, u64 size, int access);
// After loading: whatever the CPU needs before the game's code runs. Returns the
// displacement the thread-pointer loads are rewritten to read through gs.
u32 prepare();
void install_fault_handler();
void enter_thread(u64 tp);  // on each host thread before it runs game code
// A window showing the display (--window), on a thread of its own; false if this platform
// has none. Closing it ends the run.
bool open_window();
std::vector<std::string> window_report();  // how the window was drawn, for the summary
// The controller: the keys held in the window and an XInput gamepad (Windows); none elsewhere
PadInput pad_input();
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
