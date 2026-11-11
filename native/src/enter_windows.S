# SPDX-License-Identifier: GPL-3.0-or-later
#
# The crossings between the game and the loader, on Windows.
#
# The game calls with the System V convention (arguments in rdi, rsi, rdx, rcx, r8, r9); the
# loader is compiled for Windows' (first argument in rcx, 32 bytes of shadow space). The game
# never calls the loader's C++ directly: each import arrives here, the arguments are gathered
# into a Frame on the game's stack, and the Frame's address goes to mensis_dispatch in rcx.
# Windows' callee-saved registers include all of System V's, so the game's are safe.
#
# The game's thread pointer needs nothing here: its fs loads were rewritten to read a Windows
# TLS slot through gs (platform_windows.cpp).

    .intel_syntax noprefix
    .text

# struct Frame (loader.hpp):
#     0 args[6]   48 index   56 caller   64 game_fs (unused)   72 ret2 (-> rdx)
#    80 xmm0     96 xmm1    112 fret (-> xmm0)
    .globl mensis_import_common
mensis_import_common:
    push rbp
    mov rbp, rsp
    sub rsp, 128
    and rsp, -16
    movdqa [rsp + 80], xmm0
    movdqa [rsp + 96], xmm1
    pxor xmm0, xmm0
    movdqa [rsp + 112], xmm0
    mov qword ptr [rsp + 72], 0
    mov qword ptr [rsp + 64], 0
    mov [rsp + 0], rdi
    mov [rsp + 8], rsi
    mov [rsp + 16], rdx
    mov [rsp + 24], rcx
    mov [rsp + 32], r8
    mov [rsp + 40], r9
    mov [rsp + 48], r11
    mov rax, [rbp + 8]
    mov [rsp + 56], rax
    mov rcx, rsp
    sub rsp, 32
    call mensis_dispatch
    add rsp, 32
    mov rdx, [rsp + 72]
    movdqa xmm0, [rsp + 112]
    leave
    ret

# void mensis_enter_game(u64 entry, u64 params, u64 exit_handler, u64 stack)
#
# Saves the loader's callee-saved registers and the thread's stack bounds (in the TEB at gs:8
# and gs:16, which Windows checks when it handles an exception), switches to the game's stack
# and bounds, and jumps to entry with the System V arguments. Returns only through mensis_bail.
#
# Saved block, at mensis_host_rsp: 0..159 xmm6-xmm15, 160 stack base, 168 stack limit,
# then the eight pushed registers and the return address.
    .globl mensis_enter_game
mensis_enter_game:
    push rbx
    push rbp
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 184
    movdqu [rsp + 0], xmm6
    movdqu [rsp + 16], xmm7
    movdqu [rsp + 32], xmm8
    movdqu [rsp + 48], xmm9
    movdqu [rsp + 64], xmm10
    movdqu [rsp + 80], xmm11
    movdqu [rsp + 96], xmm12
    movdqu [rsp + 112], xmm13
    movdqu [rsp + 128], xmm14
    movdqu [rsp + 144], xmm15
    mov rax, qword ptr gs:[0x8]
    mov [rsp + 160], rax
    mov rax, qword ptr gs:[0x10]
    mov [rsp + 168], rax
    mov [rip + mensis_host_rsp], rsp
    mov rax, [rip + mensis_game_stack_base]
    mov qword ptr gs:[0x8], rax
    mov rax, [rip + mensis_game_stack_limit]
    mov qword ptr gs:[0x10], rax
    mov rax, rcx
    mov rdi, rdx
    mov rsi, r8
    mov rsp, r9
    xor ebp, ebp
    jmp rax

# Back to the loader's stack and state, returning from mensis_enter_game. Reached by a call
# from a stand-in (stop), or by an exception handler or the watchdog setting rip to it.
    .globl mensis_bail
mensis_bail:
    mov rsp, [rip + mensis_host_rsp]
    mov rax, [rsp + 160]
    mov qword ptr gs:[0x8], rax
    mov rax, [rsp + 168]
    mov qword ptr gs:[0x10], rax
    movdqu xmm6, [rsp + 0]
    movdqu xmm7, [rsp + 16]
    movdqu xmm8, [rsp + 32]
    movdqu xmm9, [rsp + 48]
    movdqu xmm10, [rsp + 64]
    movdqu xmm11, [rsp + 80]
    movdqu xmm12, [rsp + 96]
    movdqu xmm13, [rsp + 112]
    movdqu xmm14, [rsp + 128]
    movdqu xmm15, [rsp + 144]
    add rsp, 184
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbp
    pop rbx
    ret
