#!/usr/bin/env python3
"""Convert a Kestrel trace dump into a Chrome/Perfetto timeline + ASCII chart.

The firmware prints (shell command `trace`, or automatically in `make test`):

    #KTRACE-BEGIN hz=16000000
    #TASK 3 pi-low
    E <timestamp_cycles> <event> <task_id> <arg>
    ...
    #KTRACE-END events=123

Usage:
    python3 tools/trace2perfetto.py serial_log.txt -o trace.json
Then open https://ui.perfetto.dev and drag trace.json in.
"""
import argparse
import json
import sys

EV_SWITCH, EV_WAKE, EV_BLOCK, EV_PRIO, EV_CREATE, EV_EXIT, EV_FAULT, EV_MARK = range(1, 9)
MARK_NAMES = {1: "L locks mutex", 2: "L unlocks mutex", 3: "H gets mutex"}


def parse_blocks(lines):
    """Yield (hz, {tid: name}, [(ts, ev, tid, arg)]) for each trace block."""
    block = None
    for line in lines:
        line = line.strip()
        if line.startswith("#KTRACE-BEGIN"):
            hz = int(line.split("hz=")[1])
            block = (hz, {}, [])
        elif block is None:
            continue
        elif line.startswith("#TASK"):
            _, tid, name = line.split(maxsplit=2)
            block[1][int(tid)] = name
        elif line.startswith("E "):
            _, ts, ev, tid, arg = line.split()
            block[2].append((int(ts), int(ev), int(tid), int(arg)))
        elif line.startswith("#KTRACE-END"):
            yield block
            block = None


def unwrap(events):
    """Timestamps are 32-bit cycle counters; undo wrap-around."""
    out, offset, prev = [], 0, None
    for ts, ev, tid, arg in events:
        if prev is not None and ts < prev:
            offset += 1 << 32
        prev = ts
        out.append((ts + offset, ev, tid, arg))
    return out


def run_slices(events):
    """[(tid, start, end)] intervals during which each task was on the CPU."""
    slices, cur, start = [], None, None
    for ts, ev, tid, _ in events:
        if ev == EV_SWITCH:
            if cur is not None:
                slices.append((cur, start, ts))
            cur, start = tid, ts
    if cur is not None and events:
        slices.append((cur, start, events[-1][0]))
    return slices


def to_chrome_json(hz, names, events):
    us = 1e6 / hz
    t0 = events[0][0] if events else 0
    out = [{"name": "process_name", "ph": "M", "pid": 1, "args": {"name": "Kestrel RTOS"}}]
    for tid, name in names.items():
        out.append({"name": "thread_name", "ph": "M", "pid": 1, "tid": tid,
                    "args": {"name": f"{name} (id {tid})"}})
    for tid, a, b in run_slices(events):
        out.append({"name": names.get(tid, str(tid)), "ph": "X", "pid": 1, "tid": tid,
                    "ts": (a - t0) * us, "dur": max((b - a) * us, 0.01)})
    for ts, ev, tid, arg in events:
        t = (ts - t0) * us
        if ev == EV_PRIO:  # a counter track per task shows inheritance boosts
            out.append({"name": f"prio {names.get(tid, tid)}", "ph": "C", "pid": 1, "ts": t,
                        "args": {"priority": arg}})
        elif ev in (EV_MARK, EV_FAULT, EV_BLOCK, EV_WAKE):
            label = {EV_MARK: MARK_NAMES.get(arg, f"mark {arg}"), EV_FAULT: "FAULT",
                     EV_BLOCK: "block", EV_WAKE: "wake"}[ev]
            out.append({"name": label, "ph": "i", "s": "t", "pid": 1, "tid": tid, "ts": t})
    return {"traceEvents": out, "displayTimeUnit": "ms"}


def ascii_chart(hz, names, events, width=72):
    if not events:
        return "(empty trace)"
    t0, t1 = events[0][0], events[-1][0]
    span = max(t1 - t0, 1)
    rows = {}
    for tid, a, b in run_slices(events):
        row = rows.setdefault(tid, [" "] * width)
        lo = int((a - t0) * width / span)
        hi = max(int((b - t0) * width / span), lo + 1)
        for i in range(lo, min(hi, width)):
            row[i] = "#"
    ms = span / hz * 1000
    lines = [f"{'':14}0 ms{' ' * (width - 12)}{ms:6.1f} ms"]
    for tid in sorted(rows, key=lambda t: -max([a for _, e, x, a in events if x == t and e == EV_SWITCH] or [0])):
        lines.append(f"{names.get(tid, str(tid))[:12]:>12} |{''.join(rows[tid])}|")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="serial log containing one or more #KTRACE blocks")
    ap.add_argument("-o", "--output", help="output JSON (block N>0 gets a .N suffix)")
    ap.add_argument("--quiet", action="store_true", help="no ASCII chart")
    args = ap.parse_args()

    with open(args.log, errors="replace") as f:
        blocks = list(parse_blocks(f))
    if not blocks:
        sys.exit("no #KTRACE-BEGIN ... #KTRACE-END block found")
    for i, (hz, names, events) in enumerate(blocks):
        events = unwrap(events)
        if not args.quiet:
            print(f"--- trace {i}: {len(events)} events ---")
            print(ascii_chart(hz, names, events))
        if args.output:
            path = args.output if i == 0 else args.output.replace(".json", f".{i}.json")
            with open(path, "w") as f:
                json.dump(to_chrome_json(hz, names, events), f)
            print(f"wrote {path}")


if __name__ == "__main__":
    main()
