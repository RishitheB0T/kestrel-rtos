/*
 * tests.c - the on-target test suite. Runs on real hardware (shell command
 * `test`) and in QEMU (`make test`, used by CI). Each test returns 1 on
 * success; CHECK() prints the failing condition and returns 0.
 *
 * The test runner is the init task at priority 10. Helper tasks are created
 * above or below that priority depending on what is being tested.
 */
#include <string.h>

#include "app.h"
#include "board.h"
#include "model_gen.h"

extern volatile uint32_t k_tick_count; /* kernel RAM: tasks may not touch it */

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            k_printf("    check failed: %s  (tests.c:%d)\n", #cond, __LINE__);   \
            return 0;                                                            \
        }                                                                        \
    } while (0)

#define CFSR_DACCVIOL   (1u << 1)
#define CFSR_MSTKERR    (1u << 4)
#define CFSR_MMARVALID  (1u << 7)
#define CFSR_DIVBYZERO  (1u << 25)

static int spawn(const char *name, void (*fn)(void *), void *arg, uint8_t prio, uint32_t stack)
{
    k_task_cfg_t cfg = {.name = name, .entry = fn, .arg = arg, .prio = prio, .stack_size = stack};
    return k_task_create(&cfg);
}

static int wait_dead(int tid)
{
    k_task_info_t info;
    return wait_task_dead(tid, 2000, &info) == K_OK;
}

/* ---------------------------------------------------------------------- */

static int test_syscall_basics(void)
{
    k_task_info_t info;
    CHECK(k_task_self() >= 0);
    uint32_t t0 = k_ticks();
    k_sleep(10);
    uint32_t dt = k_ticks() - t0;
    CHECK(dt >= 10 && dt <= 11);
    CHECK(k_sem_take(12345, K_NO_WAIT) == K_EINVAL);   /* garbage handle  */
    int s = k_sem_create(1, 1);
    CHECK(s >= 0);
    CHECK(k_mutex_unlock(s) == K_EINVAL);              /* wrong type      */
    CHECK(k_sem_take(s, K_NO_WAIT) == K_OK);
    CHECK(k_sem_take(s, K_NO_WAIT) == K_EAGAIN);       /* count is zero   */
    CHECK(k_task_info(99, &info) == K_EINVAL);
    k_task_cfg_t bad = {.name = "bad", .entry = (void (*)(void *))k_yield, .prio = 40,
                        .stack_size = 1024};
    CHECK(k_task_create(&bad) == K_EINVAL);            /* priority > 31   */
    bad.prio = 5, bad.stack_size = 1000;
    CHECK(k_task_create(&bad) == K_EINVAL);            /* not a power of 2 */
    return 1;
}

static volatile int flag;
static void set_flag(void *arg) { flag = (int)(uint32_t)arg; }

static int test_preemption(void)
{
    flag = 0;
    int hi = spawn("hi", set_flag, (void *)1, PRIO_INIT + 5, 512);
    CHECK(hi >= 0);
    CHECK(flag == 1); /* higher priority: ran before k_task_create returned */
    int lo = spawn("lo", set_flag, (void *)2, PRIO_INIT - 5, 512);
    CHECK(lo >= 0);
    CHECK(flag == 1); /* lower priority: has not run yet */
    k_sleep(2);
    CHECK(flag == 2); /* ran once we blocked */
    return wait_dead(lo);
}

static volatile uint32_t rr_count[3];
static volatile int rr_stop;
static void rr_spin(void *arg)
{
    volatile uint32_t *c = &rr_count[(uint32_t)arg];
    while (!rr_stop)
        (*c)++;
}

static int test_round_robin(void)
{
    int tid[3];
    rr_stop = 0;
    for (int i = 0; i < 3; i++) {
        rr_count[i] = 0;
        tid[i] = spawn("spin", rr_spin, (void *)(uint32_t)i, 5, 512);
        CHECK(tid[i] >= 0);
    }
    k_sleep(100);
    rr_stop = 1;
    uint32_t total = rr_count[0] + rr_count[1] + rr_count[2];
    k_task_info_t info;
    for (int i = 0; i < 3; i++) {
        k_task_info(tid[i], &info);
        k_printf("    task %d: %u%% of the spins, %u time slices\n", i,
                 (unsigned)(rr_count[i] * 100u / total), (unsigned)info.switches);
        CHECK(rr_count[i] > total / 5); /* fair share is 1/3 */
        CHECK(info.switches >= 4);      /* ~100 ms / 5 ms slices / 3 tasks */
    }
    for (int i = 0; i < 3; i++)
        CHECK(wait_dead(tid[i]));
    return 1;
}

