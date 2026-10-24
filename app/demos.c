/*
 * demos.c - the priority-inversion experiment and the crash gallery.
 */
#include "app.h"

/* Burn exactly `ms` of *this task's own* CPU time. (Spinning until the wall
 * clock advances would be wrong: if we get preempted, the wall clock keeps
 * moving while we do no work.) */
void busy_cpu_ms(uint32_t ms)
{
    uint32_t target = k_cpu_cycles() + ms * (k_cpu_hz() / 1000u);
    while ((int32_t)(k_cpu_cycles() - target) < 0) {
    }
}

int wait_task_dead(int tid, uint32_t timeout_ms, k_task_info_t *info)
{
    uint32_t start = k_ticks();
    for (;;) {
        if (k_task_info(tid, info) == K_OK && info->state == K_STATE_DEAD)
            return K_OK;
        if (k_ticks() - start > timeout_ms)
            return K_ETIMEDOUT;
        k_sleep(1);
    }
}

/* ======================================================================
 * Priority inversion
 *
 *   L (prio 3): lock M, compute for CS_MS, unlock
 *   H (prio 6): wake at t=5ms, lock M (blocks: L holds it)
 *   M (prio 4): wake at t=8ms, compute for MID_MS, needs no lock
 *
 * Without inheritance, M preempts L, so H waits for M too: ~CS + MID.
 * With inheritance, L runs at H's priority until it unlocks: ~CS.
 * ====================================================================== */
#define CS_MS  20u
#define MID_MS 50u

static struct {
    int mutex, done;
    volatile uint32_t h_wait_cycles;
    volatile uint8_t l_peak_prio;
} pi;

static void pi_low(void *arg)
{
    (void)arg;
    k_task_info_t self;
    int me = k_task_self();
    k_mutex_lock(pi.mutex, K_FOREVER);
    k_trace_mark(1); /* "L entered the critical section" */
    uint32_t hz_ms = k_cpu_hz() / 1000u;
    uint32_t target = k_cpu_cycles() + CS_MS * hz_ms;
    while ((int32_t)(k_cpu_cycles() - target) < 0) {
        k_task_info(me, &self); /* watch our own effective priority */
        if (self.prio > pi.l_peak_prio)
            pi.l_peak_prio = self.prio;
        busy_cpu_ms(1);
    }
    k_trace_mark(2); /* "L leaves the critical section" */
    k_mutex_unlock(pi.mutex);
    k_sem_give(pi.done);
}

static void pi_high(void *arg)
{
    (void)arg;
    k_sleep(5);
    uint32_t t0 = k_cycles();
    k_mutex_lock(pi.mutex, K_FOREVER);
    pi.h_wait_cycles = k_cycles() - t0;
    k_trace_mark(3); /* "H got the mutex" */
    k_mutex_unlock(pi.mutex);
    k_sem_give(pi.done);
}

static void pi_mid(void *arg)
{
    (void)arg;
    k_sleep(8);
    busy_cpu_ms(MID_MS);
    k_sem_give(pi.done);
}

int demo_priority_inversion(int use_pi, pi_result_t *res)
{
    /* Kernel objects cannot be deleted, so create them once and reuse. */
    static int mutexes[2] = {-1, -1}, done = -1;
    if (mutexes[use_pi != 0] < 0)
        mutexes[use_pi != 0] = k_mutex_create(use_pi ? 0 : K_MUTEX_NO_PI);
    if (done < 0)
        done = k_sem_create(0, 3);
    pi.mutex = mutexes[use_pi != 0];
    pi.done = done;
    pi.l_peak_prio = 0;
    if (pi.mutex < 0 || pi.done < 0)
        return K_ENOMEM;

    k_task_cfg_t l = {.name = "pi-low", .entry = pi_low, .prio = 3, .stack_size = 1024};
    k_task_cfg_t h = {.name = "pi-high", .entry = pi_high, .prio = 6, .stack_size = 1024};
    k_task_cfg_t m = {.name = "pi-mid", .entry = pi_mid, .prio = 4, .stack_size = 1024};

    k_trace_start();
    /* All three are below our priority, so none runs until we block. */
    if (k_task_create(&l) < 0 || k_task_create(&h) < 0 || k_task_create(&m) < 0)
        return K_ENOMEM;
    for (int i = 0; i < 3; i++)
        k_sem_take(pi.done, K_FOREVER);
    k_sleep(2); /* let the tasks finish exiting */
    k_trace_stop();

    res->h_wait_us = pi.h_wait_cycles / (k_cpu_hz() / 1000000u);
    res->l_peak_prio = pi.l_peak_prio;
    res->cs_ms = CS_MS;
    res->m_busy_ms = MID_MS;
    return K_OK;
}

