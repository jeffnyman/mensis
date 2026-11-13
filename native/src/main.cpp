// SPDX-License-Identifier: GPL-3.0-or-later
//
// The native loader: Bloodborne's start-up run directly on the CPU, every call into the system
// logged (docs/the-loader.md).
//
//     mensis-loader <prepared.elf> <symbols.tsv> <out dir> [--seconds N] [--calls N]
//                   [--frames N] [--log N] [--window] [--no-draw] [--cpu] [--gpu-check] [--app0 DIR]...
//                   [--press F:BUTTONS[,F:BUTTONS]...]
//
// Where the Vulkan SDK was there to build with (Windows), the draws are drawn on the PC's GPU;
// --cpu draws them on the CPU instead, as the Linux build always does.
//
// --press holds the controller's buttons down at flip F (for 6 flips), for runs that go past a
// menu the same way each time: say --press 900:cross,960:down+cross. The buttons: cross,
// circle, square, triangle, up, down, left, right, l1, r1, l2, r2, l3, r3, options, touchpad. In
// the window (--window), the keyboard is the controller too, and so is an XInput gamepad
// (docs/the-loader.md has the keys).
//
// --app0 names a folder the game's own files (/app0/) are read from; give it more than once to
// lay folders over each other, first match wins. By default: work/update, then work/base (the
// 1.09 update's files over the base game's), where they exist.
//
// <prepared.elf> and <symbols.tsv> are orbis_prep.py's outputs (work/ghidra/eboot-1.09.ghidra.elf
// and .symbols.tsv). Writes <out dir>/calls.log, <out dir>/summary.md (in the same form as
// tools/loader.py, which runs the same start-up under a CPU emulator) and <out dir>/stdout.txt.
//
// The PS4's CPU is x86-64, so the game's code is mapped where it was linked to run and jumped
// into; AVX and everything else run on the real CPU. What the loader supplies:
//
// * Each import stub (one 8-byte slot per import, which orbis_prep pointed every GOT entry at)
//   is patched to jump to a small thunk, and from there to mensis_import_common (enter_*.S),
//   which gathers the arguments into a Frame and calls mensis_dispatch.
// * The game's thread pointer: TLS variant II, as in tools/loader.py, one block per thread.
//   The game reads it with one instruction, mov rax, fs:[0], which is rewritten to read through
//   gs instead (patch_thread_pointer_loads); each platform points gs at it its own way.
// * The stand-ins (standins.cpp, threads.cpp). Every other import returns 0 and is counted as
//   unhandled. The game's threads run, each on a thread of the loader's own.
//
// It stops at the exit handler, when the main thread's _start returns, at a fault (with the
// registers and the instructions there), after --calls import calls, after --frames flips of
// the display, or after --seconds (default 10).

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include "loader.hpp"

using namespace mensis;

namespace mensis {
Machine M;

void die(const std::string& what) {
    std::cerr << "mensis-loader: " << what << "\n";
    std::exit(1);
}
}  // namespace mensis

