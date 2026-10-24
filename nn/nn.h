/*
 * nn.h - a tiny int8 neural-network runtime for Cortex-M4.
 *
 * Tensors are int8, laid out NHWC (channels last). A real value r is stored
 * as an int8 q with  r = scale * (q - zero_point)  ("affine quantization",
 * the scheme used by TensorFlow Lite and TensorRT INT8).
 *
 * The only compute layer is a fused  conv2d + bias + requantize + ReLU.
 * A fully-connected layer is just a 1x1 convolution on a 1x1 image, so it
 * runs through the same kernels.
 *
 * Every conv can run three ways, all producing bit-identical results:
 *   NN_NAIVE  six nested loops; the readable reference
 *   NN_SIMD   im2col + Cortex-M4 DSP instructions (2 multiply-adds per op)
 *   NN_TILED  NN_SIMD plus 2x2 register blocking (each load reused twice)
 */
#ifndef KESTREL_NN_H
#define KESTREL_NN_H

#include <stdint.h>

typedef enum { NN_NAIVE = 0, NN_SIMD = 1, NN_TILED = 2, NN_NUM_IMPLS = 3 } nn_impl_t;

typedef struct {
    uint16_t in_h, in_w, in_c;  /* input shape (output has the same H, W) */
    uint16_t out_c;
    uint8_t k, pad;             /* square kernel, stride 1                 */
    uint16_t col_len;           /* k*k*in_c: values in one input patch     */
    uint16_t col_stride;        /* col_len rounded up to a multiple of 4   */
    int8_t in_zp;               /* input zero point (used for padding)     */
    int8_t out_zp;
    int8_t act_min, act_max;    /* clamp range; act_min = out_zp is ReLU   */
    const int8_t *weights;      /* [out_c][col_stride], zero padded        */
    const int32_t *bias;        /* [out_c], input zero point folded in     */
    const int32_t *mult;        /* [out_c] fixed-point requant multiplier  */
    const int8_t *shift;        /* [out_c] right shift after multiply      */
} nn_conv_t;

/* scratch must hold 2 * col_stride int16 values (im2col buffer). */
void nn_conv2d(const nn_conv_t *L, const int8_t *in, int8_t *out, int16_t *scratch,
               nn_impl_t impl);
void nn_maxpool2x2(const int8_t *in, int8_t *out, int h, int w, int c);

/* Fixed-point requantization: round(acc * mult / 2^shift). Exactly the same
 * arithmetic is used by the Python reference (tools/nn/intref.py). */
static inline int32_t nn_requant(int32_t acc, int32_t mult, int32_t shift)
{
    int64_t t = (int64_t)acc * mult + ((int64_t)1 << (shift - 1));
    if (shift >= 32) /* the usual case: floor(t / 2^32), then a 32-bit shift */
        return (int32_t)(t >> 32) >> (shift - 32); /* t >> 32 = high word, free */
    return (int32_t)(t >> shift);
}

#endif
