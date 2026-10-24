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


def to_svg(blocks, titles, width=900):
    """Render trace blocks as stacked Gantt charts (used for the README)."""
    colors = {"pi-high": "#d62728", "pi-mid": "#ff7f0e", "pi-low": "#1f77b4"}
    order = ["pi-high", "pi-mid", "pi-low", "init", "idle"]
    left, row_h, gap = 90, 22, 4
    parts, y = [], 10
    span_ms = max((ev[-1][0] - ev[0][0]) / hz * 1000 for hz, _, ev in blocks)
    scale = (width - left - 20) / span_ms
    for (hz, names, events), title in zip(blocks, titles):
        t0 = events[0][0]
        parts.append(f'<text x="{left}" y="{y + 14}" font-size="14" font-weight="bold">{title}</text>')
        y += 24
        rows = [n for n in order if n in names.values()]
        row_of = {tid: rows.index(n) for tid, n in names.items() if n in rows}
        for i, n in enumerate(rows):
            ry = y + i * (row_h + gap)
            parts.append(f'<text x="{left - 8}" y="{ry + 15}" font-size="12" text-anchor="end">{n}</text>')
            parts.append(f'<rect x="{left}" y="{ry}" width="{width - left - 20}" height="{row_h}" fill="#f2f2f2"/>')
        for tid, a, b in run_slices(events):
            if tid not in row_of:
                continue
            x = left + (a - t0) / hz * 1000 * scale
            w = max((b - a) / hz * 1000 * scale, 0.8)
            ry = y + row_of[tid] * (row_h + gap)
            parts.append(f'<rect x="{x:.1f}" y="{ry}" width="{w:.1f}" height="{row_h}" '
                         f'fill="{colors.get(names[tid], "#888")}"/>')
        for ts, ev, tid, arg in events:
            if ev == EV_MARK and arg == 3:  # H finally gets the mutex
                x = left + (ts - t0) / hz * 1000 * scale
                parts.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{y - 4}" '
                             f'y2="{y + len(rows) * (row_h + gap)}" stroke="#d62728" stroke-dasharray="4 3"/>')
                near_edge = x > width - 260  # put the label left of the line
                parts.append(f'<text x="{x - 4 if near_edge else x + 4:.1f}" y="{y + 4}" font-size="11" '
                             f'fill="#d62728" text-anchor="{"end" if near_edge else "start"}">'
                             f'high-prio task gets the lock at {(ts - t0) / hz * 1000:.1f} ms</text>')
        y += len(rows) * (row_h + gap) + 6
        for ms in range(0, int(span_ms) + 1, 10):
            x = left + ms * scale
            parts.append(f'<text x="{x:.1f}" y="{y + 10}" font-size="10" text-anchor="middle" fill="#555">{ms} ms</text>')
        y += 30
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{y}" '
            f'font-family="Helvetica, Arial, sans-serif">'
            f'<rect width="100%" height="100%" fill="white"/>' + "".join(parts) + "</svg>")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="serial log containing one or more #KTRACE blocks")
    ap.add_argument("-o", "--output", help="output JSON (block N>0 gets a .N suffix)")
    ap.add_argument("--quiet", action="store_true", help="no ASCII chart")
    ap.add_argument("--svg", help="also render all blocks into one SVG timeline")
    ap.add_argument("--titles", nargs="*", default=[], help="one title per block for --svg")
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
    if args.svg:
        titles = args.titles + [f"trace {i}" for i in range(len(args.titles), len(blocks))]
        with open(args.svg, "w") as f:
            f.write(to_svg([(hz, n, unwrap(e)) for hz, n, e in blocks], titles))
        print(f"wrote {args.svg}")


if __name__ == "__main__":
    main()
