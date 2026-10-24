/*
 * int regs_torture(uint32_t seed, uint32_t loops)
 *
 * Context-switch integrity test. Puts a distinct value derived from `seed`
 * into every callee-saved integer register (r4-r11) and every FPU register
 * (s0-s31), spins for `loops` iterations (being preempted many times by
 * SysTick and other tasks running the same test), then checks every value.
 * If PendSV ever failed to save/restore a register - or the lazy FPU
 * stacking were mishandled - one of these checks fails.
 *
 * Returns 0 if all registers survived, 1 otherwise.
 */
    .syntax unified
    .thumb
    .section .text.regs_torture, "ax", %progbits
    .global regs_torture
    .type regs_torture, %function
    .thumb_func
regs_torture:
    push    {r4-r11, lr}
    vpush   {s16-s31}

    add     r4, r0, #1
    add     r5, r0, #2
    add     r6, r0, #3
    add     r7, r0, #4
    add     r8, r0, #5
    add     r9, r0, #6
    add     r10, r0, #7
    add     r11, r0, #8
    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    add     r2, r0, #(16 + \n)
    vmov    s\n, r2
    .endr

1:  subs    r1, r1, #1
    bne     1b

    .irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    vmov    r2, s\n
    add     r3, r0, #(16 + \n)
    cmp     r2, r3
    bne     2f
    .endr
    add     r3, r0, #1
    cmp     r4, r3
    bne     2f
    add     r3, r0, #2
    cmp     r5, r3
    bne     2f
    add     r3, r0, #3
    cmp     r6, r3
    bne     2f
    add     r3, r0, #4
    cmp     r7, r3
    bne     2f
    add     r3, r0, #5
    cmp     r8, r3
    bne     2f
    add     r3, r0, #6
    cmp     r9, r3
    bne     2f
    add     r3, r0, #7
    cmp     r10, r3
    bne     2f
    add     r3, r0, #8
    cmp     r11, r3
    bne     2f
    movs    r0, #0
    b       3f
2:  movs    r0, #1
3:  vpop    {s16-s31}
    pop     {r4-r11, pc}
    .size regs_torture, . - regs_torture
