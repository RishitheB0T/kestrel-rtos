#ifndef KESTREL_APP_H
#define KESTREL_APP_H

#include "kestrel.h"

/* Priorities used by the application. */
#define PRIO_INIT      10   /* init / shell / test runner                 */
#define PRIO_HEARTBEAT 2

/* demos.c */
typedef struct {
    uint32_t h_wait_us;   /* how long the high-priority task waited      */
    uint8_t l_peak_prio;  /* highest priority the low task ran at        */
    uint32_t cs_ms, m_busy_ms;
} pi_result_t;

int demo_priority_inversion(int use_pi, pi_result_t *res);

enum crash_kind {
    CRASH_KERNEL_WRITE, /* write kernel RAM                   -> MemManage */
    CRASH_STACK,        /* unbounded recursion                -> MemManage */
    CRASH_NULL,         /* read address 0                     -> MemManage */
    CRASH_PERIPH,       /* touch RCC without a grant          -> MemManage */
    CRASH_PPB,          /* touch SysTick (privileged-only)    -> BusFault  */
    CRASH_DIV0,         /* integer divide by zero             -> UsageFault */
    CRASH_OTHER_STACK,  /* read another task's stack          -> MemManage */
    CRASH_NUM_KINDS
};
extern const char *const crash_names[CRASH_NUM_KINDS];
int demo_crash(enum crash_kind kind, k_task_info_t *info);

void busy_cpu_ms(uint32_t ms);
int wait_task_dead(int tid, uint32_t timeout_ms, k_task_info_t *info);
void trace_dump(void);

/* nn_app.c */
typedef struct {
    uint32_t cycles_per_inference[3];
    int mismatches[3];
    int correct, total, timed;
} nn_bench_t;

void nn_bench(nn_bench_t *b);
void nn_print_bench(const nn_bench_t *b);
void nn_demo(int count);

/* tests.c */
int run_test_suite(void);

/* shell.c */
void shell_main(void);

/* regs_torture.S: fill r4-r11 and s0-s31, spin, verify. 0 = intact. */
int regs_torture(uint32_t seed, uint32_t loops);

#endif
