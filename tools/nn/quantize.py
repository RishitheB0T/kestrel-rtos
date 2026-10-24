"""Post-training int8 quantization.

Each real value r is stored as an int8 q:   r = scale * (q - zero_point)

* Activations: one (scale, zero_point) per tensor, chosen from the min/max
  seen while running the float model on calibration data ("calibration",
  the same idea as TensorRT's INT8 calibrator, using plain min/max).
* Weights: symmetric (zero_point = 0), one scale per output channel, so a
  channel with small weights does not lose all its precision to a channel
  with large ones.
* Bias: int32 with scale = in_scale * w_scale[c].

Then every layer is rewritten into pure integer form:

  acc_c  = bias_c + sum((x - zp_in) * w)
         = (bias_c - zp_in * sum(w_c)) + sum(x * w)     <- fold zp_in offline
  y      = zp_out + round(acc_c * M_c),   M_c = in_scale * w_scale_c / out_scale
  M_c    = mult_c * 2^-shift_c            <- integer multiply + shift
"""
import math
from dataclasses import dataclass

import numpy as np


def act_qparams(lo, hi):
    lo, hi = min(float(lo), 0.0), max(float(hi), 0.0)  # real 0 must be exact
    scale = (hi - lo) / 255.0
    zp = int(round(-128 - lo / scale))
    return scale, max(-128, min(127, zp))


def quantize_multiplier(m):
    """Represent 0 < m < 2^30 as mult * 2^-shift with mult in [2^30, 2^31)."""
    assert m > 0
    frac, exp = math.frexp(m)  # m = frac * 2^exp, frac in [0.5, 1)
    mult = int(round(frac * (1 << 31)))
    if mult == 1 << 31:
        mult //= 2
        exp += 1
    shift = 31 - exp
    assert 1 <= shift <= 62, shift
    return mult, shift


@dataclass
class QConv:
    name: str
    in_h: int
    in_w: int
    in_c: int
    out_c: int
    k: int
    pad: int
    relu: bool
    in_scale: float
    in_zp: int
    out_scale: float
    out_zp: int
    w_q: np.ndarray        # (out_c, k, k, in_c) int8
    w_scale: np.ndarray    # (out_c,)
    bias_folded: np.ndarray  # (out_c,) int32
    mult: np.ndarray       # (out_c,) int32
    shift: np.ndarray      # (out_c,) int8

    @property
    def col_len(self):
        return self.k * self.k * self.in_c

    @property
    def col_stride(self):
        return (self.col_len + 3) // 4 * 4

    @property
    def act_min(self):
        return self.out_zp if self.relu else -128

    @property
    def macs(self):
        return self.in_h * self.in_w * self.out_c * self.col_len


INPUT_SCALE, INPUT_ZP = 1.0 / 255.0, -128  # pixels in [0, 1] -> [-128, 127]


def quantize_input(x):
    return np.clip(np.round(x / INPUT_SCALE) + INPUT_ZP, -128, 127).astype(np.int8)


def quantize_conv(name, layer, in_shape, in_q, out_q, relu):
    W, b = layer.W.astype(np.float64), layer.b.astype(np.float64)
    out_c = W.shape[0]
    in_scale, in_zp = in_q
    out_scale, out_zp = out_q

    w_absmax = np.abs(W).reshape(out_c, -1).max(axis=1)
    w_scale = np.where(w_absmax > 0, w_absmax / 127.0, 1.0)
    w_q = np.clip(np.round(W / w_scale[:, None, None, None]), -127, 127).astype(np.int8)

    b_q = np.round(b / (in_scale * w_scale)).astype(np.int64)
    folded = b_q - in_zp * w_q.reshape(out_c, -1).astype(np.int64).sum(axis=1)
    assert np.all(np.abs(folded) < 2**31)

    mults, shifts = zip(*(quantize_multiplier(in_scale * s / out_scale) for s in w_scale))
    return QConv(name, in_shape[0], in_shape[1], in_shape[2], out_c, layer.k, layer.pad, relu,
                 in_scale, in_zp, out_scale, out_zp, w_q, w_scale,
                 folded.astype(np.int32), np.array(mults, np.int32), np.array(shifts, np.int8))


def quantize_net(net, calib_x):
    rec = {}
    net.forward(calib_x, record=rec)
    q_in = (INPUT_SCALE, INPUT_ZP)
    q1 = act_qparams(0.0, rec["conv1"].max())
    q2 = act_qparams(0.0, rec["conv2"].max())
    q3 = act_qparams(rec["fc"].min(), rec["fc"].max())
    # Max-pool keeps its input's quantization, so conv2 reads conv1's params.
    return [
        quantize_conv("conv1", net.conv1, (8, 8, 1), q_in, q1, relu=True),
        quantize_conv("conv2", net.conv2, (4, 4, 16), q1, q2, relu=True),
        quantize_conv("fc", net.fc, (1, 1, 128), q2, q3, relu=False),
    ]
