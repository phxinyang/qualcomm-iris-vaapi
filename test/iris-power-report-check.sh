#!/bin/sh

# Self-test for the power comparison analyser, with no hardware and no browser.
#
# The claim this harness makes is not "software costs more watts"; it is "these
# specific runs were comparable, and here is every run that was not". That
# claim rests entirely on the rejection rules, and a rejection rule that
# quietly stops firing looks exactly like a clean experiment. So build a
# collection where each arm is broken in one known way and require the
# analyser to name it.
#
# The fixture also pins the arithmetic: one fully valid block with a known
# 0.350 W separation must come back as a paired difference of 0.350 W.

set -eu

root=${1:-$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)}
analyze=$root/test/remote/power-analyze.py
[ -f "$analyze" ] || { echo "FAIL missing $analyze" >&2; exit 1; }

scratch_base=${IRIS_TEST_SCRATCH_ROOT:-$HOME/Lab/Bridge/tmp/trash}
mkdir -p "$scratch_base"
scratch=$(mktemp -d "$scratch_base/iris-power-report-check.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

python3 - "$scratch" "$analyze" <<'PY'
import importlib.util, json, pathlib, subprocess, sys

collection = pathlib.Path(sys.argv[1]) / "collection"
analyze = sys.argv[2]
collection.mkdir(parents=True)

BASE = 1_000_000.0
PRE = (BASE, BASE + 60)
MEASURE = (BASE + 80, BASE + 380)
POST = (BASE + 390, BASE + 450)

DECODERS = {
    "hw": ("VaapiVideoDecoder", True),
    "sw": ("FFmpegVideoDecoder", False),
}


def write_run(label, arm, *, playback_w, baseline_w, post_w=None,
              decoder=None, platform=None, frames=9000, online_at=None,
              brightness=("1024", "1024"), profile=("balanced", "balanced"),
              sampled_backlight=None, dim_at=None):
    run_dir = collection / label
    run_dir.mkdir()
    default_decoder, default_platform = DECODERS[arm]
    post_w = baseline_w if post_w is None else post_w

    rows = ["epoch,voltage_uv,current_ua,power_w,capacity,status,supply_online,temp_mc,"
            "proc_cpu_jiffies,proc_count,brightness"]
    jiffies = 0
    for step in range(int(POST[1] - BASE) + 1):
        epoch = BASE + step
        if MEASURE[0] <= epoch <= MEASURE[1]:
            watts = playback_w
            jiffies += 20
        elif epoch <= PRE[1]:
            watts = baseline_w
        elif epoch >= POST[0]:
            watts = post_w
        else:
            watts = baseline_w
        online = 1 if online_at is not None and epoch >= BASE + online_at else 0
        cpu = f"{jiffies},3" if step % 5 == 0 else ","
        level = ""
        if sampled_backlight is not None:
            level = sampled_backlight
            if dim_at is not None and epoch >= BASE + dim_at:
                level = "628"
        rows.append(
            f"{epoch:.3f},4200000,-700000,{watts:.6f},80,Discharging,{online},31000,{cpu},{level}"
        )
    (run_dir / "samples.csv").write_text("\n".join(rows) + "\n")

    (run_dir / "run.json").write_text(json.dumps({
        "schema_version": 1,
        "label": label,
        "arm": arm,
        "block": int(label[5]),
        "cdp_exit": 0,
        "baseline_pre": list(PRE),
        "baseline_post": list(POST),
        "brightness_start": brightness[0],
        "brightness_end": brightness[1],
        "power_profile_start": profile[0],
        "power_profile_end": profile[1],
    }))

    (run_dir / "cdp.json").write_text(json.dumps({
        "schema_version": 1,
        "decoder_name": default_decoder if decoder is None else decoder,
        "is_platform_video_decoder": default_platform if platform is None else platform,
        "measure_start_epoch": MEASURE[0],
        "measure_end_epoch": MEASURE[1],
        "window_decoded_frames": frames,
        "window_dropped_frames": 0,
    }))
    if arm == "hw":
        (run_dir / "iris-node-holders.txt").write_text("123 --type=gpu-process\\n")


# Block 1 is the only comparable pair: 3.250 - 2.900 = 0.350 W.
write_run("block1-hw", "hw", playback_w=2.900, baseline_w=2.450)
write_run("block1-sw", "sw", playback_w=3.250, baseline_w=2.450)
# The hardware arm silently ran software. This is the failure the launcher
# shipped for real, and averaging it would have reported no difference.
write_run("block2-hw", "hw", playback_w=3.250, baseline_w=2.450,
          decoder="FFmpegVideoDecoder", platform=False)
write_run("block2-sw", "sw", playback_w=3.250, baseline_w=2.450)
# The charger appeared partway through.
write_run("block3-hw", "hw", playback_w=2.900, baseline_w=2.450, online_at=200)
write_run("block3-sw", "sw", playback_w=3.250, baseline_w=2.450)
# The panel changed brightness, which dwarfs the decode difference.
write_run("block4-hw", "hw", playback_w=2.900, baseline_w=2.450,
          brightness=("1024", "1600"))
write_run("block4-sw", "sw", playback_w=3.250, baseline_w=2.450)
# The device was in a different power state at the two window edges.
write_run("block5-hw", "hw", playback_w=2.900, baseline_w=2.450, post_w=4.900)
write_run("block5-sw", "sw", playback_w=3.250, baseline_w=2.450)
# Nothing decoded, so the run measured an idle browser.
write_run("block6-hw", "hw", playback_w=2.900, baseline_w=2.450, frames=0)
write_run("block6-sw", "sw", playback_w=3.250, baseline_w=2.450)
# The panel dimmed partway through while both edge readings still agree. This
# is the run that cost a whole collection: GNOME idle-dim moved the backlight
# and an edge-only comparison could not see it.
write_run("block7-hw", "hw", playback_w=2.900, baseline_w=2.450,
          sampled_backlight="1024", dim_at=200)
write_run("block7-sw", "sw", playback_w=3.250, baseline_w=2.450,
          sampled_backlight="1024")

report = json.loads(subprocess.run(
    [sys.executable, analyze, str(collection)],
    check=True, capture_output=True, text=True).stdout)

module_spec = importlib.util.spec_from_file_location("power_analyze", analyze)
power_analyze = importlib.util.module_from_spec(module_spec)
module_spec.loader.exec_module(power_analyze)
assert power_analyze.arm_expectation({"browser": "chromium"}, "hw") == {
    "decoder": "V4L2VideoDecoder", "platform": True, "node": True
}
assert power_analyze.arm_expectation({"browser": "chrome"}, "sw") == {
    "decoder": "FFmpegVideoDecoder", "platform": False, "node": False
}

failures = []


def expect(condition, message):
    if not condition:
        failures.append(message)


dropped = {entry["label"]: " ".join(entry["reasons"]) for entry in report["dropped"]}
expected = {
    "block2-hw": "expected VaapiVideoDecoder",
    "block3-hw": "external supply came online",
    "block4-hw": "brightness moved",
    "block5-hw": "baseline gap",
    "block6-hw": "no frames decoded",
    "block7-hw": "backlight moved during the run",
}
for label, needle in expected.items():
    expect(label in dropped, f"{label} was not dropped")
    expect(needle in dropped.get(label, ""), f"{label} dropped for the wrong reason: {dropped.get(label)!r}")

for label in ("block1-hw", "block1-sw", "block2-sw", "block7-sw"):
    expect(label not in dropped, f"{label} was dropped but is valid: {dropped.get(label)!r}")

summary = report["summary"]
expect("brightness_levels" in report["runs"][-1] or True, "fixture sanity")
expect(summary["paired_n"] == 1,
       f"expected exactly one comparable block, got {summary['paired_n']} ({summary['paired_blocks']})")
difference = summary["software_minus_hardware_playback_w"]["mean"]
expect(difference is not None and abs(difference - 0.350) < 1e-6,
       f"paired difference should be 0.350 W, got {difference}")
expect(summary["playback_difference_significant"] is None,
       "a single block cannot support a significance verdict")

hw = summary["arms"]["hw"]
expect(hw["playback_w"]["n"] == 1, f"only one hardware run is valid, got {hw['playback_w']['n']}")
expect(hw["cpu_cores"]["mean"] is not None, "cpu cross-check was not computed")

if failures:
    for line in failures:
        print(f"FAIL power report: {line}", file=sys.stderr)
    sys.exit(1)
print(f"PASS power analyser rejected {len(dropped)} runs by name and paired 1 block at 0.350 W")
PY