static int test_sleep_accuracy(void)
{
    static const uint32_t delays[] = {1, 7, 20, 50};
    for (unsigned i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
        uint32_t t0 = k_ticks();
        k_sleep(delays[i]);
        uint32_t dt = k_ticks() - t0;
        CHECK(dt >= delays[i] && dt <= delays[i] + 1);
    }
    return 1;
}

static int ping_sem = -1, pong_sem = -1, pp_done = -1;
#define PP_ROUNDS 1000
static void pinger(void *arg)
{
    (void)arg;
    for (int i = 0; i < PP_ROUNDS; i++) {
        k_sem_give(ping_sem);
        k_sem_take(pong_sem, K_FOREVER);
    }
    k_sem_give(pp_done);
}
static void ponger(void *arg)
{
    (void)arg;
    for (int i = 0; i < PP_ROUNDS; i++) {
        k_sem_take(ping_sem, K_FOREVER);
        k_sem_give(pong_sem);
    }
    k_sem_give(pp_done);
}

static int run_ping_pong(uint32_t *cycles)
{
    if (ping_sem < 0) {
        ping_sem = k_sem_create(0, 1);
        pong_sem = k_sem_create(0, 1);
        pp_done = k_sem_create(0, 2);
    }
    uint32_t t0 = k_cycles();
    int a = spawn("ping", pinger, 0, 5, 512);
    int b = spawn("pong", ponger, 0, 5, 512);
    if (a < 0 || b < 0)
        return 0;
    k_sem_take(pp_done, K_FOREVER);
    k_sem_take(pp_done, K_FOREVER);
    *cycles = k_cycles() - t0;
    return wait_dead(a) && wait_dead(b);
}

static int test_semaphore_ping_pong(void)
{
    uint32_t c;
    CHECK(run_ping_pong(&c));
    k_printf("    %d round trips (4 syscalls + 2 context switches each): %u cycles each\n",
             PP_ROUNDS, (unsigned)(c / PP_ROUNDS));
    return 1;
}

typedef struct {
    uint32_t seq, check;
} msg_t;
static int q_main = -1, q_done = -1;
static volatile int q_errors;
#define Q_MSGS 200
static void producer(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < Q_MSGS; i++) {
        msg_t m = {i, i * 2654435761u};
        if (k_queue_send(q_main, &m, K_FOREVER) != K_OK)
            q_errors++;
    }
    k_sem_give(q_done);
}
static void consumer(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < Q_MSGS; i++) {
        msg_t m;
        if (k_queue_recv(q_main, &m, K_FOREVER) != K_OK || m.seq != i ||
            m.check != i * 2654435761u)
            q_errors++;
    }
    k_sem_give(q_done);
}

static int test_queue_fifo(void)
{
    if (q_main < 0) {
        q_main = k_queue_create(sizeof(msg_t), 4);
        q_done = k_sem_create(0, 2);
    }
    CHECK(q_main >= 0);
    /* Round 1: consumer more important -> it is always waiting, so items
     * are copied straight into its buffer. Round 2: producer more important
     * -> the queue fills up and the producer blocks on "full". */
    for (int round = 0; round < 2; round++) {
        q_errors = 0;
        int c = spawn("consumer", consumer, 0, round == 0 ? 6 : 4, 1024);
        int p = spawn("producer", producer, 0, round == 0 ? 4 : 6, 1024);
        CHECK(c >= 0 && p >= 0);
        k_sem_take(q_done, K_FOREVER);
        k_sem_take(q_done, K_FOREVER);
        CHECK(q_errors == 0);
        CHECK(wait_dead(c) && wait_dead(p));
    }
    return 1;
}

