/*
 * app.c - the first user task ("init"). It runs unprivileged and builds
 * the rest of the system with system calls.
 */
#include "app.h"
#include "board.h"

/* Blink the board LED. This task holds an MPU grant for the LED's GPIO
 * port (inherited from init), so it may write those registers directly,
 * but nothing else on the bus. */
static void heartbeat(void *arg)
{
    (void)arg;
    volatile uint32_t *bsrr = (volatile uint32_t *)(LED_GPIO_BASE + 0x18u);
    const uint32_t on = 1u << (LED_PIN + (LED_ACTIVE_LOW ? 16u : 0u));
    const uint32_t off = 1u << (LED_PIN + (LED_ACTIVE_LOW ? 0u : 16u));
    for (;;) {
        *bsrr = on;
        k_sleep(100);
        *bsrr = off;
        k_sleep(900);
    }
}

void app_init(void *arg)
{
    (void)arg;
    static const k_task_cfg_t hb = {
        .name = "heartbeat", .entry = heartbeat, .prio = PRIO_HEARTBEAT, .stack_size = 512,
        .grant_base = LED_GPIO_BASE, .grant_size = 0x400,
    };
    k_task_create(&hb);

#ifdef KESTREL_AUTOTEST
    int failures = run_test_suite();
    k_halt(failures ? 1 : 0);
#else
    shell_main();
#endif
}
