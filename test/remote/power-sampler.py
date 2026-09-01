#!/usr/bin/env python3

"""Sample the target's battery gauge into a CSV for the power comparison.

Why not powerstat or powertop: neither is packaged for this Fedora aarch64
target and both would still read the same sysfs gauge underneath. What matters
for an A/B is that every arm is sampled by the identical code path with the
identical overhead, and that the charger state is recorded per sample rather
than assumed. ``powerstat`` remains useful as an independent cross-check
instrument and is wired up separately by the harness when it is available.

``power_now`` reads 0 on the qcom-battmgr gauge, so power is derived as
``voltage_now * |current_now|``. ``current_now`` is negative while
discharging; a non-negative value means the pack is being charged and the
sample cannot be attributed to the workload.
"""

import argparse
import os
import signal
import sys
import time

FIELDS = ("voltage_now", "current_now", "capacity", "status", "temp")


def read_field(base, name):
    try:
        with open(os.path.join(base, name), "r", encoding="utf-8") as handle:
            return handle.read().strip()
    except OSError:
        return ""


def read_int(base, name):
    raw = read_field(base, name)
    try:
        return int(raw)
    except ValueError:
        return None


def supply_online(paths):
    """Return 1 when any external supply reports itself online."""
    for path in paths:
        try:
            with open(os.path.join(path, "online"), "r", encoding="utf-8") as handle:
                if handle.read().strip() == "1":
                    return 1
        except OSError:
            continue
    return 0


def thermal_millicelsius(zone):
    if not zone:
        return None
    try:
        with open(os.path.join(zone, "temp"), "r", encoding="utf-8") as handle:
            return int(handle.read().strip())
    except (OSError, ValueError):
        return None


def matching_cpu_jiffies(needle):
    """Total utime+stime and process count for cmdlines containing ``needle``.

    This is the cross-check that separates "the video played" from "the CPU
    decoded it": the software arm should show a large aggregate, the hardware
    arm a small one. Scanning /proc costs more than reading two sysfs files,
    so the harness samples it on a slower cadence than the gauge.

    The sampler's own argv carries ``needle``, so it must exclude itself or it
    reports its own CPU time as the browser's.
    """
    total = 0
    count = 0
    self_pid = str(os.getpid())
    for entry in os.listdir("/proc"):
        if not entry.isdigit() or entry == self_pid:
            continue
        try:
            with open(f"/proc/{entry}/cmdline", "rb") as handle:
                cmdline = handle.read().replace(b"\0", b" ").decode("utf-8", "replace")
            if needle not in cmdline:
                continue
            with open(f"/proc/{entry}/stat", "r", encoding="utf-8") as handle:
                stat = handle.read()
        except OSError:
            continue
        # comm can contain spaces and parentheses, so split after the last ')'.
        try:
            fields = stat[stat.rindex(")") + 2 :].split()
            total += int(fields[11]) + int(fields[12])
            count += 1
        except (ValueError, IndexError):
            continue
    return total, count


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--battery", default="/sys/class/power_supply/qcom-battmgr-bat")
    parser.add_argument(
        "--supply",
        action="append",
        default=[],
        help="external supply directory to watch for online=1 (repeatable)",
    )
    parser.add_argument("--thermal-zone", default="")
    parser.add_argument(
        "--backlight",
        default="",
        help="backlight device directory whose brightness is recorded per sample",
    )
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument(
        "--proc-match",
        default="",
        help="cmdline substring whose aggregate CPU jiffies are recorded",
    )
    parser.add_argument(
        "--proc-every",
        type=int,
        default=5,
        help="record the process CPU aggregate every Nth gauge sample",
    )
    parser.add_argument("--seconds", type=float, default=0.0, help="0 runs until signalled")
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)

    if not os.path.isdir(args.battery):
        print(f"FAIL battery gauge not found: {args.battery}", file=sys.stderr)
        return 2

    supplies = args.supply
    if not supplies:
        root = os.path.dirname(args.battery)
        supplies = [
            os.path.join(root, name)
            for name in sorted(os.listdir(root))
            if os.path.exists(os.path.join(root, name, "online"))
        ]

    stop = {"now": False}

    def handle(_signum, _frame):
        stop["now"] = True

    signal.signal(signal.SIGTERM, handle)
    signal.signal(signal.SIGINT, handle)

    deadline = time.monotonic() + args.seconds if args.seconds > 0 else None
    sample = 0
    with open(args.out, "w", encoding="utf-8", buffering=1) as handle:
        handle.write(
            "epoch,voltage_uv,current_ua,power_w,capacity,status,supply_online,temp_mc,"
            "proc_cpu_jiffies,proc_count,brightness\n"
        )
        while not stop["now"]:
            if deadline is not None and time.monotonic() >= deadline:
                break
            voltage = read_int(args.battery, "voltage_now")
            current = read_int(args.battery, "current_now")
            power = ""
            if voltage is not None and current is not None:
                power = f"{voltage * abs(current) / 1e12:.6f}"
            jiffies, procs = "", ""
            if args.proc_match and args.proc_every > 0 and sample % args.proc_every == 0:
                jiffies, procs = matching_cpu_jiffies(args.proc_match)
            handle.write(
                "%.3f,%s,%s,%s,%s,%s,%d,%s,%s,%s,%s\n"
                % (
                    time.time(),
                    "" if voltage is None else voltage,
                    "" if current is None else current,
                    power,
                    read_field(args.battery, "capacity"),
                    read_field(args.battery, "status"),
                    supply_online(supplies),
                    thermal_millicelsius(args.thermal_zone) or "",
                    jiffies,
                    procs,
                    read_field(args.backlight, "brightness") if args.backlight else "",
                )
            )
            sample += 1
            time.sleep(args.interval)
    return 0


if __name__ == "__main__":
    sys.exit(main())
