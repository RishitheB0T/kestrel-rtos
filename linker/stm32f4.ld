/*
 * Memory layout (64 KiB of SRAM used, so it fits every STM32F4 part):
 *
 *   0x2000C000 +-----------------------+  URAM   16 KiB  (tasks: RW)
 *              | app .data / .bss      |  one MPU region shared by tasks
 *   0x20004000 +-----------------------+  STACKS 32 KiB  (no default access)
 *              | task stacks, each its |  each task gets an MPU region for
 *              | own power-of-2 block  |  exactly its own stack
 *   0x20000000 +-----------------------+  KRAM   16 KiB  (kernel only)
 *              | kernel .data/.bss     |
 *              | MSP stack (bottom)    |  overflow runs below SRAM -> fault
 *              +-----------------------+
 *
 * Kernel variables are placed in KRAM *by file name*: every object built
 * from kernel/ goes there automatically, so no annotation can be forgotten.
 * Task stacks sit right above KRAM, so a stack that overflows downwards hits
 * memory it has no MPU permission for and faults immediately.
 */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH  (rx) : ORIGIN = 0x08000000, LENGTH = 256K
    KRAM   (rw) : ORIGIN = 0x20000000, LENGTH = 16K
    STACKS (rw) : ORIGIN = 0x20004000, LENGTH = 32K
    URAM   (rw) : ORIGIN = 0x2000C000, LENGTH = 16K
}

_uram_start   = ORIGIN(URAM);
_uram_end     = ORIGIN(URAM) + LENGTH(URAM);
_stacks_start = ORIGIN(STACKS);
_stacks_end   = ORIGIN(STACKS) + LENGTH(STACKS);
_flash_start  = ORIGIN(FLASH);
_flash_end    = ORIGIN(FLASH) + LENGTH(FLASH);

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH

    .text :
    {
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > FLASH

    .ARM.exidx : { *(.ARM.exidx*) } > FLASH

    /* ---- kernel RAM ---- */
    .kstack (NOLOAD) :
    {
        . = ALIGN(8);
        _kstack_bottom = .;
        . += 2K;
        _kstack_top = .;
    } > KRAM

    .kdata :
    {
        . = ALIGN(4);
        _skdata = .;
        */kernel/*.o(.data .data.*)
        . = ALIGN(4);
        _ekdata = .;
    } > KRAM AT > FLASH
    _skdata_load = LOADADDR(.kdata);

    .kbss (NOLOAD) :
    {
        . = ALIGN(4);
        _skbss = .;
        */kernel/*.o(.bss .bss.* COMMON)
        . = ALIGN(4);
        _ekbss = .;
    } > KRAM

    /* ---- user RAM ---- */
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > URAM AT > FLASH
    _sdata_load = LOADADDR(.data);

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > URAM
}
