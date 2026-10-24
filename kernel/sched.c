/*
 * sched.c - tasks, the scheduler, and time.
 *
 * Policy: fixed-priority preemptive scheduling. The highest-priority ready
 * task always runs; tasks of equal priority share the CPU round-robin with
 * a K_SLICE_TICKS time slice.
 *
 * Finding the highest ready priority is O(1): bit p of ready_bitmap is set
 * while ready[p] is non-empty, and one CLZ (count leading zeros) instruction
 * finds the top set bit.
 *
 * The running task stays at the head of its ready list. Blocking removes it;
 * waking appends a task at the tail.
 */
#include <string.h>

#include "arch.h"
#include "board.h"
#include "kernel.h"

k_tcb_t *k_current;
volatile uint32_t k_tick_count;

static k_tcb_t tcbs[K_MAX_TASKS];
static k_list_t ready[K_NUM_PRIO];
static uint32_t ready_bitmap;
static k_list_t sleep_list;         /* sorted by wake_tick, soonest first */
static uint32_t systick_reload;
static uint32_t last_switch_cycles;
static int sched_running;

void k_arch_start(void) __attribute__((noreturn));

/* ---- ready lists -------------------------------------------------------- */

static void ready_insert(k_tcb_t *t, int at_front)
{
    if (at_front)
        list_push_front(&ready[t->prio], &t->node);
    else
        list_push_back(&ready[t->prio], &t->node);
    ready_bitmap |= 1u << t->prio;
}

static void ready_remove(k_tcb_t *t)
{
    list_remove(&t->node);
    if (list_empty(&ready[t->prio]))
        ready_bitmap &= ~(1u << t->prio);
}

static k_tcb_t *ready_highest(void)
{
    /* The idle task is always ready, so the bitmap is never zero. */
    uint32_t prio = 31u - (uint32_t)__builtin_clz(ready_bitmap);
    return container_of(ready[prio].next, k_tcb_t, node);
}

void k_reschedule(void)
{
    if (sched_running && ready_highest() != k_current)
        SCB_ICSR = ICSR_PENDSVSET; /* switch once all ISRs have finished */
}

/* ---- wait queues and the sleep list ------------------------------------ */

/* Wait queues are kept in priority order, FIFO among equals, so the most
 * important waiter is always woken first. */
void k_wq_insert(k_list_t *wait_q, k_tcb_t *t)
{
    k_list_t *it;
    list_for_each(it, wait_q) {
        if (container_of(it, k_tcb_t, node)->prio < t->prio)
            break;
    }
    list_insert_before(it, &t->node);
    t->wait_q = wait_q;
}

k_tcb_t *k_wq_first(k_list_t *wait_q)
{
    return list_empty(wait_q) ? NULL : container_of(wait_q->next, k_tcb_t, node);
}

/* Tick counts wrap after 49 days; comparing the signed difference keeps the
 * ordering correct across the wrap. */
static int time_before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

static void sleep_insert(k_tcb_t *t)
{
    k_list_t *it;
    list_for_each(it, &sleep_list) {
        if (time_before(t->wake_tick, container_of(it, k_tcb_t, tnode)->wake_tick))
            break;
    }
    list_insert_before(it, &t->tnode);
}

void k_block_current(k_list_t *wait_q, uint32_t timeout)
{
    k_tcb_t *t = k_current;

    ready_remove(t);
    t->state = K_STATE_BLOCKED;
    if (wait_q)
        k_wq_insert(wait_q, t);
    if (timeout != K_FOREVER) {
        t->wake_tick = k_tick_count + timeout;
        sleep_insert(t);
    }
    k_trace(K_TR_BLOCK, t->id, 0);
    k_reschedule();
}

void k_wake(k_tcb_t *t, int32_t result)
{
    if (t->wait_q) {
        list_remove(&t->node);
        t->wait_q = NULL;
    }
    if (list_linked(&t->tnode))
        list_remove(&t->tnode);
    t->blocked_on = NULL;
    t->svc_frame[0] = (uint32_t)result; /* becomes the syscall's return value */
    t->state = K_STATE_READY;
    ready_insert(t, 0);
    k_trace(K_TR_WAKE, t->id, 0);
}

void k_wake_all(k_list_t *wait_q, int32_t result)
{
    k_tcb_t *t;
    while ((t = k_wq_first(wait_q)) != NULL)
        k_wake(t, result);
}

/* Change a task's effective priority, keeping whatever list it is on in
 * order. Used by priority inheritance. */
void k_set_prio(k_tcb_t *t, uint8_t prio)
{
    if (t->prio == prio)
        return;
    if (t->state == K_STATE_READY || t->state == K_STATE_RUNNING) {
        ready_remove(t);
        t->prio = prio;
        ready_insert(t, 1);
    } else if (t->wait_q) {
        k_list_t *wq = t->wait_q;
        list_remove(&t->node);
        t->prio = prio;
        k_wq_insert(wq, t);
    } else {
        t->prio = prio;
    }
    k_trace(K_TR_PRIO, t->id, prio);
}

