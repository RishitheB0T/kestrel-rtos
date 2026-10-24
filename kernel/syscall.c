/*
 * syscall.c - the doorway between unprivileged tasks and the kernel.
 *
 * A task executes `svc #N`. The CPU switches to privileged Handler mode and
 * runs SVC_Handler (context.S), which passes us the task's saved registers:
 *     frame[0..3] = r0..r3 = arguments, frame[6] = return address.
 * The syscall number N is read from the svc instruction itself (the byte
 * just before the return address). Whatever we store in frame[0] becomes
 * r0, the return value, when the task resumes.
 *
 * Rule: never trust a pointer from a task. Before the kernel (which can
 * access everything) reads or writes through a user pointer, it checks
 * that the task itself would be allowed to touch that memory. Otherwise a
 * task could ask the kernel to copy kernel secrets out for it (the classic
 * "confused deputy" bug).
 */
#include <string.h>

#include "arch.h"
#include "board.h"
#include "kernel.h"
#include "kestrel_syscalls.h"

extern uint32_t _uram_start, _uram_end, _flash_start, _flash_end;

/* ---- user pointer validation ------------------------------------------- */

static int range_in(uint32_t a, uint32_t len, uint32_t lo, uint32_t hi)
{
    return a >= lo && a <= hi && len <= hi - a;
}

int k_user_can_write(const k_tcb_t *t, const void *p, uint32_t len)
{
    uint32_t a = (uint32_t)p;
    if (!t)
        return 1;
    if (range_in(a, len, (uint32_t)&_uram_start, (uint32_t)&_uram_end))
        return 1;
    uint32_t sb = (uint32_t)t->stack_base;
    return range_in(a, len, sb, sb + t->stack_size);
}

int k_user_can_read(const k_tcb_t *t, const void *p, uint32_t len)
{
    return k_user_can_write(t, p, len) ||
           range_in((uint32_t)p, len, (uint32_t)&_flash_start, (uint32_t)&_flash_end);
}

int k_is_code_addr(uint32_t addr)
{
    return range_in(addr & ~1u, 2, (uint32_t)&_flash_start, (uint32_t)&_flash_end);
}

/* ---- task syscalls ------------------------------------------------------ */

static int32_t sys_task_create(uint32_t ucfg, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a1, (void)a2, (void)a3;
    k_tcb_t *self = k_current;
    k_task_cfg_t cfg;
    char name[K_NAME_LEN] = {0};

    if (!k_user_can_read(self, (const void *)ucfg, sizeof(cfg)))
        return K_EFAULT;
    memcpy(&cfg, (const void *)ucfg, sizeof(cfg)); /* copy once: no TOCTOU */

    if (!k_is_code_addr((uint32_t)cfg.entry))
        return K_EFAULT; /* tasks may only start inside the program image */
    if (cfg.prio < K_PRIO_MIN || cfg.prio > K_PRIO_MAX)
        return K_EINVAL;
    for (int i = 0; cfg.name && i < K_NAME_LEN - 1; i++) {
        if (!k_user_can_read(self, cfg.name + i, 1))
            return K_EFAULT;
        if (!(name[i] = cfg.name[i]))
            break;
    }
    cfg.name = name;

    /* Capability rule: a task may hand its child an MMIO window only if it
     * lies inside its own window. Authority can be delegated, never grown. */
    if (cfg.grant_size) {
        uint32_t gb = cfg.grant_base, gs = cfg.grant_size;
        int inside_parent = self && self->grant_size && gb >= self->grant_base &&
                            gs <= self->grant_size &&
                            gb - self->grant_base <= self->grant_size - gs;
        if (self && !inside_parent)
            return K_EPERM;
        if (!range_in(gb, gs, PERIPH_BASE, PERIPH_END))
            return K_EPERM;
        uint32_t rasr;
        if (k_mpu_encode(gb, gs, K_MPU_ATTR_DEVICE, &rasr) != K_OK)
            return K_EINVAL;
    }
    return k_task_create_internal(&cfg);
}

static int32_t sys_yield(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a0, (void)a1, (void)a2, (void)a3;
    k_yield_current();
    return K_OK;
}

static int32_t sys_sleep(uint32_t ms, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a1, (void)a2, (void)a3;
    if (ms == 0) {
        k_yield_current();
        return K_OK;
    }
    k_block_current(NULL, ms * (K_TICK_HZ / 1000u));
    return K_PENDING;
}

static int32_t sys_exit(uint32_t code, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a1, (void)a2, (void)a3;
    k_task_kill(k_current, (int32_t)code);
    return K_PENDING; /* never returns to the task */
}

static int32_t sys_task_info(uint32_t tid, uint32_t out, uint32_t a2, uint32_t a3)
{
    (void)a2, (void)a3;
    if (!k_user_can_write(k_current, (void *)out, sizeof(k_task_info_t)))
        return K_EFAULT;
    return k_task_fill_info((int)tid, (k_task_info_t *)out);
}

static int32_t sys_task_self(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a0, (void)a1, (void)a2, (void)a3;
    return k_current ? k_current->id : -1;
}

