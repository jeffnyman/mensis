// SPDX-License-Identifier: GPL-3.0-or-later
//
// Linux: the game's thread pointer goes in fs, as on the PS4. The loader's own C library keeps
// its own fs, so mensis_import_common (enter_linux.S) swaps the two at every import call, with
// wrfsbase (allowed since Linux 5.9). Faults arrive as signals, which jump back out of the run.

#include <setjmp.h>
#include <signal.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <format>

#include "loader.hpp"

using namespace mensis;

extern "C" {
u64 mensis_host_fs = 0;  // read by enter_linux.S
[[noreturn]] void mensis_enter_game(u64 entry, u64 params, u64 exit, u64 stack, u64 fs);
}

namespace {

sigjmp_buf g_jump;

void on_signal(int sig, siginfo_t* si, void* ctx) {
    // The game's fs may be live: switch to ours before anything can use it.
    asm volatile("wrfsbase %0" ::"r"(mensis_host_fs));
    if (sig == SIGALRM && g_in_loader) {  // inside a stand-in: stop at the end of the call
        g_alarm = 1;
        return;
    }
    const auto* g = static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs;
    M.fault = {true, u64(g[REG_RIP]), u64(g[REG_RSP]), u64(g[REG_RBP]), u64(g[REG_RAX]),
               u64(g[REG_RDI]), u64(g[REG_RSI])};
    u64 addr = u64(si->si_addr);
    switch (sig) {
        case SIGSEGV: M.stop_reason = std::format("memory fault (SIGSEGV) at {:#x}", addr); break;
        case SIGBUS: M.stop_reason = std::format("bus error (SIGBUS) at {:#x}", addr); break;
        case SIGILL: M.stop_reason = std::format("invalid instruction (SIGILL) at {:#x}", addr); break;
        case SIGFPE: M.stop_reason = std::format("arithmetic fault (SIGFPE) at {:#x}", addr); break;
        case SIGTRAP: M.stop_reason = "breakpoint (SIGTRAP)"; break;
        case SIGALRM: M.stop_reason = std::format("time limit ({} s) reached", M.seconds); break;
        default: M.stop_reason = std::format("signal {}", sig);
    }
    siglongjmp(g_jump, 1);
}

}  // namespace

namespace mensis::platform {

void reserve(u64 addr, u64 size) {
    // Linux commits on first touch, so reserving maps the whole range at once.
    void* p = mmap(reinterpret_cast<void*>(addr), size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (p != reinterpret_cast<void*>(addr))
        die(std::format("can't map {:#x} bytes at {:#x} (already in use?): {}", size, addr,
                        std::strerror(errno)));
}

void commit(u64, u64) {}

void prepare() {
    constexpr unsigned long HWCAP2_FSGSBASE_BIT = 1ul << 1;
    if (!(getauxval(AT_HWCAP2) & HWCAP2_FSGSBASE_BIT))
        die("this CPU or kernel doesn't allow wrfsbase (FSGSBASE); Linux 5.9 or later is needed");
    asm volatile("rdfsbase %0" : "=r"(mensis_host_fs));
    M.notes.push_back("Thread pointer: in fs, swapped with the loader's own at each call.");
}

void run(u64 entry, u64 params, u64 exit, u64 sp, u64 tp) {
    static unsigned char altstack[1 << 16];
    stack_t ss{};
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGALRM}) sigaction(sig, &sa, nullptr);
    if (sigsetjmp(g_jump, 1) == 0) {
        alarm(M.seconds);
        mensis_enter_game(entry, params, exit, sp, tp);
    }
    alarm(0);
}

void leave() { siglongjmp(g_jump, 1); }

// The instructions at rip, through objdump, read without faulting if rip isn't mapped.
std::vector<std::string> disassemble(u64 rip) {
    unsigned char buf[48];
    iovec local{buf, sizeof buf}, remote{reinterpret_cast<void*>(rip), sizeof buf};
    ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (n <= 0) return {"  (rip is not in mapped memory)"};
    char path[] = "/tmp/mensis-code-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0 || write(fd, buf, size_t(n)) != n) return {"  (can't write a temporary file)"};
    close(fd);
    std::vector<std::string> out;
    std::string cmd = std::format(
        "objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma={:#x} {} 2>/dev/null", rip,
        path);
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char line[512];
        while (out.size() < 6 && std::fgets(line, sizeof line, p)) {
            // "  143279b:<tab>c4 e3 61 4a de 60 <tab>vblendvps xmm3,xmm3,xmm4,xmm6"
            std::string s(line);
            auto colon = s.find(":\t");
            auto tab = s.find('\t', colon + 2);
            if (colon == std::string::npos || tab == std::string::npos) continue;
            auto first = s.find_first_not_of(' ');
            std::string text = s.substr(tab + 1);
            while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
            out.push_back("  0x" + s.substr(first, colon - first) + " " + text);
        }
        pclose(p);
    }
    unlink(path);
    if (out.empty()) out.push_back("  (objdump not available)");
    return out;
}

}  // namespace mensis::platform
