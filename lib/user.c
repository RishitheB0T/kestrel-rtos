/*
 * user.c - the user-side half of every system call.
 *
 * Each wrapper loads its arguments into r0-r3 and executes `svc #N`. The
 * CPU traps into the kernel, which writes the result back into r0.
 * This file runs unprivileged, inside tasks.
 */
#include <stdarg.h>

#include "kestrel.h"
#include "kestrel_syscalls.h"

int k_vsnprintf(char *buf, uint32_t size, const char *fmt, va_list ap);

#define SVC(num, a0, a1, a2, a3)                                                  \
    __extension__({                                                               \
        register uint32_t r0_ __asm__("r0") = (uint32_t)(a0);                     \
        register uint32_t r1_ __asm__("r1") = (uint32_t)(a1);                     \
        register uint32_t r2_ __asm__("r2") = (uint32_t)(a2);                     \
        register uint32_t r3_ __asm__("r3") = (uint32_t)(a3);                     \
        __asm__ volatile("svc %[n]"                                               \
                         : "+r"(r0_)                                              \
                         : [n] "I"(num), "r"(r1_), "r"(r2_), "r"(r3_)             \
                         : "memory");                                             \
        (int32_t) r0_;                                                            \
    })

/* Every task begins here (the kernel points the new task's pc at it). */
void k_user_task_entry(void (*entry)(void *), void *arg)
{
    entry(arg);
    k_exit(0);
}

int k_task_create(const k_task_cfg_t *cfg) { return SVC(SYS_TASK_CREATE, cfg, 0, 0, 0); }
void k_yield(void) { SVC(SYS_YIELD, 0, 0, 0, 0); }
void k_sleep(uint32_t ms) { SVC(SYS_SLEEP, ms, 0, 0, 0); }
int k_task_info(int tid, k_task_info_t *out) { return SVC(SYS_TASK_INFO, tid, out, 0, 0); }
int k_task_self(void) { return SVC(SYS_TASK_SELF, 0, 0, 0, 0); }

void k_exit(int code)
{
    SVC(SYS_EXIT, code, 0, 0, 0);
    for (;;) {
    }
}

uint32_t k_ticks(void) { return (uint32_t)SVC(SYS_TICKS, 0, 0, 0, 0); }
uint32_t k_cycles(void) { return (uint32_t)SVC(SYS_CYCLES, 0, 0, 0, 0); }
uint32_t k_cpu_cycles(void) { return (uint32_t)SVC(SYS_CPU_CYCLES, 0, 0, 0, 0); }
uint32_t k_cpu_hz(void) { return (uint32_t)SVC(SYS_CPU_HZ, 0, 0, 0, 0); }

int k_sem_create(unsigned initial, unsigned max) { return SVC(SYS_SEM_CREATE, initial, max, 0, 0); }
int k_sem_take(int sem, uint32_t timeout) { return SVC(SYS_SEM_TAKE, sem, timeout, 0, 0); }
int k_sem_give(int sem) { return SVC(SYS_SEM_GIVE, sem, 0, 0, 0); }

int k_mutex_create(unsigned flags) { return SVC(SYS_MUTEX_CREATE, flags, 0, 0, 0); }
int k_mutex_lock(int m, uint32_t timeout) { return SVC(SYS_MUTEX_LOCK, m, timeout, 0, 0); }
int k_mutex_unlock(int m) { return SVC(SYS_MUTEX_UNLOCK, m, 0, 0, 0); }

int k_queue_create(unsigned item_size, unsigned depth)
{
    return SVC(SYS_QUEUE_CREATE, item_size, depth, 0, 0);
}
int k_queue_send(int q, const void *item, uint32_t timeout)
{
    return SVC(SYS_QUEUE_SEND, q, item, timeout, 0);
}
int k_queue_recv(int q, void *item, uint32_t timeout)
{
    return SVC(SYS_QUEUE_RECV, q, item, timeout, 0);
}

/* The kernel accepts as many bytes as fit in its TX buffer; when it is
 * full the call sleeps until the UART drains, then returns 0 and we retry. */
int k_write(const void *buf, size_t len)
{
    const char *p = buf;
    size_t left = len;
    while (left) {
        int n = SVC(SYS_WRITE, p, left, 0, 0);
        if (n < 0)
            return n;
        p += n;
        left -= (size_t)n;
    }
    return (int)len;
}

int k_read(void *buf, size_t len, uint32_t timeout)
{
    for (;;) {
        int n = SVC(SYS_READ, buf, len, timeout, 0);
        if (n != 0 || len == 0)
            return n; /* data, or an error such as K_ETIMEDOUT */
    }
}

int k_printf(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    int n = k_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 1)
        n = (int)sizeof(line) - 1;
    return k_write(line, (size_t)n);
}

void k_trace_start(void) { SVC(SYS_TRACE_CTL, 1, 0, 0, 0); }
void k_trace_stop(void) { SVC(SYS_TRACE_CTL, 0, 0, 0, 0); }
int k_trace_read(unsigned first, k_trace_ev_t *out, unsigned n)
{
    return SVC(SYS_TRACE_READ, first, out, n, 0);
}
void k_trace_mark(unsigned id) { SVC(SYS_TRACE_MARK, id, 0, 0, 0); }

void k_reboot(void)
{
    SVC(SYS_REBOOT, 0, 0, 0, 0);
    for (;;) {
    }
}

void k_halt(int code)
{
    SVC(SYS_HALT, code, 0, 0, 0);
    for (;;) {
    }
}
