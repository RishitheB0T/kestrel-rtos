/*
 * fault.c - what happens when code does something illegal.
 *
 * If an unprivileged *task* faults (MPU violation, bad bus access, divide by
 * zero...), we record why, kill just that task, and carry on: the rest of
 * the system keeps running. This is fault isolation, the same idea as an
 * OS killing a process with SIGSEGV instead of crashing the machine.
 *
 * If the *kernel* faults, nothing can be trusted any more: print everything
 * we know and stop (a "kernel panic").
 */
#include <stdarg.h>

#include "arch.h"
#include "board.h"
#include "kernel.h"

/* EXC_RETURN bits 3 and 2 set = the fault interrupted Thread mode code that
 * was running on the process stack, i.e. a task. */
#define EXC_RETURN_FROM_TASK 0xCu

static const char *fault_reason(uint32_t cfsr)
{
    if (cfsr & CFSR_MSTKERR)   return "stack overflow (MPU blocked exception entry)";
    if (cfsr & CFSR_MLSPERR)   return "MPU blocked lazy FPU state save";
    if (cfsr & CFSR_DACCVIOL)  return "MPU: data access violation";
    if (cfsr & CFSR_IACCVIOL)  return "MPU: instruction fetch violation";
    if (cfsr & CFSR_STKERR)    return "bus fault on exception entry";
    if (cfsr & CFSR_PRECISERR) return "bus fault (precise)";
    if (cfsr & CFSR_DIVBYZERO) return "divide by zero";
    if (cfsr & CFSR_UNDEFINSTR) return "undefined instruction";
    return "fault";
}

void k_fault_handler(uint32_t exc_return, uint32_t *psp, uint32_t *msp)
{
    uint32_t cfsr = SCB_CFSR;
    uint32_t exc = get_ipsr();       /* 4 = MemManage, 5 = BusFault, 6 = UsageFault */
    uint32_t addr = 0;

    if (cfsr & CFSR_MMARVALID)
        addr = SCB_MMFAR;
    else if (cfsr & CFSR_BFARVALID)
        addr = SCB_BFAR;
    SCB_CFSR = cfsr; /* write-1-to-clear the sticky status bits */

    k_tcb_t *t = k_current;
    if ((exc_return & EXC_RETURN_FROM_TASK) != EXC_RETURN_FROM_TASK || !t)
        k_panic("kernel fault exc=%u cfsr=0x%08x addr=0x%08x pc=0x%08x",
                (unsigned)exc, (unsigned)cfsr, (unsigned)addr, (unsigned)msp[6]);

    /* If stacking itself failed, the frame on the task stack is garbage. */
    uint32_t pc = (cfsr & (CFSR_MSTKERR | CFSR_STKERR)) ? 0 : psp[6];

    uint32_t s = irq_save();
    t->fault_cfsr = cfsr;
    t->fault_addr = addr;
    t->fault_pc = pc;
    kprintf("[kernel] task '%s' (id %u) killed: %s, pc=0x%08x addr=0x%08x cfsr=0x%08x\n",
            t->name, (unsigned)t->id, fault_reason(cfsr), (unsigned)pc, (unsigned)addr,
            (unsigned)cfsr);
    k_trace(K_TR_FAULT, t->id, (uint16_t)exc);
    k_task_kill(t, -100 - (int32_t)exc);
    irq_restore(s);
    /* Returning now tail-chains into PendSV, which switches to another task,
     * so the faulting instruction is never retried. */
}

void k_hardfault_handler(uint32_t exc_return, uint32_t *psp, uint32_t *msp)
{
    uint32_t *frame = (exc_return & 4u) ? psp : msp;
    k_panic("HardFault hfsr=0x%08x cfsr=0x%08x pc=0x%08x lr=0x%08x", (unsigned)SCB_HFSR,
            (unsigned)SCB_CFSR, (unsigned)frame[6], (unsigned)frame[5]);
}

/* ARM semihosting: lets code in QEMU ask the host to exit with a status.
 * On real hardware there is no debugger to answer, so we never call it. */
void k_semihost_exit(int code)
{
#if HAS_SEMIHOSTING
    register uint32_t r0 __asm("r0") = 0x18; /* SYS_EXIT */
    register uint32_t r1 __asm("r1") = code == 0 ? 0x20026u  /* ApplicationExit */
                                                 : 0x20024u; /* InternalError   */
    __asm volatile("bkpt 0xAB" : : "r"(r0), "r"(r1) : "memory");
#else
    (void)code;
#endif
}

void k_panic(const char *fmt, ...)
{
    char line[160];
    va_list ap;

    __asm volatile("cpsid i");
    va_start(ap, fmt);
    k_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    k_console_puts_sync("\n*** KERNEL PANIC: ");
    k_console_puts_sync(line);
    k_console_puts_sync(" ***\n");
    k_semihost_exit(1);
    for (;;) {
    }
}
