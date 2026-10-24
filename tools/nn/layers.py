"""A small float CNN in plain numpy (NHWC layout), with backprop.

Written from scratch (no PyTorch) so that every operation the firmware runs
has an obvious float counterpart here. A fully-connected layer is a 1x1
convolution on a 1x1 image, exactly as in the C runtime.
"""
import numpy as np


def im2col(x, k, pad, pad_value=0):
    """(N,H,W,C) -> (N*H*W, k*k*C): one row per output pixel ("same" conv).

    Row layout is (ky, kx, c), matching the weight layout [oc][ky][kx][ic].
    """
    n, h, w, c = x.shape
    xp = np.pad(x, ((0, 0), (pad, pad), (pad, pad), (0, 0)), constant_values=pad_value)
    cols = np.empty((n, h, w, k, k, c), dtype=x.dtype)
    for ky in range(k):
        for kx in range(k):
            cols[:, :, :, ky, kx, :] = xp[:, ky:ky + h, kx:kx + w, :]
    return cols.reshape(n * h * w, k * k * c)


def col2im(dcols, shape, k, pad):
    n, h, w, c = shape
    dcols = dcols.reshape(n, h, w, k, k, c)
    dxp = np.zeros((n, h + 2 * pad, w + 2 * pad, c), dtype=dcols.dtype)
    for ky in range(k):
        for kx in range(k):
            dxp[:, ky:ky + h, kx:kx + w, :] += dcols[:, :, :, ky, kx, :]
    return dxp[:, pad:pad + h, pad:pad + w, :]


class Conv:
    """Stride-1 'same' convolution. Weights are (out_c, k, k, in_c)."""

    def __init__(self, in_c, out_c, k, pad, rng):
        fan_in = k * k * in_c
        self.W = rng.normal(0.0, np.sqrt(2.0 / fan_in), (out_c, k, k, in_c)).astype(np.float32)
        self.b = np.zeros(out_c, np.float32)
        self.k, self.pad = k, pad

    def forward(self, x):
        self.x_shape = x.shape
        self.cols = im2col(x, self.k, self.pad)
        n, h, w, _ = x.shape
        out = self.cols @ self.W.reshape(len(self.b), -1).T + self.b
        return out.reshape(n, h, w, -1)

    def backward(self, dout):
        oc = len(self.b)
        d = dout.reshape(-1, oc)
        self.dW = (d.T @ self.cols).reshape(self.W.shape)
        self.db = d.sum(axis=0)
        return col2im(d @ self.W.reshape(oc, -1), self.x_shape, self.k, self.pad)

    def params(self):
        return [(self.W, "dW"), (self.b, "db")]


class ReLU:
    def forward(self, x):
        self.mask = x > 0
        return x * self.mask

    def backward(self, dout):
        return dout * self.mask


class MaxPool2:
    def forward(self, x):
        n, h, w, c = x.shape
        xr = x.reshape(n, h // 2, 2, w // 2, 2, c)
        out = xr.max(axis=(2, 4))
        self.mask = xr == out[:, :, None, :, None, :]
        self.shape = x.shape
        return out

    def backward(self, dout):
        d = self.mask * dout[:, :, None, :, None, :]
        d = d / self.mask.sum(axis=(2, 4), keepdims=True)  # split ties evenly
        return d.reshape(self.shape)


class Net:
    """8x8x1 -> conv3x3(16)+ReLU -> pool -> conv3x3(32)+ReLU -> pool -> fc(10)."""

    def __init__(self, rng):
        self.conv1 = Conv(1, 16, 3, 1, rng)
        self.relu1 = ReLU()
        self.pool1 = MaxPool2()
        self.conv2 = Conv(16, 32, 3, 1, rng)
        self.relu2 = ReLU()
        self.pool2 = MaxPool2()
        self.fc = Conv(128, 10, 1, 0, rng)

    def forward(self, x, record=None):
        a1 = self.relu1.forward(self.conv1.forward(x))
        p1 = self.pool1.forward(a1)
        a2 = self.relu2.forward(self.conv2.forward(p1))
        p2 = self.pool2.forward(a2)
        self.p2_shape = p2.shape
        logits = self.fc.forward(p2.reshape(len(x), 1, 1, -1)).reshape(len(x), -1)
        if record is not None:  # activation ranges, for int8 calibration
            record.update(conv1=a1, conv2=a2, fc=logits)
        return logits

    def backward(self, dlogits):
        d = self.fc.backward(dlogits.reshape(len(dlogits), 1, 1, -1)).reshape(self.p2_shape)
        d = self.conv2.backward(self.relu2.backward(self.pool2.backward(d)))
        self.conv1.backward(self.relu1.backward(self.pool1.backward(d)))

    def layers(self):
        return {"conv1": self.conv1, "conv2": self.conv2, "fc": self.fc}

    def state(self):
        out = {}
        for name, layer in self.layers().items():
            out[name + "_W"], out[name + "_b"] = layer.W, layer.b
        return out

    def load(self, state):
        for name, layer in self.layers().items():
            layer.W, layer.b = state[name + "_W"].astype(np.float32), state[name + "_b"].astype(np.float32)
