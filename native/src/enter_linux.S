# SPDX-License-Identifier: GPL-3.0-or-later
#
# The two places control crosses between the game and the loader.
#
# Each import stub (8 bytes at 0x5ae0000 + 8*i) is patched to jump to a per-import thunk that
# puts the import's index in r11 and jumps here. The game reached the stub with a call, so the
# return address is on top of its stack and the arguments are in rdi, rsi, rdx, rcx, r8, r9:
# the System V convention, which is Linux's own, so the stand-ins are ordinary C++ functions.
# What differs is fs: the game's thread pointer is not ours, so it is swapped on the way in
# and back on the way out.

    .intel_syntax noprefix
    .text

# struct Frame (loader.hpp):
#     0 args[6]   48 index   56 caller   64 game_fs   72 ret2 (-> rdx)
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
    mov [rsp + 0], rdi
    mov [rsp + 8], rsi
    mov [rsp + 16], rdx
    mov [rsp + 24], rcx
    mov [rsp + 32], r8
    mov [rsp + 40], r9
    mov [rsp + 48], r11
    mov rax, [rbp + 8]
    mov [rsp + 56], rax
    rdfsbase rax
    mov [rsp + 64], rax
    mov rax, [rip + mensis_host_fs]
    wrfsbase rax
    mov rdi, rsp
    call mensis_dispatch
    mov rdx, [rsp + 72]
    movdqa xmm0, [rsp + 112]
    mov rcx, [rsp + 64]
    wrfsbase rcx
    leave
    ret
    .size mensis_import_common, . - mensis_import_common

# void mensis_enter_game(u64 entry, u64 params, u64 exit_handler, u64 stack, u64 fs)
# Never returns: the run ends with a siglongjmp back to main.
    .globl mensis_enter_game
    .type mensis_enter_game, @function
mensis_enter_game:
    mov rax, rdi
    mov rsp, rcx
    wrfsbase r8
    mov rdi, rsi
    mov rsi, rdx
    xor ebp, ebp
    jmp rax
    .size mensis_enter_game, . - mensis_enter_game

    .section .note.GNU-stack, "", @progbits