namespace {

thread_local GameThread* t_current = nullptr;
thread_local Frame* t_frame = nullptr;

// The two ELF structures the loader reads (64-bit, little-endian).
struct ElfHeader {
    unsigned char ident[16];
    std::uint16_t type, machine;
    std::uint32_t version;
    u64 entry, phoff, shoff;
    std::uint32_t flags;
    std::uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct ProgramHeader {
    std::uint32_t type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
};
static_assert(sizeof(ElfHeader) == 64 && sizeof(ProgramHeader) == 56);
constexpr std::uint32_t PT_LOAD = 1, PT_TLS = 7, PF_X = 1;

std::string hex(i64 v) {
    return v < 0 ? std::format("-0x{:x}", u64(0) - u64(v)) : std::format("0x{:x}", u64(v));
}

// Under M.log_lock. The main thread's calls are written as tools/loader.py writes them; another
// thread's carry its number: "[t3] name(...)".
std::string format_call(const Call& c) {
    std::string who = c.thread ? std::format("[t{}] ", c.thread) : "";
    return std::format("{:6} 0x{:08x} {}{}(0x{:x}, 0x{:x}, 0x{:x}, 0x{:x}) -> {}{}", c.n,
                       c.caller, who, M.imports[c.import].name, c.args[0], c.args[1], c.args[2],
                       c.args[3], hex(c.ret), c.note.empty() ? "" : " " + c.note);
}

// Under M.log_lock: a finished call, into the log.
void log_call(GameThread* t, const Frame* f, i64 ret, std::string&& note) {
    Call c{M.total_calls++, f->caller, {f->args[0], f->args[1], f->args[2], f->args[3]}, ret,
           u32(f->index), t ? t->id : 0, std::move(note)};
    if (c.n < M.log_limit) M.calls.push_back(format_call(c));
    if (t) {
        t->last = c;
        t->calls++;
    }
    M.recent[c.n % Machine::RECENT] = std::move(c);
}

// ---- loading ------------------------------------------------------------------------------

void load_symbols(const std::string& path) {
    std::ifstream f(path);
    if (!f) die("can't read " + path);
    std::map<u64, Import> by_addr;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> p;
        std::stringstream s(line);
        for (std::string x; std::getline(s, x, '\t');) p.push_back(x);
        if (p.size() < 3) continue;
        u64 addr = std::stoull(p[0], nullptr, 16);
        if (p[1] == "IMPORT")
            by_addr[addr] = Import{p[2], p.size() > 3 ? p[3] : "", addr};
        else if (p[1] == "ENTRY")
            M.entry = addr;
    }
    if (by_addr.empty() || !M.entry) die(path + " has no IMPORT or ENTRY rows");
    for (auto& [addr, imp] : by_addr) M.imports.push_back(imp);
    M.stubs_lo = M.imports.front().stub;
    M.stubs_hi = M.imports.back().stub + 8;
    const auto& table = standins();
    for (const auto& imp : M.imports) {
        auto it = table.find(imp.name);
        M.standin.push_back(it == table.end() ? nullptr : it->second);
    }
    size_t n = M.imports.size() + 1;  // and the exit handler
    M.counts = std::make_unique<std::atomic<u64>[]>(n);
    M.seen = std::make_unique<std::atomic<bool>[]>(n);
    M.unhandled.assign(n, 0);
    M.recent.resize(Machine::RECENT);
}

void load_elf(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) die("can't read " + path);
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < sizeof(ElfHeader) || std::memcmp(data.data(), "\177ELF", 4) != 0)
        die(path + " is not an ELF");
    auto* eh = reinterpret_cast<const ElfHeader*>(data.data());
    std::vector<const ProgramHeader*> loads;
    for (int i = 0; i < eh->phnum; i++) {
        auto* ph = reinterpret_cast<const ProgramHeader*>(data.data() + eh->phoff +
                                                          u64(i) * eh->phentsize);
        if (ph->type == PT_LOAD) loads.push_back(ph);
        if (ph->type == PT_TLS)
            M.tls = {ph->vaddr, ph->filesz, ph->memsz, std::max<u64>(ph->align, 16)};
    }
    if (loads.empty()) die(path + " has nothing to load");
    // One reservation for the whole image (Windows reserves in 64 KiB units, and the segments
    // aren't 64 KiB apart), then each segment made usable and filled.
    u64 lo = ~u64(0), hi = 0;
    for (auto* ph : loads) {
        lo = std::min(lo, ph->vaddr / 0x10000 * 0x10000);
        hi = std::max(hi, up(ph->vaddr + ph->memsz, 0x10000));
    }
    platform::reserve(lo, hi - lo);
    for (auto* ph : loads) {
        u64 a = ph->vaddr / PAGE * PAGE;
        platform::commit(a, up(ph->vaddr + ph->memsz) - a);
        std::memcpy(reinterpret_cast<void*>(ph->vaddr), data.data() + ph->offset, ph->filesz);
        if (ph->flags & PF_X) M.code.emplace_back(ph->vaddr, ph->vaddr + ph->filesz);
        M.image.emplace_back(ph->vaddr, ph->vaddr + ph->memsz);
    }
}

// mov r11d, index; movabs rax, mensis_import_common; jmp rax
void write_thunk(u64 at, u32 index) {
    auto* p = reinterpret_cast<unsigned char*>(at);
    u64 target = reinterpret_cast<u64>(&mensis_import_common);
    p[0] = 0x41, p[1] = 0xBB;
    std::memcpy(p + 2, &index, 4);
    p[6] = 0x48, p[7] = 0xB8;
    std::memcpy(p + 8, &target, 8);
    p[16] = 0xFF, p[17] = 0xE0;
}

