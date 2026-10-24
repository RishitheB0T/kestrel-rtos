#include "kestrel.h"

static void worker(void *arg)
{
    for (int i = 0; i < 3; i++) {
        k_printf("%s: tick %u\n", (const char *)arg, (unsigned)k_ticks());
        k_sleep(10);
    }
}

void app_init(void *arg)
{
    (void)arg;
    k_printf("init: hello from an unprivileged task (id %d)\n", k_task_self());
    k_task_cfg_t a = {.name = "A", .entry = worker, .arg = "A", .prio = 5, .stack_size = 1024};
    k_task_cfg_t b = {.name = "B", .entry = worker, .arg = "B", .prio = 5, .stack_size = 1024};
    k_printf("create A -> %d\n", k_task_create(&a));
    k_printf("create B -> %d\n", k_task_create(&b));
    k_sleep(100);
    k_printf("init: done\n");
    k_halt(0);
}