static int test_queue_timeout(void)
{
    static int q = -1;
    msg_t m = {1, 2};
    if (q < 0)
        q = k_queue_create(sizeof(msg_t), 1);
    CHECK(q >= 0);
    uint32_t t0 = k_ticks();
    CHECK(k_queue_recv(q, &m, 15) == K_ETIMEDOUT);
    uint32_t dt = k_ticks() - t0;
    CHECK(dt >= 15 && dt <= 16);
    CHECK(k_queue_send(q, &m, K_NO_WAIT) == K_OK);
    CHECK(k_queue_send(q, &m, K_NO_WAIT) == K_EAGAIN); /* full */
    CHECK(k_queue_send(q, &m, 5) == K_ETIMEDOUT);
    CHECK(k_queue_recv(q, &m, K_NO_WAIT) == K_OK && m.seq == 1 && m.check == 2);
    return 1;
}

static int test_priority_inheritance(void)
{
    pi_result_t off, on;
    CHECK(demo_priority_inversion(0, &off) == K_OK);
#ifdef KESTREL_AUTOTEST
    trace_dump(); /* CI turns this into build/trace_pi_off.json */
#endif
    CHECK(demo_priority_inversion(1, &on) == K_OK);
#ifdef KESTREL_AUTOTEST
    trace_dump();
#endif
    k_printf("    without PI: H waited %u us, L stayed at prio %u\n", (unsigned)off.h_wait_us,
             off.l_peak_prio);
    k_printf("    with PI:    H waited %u us, L was boosted to prio %u\n", (unsigned)on.h_wait_us,
             on.l_peak_prio);
    CHECK(off.l_peak_prio == 3);
    CHECK(off.h_wait_us > off.m_busy_ms * 1000u);   /* stuck behind M     */
    CHECK(on.l_peak_prio == 6);                     /* inherited H's prio */
    CHECK(on.h_wait_us < on.cs_ms * 1000u);         /* only waited for L  */
    return 1;
}

/* Transitive inheritance: C waits for B, B waits for A -> A runs at C's. */
static int m1 = -1, m2 = -1;
static volatile int a_has, b_has, chain_release;
static volatile uint8_t a_prio_after;
static void chain_a(void *arg)
{
    (void)arg;
    k_task_info_t info;
    k_mutex_lock(m1, K_FOREVER);
    a_has = 1;
    while (!chain_release)
        k_sleep(1);
    k_mutex_unlock(m1);
    k_task_info(k_task_self(), &info);
    a_prio_after = info.prio;
}
static void chain_b(void *arg)
{
    (void)arg;
    k_mutex_lock(m2, K_FOREVER);
    b_has = 1;
    k_mutex_lock(m1, K_FOREVER);
    k_mutex_unlock(m1);
    k_mutex_unlock(m2);
}
static void chain_c(void *arg)
{
    (void)arg;
    k_mutex_lock(m2, K_FOREVER);
    k_mutex_unlock(m2);
}

static int test_pi_chain(void)
{
    k_task_info_t ia, ib;
    if (m1 < 0) {
        m1 = k_mutex_create(0);
        m2 = k_mutex_create(0);
    }
    a_has = b_has = chain_release = 0;
    int a = spawn("chain-a", chain_a, 0, 3, 1024);
    while (!a_has)
        k_sleep(1);
    int b = spawn("chain-b", chain_b, 0, 5, 1024);
    while (!b_has)
        k_sleep(1);
    k_sleep(2); /* B is now blocked on m1 */
    int c = spawn("chain-c", chain_c, 0, 8, 1024);
    k_sleep(2); /* C is now blocked on m2 */
    k_task_info(a, &ia);
    k_task_info(b, &ib);
    k_printf("    A: base %u -> effective %u, B: base %u -> effective %u\n", ia.base_prio, ia.prio,
             ib.base_prio, ib.prio);
    CHECK(ib.prio == 8); /* B inherits from C directly            */
    CHECK(ia.prio == 8); /* A inherits through B (transitively)   */
    chain_release = 1;
    CHECK(wait_dead(a) && wait_dead(b) && wait_dead(c));
    CHECK(a_prio_after == 3); /* boost dropped as soon as A unlocked */
    return 1;
}

