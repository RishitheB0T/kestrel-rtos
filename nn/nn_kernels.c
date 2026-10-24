/*
 * nn_kernels.c - int8 convolution and max-pool kernels.
 *
 * All three conv implementations compute exactly the same integers:
 *
 *   acc[oc]  = bias[oc] + sum over the k*k*in_c patch of  x * w[oc]
 *   out[oc]  = clamp(out_zp + requant(acc[oc]), act_min, act_max)
 *
 * The input zero point is folded into bias offline (tools/nn/quantize.py),
 * which is why the inner loop is a plain int8 x int8 dot product.
 */
#include "nn.h"
#include "nn_simd.h"

static inline int8_t finish(const nn_conv_t *L, int32_t acc, int oc)
{
    int32_t v = nn_requant(acc, L->mult[oc], L->shift[oc]) + L->out_zp;
    if (v < L->act_min)
        v = L->act_min; /* with act_min == out_zp this *is* the ReLU */
    if (v > L->act_max)
        v = L->act_max;
    return (int8_t)v;
}

/* ---- 1. naive: the reference everything else is checked against -------- */

static void conv_naive(const nn_conv_t *L, const int8_t *in, int8_t *out)
{
    const int H = L->in_h, W = L->in_w, C = L->in_c, K = L->k, P = L->pad;

    for (int oy = 0; oy < H; oy++)
        for (int ox = 0; ox < W; ox++)
            for (int oc = 0; oc < L->out_c; oc++) {
                const int8_t *w = L->weights + oc * L->col_stride;
                int32_t acc = L->bias[oc];
                for (int ky = 0; ky < K; ky++)
                    for (int kx = 0; kx < K; kx++) {
                        int iy = oy + ky - P, ix = ox + kx - P;
                        int inside = iy >= 0 && iy < H && ix >= 0 && ix < W;
                        for (int ic = 0; ic < C; ic++) {
                            /* padding is "real zero", i.e. the zero point */
                            int32_t x = inside ? in[(iy * W + ix) * C + ic] : L->in_zp;
                            acc += x * w[(ky * K + kx) * C + ic];
                        }
                    }
                out[(oy * W + ox) * L->out_c + oc] = finish(L, acc, oc);
            }
}

/* ---- im2col ------------------------------------------------------------- */

/* Copy the input patch under output pixel (oy, ox) into a flat int16 row,
 * so the conv becomes one dot product per output channel. Each group of
 * four values x0 x1 x2 x3 is stored as x0 x2 x1 x3: then the two 32-bit
 * words are (x0,x2) and (x1,x3), which line up with what SXTB16 and
 * SXTB16-ROR8 extract from four packed int8 weights (w0,w2) and (w1,w3).
 * Reordering here, once per pixel, saves work on every channel. */
static void im2col(const nn_conv_t *L, const int8_t *in, int oy, int ox, int16_t *col)
{
    static const uint8_t lane[4] = {0, 2, 1, 3};
    const int H = L->in_h, W = L->in_w, C = L->in_c, K = L->k, P = L->pad;
    int i = 0;

    for (int ky = 0; ky < K; ky++)
        for (int kx = 0; kx < K; kx++) {
            int iy = oy + ky - P, ix = ox + kx - P;
            int inside = iy >= 0 && iy < H && ix >= 0 && ix < W;
            const int8_t *src = inside ? in + (iy * W + ix) * C : in;
            for (int ic = 0; ic < C; ic++, i++)
                col[(i & ~3) | lane[i & 3]] = inside ? src[ic] : L->in_zp;
        }
    for (; i < L->col_stride; i++)
        col[(i & ~3) | lane[i & 3]] = 0; /* weights are zero there too */
}

/* 4 multiply-accumulates in 2 SMLAD instructions. */
static inline int32_t dot_q7_q15(const int8_t *w, const int16_t *x, int n, int32_t acc)
{
    for (int i = 0; i < n; i += 4) {
        uint32_t wv = rd32(w + i);
        acc = smlad(sxtb16(wv), rd32(x + i), acc);          /* (w0,w2).(x0,x2) */
        acc = smlad(sxtb16_ror8(wv), rd32(x + i + 2), acc); /* (w1,w3).(x1,x3) */
    }
    return acc;
}

/* ---- 2. SIMD: im2col + DSP dot products --------------------------------- */

