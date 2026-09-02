#!/usr/bin/env python3

"""Aggregate an iris-power-compare.sh collection into one report.

The arithmetic is deliberately boring; the value is in what it refuses to
average. A run is dropped when the charger appeared, the panel changed
brightness, the power profile switched, the browser never named its decoder,
or the arm did not get the decoder it was supposed to get. Dropped runs are
reported by name and reason rather than silently excluded, because a
comparison that quietly discards its inconvenient samples is not a comparison.

Blocks are paired: each block contributes one hardware and one software run
recorded minutes apart under the same conditions, and the reported statistic
is the mean paired difference with a two-sided 95% t interval. Pairing is what
lets a five-block run say anything at all against a gauge whose idle noise is
a sizeable fraction of the effect.
"""

import argparse
import csv
import json
import math
import os
import sys

# Two-sided 95% critical values by degrees of freedom. scipy is not available
# on the target and a hand-rolled inverse CDF would be more code than table.
T_95 = {
    1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365,
    8: 2.306, 9: 2.262, 10: 2.228, 11: 2.201, 12: 2.179, 13: 2.160, 14: 2.145,
    15: 2.131, 16: 2.120, 17: 2.110, 18: 2.101, 19: 2.093, 20: 2.086,
    21: 2.080, 22: 2.074, 23: 2.069, 24: 2.064, 25: 2.060, 26: 2.056,
    27: 2.052, 28: 2.048, 29: 2.045, 30: 2.042,
}

CLK_TCK = os.sysconf("SC_CLK_TCK") if hasattr(os, "sysconf") else 100

def arm_expectation(manifest, arm):
    """Return the decoder and device-ownership contract for this collection."""
    if arm == "sw":
        return {"decoder": "FFmpegVideoDecoder", "platform": False, "node": False}
    if manifest.get("browser", "chrome") == "chromium":
        return {"decoder": "V4L2VideoDecoder", "platform": True, "node": True}
    return {"decoder": "VaapiVideoDecoder", "platform": True, "node": True}


def mean(values):
    return sum(values) / len(values) if values else None


def t_interval(values):
    """Mean and two-sided 95% half-width. None when n < 2."""
    n = len(values)
    if n < 2:
        return (mean(values), None)
    average = mean(values)
    variance = sum((v - average) ** 2 for v in values) / (n - 1)
    stderr = math.sqrt(variance / n)
    critical = T_95.get(n - 1, 1.96)
    return (average, critical * stderr)


