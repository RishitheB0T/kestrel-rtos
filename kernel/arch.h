/*
 * arch.h - Cortex-M4 core registers and tiny helpers.
 *
 * Everything here is defined in the ARMv7-M Architecture Reference Manual.
 * We deliberately avoid CMSIS so every register the kernel touches is
 * visible in one place.
 */
#ifndef KESTREL_ARCH_H
#define KESTREL_ARCH_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))
#define REG8(addr)  (*(volatile uint8_t *)(addr))

/* ---- System Control Block ---------------------------------------------- */
#define SCB_ICSR   REG32(0xE000ED04u)  /* interrupt control & state       */
#define SCB_VTOR   REG32(0xE000ED08u)  /* vector table offset             */
#define SCB_AIRCR  REG32(0xE000ED0Cu)  /* reset control                   */
#define SCB_CCR    REG32(0xE000ED14u)  /* configuration & control         */
#define SCB_SHPR2  REG32(0xE000ED1Cu)  /* SVCall priority                 */
#define SCB_SHPR3  REG32(0xE000ED20u)  /* PendSV / SysTick priority       */
#define SCB_SHCSR  REG32(0xE000ED24u)  /* fault handler enables           */
#define SCB_CFSR   REG32(0xE000ED28u)  /* configurable fault status       */
#define SCB_HFSR   REG32(0xE000ED2Cu)  /* hard fault status               */
#define SCB_MMFAR  REG32(0xE000ED34u)  /* MemManage fault address         */
#define SCB_BFAR   REG32(0xE000ED38u)  /* BusFault address                */
#define SCB_CPACR  REG32(0xE000ED88u)  /* coprocessor (FPU) access        */
#define FPU_FPCCR  REG32(0xE000EF34u)  /* FP context control              */

#define ICSR_PENDSVSET   (1u << 28)
#define ICSR_PENDSTSET   (1u << 26)
#define AIRCR_SYSRESET   (0x05FA0000u | (1u << 2))
#define CCR_DIV_0_TRP    (1u << 4)
#define SHCSR_MEMFAULTENA (1u << 16)
#define SHCSR_BUSFAULTENA (1u << 17)
#define SHCSR_USGFAULTENA (1u << 18)
#define FPCCR_LSPACT     (1u << 0)     /* a lazy FP save is still pending */

/* CFSR bits we decode in fault reports */
#define CFSR_IACCVIOL    (1u << 0)
#define CFSR_DACCVIOL    (1u << 1)
#define CFSR_MSTKERR     (1u << 4)
#define CFSR_MLSPERR     (1u << 5)
#define CFSR_MMARVALID   (1u << 7)
#define CFSR_PRECISERR   (1u << 9)
#define CFSR_STKERR      (1u << 12)
#define CFSR_BFARVALID   (1u << 15)
#define CFSR_UNDEFINSTR  (1u << 16)
#define CFSR_DIVBYZERO   (1u << 25)

/* ---- SysTick ------------------------------------------------------------ */
#define SYST_CSR REG32(0xE000E010u)
#define SYST_RVR REG32(0xE000E014u)
#define SYST_CVR REG32(0xE000E018u)
#define SYST_CSR_ENABLE    (1u << 0)
#define SYST_CSR_TICKINT   (1u << 1)
#define SYST_CSR_CLKSOURCE (1u << 2)

/* ---- NVIC --------------------------------------------------------------- */
#define NVIC_ISER(n) REG32(0xE000E100u + 4u * (n))
#define NVIC_IPR(n)  REG8(0xE000E400u + (n))

/* ---- MPU (PMSAv7: 8 regions, power-of-two sizes) ------------------------ */
#define MPU_TYPE REG32(0xE000ED90u)
#define MPU_CTRL REG32(0xE000ED94u)
#define MPU_RNR  REG32(0xE000ED98u)
#define MPU_RBAR REG32(0xE000ED9Cu)
#define MPU_RASR REG32(0xE000EDA0u)

/* ---- Address map -------------------------------------------------------- */
#define FLASH_BASE      0x08000000u
#define PERIPH_BASE     0x40000000u
#define PERIPH_END      0x60000000u

/* ---- Exception priorities (STM32F4 implements the top 4 bits) ----------- */
#define PRIO_UART    0x80u
#define PRIO_SVC     0x80u
#define PRIO_SYSTICK 0xC0u
#define PRIO_PENDSV  0xF0u   /* lowest: context switches happen last      */

/* ---- Helpers ------------------------------------------------------------ */

/* Disable interrupts and return the previous state, so sections nest. */
static inline uint32_t irq_save(void)
{
    uint32_t primask;
    __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");
    return primask;
}

static inline void irq_restore(uint32_t primask)
{
    __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
}

static inline void dsb(void) { __asm volatile("dsb" ::: "memory"); }
static inline void isb(void) { __asm volatile("isb" ::: "memory"); }

static inline uint32_t get_ipsr(void)
{
    uint32_t v;
    __asm volatile("mrs %0, ipsr" : "=r"(v));
    return v;
}

static inline uint32_t get_control(void)
{
    uint32_t v;
    __asm volatile("mrs %0, control" : "=r"(v));
    return v;
}

#endif