static int dl_m1 = -1, dl_m2 = -1;
static volatile int dl_ready, dl_go, dl_a_result, dl_b_result;
static void dl_a(void *arg)
{
    (void)arg;
    k_mutex_lock(dl_m1, K_FOREVER);
    dl_ready = 1;
    while (!dl_go)
        k_sleep(1);
    dl_a_result = k_mutex_lock(dl_m2, 100); /* B holds m2 and waits for m1 */
    k_mutex_unlock(dl_m1);
}
static void dl_b(void *arg)
{
    (void)arg;
    k_mutex_lock(dl_m2, K_FOREVER);
    dl_b_result = k_mutex_lock(dl_m1, K_FOREVER);
    k_mutex_unlock(dl_m1);
    k_mutex_unlock(dl_m2);
}

static int test_deadlock_detection(void)
{
    if (dl_m1 < 0) {
        dl_m1 = k_mutex_create(0);
        dl_m2 = k_mutex_create(0);
    }
    dl_ready = dl_go = 0;
    dl_a_result = dl_b_result = 1;
    int a = spawn("dl-a", dl_a, 0, 4, 1024);
    while (!dl_ready)
        k_sleep(1);
    int b = spawn("dl-b", dl_b, 0, 5, 1024);
    k_sleep(2);
    dl_go = 1;
    CHECK(wait_dead(a) && wait_dead(b));
    CHECK(dl_a_result == K_EDEADLK); /* refused instead of hanging forever */
    CHECK(dl_b_result == K_OK);
    return 1;
}

static int robust_m = -1;
static volatile int robust_ready, robust_result;
static void robust_holder(void *arg)
{
    (void)arg;
    k_mutex_lock(robust_m, K_FOREVER);
    robust_ready = 1;
    k_sleep(5);
    *(volatile uint32_t *)0 = 1; /* crash while holding the mutex */
}
static void robust_waiter(void *arg)
{
    (void)arg;
    robust_result = k_mutex_lock(robust_m, K_FOREVER);
    if (robust_result == K_EOWNERDEAD)
        k_mutex_unlock(robust_m);
}

static int test_robust_mutex(void)
{
    if (robust_m < 0)
        robust_m = k_mutex_create(0);
    robust_ready = 0;
    robust_result = 1;
    int a = spawn("holder", robust_holder, 0, 4, 1024);
    while (!robust_ready)
        k_sleep(1);
    int b = spawn("waiter", robust_waiter, 0, 5, 1024);
    CHECK(wait_dead(a) && wait_dead(b));
    CHECK(robust_result == K_EOWNERDEAD); /* told the owner died, not hung */
    CHECK(k_mutex_lock(robust_m, K_NO_WAIT) == K_OK); /* and it is usable */
    CHECK(k_mutex_unlock(robust_m) == K_OK);
    return 1;
}

static volatile int torture_result[3];
static uint32_t torture_loops;
static void torture_task(void *arg)
{
    uint32_t i = (uint32_t)arg;
    torture_result[i] = regs_torture(0x1000u * (i + 1), torture_loops);
}

static int test_context_switch_integrity(void)
{
    /* Calibrate: how many spins make ~40 ms of CPU time on this machine? */
    uint32_t c0 = k_cpu_cycles();
    CHECK(regs_torture(0x777, 100000) == 0);
    uint32_t per_100k = k_cpu_cycles() - c0;
    torture_loops = (uint32_t)((uint64_t)100000 * (k_cpu_hz() / 25u) / (per_100k ? per_100k : 1));

    int tid[3];
    for (int i = 0; i < 3; i++) {
        torture_result[i] = -1;
        tid[i] = spawn("regs", torture_task, (void *)(uint32_t)i, 5, 512);
        CHECK(tid[i] >= 0);
    }
    for (int i = 0; i < 3; i++) {
        k_task_info_t info;
        CHECK(wait_task_dead(tid[i], 5000, &info) == K_OK);
        CHECK(torture_result[i] == 0); /* all 40 registers intact */
        CHECK(info.switches >= 3);     /* and it really was preempted */
    }
    return 1;
}

static int expect_fault(enum crash_kind kind, int32_t exit_code, uint32_t cfsr_bits,
                        int check_addr, uint32_t addr)
{
    k_task_info_t info;
    CHECK(demo_crash(kind, &info) == K_OK);
    CHECK(info.exit_code == exit_code);
    CHECK((info.fault_cfsr & cfsr_bits) != 0);
    if (check_addr)
        CHECK(info.fault_addr == addr);
    return 1;
}

static int test_mpu_kernel_memory(void)
{
    return expect_fault(CRASH_KERNEL_WRITE, K_EXIT_MEMFAULT, CFSR_DACCVIOL, 1,
                        (uint32_t)&k_tick_count);
}

static int test_mpu_stack_overflow(void)
{
    return expect_fault(CRASH_STACK, K_EXIT_MEMFAULT, CFSR_DACCVIOL | CFSR_MSTKERR, 0, 0);
}

static int test_null_pointer(void) { return expect_fault(CRASH_NULL, K_EXIT_MEMFAULT, CFSR_DACCVIOL, 1, 0); }

static int test_peripheral_isolation(void)
{
    return expect_fault(CRASH_PERIPH, K_EXIT_MEMFAULT, CFSR_DACCVIOL, 1, 0x40023830u);
}

static int test_ppb_privileged(void)
{
    k_task_info_t info;
    CHECK(demo_crash(CRASH_PPB, &info) == K_OK);
    CHECK(info.exit_code == K_EXIT_BUSFAULT || info.exit_code == K_EXIT_MEMFAULT);
    return 1;
}

static int test_div_by_zero(void)
{
    return expect_fault(CRASH_DIV0, K_EXIT_USAGEFAULT, CFSR_DIVBYZERO, 0, 0);
}

static int test_other_task_stack(void)
{
    return expect_fault(CRASH_OTHER_STACK, K_EXIT_MEMFAULT, CFSR_DACCVIOL, 0, 0);
}

static void noop(void *arg) { (void)arg; }

static int test_syscall_pointer_checks(void)
{
    static int q = -1;
    static uint32_t ram_word;
    k_trace_ev_t *kptr = (k_trace_ev_t *)0x20000100u; /* kernel RAM */
    if (q < 0)
        q = k_queue_create(4, 1);

    /* The kernel must refuse to read or write memory the caller can't. */
    CHECK(k_write((const void *)&k_tick_count, 4) == K_EFAULT);
    CHECK(k_task_info(0, (k_task_info_t *)0x20000000u) == K_EFAULT);
    CHECK(k_queue_recv(q, (void *)0x20000100u, K_NO_WAIT) == K_EFAULT);
    CHECK(k_queue_send(q, (void *)0x20000100u, K_NO_WAIT) == K_EFAULT);
    k_trace_start();
    k_trace_stop();
    CHECK(k_trace_read(0, kptr, 1) == K_EFAULT);

    k_task_cfg_t cfg = {.name = "x", .entry = noop, .prio = PRIO_INIT + 1, .stack_size = 512};
    cfg.entry = (void (*)(void *))(void *)&ram_word;    /* code must be in flash */
    CHECK(k_task_create(&cfg) == K_EFAULT);
    cfg.entry = noop;
    cfg.grant_base = 0x40023800u, cfg.grant_size = 0x400; /* RCC: not ours to give */
    CHECK(k_task_create(&cfg) == K_EPERM);
    cfg.grant_base = 0x20000000u, cfg.grant_size = 0x4000; /* kernel RAM */
    CHECK(k_task_create(&cfg) == K_EPERM);
    cfg.grant_base = LED_GPIO_BASE, cfg.grant_size = 0x400; /* delegating our own: ok */
    int t = k_task_create(&cfg);
    CHECK(t >= 0);
    return wait_dead(t);
}

static int test_stack_pool_reuse(void)
{
    /* Each task runs to completion immediately (higher priority), so 60
     * creations only succeed if stacks and TCBs are recycled correctly. */
    for (int i = 0; i < 60; i++) {
        int t = spawn("churn", noop, 0, PRIO_INIT + 1, i % 2 ? 2048 : 4096);
        CHECK(t >= 0);
    }
    /* And the pool can still hand out several large stacks at once. */
    int tid[4];
    for (int i = 0; i < 4; i++)
        CHECK((tid[i] = spawn("big", noop, 0, PRIO_INIT - 1, 4096)) >= 0);
    for (int i = 0; i < 4; i++)
        CHECK(wait_dead(tid[i]));
    return 1;
}

static void wall_spinner(void *arg)
{
    (void)arg;
    uint32_t t0 = k_ticks();
    while (k_ticks() - t0 < 30) {
    }
}

static int test_cpu_accounting(void)
{
    k_task_info_t info;
    int t = spawn("acct", wall_spinner, 0, PRIO_INIT - 1, 512);
    CHECK(t >= 0);
    CHECK(wait_task_dead(t, 1000, &info) == K_OK);
    uint32_t ms = info.run_cycles_lo / (k_cpu_hz() / 1000u);
    k_printf("    task spun for 30 ms of wall time; kernel charged it %u ms of CPU\n", (unsigned)ms);
    CHECK(ms >= 28 && ms <= 33);
    return 1;
}

static int test_trace_capture(void)
{
    k_trace_ev_t ev[32];
    uint32_t c;
    k_trace_start();
    CHECK(run_ping_pong(&c));
    k_trace_stop();
    int n = k_trace_read(0, ev, 32);
    CHECK(n == 32);
    int switches = 0;
    for (int i = 0; i < n; i++) {
        switches += ev[i].ev == K_TR_SWITCH;
        if (i)
            CHECK((int32_t)(ev[i].ts - ev[i - 1].ts) >= 0); /* time never runs backwards */
    }
    CHECK(switches >= 8);
    return 1;
}

static int test_nn_inference(void)
{
    nn_bench_t b;
    nn_bench(&b);
    nn_print_bench(&b);
    for (int i = 0; i < NN_NUM_IMPLS; i++)
        CHECK(b.mismatches[i] == 0); /* bit-exact with the Python reference */
    CHECK(b.correct * 100 >= b.total * 97);
    return 1;
}

/* ---------------------------------------------------------------------- */

static const struct {
    const char *name;
    int (*fn)(void);
} tests[] = {
    {"syscall_basics", test_syscall_basics},
    {"preemption", test_preemption},
    {"round_robin", test_round_robin},
    {"sleep_accuracy", test_sleep_accuracy},
    {"semaphore_ping_pong", test_semaphore_ping_pong},
    {"queue_fifo", test_queue_fifo},
    {"queue_timeout", test_queue_timeout},
    {"priority_inheritance", test_priority_inheritance},
    {"pi_chain_transitive", test_pi_chain},
    {"deadlock_detection", test_deadlock_detection},
    {"robust_mutex", test_robust_mutex},
    {"context_switch_integrity", test_context_switch_integrity},
    {"mpu_kernel_memory", test_mpu_kernel_memory},
    {"mpu_stack_overflow", test_mpu_stack_overflow},
    {"mpu_null_pointer", test_null_pointer},
    {"mpu_peripheral_isolation", test_peripheral_isolation},
    {"ppb_privileged_only", test_ppb_privileged},
    {"div_by_zero_trap", test_div_by_zero},
    {"mpu_other_task_stack", test_other_task_stack},
    {"syscall_pointer_checks", test_syscall_pointer_checks},
    {"stack_pool_reuse", test_stack_pool_reuse},
    {"cpu_accounting", test_cpu_accounting},
    {"trace_capture", test_trace_capture},
    {"nn_inference_bitexact", test_nn_inference},
};

int run_test_suite(void)
{
    int n = (int)(sizeof(tests) / sizeof(tests[0])), failed = 0;
    uint32_t start = k_ticks();

    k_printf("\n==== Kestrel on-target test suite (%d tests) ====\n", n);
    for (int i = 0; i < n; i++) {
        k_printf("[ RUN    ] %s\n", tests[i].name);
        uint32_t t0 = k_ticks();
        int ok = tests[i].fn();
        failed += !ok;
        k_printf("[ %s ] %s (%u ms)\n", ok ? "    OK" : "FAILED", tests[i].name,
                 (unsigned)(k_ticks() - t0));
    }
    k_printf("==== %d/%d tests passed in %u ms ====\n", n - failed, n,
             (unsigned)(k_ticks() - start));
    k_printf(failed ? "TESTS FAILED\n" : "ALL TESTS PASSED\n");
    return failed;
}