/* ======================================================================
 * Crash gallery: each task does one illegal thing. The kernel kills just
 * that task and reports why; everything else keeps running.
 * ====================================================================== */
extern volatile uint32_t k_tick_count; /* lives in kernel RAM */

const char *const crash_names[CRASH_NUM_KINDS] = {
    "kernel", "stack", "null", "periph", "ppb", "div0", "otherstack",
};

static int recurse(int depth)
{
    volatile uint8_t frame[64];
    frame[0] = (uint8_t)depth;
    if (depth > 1000000)
        return 0;
    return recurse(depth + 1) + frame[0];
}

static void crash_task(void *arg)
{
    uint32_t kind = (uint32_t)arg & 0xFFu;
    volatile int a = 1, b = 0;

    switch (kind) {
    case CRASH_KERNEL_WRITE:
        k_tick_count = 0;
        break;
    case CRASH_STACK:
        recurse(0);
        break;
    case CRASH_NULL:
        (void)*(volatile uint32_t *)0;
        break;
    case CRASH_PERIPH:
        *(volatile uint32_t *)0x40023830u = 0; /* RCC_AHB1ENR: clocks off! */
        break;
    case CRASH_PPB:
        *(volatile uint32_t *)0xE000E014u = 0; /* SysTick reload */
        break;
    case CRASH_DIV0:
        a = a / b;
        break;
    }
    k_printf("crash task survived?!\n");
}

static int victim_release = -1;

static void victim_task(void *arg)
{
    volatile uint32_t secret = 0xC0FFEEu;
    *(volatile uint32_t **)arg = (volatile uint32_t *)&secret; /* publish address */
    k_sem_take(victim_release, K_FOREVER);                    /* then wait */
}

static void other_stack_reader(void *arg)
{
    volatile uint32_t *p = *(volatile uint32_t **)arg;
    k_printf("read %08x\n", (unsigned)*p);
}

int demo_crash(enum crash_kind kind, k_task_info_t *info)
{
    static volatile uint32_t *victim_addr;
    int victim = -1;
    k_task_cfg_t cfg = {.name = "crash", .entry = crash_task, .arg = (void *)(uint32_t)kind,
                        .prio = PRIO_INIT - 1, .stack_size = 1024};

    if (kind == CRASH_OTHER_STACK) {
        if (victim_release < 0)
            victim_release = k_sem_create(0, 1);
        k_task_cfg_t v = {.name = "victim", .entry = victim_task, .arg = (void *)&victim_addr,
                          .prio = PRIO_INIT - 1, .stack_size = 512};
        victim_addr = 0;
        victim = k_task_create(&v);
        while (!victim_addr)
            k_sleep(1);
        cfg.entry = other_stack_reader;
        cfg.arg = (void *)&victim_addr;
    }

    int tid = k_task_create(&cfg);
    if (tid < 0)
        return tid;
    int r = wait_task_dead(tid, 500, info);
    if (victim >= 0) { /* the victim was never hurt; let it exit normally */
        k_task_info_t vinfo;
        k_sem_give(victim_release);
        wait_task_dead(victim, 500, &vinfo);
    }
    return r;
}

/* ======================================================================
 * Trace dump, in the text format tools/trace2perfetto.py understands.
 * ====================================================================== */
void trace_dump(void)
{
    k_trace_ev_t ev[16];
    k_task_info_t info;
    unsigned first = 0;
    int n;

    k_printf("#KTRACE-BEGIN hz=%u\n", (unsigned)k_cpu_hz());
    for (int tid = 0; tid < 16; tid++)
        if (k_task_info(tid, &info) == K_OK)
            k_printf("#TASK %d %s\n", tid, info.name);
    while ((n = k_trace_read(first, ev, 16)) > 0) {
        for (int i = 0; i < n; i++)
            k_printf("E %u %u %u %u\n", (unsigned)ev[i].ts, ev[i].ev, ev[i].tid, ev[i].arg);
        first += (unsigned)n;
    }
    k_printf("#KTRACE-END events=%u\n", first);
}
