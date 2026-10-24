"""Property test for the memory planner: on many random problems, no two
buffers that are alive at the same time may share bytes, and the arena must
never be larger than giving every buffer its own memory."""
import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "nn"))
from memplan import Buffer, check, plan  # noqa: E402


def main():
    rng = random.Random(1)
    for trial in range(2000):
        n_ops = rng.randint(1, 12)
        bufs = []
        for i in range(rng.randint(1, 15)):
            a = rng.randrange(n_ops)
            b = rng.randrange(a, n_ops)
            bufs.append(Buffer(f"b{i}", rng.randint(1, 600), a, b))
        arena = plan(bufs)
        check(bufs)
        assert arena <= sum((b.size + 3) // 4 * 4 for b in bufs), trial
    print("PASS: memory planner, 2000 random problems")


if __name__ == "__main__":
    main()