void k_yield_current(void)
{
    k_tcb_t *t = k_current;
    ready_remove(t);
    ready_insert(t, 0); /* back of the line among equal priorities */
    k_reschedule();
}

/* ---- time --------------------------------------------------------------- */

/* A cycle-accurate timestamp built from SysTick, so it works on every board
 * and in QEMU (which does not emulate the DWT cycle counter). */
uint32_t k_now_cycles(void)
{
    uint32_t s = irq_save();
    uint32_t ticks = k_tick_count;
    uint32_t val = SYST_CVR;
    if (SCB_ICSR & ICSR_PENDSTSET) {
        /* SysTick wrapped but its handler has not run yet (interrupts are
         * off): count that tick, and re-read so val is post-wrap. */
        ticks++;
        val = SYST_CVR;
    }
    uint32_t now = ticks * (systick_reload + 1u) + (systick_reload - val);
    irq_restore(s);
    return now;
}

uint32_t k_current_cpu_cycles(void)
{
    return (uint32_t)k_current->run_cycles + (k_now_cycles() - last_switch_cycles);
}

void SysTick_Handler(void)
{
    uint32_t s = irq_save();
    k_tick_count++;

    /* Wake every task whose sleep or timeout has ended. */
    while (!list_empty(&sleep_list)) {
        k_tcb_t *t = container_of(sleep_list.next, k_tcb_t, tnode);
        if (time_before(k_tick_count, t->wake_tick))
            break;
        struct k_mutex *m = t->blocked_on;
        k_wake(t, t->wait_q ? K_ETIMEDOUT : K_OK);
        if (m)
            k_mutex_waiter_gone(m); /* the owner may lose inherited priority */
    }

    /* Round-robin among tasks of the current task's priority. */
    k_tcb_t *c = k_current;
    if (c && c->state == K_STATE_RUNNING && --c->slice_left == 0) {
        c->slice_left = K_SLICE_TICKS;
        k_list_t *q = &ready[c->prio];
        if (q->next != q->prev) { /* someone else is waiting at this prio */
            list_remove(&c->node);
            list_push_back(q, &c->node);
        }
    }

    k_reschedule();
    irq_restore(s);
}

/* ---- the context switch (called from PendSV in context.S) -------------- */

uint32_t *k_switch(uint32_t *old_sp)
{
    uint32_t now = k_now_cycles();
    k_tcb_t *prev = k_current;

    if (prev) {
        prev->sp = old_sp;
        prev->run_cycles += now - last_switch_cycles;
        if (prev->state == K_STATE_RUNNING)
            prev->state = K_STATE_READY;
    }
    last_switch_cycles = now;

    k_tcb_t *next = ready_highest();
    if (next != prev) {
        next->slice_left = K_SLICE_TICKS;
        next->switches++;
        k_trace(K_TR_SWITCH, next->id, next->prio);
    }
    next->state = K_STATE_RUNNING;
    k_current = next;
    k_mpu_load(next);
    return next->sp;
}

/* ---- task lifecycle ----------------------------------------------------- */

#define EXC_RETURN_THREAD_PSP 0xFFFFFFFDu /* thread mode, PSP, no FP frame */
#define XPSR_THUMB            0x01000000u

static uint32_t stack_peak(const k_tcb_t *t)
{
    const uint32_t *p = t->stack_base;
    const uint32_t *end = t->stack_base + t->stack_size / 4u;
    while (p < end && *p == K_STACK_FILL)
        p++;
    return (uint32_t)(end - p) * 4u;
}

int32_t k_task_create_internal(const k_task_cfg_t *cfg)
{
    uint32_t size = cfg->stack_size;
    if (cfg->prio > K_PRIO_MAX || size < K_STACK_MIN ||
        size > K_STACK_MAX || (size & (size - 1u)))
        return K_EINVAL;

    /* Prefer a never-used slot; otherwise recycle a dead task's slot. */
    k_tcb_t *t = NULL;
    for (int i = 0; i < K_MAX_TASKS && !t; i++)
        if (tcbs[i].state == K_STATE_FREE)
            t = &tcbs[i];
    for (int i = 0; i < K_MAX_TASKS && !t; i++)
        if (tcbs[i].state == K_STATE_DEAD)
            t = &tcbs[i];
    if (!t)
        return K_ENOMEM;

    uint32_t *stack = k_stack_alloc(size);
    if (!stack)
        return K_ENOMEM;

    uint8_t id = (uint8_t)(t - tcbs);
    memset(t, 0, sizeof(*t));
    t->id = id;
    t->stack_base = stack;
    t->stack_size = size;
    k_mpu_encode((uint32_t)stack, size, K_MPU_ATTR_STACK, &t->stack_rasr);
    if (cfg->grant_size) {
        t->grant_base = cfg->grant_base;
        t->grant_size = cfg->grant_size;
        k_mpu_encode(cfg->grant_base, cfg->grant_size, K_MPU_ATTR_DEVICE, &t->grant_rasr);
    }
    for (int i = 0; i < K_NAME_LEN - 1 && cfg->name && cfg->name[i]; i++)
        t->name[i] = cfg->name[i];

    /* Paint the stack so we can later see how deep it ever got. */
    for (uint32_t i = 0; i < size / 4u; i++)
        stack[i] = K_STACK_FILL;

    /* Build a fake "interrupted" frame so the first switch to this task
     * looks exactly like returning to it after a preemption. */
    uint32_t *sp = stack + size / 4u;
    *--sp = XPSR_THUMB;                                 /* xPSR            */
    *--sp = (uint32_t)k_user_task_entry & ~1u;          /* pc              */
    *--sp = 0;                                          /* lr              */
    *--sp = 0;                                          /* r12             */
    *--sp = 0;                                          /* r3              */
    *--sp = 0;                                          /* r2              */
    *--sp = (uint32_t)cfg->arg;                         /* r1 = arg        */
    *--sp = (uint32_t)cfg->entry;                       /* r0 = entry      */
    *--sp = EXC_RETURN_THREAD_PSP;                      /* EXC_RETURN      */
    for (int r = 11; r >= 4; r--)
        *--sp = 0;                                      /* r11..r4         */
    *--sp = 1;                                          /* CONTROL: nPRIV  */
    t->sp = sp;

    t->prio = t->base_prio = cfg->prio;
    t->state = K_STATE_READY;
    ready_insert(t, 0);
    k_trace(K_TR_CREATE, id, cfg->prio);
    k_reschedule();
    return id;
}

