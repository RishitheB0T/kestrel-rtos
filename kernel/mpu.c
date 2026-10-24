/*
 * mpu.c - Memory Protection Unit setup.
 *
 * The Cortex-M4 MPU has 8 regions. Each region is a power-of-two sized
 * block, aligned to its own size, with access rules. Our map:
 *
 *   region 0  flash        read-only + executable   for everyone
 *   region 1  URAM         read/write, no execute   for everyone (app data)
 *   region 2  task stack   read/write, no execute   reloaded on every switch
 *   region 3  MMIO grant   read/write device memory reloaded on every switch
 *
 * PRIVDEFENA lets privileged code (the kernel) use the default memory map,
 * so the kernel can reach everything. Unprivileged code can reach only what
 * a region allows: kernel RAM, other tasks' stacks, and peripherals are off
 * limits, and touching them raises a MemManage fault.
 */
#include "arch.h"
#include "board.h"
#include "kernel.h"

#define MPU_CTRL_ENABLE     (1u << 0)
#define MPU_CTRL_PRIVDEFENA (1u << 2)
#define RBAR_VALID          (1u << 4)
#define RASR_ENABLE         (1u << 0)
#define RASR_XN             (1u << 28)
#define RASR_AP(x)          ((uint32_t)(x) << 24)
#define AP_FULL_ACCESS      3u /* privileged RW, unprivileged RW */
#define AP_READ_ONLY        6u /* privileged RO, unprivileged RO */
/* Memory type bits (TEX, S, C, B) */
#define ATTR_FLASH  ((1u << 17))                 /* normal, write-through  */
#define ATTR_SRAM   ((1u << 18) | (1u << 17))    /* normal, shareable      */
#define ATTR_DEVICE ((1u << 18) | (1u << 16))    /* shareable device       */

extern uint32_t _uram_start, _uram_end;

int k_mpu_encode(uint32_t base, uint32_t size, uint32_t attrs, uint32_t *rasr)
{
    if (size < 32 || (size & (size - 1u)) || (base & (size - 1u)))
        return K_EINVAL;
    uint32_t log2 = 31u - (uint32_t)__builtin_clz(size);
    uint32_t bits = attrs == K_MPU_ATTR_DEVICE ? ATTR_DEVICE : ATTR_SRAM;
    *rasr = RASR_XN | RASR_AP(AP_FULL_ACCESS) | bits | ((log2 - 1u) << 1) | RASR_ENABLE;
    return K_OK;
}

static void set_region(uint32_t n, uint32_t base, uint32_t rasr)
{
    MPU_RBAR = base | RBAR_VALID | n; /* VALID: also selects region n */
    MPU_RASR = rasr;
}

void k_mpu_init(void)
{
    uint32_t log2;

    if (((MPU_TYPE >> 8) & 0xFFu) < 8u)
        k_panic("MPU missing or too small");
    MPU_CTRL = 0;

    log2 = 31u - (uint32_t)__builtin_clz(FLASH_REGION_SIZE);
    set_region(0, FLASH_BASE,
               RASR_AP(AP_READ_ONLY) | ATTR_FLASH | ((log2 - 1u) << 1) | RASR_ENABLE);

    uint32_t ubase = (uint32_t)&_uram_start;
    uint32_t usize = (uint32_t)&_uram_end - ubase;
    uint32_t rasr;
    if (k_mpu_encode(ubase, usize, K_MPU_ATTR_STACK, &rasr) != K_OK)
        k_panic("URAM is not a legal MPU region");
    set_region(1, ubase, rasr);

    for (uint32_t n = 2; n < 8; n++)
        set_region(n, 0, 0);

    MPU_CTRL = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA;
    SCB_SHCSR |= SHCSR_MEMFAULTENA | SHCSR_BUSFAULTENA | SHCSR_USGFAULTENA;
    SCB_CCR |= CCR_DIV_0_TRP; /* integer divide by zero traps, not returns 0 */
    dsb();
    isb();
}

/* Called on every context switch (from PendSV, interrupts off). */
void k_mpu_load(const k_tcb_t *t)
{
    set_region(2, (uint32_t)t->stack_base, t->stack_rasr);
    set_region(3, t->grant_base, t->grant_rasr);
    dsb();
    isb();
}