void patch_imports() {
    // the thunks go just past the stubs, so each 8-byte stub can reach its own with a jmp rel32
    M.thunks = up(M.stubs_hi + PAGE, 0x10000);
    u64 n = M.imports.size();
    u64 size = up((n + 1) * THUNK_SIZE, 0x10000);
    platform::reserve(M.thunks, size);
    platform::commit(M.thunks, size);
    for (u64 i = 0; i < n; i++) {
        u64 thunk = M.thunks + i * THUNK_SIZE, stub = M.imports[i].stub;
        write_thunk(thunk, u32(i));
        auto* s = reinterpret_cast<unsigned char*>(stub);
        auto rel = std::int32_t(i64(thunk) - i64(stub + 5));
        s[0] = 0xE9;
        std::memcpy(s + 1, &rel, 4);
    }
}

// The game reads its thread pointer with one instruction, in 17,127 places:
//
//     64 48 8B 04 25 00 00 00 00      mov rax, qword ptr fs:[0]
//
// (under TLS variant II, fs:[0] holds the thread pointer itself). Each becomes, at the same
// length, a read through gs at the displacement the platform chose:
//
//     65 48 8B 04 25 <disp32>         mov rax, qword ptr gs:[disp]
u64 patch_thread_pointer_loads(u32 disp) {
    static const unsigned char load_fs[9] = {0x64, 0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0};
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
    return patched;
}

void map(u64 addr, u64 size) {
    platform::reserve(addr, size);
    platform::commit(addr, size);
}

// The main stack, heap, TLS area, entry parameters, thread stacks and sync objects.
void setup() {
    map(STACK_TOP - STACK_SIZE, STACK_SIZE);
    bool heap = false;
    for (u64 base : HEAP_BASES)
        if (platform::try_reserve(base, HEAP_RESERVE)) {
            M.heap_base = M.heap_top = M.heap_committed = base;
            heap = true;
            break;
        }
    if (!heap) platform::reserve(HEAP_BASE, HEAP_RESERVE);  // says what is in the way
    if (M.heap_base != HEAP_BASE)
        M.notes.push_back(std::format("Heap: at {:#x} ({:#x} was taken).", M.heap_base, HEAP_BASE));
    platform::reserve(TLS_BASE, TLS_SLOT * MAX_THREADS);
    platform::reserve(THREAD_STACKS, THREAD_STACK_SLOT * MAX_THREADS);
    platform::reserve(SYNC_BASE, SYNC_RESERVE);
    // entry parameters: { int argc; int pad; const char *argv[3]; }, the exit handler and
    // LIBC_DATA
    map(PARAMS, 0x10000);
    init_libc_data();
    std::memcpy(reinterpret_cast<char*>(PARAMS + 0x100), "eboot.bin", 10);
    struct { std::int32_t argc, pad; u64 argv[3]; } params = {1, 0, {PARAMS + 0x100, 0, 0}};
    std::memcpy(reinterpret_cast<void*>(PARAMS), &params, sizeof params);
    write_thunk(EXIT_STUB, u32(M.imports.size()));
}

