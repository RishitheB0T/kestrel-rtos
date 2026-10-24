/*
 * kestrel.h - the public API that application tasks use.
 *
 * Every function here is a *system call*: tasks run unprivileged, so they
 * cannot touch kernel memory or hardware directly. Each call executes an
 * `svc` instruction, which traps into the kernel (see kernel/syscall.c).
 */
#ifndef KESTREL_H
#define KESTREL_H

#include <stddef.h>
#include <stdint.h>

/* ---- Timeouts --------------------------------------------------------- */
#define K_NO_WAIT 0u           /* fail immediately instead of blocking   */
#define K_FOREVER 0xFFFFFFFFu  /* block until the event happens          */

/* ---- Error codes (all negative; 0 = success) -------------------------- */
#define K_OK          0
#define K_ETIMEDOUT  -1   /* the timeout expired before the event          */
#define K_EINVAL     -2   /* bad argument or bad handle                    */
#define K_ENOMEM     -3   /* out of task slots / stack / queue memory      */
#define K_EFAULT     -4   /* a pointer you passed is not yours to use      */
#define K_EPERM      -5   /* not allowed (e.g. unlocking someone's mutex)  */
#define K_EOWNERDEAD -6   /* got the mutex, but its last owner died in it  */
#define K_EAGAIN     -7   /* would block, and you said K_NO_WAIT           */
#define K_EDEADLK    -8   /* locking this mutex would deadlock             */
#define K_ENOSYS     -9   /* unknown system call                           */

/* Exit codes the kernel assigns to tasks it kills after a CPU fault. */
#define K_EXIT_MEMFAULT   -104  /* MemManage: MPU blocked an access        */
#define K_EXIT_BUSFAULT   -105  /* BusFault: bad bus access (e.g. PPB)     */
#define K_EXIT_USAGEFAULT -106  /* UsageFault: div by 0, undefined instr   */

/* ---- Tasks ------------------------------------------------------------ */
#define K_PRIO_IDLE 0    /* reserved for the idle task                    */
#define K_PRIO_MIN  1
#define K_PRIO_MAX  31   /* higher number = more important                */
#define K_NAME_LEN  12

typedef struct {
    const char *name;
    void (*entry)(void *arg);
    void *arg;
    uint8_t prio;          /* K_PRIO_MIN..K_PRIO_MAX                       */
    uint32_t stack_size;   /* power of two, 512..8192 bytes (MPU rule)     */
    uint32_t grant_base;   /* optional MMIO window the task may touch      */
    uint32_t grant_size;   /* 0 = no window. Must lie inside the creator's */
} k_task_cfg_t;

enum k_task_state {
    K_STATE_FREE, K_STATE_READY, K_STATE_RUNNING, K_STATE_BLOCKED, K_STATE_DEAD
};

typedef struct {
    char name[K_NAME_LEN];
    uint8_t id, state, prio, base_prio;
    uint32_t stack_size, stack_peak;   /* bytes; peak = high-water mark    */
    uint32_t run_cycles_lo, run_cycles_hi; /* CPU time consumed, in cycles */
    uint32_t switches;                 /* times this task was switched in  */
    uint32_t fault_cfsr, fault_addr, fault_pc;
    int32_t exit_code;
} k_task_info_t;

int  k_task_create(const k_task_cfg_t *cfg);   /* returns task id or error */
void k_yield(void);
void k_sleep(uint32_t ms);
void k_exit(int code) __attribute__((noreturn));
int  k_task_info(int tid, k_task_info_t *out);
int  k_task_self(void);

/* ---- Time ------------------------------------------------------------- */
uint32_t k_ticks(void);       /* milliseconds since the scheduler started */
uint32_t k_cycles(void);      /* CPU cycle timestamp (wraps)              */
uint32_t k_cpu_cycles(void);  /* CPU cycles used by the calling task      */
uint32_t k_cpu_hz(void);

/* ---- Counting semaphores ---------------------------------------------- */
int k_sem_create(unsigned initial, unsigned max);
int k_sem_take(int sem, uint32_t timeout);
int k_sem_give(int sem);

/* ---- Mutexes (priority inheritance on by default) --------------------- */
#define K_MUTEX_NO_PI 1u  /* disable priority inheritance (for the demo)  */
int k_mutex_create(unsigned flags);
int k_mutex_lock(int mutex, uint32_t timeout);
int k_mutex_unlock(int mutex);

/* ---- Message queues (fixed-size items, copied by the kernel) ---------- */
int k_queue_create(unsigned item_size, unsigned depth);
int k_queue_send(int queue, const void *item, uint32_t timeout);
int k_queue_recv(int queue, void *item, uint32_t timeout);

/* ---- Console (UART) --------------------------------------------------- */
int k_write(const void *buf, size_t len);              /* blocks if full  */
int k_read(void *buf, size_t len, uint32_t timeout);   /* >=1 byte or err */
int k_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- Tracing ---------------------------------------------------------- */
enum k_trace_event {
    K_TR_SWITCH = 1, /* tid = task switched in, arg = its priority        */
    K_TR_WAKE,       /* tid = task made ready                             */
    K_TR_BLOCK,      /* tid = task that blocked                           */
    K_TR_PRIO,       /* tid = task whose priority changed, arg = new prio */
    K_TR_CREATE,     /* tid = new task, arg = priority                    */
    K_TR_EXIT,       /* tid = task that ended, arg = exit code            */
    K_TR_FAULT,      /* tid = task killed by a fault, arg = exception no. */
    K_TR_MARK,       /* tid = caller, arg = user-chosen marker id         */
};

typedef struct {
    uint32_t ts;     /* k_cycles() timestamp */
    uint8_t ev, tid;
    uint16_t arg;
} k_trace_ev_t;

void k_trace_start(void);
void k_trace_stop(void);
int  k_trace_read(unsigned first, k_trace_ev_t *out, unsigned n);
void k_trace_mark(unsigned id);

/* ---- System ----------------------------------------------------------- */
void k_reboot(void) __attribute__((noreturn));
void k_halt(int code) __attribute__((noreturn));  /* QEMU: exit(code)    */

#endif