static int32_t sys_ticks(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a0, (void)a1, (void)a2, (void)a3;
    return (int32_t)k_tick_count;
}

static int32_t sys_cycles(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a0, (void)a1, (void)a2, (void)a3;
    return (int32_t)k_now_cycles();
}

static int32_t sys_cpu_cycles(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a0, (void)a1, (void)a2, (void)a3;
    return (int32_t)k_current_cpu_cycles();
}

static int32_t sys_cpu_hz(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a0, (void)a1, (void)a2, (void)a3;
    return (int32_t)CPU_HZ;
}

/* ---- console syscalls --------------------------------------------------- */

static int32_t sys_write(uint32_t buf, uint32_t len, uint32_t a2, uint32_t a3)
{
    (void)a2, (void)a3;
    if (!k_user_can_read(k_current, (const void *)buf, len))
        return K_EFAULT;
    uint32_t n = k_console_put((const uint8_t *)buf, len);
    if (n == 0 && len > 0) {
        /* TX buffer full: sleep until the UART interrupt drains it. The
         * user-side wrapper sees 0 bytes written and simply retries. */
        k_block_current(&k_console_tx_waiters, K_FOREVER);
        return K_PENDING;
    }
    return (int32_t)n;
}

static int32_t sys_read(uint32_t buf, uint32_t len, uint32_t timeout, uint32_t a3)
{
    (void)a3;
    if (!k_user_can_write(k_current, (void *)buf, len))
        return K_EFAULT;
    uint32_t n = k_console_get((uint8_t *)buf, len);
    if (n > 0 || len == 0)
        return (int32_t)n;
    if (timeout == K_NO_WAIT)
        return K_EAGAIN;
    k_block_current(&k_console_rx_waiters, timeout);
    return K_PENDING;
}

/* ---- system ------------------------------------------------------------- */

static int32_t sys_reboot(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a0, (void)a1, (void)a2, (void)a3;
    k_console_puts_sync("\r\n[kernel] rebooting\r\n");
    dsb();
    SCB_AIRCR = AIRCR_SYSRESET;
    for (;;) {
    }
    return K_OK; /* not reached */
}

static int32_t sys_halt(uint32_t code, uint32_t a1, uint32_t a2, uint32_t a3)
{
    (void)a1, (void)a2, (void)a3;
    kprintf("[kernel] halt(%d)\n", (int)code);
    k_semihost_exit((int)code);
    __asm volatile("cpsid i");
    for (;;)
        __asm volatile("wfi");
    return K_OK; /* not reached */
}

/* ---- dispatch ----------------------------------------------------------- */

typedef int32_t (*k_syscall_fn)(uint32_t, uint32_t, uint32_t, uint32_t);

static const k_syscall_fn syscall_table[SYS_COUNT] = {
    [SYS_TASK_CREATE]  = sys_task_create,
    [SYS_YIELD]        = sys_yield,
    [SYS_SLEEP]        = sys_sleep,
    [SYS_EXIT]         = sys_exit,
    [SYS_TASK_INFO]    = sys_task_info,
    [SYS_TASK_SELF]    = sys_task_self,
    [SYS_TICKS]        = sys_ticks,
    [SYS_CYCLES]       = sys_cycles,
    [SYS_CPU_CYCLES]   = sys_cpu_cycles,
    [SYS_CPU_HZ]       = sys_cpu_hz,
    [SYS_SEM_CREATE]   = sys_sem_create,
    [SYS_SEM_TAKE]     = sys_sem_take,
    [SYS_SEM_GIVE]     = sys_sem_give,
    [SYS_MUTEX_CREATE] = sys_mutex_create,
    [SYS_MUTEX_LOCK]   = sys_mutex_lock,
    [SYS_MUTEX_UNLOCK] = sys_mutex_unlock,
    [SYS_QUEUE_CREATE] = sys_queue_create,
    [SYS_QUEUE_SEND]   = sys_queue_send,
    [SYS_QUEUE_RECV]   = sys_queue_recv,
    [SYS_WRITE]        = sys_write,
    [SYS_READ]         = sys_read,
    [SYS_TRACE_CTL]    = sys_trace_ctl,
    [SYS_TRACE_READ]   = sys_trace_read,
    [SYS_TRACE_MARK]   = sys_trace_mark,
    [SYS_REBOOT]       = sys_reboot,
    [SYS_HALT]         = sys_halt,
};

void k_svc_dispatch(uint32_t *frame)
{
    /* One big lock: the whole syscall runs with interrupts off. Every
     * syscall is short, and it makes the kernel easy to reason about. */
    uint32_t s = irq_save();
    uint8_t num = ((const uint8_t *)frame[6])[-2];
    k_tcb_t *caller = k_current;
    int32_t ret = K_ENOSYS;

    if (caller)
        caller->svc_frame = frame;
    if (num < SYS_COUNT && syscall_table[num])
        ret = syscall_table[num](frame[0], frame[1], frame[2], frame[3]);
    if (ret != K_PENDING)
        frame[0] = (uint32_t)ret;
    irq_restore(s);
}
