# SPDX-License-Identifier: GPL-3.0-or-later
#
# The crossings between the game and the loader, on Linux.
#
# The game calls with the System V convention, Linux's own. Each import arrives at
# mensis_import_common (through its stub and thunk, with its index in r11), which gathers the
# arguments into a Frame on the game's stack and hands its address to mensis_dispatch.
#
# The game's thread pointer is in gs (its fs loads were rewritten to read gs:[0]); the loader
# never uses gs, so nothing needs swapping.

    .intel_syntax noprefix
    .text

# struct Frame (loader.hpp):
#     0 args[6]   48 index   56 caller   64 unused   72 ret2 (-> rdx)
#    80 xmm0     96 xmm1    112 fret (-> xmm0)
    .globl mensis_import_common
    .type mensis_import_common, @function
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
    mov rdi, rsp
    call mensis_dispatch
    mov rdx, [rsp + 72]
    movdqa xmm0, [rsp + 112]
    leave
    ret
    .size mensis_import_common, . - mensis_import_common

# u64 mensis_enter_game(EnterArgs *a)
#     EnterArgs: 0 entry, 8 arg0, 16 arg1, 24 stack_top, 32 stack_limit, 40 saved_rsp
#
# Saves the loader's callee-saved registers and records where (a->saved_rsp), then calls
# entry(arg0, arg1) on the game's stack. The game's frame starts at stack_top - 24, eight bytes
# off 16-byte alignment as after a call: [0] the return address, mensis_game_returned, and [8]
# the loader's stack pointer, for that return.
    .globl mensis_enter_game
    .type mensis_enter_game, @function
mensis_enter_game:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov [rdi + 40], rsp
    mov r11, rsp
    mov rax, [rdi + 24]
    lea rsp, [rax - 24]
    lea rcx, [rip + mensis_game_returned]
    mov [rsp], rcx
    mov [rsp + 8], r11
    mov rax, [rdi]
    mov rsi, [rdi + 16]
    mov rdi, [rdi + 8]
    xor ebp, ebp
    jmp rax
    .size mensis_enter_game, . - mensis_enter_game

# Back to the loader from anywhere in the game (a stand-in that stops the run or ends the
# thread, or a fault handler setting rip here): mensis_enter_game returns 0.
    .globl mensis_restore
    .type mensis_restore, @function
mensis_restore:
    mov rsp, rdi
    xor eax, eax
    jmp 1f
    .size mensis_restore, . - mensis_restore

# Where the game's function returns to: mensis_enter_game returns its result.
    .globl mensis_game_returned
    .type mensis_game_returned, @function
mensis_game_returned:
    mov rsp, [rsp]
1:
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret
    .size mensis_game_returned, . - mensis_game_returned

    .section .note.GNU-stack, "", @progbits
