// SPDX-License-Identifier: GPL-3.0-or-later
//
// The native loader: Bloodborne's start-up run directly on the CPU, every call into the system
// logged (docs/the-loader.md).
//
//     mensis-loader <prepared.elf> <symbols.tsv> <out dir> [--seconds N] [--calls N]
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
// * The game's thread pointer: TLS variant II, as in tools/loader.py. How the game reaches it
//   differs by platform (platform_linux.cpp, platform_windows.cpp).
// * The stand-ins (standins.cpp): tools/loader.py's, and more. Every other import returns 0
//   and is counted as unhandled. Threads are recorded, not run.
//
// It stops at the first GPU submission, at the exit handler, at a fault with the registers and
// the instructions there, after --calls import calls, or after --seconds (default 10).

#include <chrono>
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
volatile int g_in_loader = 0, g_alarm = 0;

void die(const std::string& what) {
    std::cerr << "mensis-loader: " << what << "\n";
    std::exit(1);
}
}  // namespace mensis

namespace {

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

void log_call(const Frame* f, const std::string& name, const std::string& tail) {
    std::string line = std::format("{:6} 0x{:08x} {}(0x{:x}, 0x{:x}, 0x{:x}, 0x{:x}) {}",
                                   M.calls.size(), f->caller, name, f->args[0], f->args[1],
                                   f->args[2], f->args[3], tail);
    M.calls.push_back(line);
    M.last_calls.push_back(line);
    if (M.last_calls.size() > 12) M.last_calls.pop_front();
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

void map(u64 addr, u64 size) {
    platform::reserve(addr, size);
    platform::commit(addr, size);
}

// The stack, heap, thread pointer and entry parameters, at tools/loader.py's addresses.
// Returns the thread pointer.
u64 setup() {
    map(STACK_TOP - STACK_SIZE, STACK_SIZE);
    platform::reserve(HEAP_BASE, HEAP_RESERVE);
    // TLS, variant II: [image][thread pointer -> self]
    map(TLS_BASE, 0x10000);
    u64 block = up(M.tls.memsz, M.tls.align);
    u64 tp = TLS_BASE + block + 0x100;
    if (M.tls.filesz)
        std::memcpy(reinterpret_cast<void*>(tp - block), reinterpret_cast<void*>(M.tls.vaddr),
                    M.tls.filesz);
    *reinterpret_cast<u64*>(tp) = tp;
    // entry parameters: { int argc; int pad; const char *argv[3]; }, the exit handler and
    // LIBC_DATA
    map(PARAMS, 0x10000);
    init_libc_data();
    std::memcpy(reinterpret_cast<char*>(PARAMS + 0x100), "eboot.bin", 10);
    struct { std::int32_t argc, pad; u64 argv[3]; } params = {1, 0, {PARAMS + 0x100, 0, 0}};
    std::memcpy(reinterpret_cast<void*>(PARAMS), &params, sizeof params);
    write_thunk(EXIT_STUB, u32(M.imports.size()));
    return tp;
}

void write_outputs(const std::string& dir, double seconds) {
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir + "/calls.log", std::ios::binary);
        for (const auto& line : M.calls) f << line << "\n";
    }
    std::ofstream(dir + "/stdout.txt", std::ios::binary) << M.stdout_text;
    u64 unhandled_calls = 0;
    for (const auto& [n, c] : M.unhandled) unhandled_calls += c;
    std::vector<std::string> s = {
        "# Native loader run\n",
        std::format("Stopped: {}. {} import calls, {} distinct; {} calls to {} imports without a "
                    "stand-in; {} threads asked for, not run. Ran for {:.2f} s.\n",
                    M.stop_reason, M.calls.size(), M.counts.size(), unhandled_calls,
                    M.unhandled.size(), M.threads.size(), seconds),
    };
    for (const auto& n : M.notes) s.push_back(n + "\n");
    s.insert(s.end(), {"## Where it stopped\n", "```"});
    if (M.fault.valid) {
        const auto& g = M.fault;
        s.push_back(std::format("rip {:#x}, rsp {:#x}, rbp {:#x}, rax {:#x}, rdi {:#x}, rsi {:#x}",
                                g.rip, g.rsp, g.rbp, g.rax, g.rdi, g.rsi));
        for (auto& line : platform::disassemble(g.rip)) s.push_back(line);
    } else {
        s.push_back("(in the loader, during the last call below)");
    }
    s.insert(s.end(), {"```\n", "Last calls:\n", "```"});
    s.insert(s.end(), M.last_calls.begin(), M.last_calls.end());
    s.insert(s.end(), {"```\n", "## Imports by first use\n", "| Import | Calls | Stand-in |",
                       "|---|---:|---|"});
    for (const auto& n : M.first_use)
        s.push_back(std::format("| {} | {} | {} |", n, M.counts[n],
                                standins().contains(n) ? "yes" : ""));
    std::ofstream f(dir + "/summary.md", std::ios::binary);
    for (const auto& line : s) f << line << "\n";
    for (size_t i = 0; i < 2 && i < s.size(); i++) std::cout << s[i] << "\n";
}

}  // namespace