// Under M.log_lock.
void write_outputs(const std::string& reason) {
    const std::string& dir = M.out_dir;
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - M.t0).count();
    std::filesystem::create_directories(dir);
    {
        // the first log_limit calls, then the last RECENT of the rest
        std::ofstream f(dir + "/calls.log", std::ios::binary);
        for (const auto& line : M.calls) f << line << "\n";
        u64 from = std::max<u64>(M.calls.size(),
                                 M.total_calls > Machine::RECENT ? M.total_calls - Machine::RECENT : 0);
        if (from > M.calls.size())
            f << std::format("... {} calls not kept (--log {}) ...\n", from - M.calls.size(),
                             M.log_limit);
        for (u64 n = from; n < M.total_calls; n++)
            f << format_call(M.recent[n % Machine::RECENT]) << "\n";
    }
    std::ofstream(dir + "/stdout.txt", std::ios::binary) << M.stdout_text;
    u64 unhandled_calls = 0, unhandled_imports = 0;
    for (u64 c : M.unhandled) {
        unhandled_calls += c;
        unhandled_imports += c != 0;
    }
    std::vector<std::string> s = {
        "# Native loader run\n",
        std::format("Stopped: {}. {} import calls, {} distinct; {} calls to {} imports without a "
                    "stand-in; {} threads. Ran for {:.2f} s.\n",
                    reason, M.total_calls, M.first_use.size(), unhandled_calls, unhandled_imports,
                    M.threads.size(), seconds),
    };
    for (const auto& n : M.notes) s.push_back(n + "\n");
    for (const auto& line : video_report()) s.push_back(line);
    for (const auto& line : pad_report()) s.push_back(line);
    for (const auto& line : platform::window_report()) s.push_back(line);
    s.insert(s.end(), {"## Where it stopped\n", "```"});
    if (M.fault.valid) {
        const auto& g = M.fault;
        s.push_back(std::format("rip {:#x}, rsp {:#x}, rbp {:#x}, rax {:#x}, rdi {:#x}, rsi {:#x}",
                                g.rip, g.rsp, g.rbp, g.rax, g.rdi, g.rsi));
        for (auto& line : platform::disassemble(g.rip)) s.push_back(line);
        // The way back: the values on the stack that point into the game's code, nearest
        // first. Most are return addresses (some are leftovers); look each up in Ghidra.
        std::string back;
        int found = 0;
        bool on_stack = (g.rsp >= THREAD_STACKS && g.rsp < THREAD_STACKS + THREAD_STACK_SLOT * MAX_THREADS) ||
                        (g.rsp >= STACK_TOP - STACK_SIZE && g.rsp < STACK_TOP);
        for (u64 p = g.rsp; on_stack && found < 16 && p < g.rsp + 0x2000 && (p & 0xffff) != 0; p += 8) {
            u64 v = *reinterpret_cast<const u64*>(p);
            for (auto [lo, hi] : M.code)
                if (v >= lo && v < hi && v < M.stubs_lo) {
                    back += std::format(" {:#x}", v);
                    found++;
                }
        }
        if (!back.empty()) s.push_back("code addresses on the stack:" + back);
    } else {
        s.push_back("(not at a fault: see the threads and the last calls below)");
    }
    s.insert(s.end(), {"```\n", "## Threads\n", "| # | Name | Calls | State | Last call |",
                       "|---:|---|---:|---|---|"});
    for (const auto& t : M.threads) {
        std::string last = t->calls ? format_call(t->last) : "";
        std::replace(last.begin(), last.end(), '|', '/');
        auto first = last.find_first_not_of(' ');
        std::string state = t->state;
        u32 in = t->in_call.load();
        const Frame* fr = t->frame.load();
        if (in && fr && state == "running game code")  // in a call: which, and with what
            state = std::format("in {}({:#x}, {:#x}, {:#x})", M.imports[in - 1].name, fr->args[0],
                                fr->args[1], fr->args[2]);
        s.push_back(std::format("| {} | {} | {} | {} | `{}` |", t->id, t->name, t->calls,
                                state, first == std::string::npos ? "" : last.substr(first)));
    }
    s.push_back("");
    for (const auto& line : gpu_report()) s.push_back(line);
    for (const auto& line : draw_report()) s.push_back(line);
    s.insert(s.end(), {"Last calls:\n", "```"});
    for (u64 n = M.total_calls > 12 ? M.total_calls - 12 : 0; n < M.total_calls; n++)
        s.push_back(format_call(M.recent[n % Machine::RECENT]));
    s.insert(s.end(), {"```\n", "## Imports by first use\n", "| Import | Calls | Stand-in |",
                       "|---|---:|---|"});
    for (u32 i : M.first_use)
        s.push_back(std::format("| {} | {} | {} |", M.imports[i].name, M.counts[i].load(),
                                M.standin[i] ? "yes" : ""));
    std::ofstream f(dir + "/summary.md", std::ios::binary);
    for (const auto& line : s) f << line << "\n";
    f.close();
    for (size_t i = 0; i < 2 && i < s.size(); i++) std::cout << s[i] << "\n";
    std::cout.flush();
}

}  // namespace

