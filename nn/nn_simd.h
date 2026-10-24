/*
 * nn_simd.h - the Cortex-M4 DSP instructions the kernels use.
 *
 * On the target these are single instructions (ACLE intrinsics). On a PC
 * (host tests) we model each instruction in plain C, bit for bit, so the
 * exact same kernel code can be tested without hardware.
 *
 *   SXTB16 x        take bytes 0 and 2 of x, sign-extend each to 16 bits
 *   SXTB16 x,ROR#8  same for bytes 1 and 3
 *   SMLAD a,b,acc   acc + a.lo*b.lo + a.hi*b.hi   (two MACs, one cycle)
 */
#ifndef KESTREL_NN_SIMD_H
#define KESTREL_NN_SIMD_H

#include <stdint.h>
#include <string.h>

#if defined(__ARM_FEATURE_SIMD32)
#include <arm_acle.h>

static inline uint32_t sxtb16(uint32_t x) { return (uint32_t)__sxtb16((int32_t)x); }
static inline uint32_t sxtb16_ror8(uint32_t x)
{
    uint32_t r;
    __asm("sxtb16 %0, %1, ror #8" : "=r"(r) : "r"(x)); /* rotate is free here */
    return r;
}
static inline int32_t smlad(uint32_t a, uint32_t b, int32_t acc)
{
    return __smlad((int16x2_t)a, (int16x2_t)b, acc);
}

#else /* portable model of the same instructions */

static inline uint32_t sxtb16(uint32_t x)
{
    uint32_t lo = (uint16_t)(int16_t)(int8_t)(x & 0xFFu);
    uint32_t hi = (uint16_t)(int16_t)(int8_t)((x >> 16) & 0xFFu);
    return lo | (hi << 16);
}
static inline uint32_t sxtb16_ror8(uint32_t x) { return sxtb16((x >> 8) | (x << 24)); }
static inline int32_t smlad(uint32_t a, uint32_t b, int32_t acc)
{
    return acc + (int16_t)(a & 0xFFFFu) * (int16_t)(b & 0xFFFFu) +
           (int16_t)(a >> 16) * (int16_t)(b >> 16);
}
#endif

/* Unaligned-safe 32-bit load; compiles to a single LDR on Cortex-M4.
 * (__builtin_memcpy, because -ffreestanding turns plain memcpy into a real
 * function call - which once made the "fast" kernel 2x slower than naive.) */
static inline uint32_t rd32(const void *p)
{
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    return v;
}

#endif
