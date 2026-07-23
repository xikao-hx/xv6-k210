    .section .text.entry
    .globl _start
_start:
    add t0, a0, 1
    slli t0, t0, 14
    la sp, boot_stack
    add sp, sp, t0

    // The raw K210 image does not carry the NOBITS .bss payload.
    // Clear it before C code uses zero-initialized state. Only hart 0
    // performs the clear; other harts wait without touching the stack.
    bnez a0, wait_bss
    la t1, __bss_start
    la t2, __bss_end
clear_bss:
    bgeu t1, t2, bss_cleared
    sd zero, 0(t1)
    addi t1, t1, 8
    j clear_bss

bss_cleared:
    fence rw, rw
    la t1, bss_ready
    li t2, 1
    sw t2, 0(t1)
    fence rw, rw
    j enter_main

wait_bss:
    la t1, bss_ready
wait_bss_loop:
    lw t2, 0(t1)
    beqz t2, wait_bss_loop
    fence rw, rw

enter_main:
    call main

loop:
    j loop

    .section .data
    .align 2
bss_ready:
    .word 0

    .section .bss.stack
    .align 12
    .globl boot_stack
boot_stack:
    .space 4096 * 4 * 2
    .globl boot_stack_top
boot_stack_top:
