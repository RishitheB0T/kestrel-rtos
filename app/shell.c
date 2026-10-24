/*
 * shell.c - a small interactive shell over the serial console.
 */
#include <string.h>

#include "app.h"

static const char *const state_names[] = {"free", "ready", "RUN", "blocked", "dead"};

/* "6", or "3>6" when the task is running on an inherited priority. */
static void fmt_prio(char *out, const k_task_info_t *t)
{
    char *p = out;
    if (t->prio != t->base_prio) {
        if (t->base_prio >= 10)
            *p++ = (char)('0' + t->base_prio / 10);
        *p++ = (char)('0' + t->base_prio % 10);
        *p++ = '>';
    }
    if (t->prio >= 10)
        *p++ = (char)('0' + t->prio / 10);
    *p++ = (char)('0' + t->prio % 10);
    *p = '\0';
}

/* CPU usage is reported over the interval since the previous `ps`. */
static void cmd_ps(void)
{
    static uint64_t prev_task[16];
    static uint32_t prev_total;
    uint64_t now_task[16] = {0};
    k_task_info_t info[16];
    int valid[16] = {0};
    uint64_t sum = 0;

    uint32_t total = k_cycles();
    for (int i = 0; i < 16; i++) {
        if (k_task_info(i, &info[i]) == K_OK) {
            valid[i] = 1;
            now_task[i] = ((uint64_t)info[i].run_cycles_hi << 32) | info[i].run_cycles_lo;
        }
    }
    uint32_t window = total - prev_total;

    k_printf(" ID NAME         STATE    PRIO   STACK used/size   CPU%%  SWITCHES  EXIT\n");
    for (int i = 0; i < 16; i++) {
        if (!valid[i])
            continue;
        k_task_info_t *t = &info[i];
        uint64_t used = now_task[i] >= prev_task[i] ? now_task[i] - prev_task[i] : now_task[i];
        sum += used;
        uint32_t pct10 = window ? (uint32_t)(used * 1000u / window) : 0;
        char prio[8];
        fmt_prio(prio, t);
        k_printf(" %2d %-12s %-8s %5s %8u/%-6u %3u.%u %9u  ", t->id, t->name,
                 state_names[t->state], prio, (unsigned)t->stack_peak, (unsigned)t->stack_size,
                 (unsigned)(pct10 / 10), (unsigned)(pct10 % 10), (unsigned)t->switches);
        if (t->state == K_STATE_DEAD)
            k_printf("%d%s\n", (int)t->exit_code, t->fault_cfsr ? " (fault)" : "");
        else
            k_printf("-\n");
        prev_task[i] = now_task[i];
    }
    prev_total = total;
}

static void cmd_pi(const char *arg)
{
    int modes[2] = {0, 1}, first = 0, last = 1;
    if (!strcmp(arg, "on"))
        first = 1;
    else if (!strcmp(arg, "off"))
        last = 0;
    for (int i = first; i <= last; i++) {
        pi_result_t r;
        if (demo_priority_inversion(modes[i], &r) != K_OK) {
            k_printf("pi demo failed to start\n");
            return;
        }
        k_printf("priority inheritance %-3s: high-prio task waited %5u us, "
                 "low task ran at prio up to %u  (critical section %u ms, mid task %u ms)\n",
                 modes[i] ? "ON" : "OFF", (unsigned)r.h_wait_us, r.l_peak_prio,
                 (unsigned)r.cs_ms, (unsigned)r.m_busy_ms);
    }
    k_printf("(run `trace` to dump the last run's timeline)\n");
}

static void cmd_crash(const char *arg)
{
    for (int k = 0; k < CRASH_NUM_KINDS; k++) {
        if (!strcmp(arg, crash_names[k])) {
            k_task_info_t info;
            int r = demo_crash((enum crash_kind)k, &info);
            if (r == K_OK)
                k_printf("shell: still alive. crash task exit code %d, cfsr=0x%08x\n",
                         (int)info.exit_code, (unsigned)info.fault_cfsr);
            else
                k_printf("shell: crash task did not die (%d)\n", r);
            return;
        }
    }
    k_printf("usage: crash <");
    for (int k = 0; k < CRASH_NUM_KINDS; k++)
        k_printf("%s%s", crash_names[k], k + 1 < CRASH_NUM_KINDS ? "|" : ">\n");
}

static void cmd_help(void)
{
    static const char *const lines[] = {
        "  ps               tasks, stack high-water marks, CPU usage",
        "  pi [on|off]      priority inversion experiment (default: both)",
        "  crash <kind>     run a task that faults; the kernel kills only it",
        "  nn [count]       classify some test digits (shown as ASCII art)",
        "  bench            time the 3 NN kernels on all test images",
        "  test             run the full on-target test suite",
        "  trace            dump the last trace capture (for trace2perfetto.py)",
        "  uptime           time since boot",
        "  reboot           reset the board",
    };
    k_printf("commands:\n");
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        k_printf("%s\n", lines[i]);
}

static int read_line(char *buf, int size)
{
    int len = 0;
    for (;;) {
        char c;
        if (k_read(&c, 1, K_FOREVER) != 1)
            continue;
        if (c == '\r' || c == '\n') {
            k_write("\n", 1);
            buf[len] = '\0';
            return len;
        }
        if ((c == 0x7F || c == '\b') && len > 0) {
            len--;
            k_write("\b \b", 3);
        } else if (c >= ' ' && c < 0x7F && len < size - 1) {
            buf[len++] = c;
            k_write(&c, 1);
        }
    }
}

void shell_main(void)
{
    char line[64];

    k_printf("\nKestrel shell. Type 'help'.\n");
    for (;;) {
        k_printf("kestrel> ");
        read_line(line, sizeof(line));

        char *cmd = line, *arg = line;
        while (*arg && *arg != ' ')
            arg++;
        if (*arg)
            *arg++ = '\0';

        if (!*cmd)
            continue;
        else if (!strcmp(cmd, "help"))
            cmd_help();
        else if (!strcmp(cmd, "ps"))
            cmd_ps();
        else if (!strcmp(cmd, "pi"))
            cmd_pi(arg);
        else if (!strcmp(cmd, "crash"))
            cmd_crash(arg);
        else if (!strcmp(cmd, "nn")) {
            int n = 0;
            for (const char *c = arg; *c >= '0' && *c <= '9'; c++)
                n = n * 10 + (*c - '0');
            nn_demo(n ? n : 4);
        }
        else if (!strcmp(cmd, "bench")) {
            nn_bench_t b;
            k_printf("running (about 15 s at 16 MHz)...\n");
            nn_bench(&b);
            nn_print_bench(&b);
        } else if (!strcmp(cmd, "test"))
            run_test_suite();
        else if (!strcmp(cmd, "trace"))
            trace_dump();
        else if (!strcmp(cmd, "uptime")) {
            uint32_t t = k_ticks();
            k_printf("up %u.%03u s\n", (unsigned)(t / 1000), (unsigned)(t % 1000));
        } else if (!strcmp(cmd, "reboot"))
            k_reboot();
        else
            k_printf("unknown command '%s' (try 'help')\n", cmd);
    }
}