namespace mensis {

u64 alloc(u64 size, u64 align) {
    size = std::max<u64>(size, 1);
    u64 addr = up(M.heap_top, std::max<u64>(align, 16));
    if (addr + size > HEAP_BASE + HEAP_RESERVE) stop("heap reservation used up");
    while (addr + size > M.heap_committed) {
        platform::commit(M.heap_committed, HEAP_STEP);
        M.heap_committed += HEAP_STEP;
    }
    M.heap_top = addr + size;
    M.allocs[addr] = size;
    return addr;
}

void stop(const std::string& reason) {
    M.stop_reason = reason;
    platform::leave();
}

}  // namespace mensis

extern "C" i64 mensis_dispatch(Frame* f) {
    g_in_loader = 1;
    if (f->index >= M.imports.size())
        stop(std::format("exit handler called with {:#x}", f->args[0]));
    const std::string& name = M.imports[f->index].name;
    if (M.max_calls && M.calls.size() >= M.max_calls)
        stop(std::format("call budget ({}) used up", M.max_calls));
    if (M.counts[name]++ == 0) M.first_use.push_back(name);
    if (is_submit(name)) {
        log_call(f, name, "stop");
        stop("first GPU submission: " + name);
    }
    std::string note;
    i64 ret = 0;
    M.frame = f;
    if (Standin fn = M.standin[f->index]) {
        ret = fn(f->args, note);
    } else {
        M.unhandled[name]++;
        note = "unhandled";
    }
    log_call(f, name, "-> " + hex(ret) + (note.empty() ? "" : " " + note));
    g_in_loader = 0;
    if (g_alarm) stop(std::format("time limit ({} s) reached", M.seconds));
    return ret;
}

int main(int argc, char** argv) {
    std::vector<std::string> pos;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if ((a == "--seconds" || a == "--calls") && i + 1 < argc) {
            u64 v = std::stoull(argv[++i]);
            (a == "--seconds" ? (void)(M.seconds = unsigned(v)) : (void)(M.max_calls = v));
        } else {
            pos.push_back(a);
        }
    }
    if (pos.size() != 3) {
        std::cerr << "usage: mensis-loader <prepared.elf> <symbols.tsv> <out dir> "
                     "[--seconds N] [--calls N]\n";
        return 2;
    }

    load_symbols(pos[1]);
    load_elf(pos[0]);
    patch_imports();
    u64 tp = setup();
    platform::prepare();

    // _start is an ordinary function: it expects to have been called, with a return address on
    // top of a 16-byte-aligned stack (its AVX code faults on a misaligned one). The exit
    // handler serves as that return address.
    u64 sp = STACK_TOP - 0x1000 - 8;
    *reinterpret_cast<u64*>(sp) = EXIT_STUB;

    auto t0 = std::chrono::steady_clock::now();
    platform::run(M.entry, PARAMS, EXIT_STUB, sp, tp);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    write_outputs(pos[2], secs);
    return 0;
}
