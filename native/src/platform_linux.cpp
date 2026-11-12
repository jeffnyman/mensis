// SPDX-License-Identifier: GPL-3.0-or-later
//
// Linux: the game's thread-pointer loads were rewritten to read gs:[0], and each game thread's
// gs base is set to its thread pointer (wrgsbase, allowed since Linux 5.9). The loader itself
// never uses gs, and keeps fs for its own C library. A fault arrives as a signal, on a stack of
// the thread's own; the handler records it and resumes the thread in mensis_restore, back on the
// loader's stack.

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

namespace {

void on_signal(int sig, siginfo_t* si, void* ctx) {
    GameThread* t = current_thread();
    auto* uc = static_cast<ucontext_t*>(ctx);
    auto* g = uc->uc_mcontext.gregs;
    if (!t || !t->enter.saved_rsp) {  // the loader's own fault: nothing to return to
        signal(sig, SIG_DFL);
        return;
    }
    u64 addr = u64(si->si_addr);
    switch (sig) {
        case SIGSEGV: t->fault_reason = std::format("memory fault (SIGSEGV) at {:#x}", addr); break;
        case SIGBUS: t->fault_reason = std::format("bus error (SIGBUS) at {:#x}", addr); break;
        case SIGILL: t->fault_reason = std::format("invalid instruction (SIGILL) at {:#x}", addr); break;
        case SIGFPE: t->fault_reason = std::format("arithmetic fault (SIGFPE) at {:#x}", addr); break;
        case SIGTRAP: t->fault_reason = "breakpoint (SIGTRAP)"; break;
        default: t->fault_reason = std::format("signal {}", sig);
    }
    t->fault = {true, u64(g[REG_RIP]), u64(g[REG_RSP]), u64(g[REG_RBP]), u64(g[REG_RAX]),
                u64(g[REG_RDI]), u64(g[REG_RSI])};
    t->faulted = true;
    g[REG_RIP] = i64(reinterpret_cast<u64>(&mensis_restore));
    g[REG_RDI] = i64(t->enter.saved_rsp);
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

bool try_reserve(u64 addr, u64 size) {
    void* p = mmap(reinterpret_cast<void*>(addr), size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (p == reinterpret_cast<void*>(addr)) return true;
    if (p != MAP_FAILED) munmap(p, size);  // an old kernel placed it elsewhere
    return false;
}

void commit(u64, u64) {}

u32 prepare() {
    constexpr unsigned long HWCAP2_FSGSBASE_BIT = 1ul << 1;
    if (!(getauxval(AT_HWCAP2) & HWCAP2_FSGSBASE_BIT))
        die("this CPU or kernel doesn't allow wrgsbase (FSGSBASE); Linux 5.9 or later is needed");
    return 0;  // gs:[0]: gs is the thread pointer, and [tp] holds the thread pointer
}

void install_fault_handler() {
    struct sigaction sa{};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP}) sigaction(sig, &sa, nullptr);
}

void enter_thread(u64 tp) {
    // a stack for the signal handler, as the game's may be the trouble
    stack_t ss{};
    ss.ss_size = 1 << 18;
    ss.ss_sp = new unsigned char[ss.ss_size];
    sigaltstack(&ss, nullptr);
    asm volatile("wrgsbase %0" ::"r"(tp));
}

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