static void conv_simd(const nn_conv_t *L, const int8_t *in, int8_t *out, int16_t *col)
{
    const int W = L->in_w, OC = L->out_c, S = L->col_stride;

    for (int p = 0; p < L->in_h * W; p++) {
        im2col(L, in, p / W, p % W, col);
        for (int oc = 0; oc < OC; oc++)
            out[p * OC + oc] = finish(L, dot_q7_q15(L->weights + oc * S, col, S, L->bias[oc]), oc);
    }
}

/* ---- 3. tiled: 2 pixels x 2 channels per inner loop --------------------- */

/* Same math, but each loaded weight word is used for two pixels and each
 * loaded input word for two channels, halving memory traffic per MAC.
 * (This is the same "register blocking" idea GPU GEMM kernels use with
 * shared memory and registers, at a much smaller scale.) */
static void conv_tiled(const nn_conv_t *L, const int8_t *in, int8_t *out, int16_t *scratch)
{
    const int W = L->in_w, OC = L->out_c, S = L->col_stride;
    const int npix = L->in_h * W;
    int16_t *colA = scratch, *colB = scratch + S;
    int p = 0;

    for (; p + 1 < npix; p += 2) {
        im2col(L, in, p / W, p % W, colA);
        im2col(L, in, (p + 1) / W, (p + 1) % W, colB);
        int8_t *outA = out + p * OC, *outB = outA + OC;
        int oc = 0;

        for (; oc + 1 < OC; oc += 2) {
            const int8_t *w0 = L->weights + oc * S, *w1 = w0 + S;
            int32_t a0 = L->bias[oc], a1 = L->bias[oc + 1];
            int32_t b0 = a0, b1 = a1;
            for (int i = 0; i < S; i += 4) {
                uint32_t w0v = rd32(w0 + i), w1v = rd32(w1 + i);
                uint32_t w0lo = sxtb16(w0v), w0hi = sxtb16_ror8(w0v);
                uint32_t w1lo = sxtb16(w1v), w1hi = sxtb16_ror8(w1v);
                uint32_t x = rd32(colA + i);
                a0 = smlad(w0lo, x, a0);
                a1 = smlad(w1lo, x, a1);
                x = rd32(colA + i + 2);
                a0 = smlad(w0hi, x, a0);
                a1 = smlad(w1hi, x, a1);
                x = rd32(colB + i);
                b0 = smlad(w0lo, x, b0);
                b1 = smlad(w1lo, x, b1);
                x = rd32(colB + i + 2);
                b0 = smlad(w0hi, x, b0);
                b1 = smlad(w1hi, x, b1);
            }
            outA[oc] = finish(L, a0, oc);
            outA[oc + 1] = finish(L, a1, oc + 1);
            outB[oc] = finish(L, b0, oc);
            outB[oc + 1] = finish(L, b1, oc + 1);
        }
        for (; oc < OC; oc++) { /* odd channel count: finish one by one */
            const int8_t *w = L->weights + oc * S;
            outA[oc] = finish(L, dot_q7_q15(w, colA, S, L->bias[oc]), oc);
            outB[oc] = finish(L, dot_q7_q15(w, colB, S, L->bias[oc]), oc);
        }
    }
    if (p < npix) { /* odd pixel count: last pixel alone */
        im2col(L, in, p / W, p % W, colA);
        for (int oc = 0; oc < OC; oc++)
            out[p * OC + oc] = finish(L, dot_q7_q15(L->weights + oc * S, colA, S, L->bias[oc]), oc);
    }
}

void nn_conv2d(const nn_conv_t *L, const int8_t *in, int8_t *out, int16_t *scratch,
               nn_impl_t impl)
{
    switch (impl) {
    case NN_NAIVE:
        conv_naive(L, in, out);
        break;
    case NN_SIMD:
        conv_simd(L, in, out, scratch);
        break;
    default:
        conv_tiled(L, in, out, scratch);
        break;
    }
}

/* Max-pooling works directly on int8: quantization is monotonic, so the
 * largest int8 is the largest real value, and in/out share scale. */
void nn_maxpool2x2(const int8_t *in, int8_t *out, int h, int w, int c)
{
    for (int oy = 0; oy < h / 2; oy++)
        for (int ox = 0; ox < w / 2; ox++)
            for (int ch = 0; ch < c; ch++) {
                const int8_t *p = in + ((2 * oy) * w + 2 * ox) * c + ch;
                int8_t m = p[0];
                if (p[c] > m)
                    m = p[c];
                if (p[w * c] > m)
                    m = p[w * c];
                if (p[w * c + c] > m)
                    m = p[w * c + c];
                out[(oy * (w / 2) + ox) * c + ch] = m;
            }
}
