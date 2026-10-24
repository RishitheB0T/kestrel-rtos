/*
 * context.S - the only assembly in the kernel: entering and leaving tasks.
 *
 * When any exception is taken, the CPU *automatically* pushes r0-r3, r12,
 * lr, pc and xPSR onto the interrupted task's stack (plus s0-s15 and FPSCR
 * if the task was using the FPU). That is half of a task's state. To switch
 * tasks we save the other half (r4-r11, s16-s31 and a few control bits)
 * ourselves, swap stack pointers, and restore the other task's half. The
 * CPU then pops the first half on exception return.
 *
 * Saved software frame, lowest address first:
 *     CONTROL(nPRIV bit), r4, r5, r6, r7, r8, r9, r10, r11, EXC_RETURN
 *     [s16-s31 above it, only if EXC_RETURN bit 4 is 0 = FPU in use]
 */
    .syntax unified
    .thumb

/* ------------------------------------------------------------------------
 * PendSV: the context switch. It has the lowest priority, so it only runs
 * once every other interrupt has finished - switches never nest.
 * ------------------------------------------------------------------------ */
    .section .text.PendSV_Handler, "ax", %progbits
    .global PendSV_Handler
    .type PendSV_Handler, %function
    .thumb_func
PendSV_Handler:
    cpsid   i
    ldr     r3, =k_current
    ldr     r2, [r3]
    cbz     r2, 1f                  /* no current task (boot, or it died):
                                       its context is thrown away          */
    mrs     r0, psp
    isb
    tst     lr, #0x10               /* EXC_RETURN bit 4 == 0 -> FP frame    */
    it      eq
    vstmdbeq r0!, {s16-s31}         /* also triggers any lazy s0-s15 save   */
    mrs     r1, control
    and     r1, r1, #1              /* keep only nPRIV                      */
    stmdb   r0!, {r1, r4-r11, lr}
    b       2f
1:  movs    r0, #0
2:  bl      k_switch                /* r0 = old sp -> r0 = new sp           */

    ldmia   r0!, {r1, r4-r11, lr}
    mrs     r2, control
    bic     r2, r2, #1
    orr     r2, r2, r1
    msr     control, r2             /* privilege level of the next task     */
    tst     lr, #0x10
    it      eq
    vldmiaeq r0!, {s16-s31}
    msr     psp, r0
    isb
    cpsie   i
    bx      lr                      /* exception return pops the hw frame   */
    .size PendSV_Handler, . - PendSV_Handler

/* ------------------------------------------------------------------------
 * SVC: system call entry. Find which stack the caller was using and hand
 * the stacked registers to C. The C code reads arguments from frame[0..3]
 * and writes the return value into frame[0] (the caller's r0).
 * ------------------------------------------------------------------------ */
    .section .text.SVC_Handler, "ax", %progbits
    .global SVC_Handler
    .type SVC_Handler, %function
    .thumb_func
SVC_Handler:
    tst     lr, #4
    ite     eq
    mrseq   r0, msp
    mrsne   r0, psp
    b       k_svc_dispatch          /* tail call; lr still holds EXC_RETURN */
    .size SVC_Handler, . - SVC_Handler

/* ------------------------------------------------------------------------
 * Faults: pass EXC_RETURN and both stack pointers to C, which decides
 * whether a task misbehaved (kill it) or the kernel did (panic).
 * ------------------------------------------------------------------------ */
    .section .text.fault_entry, "ax", %progbits
    .global MemManage_Handler
    .global BusFault_Handler
    .global UsageFault_Handler
    .global HardFault_Handler
    .type MemManage_Handler, %function
    .type BusFault_Handler, %function
    .type UsageFault_Handler, %function
    .type HardFault_Handler, %function
    .thumb_func
MemManage_Handler:
    .thumb_func
BusFault_Handler:
    .thumb_func
UsageFault_Handler:
    mov     r0, lr
    mrs     r1, psp
    mrs     r2, msp
    b       k_fault_handler
    .thumb_func
HardFault_Handler:
    mov     r0, lr
    mrs     r1, psp
    mrs     r2, msp
    b       k_hardfault_handler

/* ------------------------------------------------------------------------
 * k_arch_start: called once from main. Reclaim the boot stack, then let
 * PendSV pick and enter the first task. Never returns.
 * ------------------------------------------------------------------------ */
    .section .text.k_arch_start, "ax", %progbits
    .global k_arch_start
    .type k_arch_start, %function
    .thumb_func
k_arch_start:
    ldr     r0, =_kstack_top
    msr     msp, r0
    isb
    ldr     r1, =0xE000ED04         /* SCB->ICSR                            */
    ldr     r2, =0x10000000         /* PENDSVSET                            */
    str     r2, [r1]
    dsb
    cpsie   i                       /* PendSV fires right here              */
    isb
3:  b       3b
    .size k_arch_start, . - k_arch_start
