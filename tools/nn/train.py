"""Dataset loading and float training (numpy + Adam)."""
import numpy as np

from layers import Net


def load_digits_split(seed=0, test_frac=0.2):
    """UCI handwritten digits (bundled with scikit-learn): 1797 8x8 images."""
    from sklearn.datasets import load_digits

    d = load_digits()
    x = (d.images.astype(np.float32) / 16.0)[..., None]  # NHWC, values in [0, 1]
    y = d.target.astype(np.int64)
    idx = np.random.default_rng(seed).permutation(len(x))
    n_test = int(len(x) * test_frac)
    test, train = idx[:n_test], idx[n_test:]
    return x[train], y[train], x[test], y[test]


def softmax_xent(logits, y):
    z = logits - logits.max(axis=1, keepdims=True)
    p = np.exp(z)
    p /= p.sum(axis=1, keepdims=True)
    loss = -np.log(p[np.arange(len(y)), y] + 1e-12).mean()
    grad = p
    grad[np.arange(len(y)), y] -= 1.0
    return loss, grad / len(y)


def random_shift(x, rng):
    """Shift each image by up to 1 pixel: cheap augmentation for 8x8 digits."""
    out = np.zeros_like(x)
    for i in range(len(x)):
        dy, dx = rng.integers(-1, 2, size=2)
        src = x[i, max(0, -dy):8 - max(0, dy), max(0, -dx):8 - max(0, dx)]
        out[i, max(0, dy):max(0, dy) + src.shape[0], max(0, dx):max(0, dx) + src.shape[1]] = src
    return out


def accuracy(net, x, y):
    return float((net.forward(x).argmax(axis=1) == y).mean())


def train(x, y, epochs=40, batch=32, lr=3e-3, seed=0, log=print):
    rng = np.random.default_rng(seed)
    net = Net(rng)
    params = [(layer, attr, grad) for layer in net.layers().values()
              for attr, grad in (("W", "dW"), ("b", "db"))]
    m = [np.zeros_like(getattr(l, a)) for l, a, _ in params]
    v = [np.zeros_like(getattr(l, a)) for l, a, _ in params]
    b1, b2, eps, step = 0.9, 0.999, 1e-8, 0

    for epoch in range(epochs):
        order = rng.permutation(len(x))
        total = 0.0
        for start in range(0, len(x), batch):
            idx = order[start:start + batch]
            xb = random_shift(x[idx], rng)
            loss, dlogits = softmax_xent(net.forward(xb), y[idx])
            net.backward(dlogits)
            step += 1
            lr_t = lr * (0.5 * (1 + np.cos(np.pi * epoch / epochs)))  # cosine decay
            for i, (layer, attr, gname) in enumerate(params):
                g = getattr(layer, gname)
                m[i] = b1 * m[i] + (1 - b1) * g
                v[i] = b2 * v[i] + (1 - b2) * g * g
                mh, vh = m[i] / (1 - b1 ** step), v[i] / (1 - b2 ** step)
                setattr(layer, attr, (getattr(layer, attr) - lr_t * mh / (np.sqrt(vh) + eps)).astype(np.float32))
            total += loss * len(idx)
        if epoch % 10 == 9 or epoch == epochs - 1:
            log(f"  epoch {epoch + 1:3d}  loss {total / len(x):.4f}  train acc {accuracy(net, x, y):.4f}")
    return net
