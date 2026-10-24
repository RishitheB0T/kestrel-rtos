/*
 * kernel.h - data structures and functions shared *inside* the kernel.
 * Application code must not include this; it uses include/kestrel.h.
 */
#ifndef KESTREL_KERNEL_H
#define KESTREL_KERNEL_H

#include <stdarg.h>
#include <stdint.h>

#include "kestrel.h"
#include "list.h"

#define K_MAX_TASKS   16
#define K_NUM_PRIO    32
#define K_TICK_HZ     1000u   /* 1 tick = 1 ms                              */
#define K_SLICE_TICKS 5u      /* round-robin time slice for equal priority  */
#define K_STACK_MIN   512u
#define K_STACK_MAX   8192u
#define K_STACK_FILL  0xDEADBEEFu  /* pattern for stack high-water marks    */

/* A syscall handler returns this when it blocked the caller. The real
 * result is written into the caller's saved r0 later, by whoever wakes it. */
#define K_PENDING ((int32_t)0x7FFFFFFF)

/* Task Control Block: everything the kernel knows about one task. */
typedef struct k_tcb {
    uint32_t *sp;              /* saved stack pointer while switched out     */
    k_list_t node;             /* links into a ready list OR a wait queue    */
    k_list_t tnode;            /* links into the sleep/timeout list          */
    k_list_t *wait_q;          /* wait queue we are in, if blocked on one    */
    struct k_mutex *blocked_on;/* mutex we wait for (priority inheritance)   */
    uint32_t *svc_frame;       /* registers saved by the current syscall     */
    void *wait_buf;            /* queue item being sent/received while blocked */
    uint32_t wake_tick;        /* when a sleep/timeout ends                  */
    uint32_t *stack_base;
    uint32_t stack_size;
    uint32_t stack_rasr;       /* precomputed MPU attributes for the stack   */
    uint32_t grant_base, grant_size, grant_rasr; /* optional MMIO window    */
    uint64_t run_cycles;       /* CPU time used                              */
    uint32_t switches;
    uint32_t stack_peak;
    uint32_t fault_cfsr, fault_addr, fault_pc;
    int32_t exit_code;
    uint8_t id, state, prio, base_prio, slice_left;
    char name[K_NAME_LEN];
} k_tcb_t;

extern k_tcb_t *k_current;           /* the running task (NULL at boot)     */
extern volatile uint32_t k_tick_count;

/* sched.c */
void k_sched_init(void);
void k_sched_start(void) __attribute__((noreturn));
void k_reschedule(void);
void k_block_current(k_list_t *wait_q, uint32_t timeout);
void k_wake(k_tcb_t *t, int32_t result);
void k_wake_all(k_list_t *wait_q, int32_t result);
void k_wq_insert(k_list_t *wait_q, k_tcb_t *t);
k_tcb_t *k_wq_first(k_list_t *wait_q);
void k_set_prio(k_tcb_t *t, uint8_t prio);
void k_yield_current(void);
void k_task_kill(k_tcb_t *t, int32_t code);
int32_t k_task_create_internal(const k_task_cfg_t *cfg);
k_tcb_t *k_task_by_id(int tid);
int32_t k_task_fill_info(int tid, k_task_info_t *out);
uint32_t k_now_cycles(void);
uint32_t k_current_cpu_cycles(void);

/* sync.c */
void k_sync_init(void);
void k_mutex_waiter_gone(struct k_mutex *m);
void k_mutex_owner_died(k_tcb_t *t);
int32_t sys_sem_create(uint32_t initial, uint32_t max, uint32_t a2, uint32_t a3);
int32_t sys_sem_take(uint32_t h, uint32_t timeout, uint32_t a2, uint32_t a3);
int32_t sys_sem_give(uint32_t h, uint32_t a1, uint32_t a2, uint32_t a3);
int32_t sys_mutex_create(uint32_t flags, uint32_t a1, uint32_t a2, uint32_t a3);
int32_t sys_mutex_lock(uint32_t h, uint32_t timeout, uint32_t a2, uint32_t a3);
int32_t sys_mutex_unlock(uint32_t h, uint32_t a1, uint32_t a2, uint32_t a3);
int32_t sys_queue_create(uint32_t isz, uint32_t depth, uint32_t a2, uint32_t a3);
int32_t sys_queue_send(uint32_t h, uint32_t item, uint32_t timeout, uint32_t a3);
int32_t sys_queue_recv(uint32_t h, uint32_t item, uint32_t timeout, uint32_t a3);

/* syscall.c - checks that a user pointer really belongs to the caller. */
int k_user_can_read(const k_tcb_t *t, const void *p, uint32_t len);
int k_user_can_write(const k_tcb_t *t, const void *p, uint32_t len);
int k_is_code_addr(uint32_t addr);

/* mpu.c */
void k_mpu_init(void);
void k_mpu_load(const k_tcb_t *t);
int k_mpu_encode(uint32_t base, uint32_t size, uint32_t attrs, uint32_t *rasr);
#define K_MPU_ATTR_STACK  0  /* normal SRAM, read/write, never execute */
#define K_MPU_ATTR_DEVICE 1  /* device memory, read/write, never execute */

/* stackpool.c */
void k_stack_pool_init(void);
uint32_t *k_stack_alloc(uint32_t size);
void k_stack_free(uint32_t *base, uint32_t size);

/* trace.c */
void k_trace(uint8_t ev, uint8_t tid, uint16_t arg);
int32_t sys_trace_ctl(uint32_t on, uint32_t a1, uint32_t a2, uint32_t a3);
int32_t sys_trace_read(uint32_t first, uint32_t out, uint32_t n, uint32_t a3);
int32_t sys_trace_mark(uint32_t id, uint32_t a1, uint32_t a2, uint32_t a3);

/* console.c - interrupt-driven UART driver */
void k_console_init(void);
uint32_t k_console_put(const uint8_t *p, uint32_t len);
uint32_t k_console_get(uint8_t *p, uint32_t len);
extern k_list_t k_console_tx_waiters, k_console_rx_waiters;
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void k_console_puts_sync(const char *s);

/* fault.c */
void k_panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void k_semihost_exit(int code);

/* lib/printf.c (shared with user code) */
int k_vsnprintf(char *buf, uint32_t size, const char *fmt, va_list ap);

/* lib/user.c - where every task starts (runs unprivileged) */
void k_user_task_entry(void (*entry)(void *), void *arg);

/* app - the first user task, like Unix's PID 1 */
void app_init(void *arg);

#endif
