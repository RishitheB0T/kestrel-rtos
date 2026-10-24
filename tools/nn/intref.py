"""Bit-exact integer reference of the firmware's NN kernels.

This is the *specification* the C code must match exactly: it produces the
golden outputs that the on-target and host tests compare against.
"""
import numpy as np

from layers import im2col


def requant(acc, mult, shift):
    """round(acc * mult / 2^shift), half rounded up: same as nn_requant()."""
    acc = acc.astype(np.int64)
    mult = mult.astype(np.int64)
    shift = shift.astype(np.int64)
    return (acc * mult + (np.int64(1) << (shift - 1))) >> shift


def conv_int(q, x):
    """x: (N, H, W, C) int8 -> (N, H, W, out_c) int8."""
    n, h, w, _ = x.shape
    cols = im2col(x.astype(np.int64), q.k, q.pad, pad_value=q.in_zp)
    acc = cols @ q.w_q.reshape(q.out_c, -1).T.astype(np.int64) + q.bias_folded.astype(np.int64)
    y = requant(acc, q.mult[None, :], q.shift[None, :]) + q.out_zp
    return np.clip(y, q.act_min, 127).astype(np.int8).reshape(n, h, w, q.out_c)


def maxpool_int(x):
    n, h, w, c = x.shape
    return x.reshape(n, h // 2, 2, w // 2, 2, c).max(axis=(2, 4))


def run_int(qlayers, x_q):
    conv1, conv2, fc = qlayers
    t = maxpool_int(conv_int(conv1, x_q))
    t = maxpool_int(conv_int(conv2, t))
    return conv_int(fc, t.reshape(len(t), 1, 1, -1)).reshape(len(t), -1)
