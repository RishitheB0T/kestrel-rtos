/*
 * boot.c - kernel entry point: initialise everything, start the first task.
 *
 * After this, main() never runs again. The first user task, `init`, runs
 * app_init() unprivileged, exactly like PID 1 on Unix: it creates the rest
 * of the system through system calls.
 */
#include "arch.h"
#include "board.h"
#include "kernel.h"

#define RCC_AHB1ENR REG32(0x40023830u)
#define DBGMCU_CR   REG32(0xE0042004u)

/* Configure the board LED pin as an output. Only the kernel can do this
 * (it needs the RCC clock registers); afterwards a task holding a grant
 * for the GPIO port can toggle the pin directly. */
static void led_init(void)
{
    volatile uint32_t *moder = (volatile uint32_t *)(LED_GPIO_BASE + 0x00u);
    volatile uint32_t *bsrr = (volatile uint32_t *)(LED_GPIO_BASE + 0x18u);

    RCC_AHB1ENR |= 1u << LED_PORT;
    (void)RCC_AHB1ENR;
    *moder = (*moder & ~(3u << (LED_PIN * 2u))) | (1u << (LED_PIN * 2u));
    *bsrr = 1u << (LED_PIN + (LED_ACTIVE_LOW ? 0u : 16u)); /* LED off */
}

int main(void)
{
#ifndef BOARD_QEMU
    /* Keep the debug port clocked while the idle task sleeps in WFI, so the
     * ST-LINK can still connect and reflash a running board. */
    DBGMCU_CR |= 0x7u; /* DBG_SLEEP | DBG_STOP | DBG_STANDBY */
#endif
    k_console_init();
    kprintf("\n\nKestrel RTOS - %s, %u MHz\n", BOARD_NAME, (unsigned)(CPU_HZ / 1000000u));

    k_mpu_init();
    led_init();
    k_stack_pool_init();
    k_sync_init();
    k_sched_init(); /* also creates the idle task */

    static const k_task_cfg_t init = {
        .name = "init",
        .entry = app_init,
        .prio = 10,
        .stack_size = 4096,         /* the shell's `ps` keeps 16 records */
        .grant_base = LED_GPIO_BASE, /* init may toggle the LED and pass */
        .grant_size = 0x400,         /* this right on to its children    */
    };
    if (k_task_create_internal(&init) < 0)
        k_panic("cannot create init task");

    kprintf("[kernel] MPU on, starting scheduler\n");
    k_sched_start();
}
