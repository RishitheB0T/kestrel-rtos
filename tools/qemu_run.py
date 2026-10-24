#!/usr/bin/env python3
"""Boot the autotest firmware in QEMU, stream its console, and report.

Exit status is 0 only if the firmware printed ALL TESTS PASSED and QEMU
exited cleanly (the firmware ends with a semihosting exit call).
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import trace2perfetto  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--qemu", default="qemu-system-arm")
    ap.add_argument("--timeout", type=float, default=300)
    ap.add_argument("--log", default="build/test.log")
    ap.add_argument("--trace-json", help="write Perfetto JSON for each trace block")
    args = ap.parse_args()

    cmd = [args.qemu, "-M", "netduinoplus2", "-display", "none", "-monitor", "none",
           "-serial", "null", "-serial", "stdio",
           "-semihosting-config", "enable=on,target=native", "-kernel", args.elf]
    os.makedirs(os.path.dirname(os.path.abspath(args.log)), exist_ok=True)
    start = time.time()
    lines = []
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, stdin=subprocess.DEVNULL,
                          stderr=subprocess.STDOUT, text=True, errors="replace") as proc:
        try:
            for line in proc.stdout:
                lines.append(line)
                if not (line.startswith("E ") or line.startswith("#TASK")):
                    sys.stdout.write(line)
                    sys.stdout.flush()
                if time.time() - start > args.timeout:
                    proc.kill()
                    print(f"\nTIMEOUT after {args.timeout:.0f} s")
                    break
            code = proc.wait(timeout=10)
        except KeyboardInterrupt:
            proc.kill()
            raise
    with open(args.log, "w") as f:
        f.writelines(lines)

    if args.trace_json:
        for i, (hz, names, events) in enumerate(trace2perfetto.parse_blocks(lines)):
            events = trace2perfetto.unwrap(events)
            path = args.trace_json.replace(".json", f".{i}.json")
            with open(path, "w") as f:
                import json
                json.dump(trace2perfetto.to_chrome_json(hz, names, events), f)
            print(f"\n[trace {i}] {len(events)} events -> {path}")
            print(trace2perfetto.ascii_chart(hz, names, events))

    passed = any("ALL TESTS PASSED" in l for l in lines)
    print(f"\nqemu exit code {code}, {'PASS' if passed and code == 0 else 'FAIL'} "
          f"({time.time() - start:.1f} s)")
    sys.exit(0 if passed and code == 0 else 1)


if __name__ == "__main__":
    main()