def load_samples(path):
    rows = []
    with open(path, newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            try:
                row["epoch"] = float(row["epoch"])
            except (TypeError, ValueError):
                continue
            row["power_w"] = float(row["power_w"]) if row.get("power_w") else None
            row["supply_online"] = int(row["supply_online"]) if row.get("supply_online") else 0
            row["temp_mc"] = int(row["temp_mc"]) if row.get("temp_mc") else None
            row["proc_cpu_jiffies"] = (
                int(row["proc_cpu_jiffies"]) if row.get("proc_cpu_jiffies") else None
            )
            row["brightness"] = (row.get("brightness") or "").strip()
            rows.append(row)
    return rows


def window(rows, start, end):
    return [r for r in rows if start <= r["epoch"] <= end and r["power_w"] is not None]


def cpu_cores(rows):
    """Average cores used by the matched processes across a window."""
    marked = [r for r in rows if r["proc_cpu_jiffies"] is not None]
    if len(marked) < 2:
        return None
    span = marked[-1]["epoch"] - marked[0]["epoch"]
    if span <= 0:
        return None
    delta = marked[-1]["proc_cpu_jiffies"] - marked[0]["proc_cpu_jiffies"]
    return delta / CLK_TCK / span


def analyse_run(run_dir, settle, baseline_gap_limit, manifest):
    run_path = os.path.join(run_dir, "run.json")
    cdp_path = os.path.join(run_dir, "cdp.json")
    samples_path = os.path.join(run_dir, "samples.csv")
    if not os.path.exists(run_path) or not os.path.exists(samples_path):
        return None

    with open(run_path, encoding="utf-8") as handle:
        run = json.load(handle)
    result = {
        "label": run["label"],
        "arm": run["arm"],
        "block": run["block"],
        "valid": True,
        "reasons": [],
    }

    cdp = None
    if os.path.exists(cdp_path):
        with open(cdp_path, encoding="utf-8") as handle:
            cdp = json.load(handle)

    if run.get("cdp_exit") != 0:
        result["valid"] = False
        result["reasons"].append(f"cdp exit {run.get('cdp_exit')}")
    if cdp is None:
        result["valid"] = False
        result["reasons"].append("no cdp report")
        return result

    result["decoder_name"] = cdp.get("decoder_name")
    result["is_platform_video_decoder"] = cdp.get("is_platform_video_decoder")
    result["decoded_frames"] = cdp.get("window_decoded_frames")
    result["dropped_frames"] = cdp.get("window_dropped_frames")

    expectation = arm_expectation(manifest, run["arm"])
    if not result["decoder_name"]:
        result["valid"] = False
        result["reasons"].append("decoder name missing")
    elif expectation.get("decoder") and result["decoder_name"] != expectation["decoder"]:
        result["valid"] = False
        result["reasons"].append(
            f"arm {run['arm']} expected {expectation['decoder']}, got {result['decoder_name']}"
        )
    if result["is_platform_video_decoder"] is not expectation.get("platform"):
        result["valid"] = False
        result["reasons"].append(
            f"arm {run['arm']} expected platform={expectation.get('platform')}, "
            f"got {result['is_platform_video_decoder']}"
        )
    if not isinstance(result["decoded_frames"], int) or result["decoded_frames"] <= 0:
        result["valid"] = False
        result["reasons"].append("no frames decoded inside the window")

    if run.get("power_profile_start") != run.get("power_profile_end"):
        result["valid"] = False
        result["reasons"].append("power profile switched mid-run")

    rows = load_samples(samples_path)
    if not rows:
        result["valid"] = False
        result["reasons"].append("no gauge samples")
        return result
    if any(r["supply_online"] for r in rows):
        result["valid"] = False
        result["reasons"].append("external supply came online during the run")
    if any(r.get("status") not in ("", "Discharging") for r in rows):
        result["valid"] = False
        result["reasons"].append("battery left the discharging state")

    # Panel power dwarfs the decode difference, so a backlight that moves
    # inside a run makes that run uncomparable. Prefer the per-sample column:
    # comparing two edge readings cannot distinguish a panel that dimmed and
    # recovered from one that never moved, and an edge captured before the
    # first block reports every later run as broken.
    levels = sorted({r["brightness"] for r in rows if r.get("brightness")})
    if levels:
        result["brightness_levels"] = levels
        if len(levels) > 1:
            result["valid"] = False
            result["reasons"].append("backlight moved during the run: " + ", ".join(levels))
    elif run.get("brightness_start") != run.get("brightness_end"):
        result["valid"] = False
        result["reasons"].append(
            f"brightness moved {run.get('brightness_start')} -> {run.get('brightness_end')}"
        )

    pre_start, pre_end = run["baseline_pre"]
    post_start, post_end = run["baseline_post"]
    measure_start = cdp.get("measure_start_epoch")
    measure_end = cdp.get("measure_end_epoch")
    if not measure_start or not measure_end:
        result["valid"] = False
        result["reasons"].append("cdp did not record a measurement window")
        return result

    pre = window(rows, pre_start + settle, pre_end)
    post = window(rows, post_start + settle, post_end)
    play = window(rows, measure_start, measure_end)
    empty = [name for name, rowset in (("pre", pre), ("playback", play), ("post", post)) if not rowset]
    if empty:
        result["valid"] = False
        result["reasons"].append(
            "no gauge samples inside these windows: " + ", ".join(empty)
        )
        return result

    result["samples"] = {"pre": len(pre), "playback": len(play), "post": len(post)}
    result["baseline_pre_w"] = mean([r["power_w"] for r in pre])
    result["baseline_post_w"] = mean([r["power_w"] for r in post])
    result["playback_w"] = mean([r["power_w"] for r in play])
    baseline = (result["baseline_pre_w"] + result["baseline_post_w"]) / 2
    result["baseline_w"] = baseline
    result["delta_w"] = result["playback_w"] - baseline
    gap = abs(result["baseline_post_w"] - result["baseline_pre_w"])
    result["baseline_gap_w"] = gap
    if gap > baseline_gap_limit:
        result["valid"] = False
        result["reasons"].append(
            f"pre/post baseline gap {gap:.3f} W exceeds {baseline_gap_limit:.3f} W"
        )

    result["cpu_cores"] = cpu_cores(play)
    temps = [r["temp_mc"] for r in play if r["temp_mc"] is not None]
    if temps:
        result["temp_c_mean"] = mean(temps) / 1000.0
        result["temp_c_max"] = max(temps) / 1000.0
    capacities = [int(r["capacity"]) for r in rows if r.get("capacity", "").isdigit()]
    if capacities:
        result["capacity_start"] = capacities[0]
        result["capacity_end"] = capacities[-1]

    holders = os.path.join(run_dir, "iris-node-holders.txt")
    holder_lines = []
    if os.path.exists(holders):
        with open(holders, encoding="utf-8") as handle:
            holder_lines = [line.strip() for line in handle if line.strip()]
        result["iris_node_holders"] = holder_lines
    if expectation.get("node") and not holder_lines:
        result["valid"] = False
        result["reasons"].append("hardware arm held no resolved Iris node")
    if not expectation.get("node") and holder_lines:
        result["valid"] = False
        result["reasons"].append("software arm held the resolved Iris node")
    return result


def summarise(runs):
    by_block = {}
    for run in runs:
        if run.get("valid"):
            by_block.setdefault(run["block"], {})[run["arm"]] = run

    paired = [b for b in sorted(by_block) if "hw" in by_block[b] and "sw" in by_block[b]]
    playback_diffs = [
        by_block[b]["sw"]["playback_w"] - by_block[b]["hw"]["playback_w"] for b in paired
    ]
    delta_diffs = [by_block[b]["sw"]["delta_w"] - by_block[b]["hw"]["delta_w"] for b in paired]

    playback_mean, playback_ci = t_interval(playback_diffs)
    delta_mean, delta_ci = t_interval(delta_diffs)

    def arm_stats(arm, key):
        values = [r[key] for r in runs if r.get("valid") and r["arm"] == arm and r.get(key) is not None]
        average, half = t_interval(values)
        return {"n": len(values), "mean": average, "ci95": half}

    summary = {
        "paired_blocks": paired,
        "paired_n": len(paired),
        "software_minus_hardware_playback_w": {"mean": playback_mean, "ci95": playback_ci},
        "software_minus_hardware_delta_w": {"mean": delta_mean, "ci95": delta_ci},
        "arms": {
            arm: {
                "playback_w": arm_stats(arm, "playback_w"),
                "delta_w": arm_stats(arm, "delta_w"),
                "baseline_w": arm_stats(arm, "baseline_w"),
                "cpu_cores": arm_stats(arm, "cpu_cores"),
            }
            for arm in ("hw", "sw")
        },
    }
    # A confidence interval that straddles zero is not a result, and saying so
    # here keeps the conclusion out of the reader's optimism.
    if playback_mean is not None and playback_ci is not None:
        summary["playback_difference_significant"] = (playback_mean - playback_ci) > 0
    else:
        summary["playback_difference_significant"] = None
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("collection", help="directory produced by iris-power-compare.sh")
    parser.add_argument("--settle", type=float, default=10.0,
                        help="seconds discarded at the start of each baseline window")
    parser.add_argument("--baseline-gap-limit", type=float, default=0.5,
                        help="maximum tolerated pre/post baseline difference in watts")
    parser.add_argument("--out", help="write the report here instead of stdout")
    args = parser.parse_args(argv)

    manifest_path = os.path.join(args.collection, "manifest.json")
    manifest = {}
    if os.path.exists(manifest_path):
        with open(manifest_path, encoding="utf-8") as handle:
            manifest = json.load(handle)

    runs = []
    for name in sorted(os.listdir(args.collection)):
        run_dir = os.path.join(args.collection, name)
        if not os.path.isdir(run_dir) or not name.startswith("block"):
            continue
        analysed = analyse_run(run_dir, args.settle, args.baseline_gap_limit, manifest)
        if analysed:
            runs.append(analysed)
    runs.sort(key=lambda r: (r["block"], r["arm"]))

    report = {
        "schema_version": 1,
        "report": "iris-browser-power-compare",
        "manifest": manifest,
        "runs": runs,
        "dropped": [
            {"label": r["label"], "reasons": r["reasons"]} for r in runs if not r["valid"]
        ],
        "summary": summarise(runs),
    }
    text = json.dumps(report, indent=2, sort_keys=True)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(text + "\n")
        print(f"PASS power report written: {args.out}")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