namespace mensis {

Frame* current_frame() { return t_frame; }
GameThread* current_thread() { return t_current; }

u64 alloc(u64 size, u64 align) {
    size = std::max<u64>(size, 1);
    u64 addr;
    {
        std::lock_guard lock(M.heap_lock);
        addr = up(M.heap_top, std::max<u64>(align, 16));
        if (addr + size <= M.heap_base + HEAP_RESERVE) {
            while (addr + size > M.heap_committed) {
                platform::commit(M.heap_committed, HEAP_STEP);
                M.heap_committed += HEAP_STEP;
            }
            M.heap_top = addr + size;
            M.allocs[addr] = size;
            return addr;
        }
    }
    stop("heap reservation used up");
}

u64 sync_alloc(u64 size, u64 align) {
    static std::mutex lock;
    static u64 top = SYNC_BASE, committed = SYNC_BASE;
    std::lock_guard g(lock);
    u64 addr = up(top, std::max<u64>(align, 16));
    if (addr + size > SYNC_BASE + SYNC_RESERVE) die("the loader's object area is full");
    while (addr + size > committed) {
        platform::commit(committed, 0x100000);
        committed += 0x100000;
    }
    top = addr + size;
    return addr;
}

void stop(const std::string& reason) {
    GameThread* t = t_current;
    if (!t) finish(reason);
    t->stop_reason = reason;
    mensis_restore(t->enter.saved_rsp);
}

void finish(const std::string& reason) {
    static std::atomic<bool> finishing{false};
    if (finishing.exchange(true)) {  // another thread is writing the outputs; it will end us
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    {
        std::lock_guard lock(M.log_lock);
        write_outputs(reason);
    }
    std::_Exit(0);
}

GameThread* new_thread(const std::string& name, u64 entry, u64 arg0, u64 arg1, u64 stack_size) {
    std::lock_guard lock(M.log_lock);
    u32 id = u32(M.threads.size());
    if (id >= MAX_THREADS) die("too many threads");
    auto t = std::make_unique<GameThread>();
    t->id = id;
    t->name = name;
    t->handle = 0x1000 + id;
    // Its TLS block, variant II: [image][thread pointer -> self]
    u64 base = TLS_BASE + id * TLS_SLOT;
    platform::commit(base, TLS_SLOT);
    u64 block = up(M.tls.memsz, M.tls.align);
    t->tp = base + block + 0x100;
    if (M.tls.filesz)
        std::memcpy(reinterpret_cast<void*>(t->tp - block),
                    reinterpret_cast<void*>(M.tls.vaddr), M.tls.filesz);
    *reinterpret_cast<u64*>(t->tp) = t->tp;
    // Its stack: the main thread's is tools/loader.py's; each other gets its own 64 MiB slot,
    // with the size it asked for and a megabyte more for the loader's own code, which runs on
    // the game's stack during a call.
    u64 top, limit;
    if (id == 0) {
        top = STACK_TOP - 0x1000 + 16;  // so _start starts at STACK_TOP - 0x1000 - 8
        limit = STACK_TOP - STACK_SIZE;
    } else {
        u64 size = up(std::max<u64>(stack_size, 0x10000) + (1 << 20), 0x10000);
        size = std::min(size, THREAD_STACK_SLOT);
        top = THREAD_STACKS + u64(id) * THREAD_STACK_SLOT;
        limit = top - size;
        platform::commit(limit, size);
    }
    t->enter = {entry, arg0, arg1, top, limit, 0};
    M.threads.push_back(std::move(t));
    return M.threads.back().get();
}

void run_thread(GameThread* t) {
    t_current = t;
    platform::enter_thread(t->tp);
    {
        std::lock_guard lock(M.log_lock);
        t->state = "running game code";
    }
    u64 r = mensis_enter_game(&t->enter);
    // back on the loader's own stack
    if (t->faulted) {
        {
            std::lock_guard lock(M.log_lock);
            if (!M.fault.valid) M.fault = t->fault;
            t->state = "faulted";
        }
        finish(t->id ? std::format("{} in thread {} ({})", t->fault_reason, t->id, t->name)
                     : t->fault_reason);
    }
    if (!t->stop_reason.empty()) finish(t->stop_reason);
    if (t->id == 0) finish(std::format("_start returned {:#x}", r));
    {
        std::lock_guard lock(M.log_lock);
        t->state = t->exited ? "exited" : "returned";
    }
    std::lock_guard lock(t->m);
    if (!t->exited) t->result = r;
    t->finished = true;
    t->cv.notify_all();
}

}  // namespace mensis

extern "C" i64 mensis_dispatch(Frame* f) {
    GameThread* t = t_current;
    t_frame = f;
    if (f->index >= M.imports.size())
        stop(std::format("exit handler called with {:#x}", f->args[0]));
    // Most of a call happens without the lock: only an import's first call, and the log entry
    // once a call is done, take it.
    u32 index = u32(f->index);
    if (M.max_calls && M.started++ >= M.max_calls)
        stop(std::format("call budget ({}) used up", M.max_calls));
    M.counts[index]++;
    if (!M.seen[index].exchange(true)) {
        std::lock_guard lock(M.log_lock);
        M.first_use.push_back(index);
    }
    if (t) {
        t->frame = f;
        t->in_call = index + 1;
    }
    std::string note;
    i64 ret = 0;
    Standin fn = M.standin[index];
    if (fn)
        ret = fn(f->args, note);
    else
        note = "unhandled";
    if (t) {
        t->in_call = 0;
        t->frame = nullptr;
    }
    std::lock_guard lock(M.log_lock);
    if (!fn) M.unhandled[index]++;
    log_call(t, f, ret, std::move(note));
    return ret;
}

int main(int argc, char** argv) {
    std::vector<std::string> pos;
    bool window = false, seconds_given = false;
#ifdef MENSIS_VULKAN
    M.gpu = true;
#endif
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--window") {
            window = true;
        } else if (a == "--no-draw") {
            M.draw = false;
        } else if (a == "--gpu-check") {
            M.gpu_check = true;
        } else if (a == "--cpu") {
            M.gpu = false;
        } else if (a == "--press" && i + 1 < argc) {
            std::string list = argv[++i];
            for (size_t at = 0; at <= list.size();) {
                size_t end = std::min(list.find(',', at), list.size());
                std::string item = list.substr(at, end - at);
                size_t colon = item.find(':');
                u32 bits = 0;
                bool ok = colon != std::string::npos && colon > 0 &&
                          item.find_first_not_of("0123456789") == colon;
                for (size_t b = colon + 1; ok && b <= item.size();) {
                    size_t plus = std::min(item.find('+', b), item.size());
                    u32 bit = pad_button(item.substr(b, plus - b));
                    ok = bit != 0;
                    bits |= bit;
                    b = plus + 1;
                }
                if (!ok) {
                    std::cerr << "--press: \"" << item << "\" isn't frame:button[+button...]\n";
                    return 2;
                }
                M.presses.push_back({std::stoull(item.substr(0, colon)), bits});
                at = end + 1;
            }
        } else if (a == "--app0" && i + 1 < argc) {
            M.app0.push_back(argv[++i]);
        } else if ((a == "--seconds" || a == "--calls" || a == "--frames" || a == "--log") &&
                   i + 1 < argc) {
            u64 v = std::stoull(argv[++i]);
            if (a == "--seconds") M.seconds = unsigned(v), seconds_given = true;
            else if (a == "--calls") M.max_calls = v;
            else if (a == "--log") M.log_limit = v;
            else M.max_flips = v;
        } else {
            pos.push_back(a);
        }
    }
    if (pos.size() != 3) {
        std::cerr << "usage: mensis-loader <prepared.elf> <symbols.tsv> <out dir> "
                     "[--seconds N] [--calls N] [--frames N] [--log N] [--window] [--no-draw] [--cpu] [--gpu-check] [--app0 DIR]... "
                     "[--press F:BUTTONS[,F:BUTTONS]...]\n";
        return 2;
    }
    M.out_dir = pos[2];
    if (M.app0.empty())
        for (const char* d : {"work/update", "work/base"})
            if (std::filesystem::is_directory(d)) M.app0.push_back(d);
    std::string folders;
    for (const auto& d : M.app0) folders += (folders.empty() ? "" : ", then ") + d;
    M.notes.push_back("/app0/: " + (folders.empty() ? std::string("(no folders)") : folders) +
                      ".");

    load_symbols(pos[1]);
    load_elf(pos[0]);
    patch_imports();
    setup();
    u32 disp = platform::prepare();
    u64 patched = patch_thread_pointer_loads(disp);
    M.notes.push_back(std::format(
        "Thread pointer: {} loads of fs:[0] rewritten to read gs:[{:#x}].", patched, disp));
    platform::install_fault_handler();

    // _start(params, exit handler): an ordinary function, called on a 16-byte-aligned stack.
    GameThread* main_thread = new_thread("main", M.entry, PARAMS, EXIT_STUB, 0);

    // With a window, the run goes on until it is closed, unless --seconds says otherwise.
    if (window && !seconds_given) M.seconds = 0;
    if (window && !platform::open_window())
        M.notes.push_back("--window: there is no window on this platform yet; frame.bmp shows the screen at the end.");
    M.t0 = std::chrono::steady_clock::now();
    if (M.seconds)
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(M.seconds));
            finish(std::format("time limit ({} s) reached", M.seconds));
        }).detach();
    run_thread(main_thread);  // never returns: the main thread's end finishes the run
    return 0;
}