void k_task_kill(k_tcb_t *t, int32_t code)
{
    if (t->state == K_STATE_FREE || t->state == K_STATE_DEAD)
        return;

    if (t == k_current) {
        uint32_t now = k_now_cycles();
        t->run_cycles += now - last_switch_cycles;
        last_switch_cycles = now;
        /* If the task used the FPU, the CPU may still owe a "lazy" save of
         * s0-s15 into this task's stack. That stack is about to be freed,
         * so cancel the pending save rather than let it scribble on memory
         * that may soon belong to another task. */
        FPU_FPCCR &= ~FPCCR_LSPACT;
    }

    if (t->state == K_STATE_READY || t->state == K_STATE_RUNNING)
        ready_remove(t);
    if (t->wait_q) {
        list_remove(&t->node);
        t->wait_q = NULL;
    }
    if (list_linked(&t->tnode))
        list_remove(&t->tnode);
    struct k_mutex *m = t->blocked_on;
    t->blocked_on = NULL;
    if (m)
        k_mutex_waiter_gone(m);

    t->state = K_STATE_DEAD;
    t->exit_code = code;
    k_mutex_owner_died(t); /* robust mutexes: hand them to the next waiter */
    t->stack_peak = stack_peak(t);
    k_stack_free(t->stack_base, t->stack_size);
    k_trace(K_TR_EXIT, t->id, (uint16_t)code);

    if (t == k_current)
        k_current = NULL; /* PendSV will not save the dead task's context */
    k_reschedule();
}

k_tcb_t *k_task_by_id(int tid)
{
    if (tid < 0 || tid >= K_MAX_TASKS || tcbs[tid].state == K_STATE_FREE)
        return NULL;
    return &tcbs[tid];
}

int32_t k_task_fill_info(int tid, k_task_info_t *out)
{
    k_tcb_t *t = k_task_by_id(tid);
    if (!t)
        return K_EINVAL;
    uint64_t cycles = t->run_cycles;
    if (t == k_current)
        cycles += k_now_cycles() - last_switch_cycles;
    memcpy(out->name, t->name, K_NAME_LEN);
    out->id = t->id;
    out->state = t->state;
    out->prio = t->prio;
    out->base_prio = t->base_prio;
    out->stack_size = t->stack_size;
    out->stack_peak = t->state == K_STATE_DEAD ? t->stack_peak : stack_peak(t);
    out->run_cycles_lo = (uint32_t)cycles;
    out->run_cycles_hi = (uint32_t)(cycles >> 32);
    out->switches = t->switches;
    out->fault_cfsr = t->fault_cfsr;
    out->fault_addr = t->fault_addr;
    out->fault_pc = t->fault_pc;
    out->exit_code = t->exit_code;
    return K_OK;
}

/* ---- boot --------------------------------------------------------------- */

static void idle_task(void *arg)
{
    (void)arg;
    for (;;)
        __asm volatile("wfi"); /* sleep the core until the next interrupt */
}

void k_sched_init(void)
{
    for (int p = 0; p < K_NUM_PRIO; p++)
        list_init(&ready[p]);
    list_init(&sleep_list);

    SCB_SHPR2 = (uint32_t)PRIO_SVC << 24;
    SCB_SHPR3 = ((uint32_t)PRIO_SYSTICK << 24) | ((uint32_t)PRIO_PENDSV << 16);

    static const k_task_cfg_t idle = {
        .name = "idle", .entry = idle_task, .prio = K_PRIO_IDLE, .stack_size = 512,
    };
    k_task_create_internal(&idle);
}

void k_sched_start(void)
{
    systick_reload = CPU_HZ / K_TICK_HZ - 1u;
    SYST_RVR = systick_reload;
    SYST_CVR = 0;
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;

    sched_running = 1;
    last_switch_cycles = k_now_cycles();
    k_arch_start();
}
