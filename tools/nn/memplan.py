"""Static memory planner for intermediate tensors.

Two tensors that are never alive at the same time can share memory. Each
buffer is alive from the op that writes it to the last op that reads it.
We place buffers biggest-first, each at the lowest offset that does not
collide with an already-placed buffer whose lifetime overlaps
(TensorFlow Lite Micro's "greedy by size" planner does the same).
The result is one fixed arena, sized at compile time: no malloc at runtime.
"""
from dataclasses import dataclass


@dataclass
class Buffer:
    name: str
    size: int
    first_op: int
    last_op: int
    offset: int = -1


def lifetimes_overlap(a, b):
    return not (a.last_op < b.first_op or b.last_op < a.first_op)


def align_up(x, a):
    return (x + a - 1) // a * a


def plan(buffers, align=4):
    placed = []
    for buf in sorted(buffers, key=lambda b: (-b.size, b.first_op, b.name)):
        offset = 0
        for other in sorted((p for p in placed if lifetimes_overlap(p, buf)), key=lambda p: p.offset):
            if offset + buf.size <= other.offset:
                break  # fits in the gap before `other`
            offset = max(offset, align_up(other.offset + other.size, align))
        buf.offset = offset
        placed.append(buf)
    return align_up(max((b.offset + b.size for b in buffers), default=0), align)


def check(buffers):
    """Raise if two simultaneously-alive buffers overlap in memory."""
    for i, a in enumerate(buffers):
        for b in buffers[i + 1:]:
            if lifetimes_overlap(a, b):
                assert a.offset + a.size <= b.offset or b.offset + b.size <= a.offset, (a, b)
