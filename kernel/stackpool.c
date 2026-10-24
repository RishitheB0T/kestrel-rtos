/*
 * stackpool.c - hands out task stacks from the 32 KiB STACKS area.
 *
 * The MPU can only protect a power-of-two block aligned to its own size, so
 * a 2 KiB stack must start at a 2 KiB boundary. We track the pool in 512-byte
 * units with a bitmap and do a first-fit search over aligned positions.
 * (This is a tiny cousin of a buddy allocator; the cost is some internal
 * fragmentation, which the ARMv8-M MPU later removed by dropping the
 * power-of-two rule.)
 */
#include "kernel.h"

#define UNIT 512u

extern uint32_t _stacks_start, _stacks_end;

static uint64_t used_bitmap; /* bit i = unit i in use (64 units = 32 KiB) */
static uint32_t num_units;

void k_stack_pool_init(void)
{
    num_units = ((uint32_t)&_stacks_end - (uint32_t)&_stacks_start) / UNIT;
    if (num_units > 64)
        num_units = 64;
    used_bitmap = 0;
}

static uint64_t run_mask(uint32_t first, uint32_t count)
{
    uint64_t bits = count >= 64 ? ~0ull : ((1ull << count) - 1u);
    return bits << first;
}

uint32_t *k_stack_alloc(uint32_t size)
{
    uint32_t count = size / UNIT;
    uint32_t base = (uint32_t)&_stacks_start;
    for (uint32_t first = 0; first + count <= num_units; first += count) {
        uint32_t addr = base + first * UNIT;
        if (addr & (size - 1u))
            continue; /* not aligned to its own size */
        uint64_t mask = run_mask(first, count);
        if (!(used_bitmap & mask)) {
            used_bitmap |= mask;
            return (uint32_t *)addr;
        }
    }
    return NULL;
}

void k_stack_free(uint32_t *stack, uint32_t size)
{
    uint32_t first = ((uint32_t)stack - (uint32_t)&_stacks_start) / UNIT;
    used_bitmap &= ~run_mask(first, size / UNIT);
}
