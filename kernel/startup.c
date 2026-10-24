/*
 * startup.c - what runs first after reset.
 *
 * On reset the Cortex-M4 reads two words from the vector table: the
 * initial stack pointer (entry 0) and the address of Reset_Handler
 * (entry 1). Reset_Handler then has to build the C runtime by hand:
 * copy initialised variables from flash to RAM, zero the rest, and turn
 * on the FPU before any float instruction can run.
 */
#include <stdint.h>

#include "arch.h"

extern uint32_t _kstack_top;
extern uint32_t _skdata, _ekdata, _skdata_load, _skbss, _ekbss;
extern uint32_t _sdata, _edata, _sdata_load, _sbss, _ebss;

int main(void);
void Reset_Handler(void);
void Default_Handler(void);

/* Handlers implemented elsewhere in the kernel. */
void NMI_Handler(void) __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SVC_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);
void USART2_IRQHandler(void);

#define NUM_IRQS 82 /* STM32F405/407; smaller parts just never raise the rest */

__attribute__((section(".isr_vector"), used))
const void *const vector_table[16 + NUM_IRQS] = {
    [0]  = &_kstack_top,
    [1]  = Reset_Handler,
    [2]  = NMI_Handler,
    [3]  = HardFault_Handler,
    [4]  = MemManage_Handler,
    [5]  = BusFault_Handler,
    [6]  = UsageFault_Handler,
    [11] = SVC_Handler,
    [14] = PendSV_Handler,
    [15] = SysTick_Handler,
    [16 + 38] = USART2_IRQHandler,
};

static void copy_words(uint32_t *dst, const uint32_t *src, const uint32_t *end)
{
    while (dst < end)
        *dst++ = *src++;
}

static void zero_words(uint32_t *dst, const uint32_t *end)
{
    while (dst < end)
        *dst++ = 0;
}

void Reset_Handler(void)
{
    __asm volatile("cpsid i"); /* no interrupts until the kernel is ready */

    copy_words(&_skdata, &_skdata_load, &_ekdata);
    zero_words(&_skbss, &_ekbss);
    copy_words(&_sdata, &_sdata_load, &_edata);
    zero_words(&_sbss, &_ebss);

    SCB_CPACR |= 0xFu << 20; /* full access to CP10/CP11 = the FPU */
    dsb();
    isb();

    SCB_VTOR = (uint32_t)vector_table;

    main();
    for (;;) {
    }
}

/* Any exception we did not expect. Gaps in vector_table are zero, so an
 * unexpected IRQ there escalates to HardFault, which also reports it. */
void k_panic(const char *fmt, ...) __attribute__((noreturn));
void Default_Handler(void)
{
    k_panic("unexpected exception %u", (unsigned)get_ipsr());
}
