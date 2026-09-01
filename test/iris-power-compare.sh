#!/bin/sh

# Battery A/B for real Chrome playback: hardware VA-API decode against the
# browser's software decoder, on the same clip, on the same tablet, within the
# same discharge window.
#
# The existing comparison in TEST-RESULTS.md measured paced GStreamer/FFmpeg
# pipelines into fakesink and says so: it cannot answer what a browser costs.
# This harness closes that gap and is deliberately stricter than the earlier
# ad-hoc run in four ways.
#
#   1. Interleaved ABBA blocks. A straight "all hardware then all software"
#      sequence confounds the arm with battery depletion, panel warm-up and
#      thermal drift. Blocks alternate order so a monotonic drift cancels in
#      the paired difference.
#   2. Paired analysis. Each block yields one hardware and one software run
#      under near-identical conditions; the statistic is the per-block
#      difference, not two independent pooled means.
#   3. Decoder provenance per run. "The video played" is true in both arms.
#      A run only counts when the Media domain names the decoder and the
#      frame counters advanced inside the window.
#   4. Fail-closed environment. Charger, brightness, idle blanking and power
#      profile are pinned and re-checked; a run that saw the charger, a
#      brightness change or a profile switch is marked invalid rather than
#      averaged in.
#
# It must run on the target, inside the graphical session's user, because the
# browser has to render to the real panel.

set -eu

usage() {
    cat <<'EOF'
Usage: iris-power-compare.sh --clip PATH [OPTIONS]

Options:
  --clip PATH             Video file to loop (required).
  --out DIR               Artifact directory (default: lab artifacts/power-browser-DATE).
  --blocks N              Interleaved ABBA blocks; each runs both arms (default 5).
  --measure-seconds N     Measurement window per run (default 300).
  --warmup-seconds N      Settle time after playback starts (default 20).
  --baseline-seconds N    Idle baseline before and after each run (default 60).
  --cooldown-seconds N    Idle gap between runs (default 30).
  --brightness N          Raw backlight value to pin (default: keep current).
  --port N                DevTools port (default 9333).
  -h, --help              Show this help.

Environment:
  IRIS_POWER_BATTERY      Battery gauge directory.
  IRIS_POWER_THERMAL_ZONE Thermal zone directory sampled alongside the gauge.
  IRIS_BROWSER_LAUNCHER   Path to iris-vaapi-browser.
EOF
}

fail() {
    echo "FAIL iris-power-compare: $*" >&2
    exit 1
}

note() { printf '%s %s\n' "$(date -Is)" "$*"; }

test_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
. "$test_dir/lib/iris-env.sh"

clip=''
out=''
blocks=5
measure_seconds=300
warmup_seconds=20
baseline_seconds=60
cooldown_seconds=30
brightness=''
port=9333

while [ "$#" -gt 0 ]; do
    case "$1" in
        --clip) clip=${2:?--clip needs a path}; shift 2 ;;
        --out) out=${2:?--out needs a path}; shift 2 ;;
        --blocks) blocks=${2:?}; shift 2 ;;
        --measure-seconds) measure_seconds=${2:?}; shift 2 ;;
        --warmup-seconds) warmup_seconds=${2:?}; shift 2 ;;
        --baseline-seconds) baseline_seconds=${2:?}; shift 2 ;;
        --cooldown-seconds) cooldown_seconds=${2:?}; shift 2 ;;
        --brightness) brightness=${2:?}; shift 2 ;;
        --port) port=${2:?}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; fail "unknown argument: $1" ;;
    esac
done

for value in "$blocks" "$measure_seconds" "$warmup_seconds" "$baseline_seconds" \
    "$cooldown_seconds" "$port"; do
    case "$value" in
        ''|*[!0-9]*) fail "numeric option expected, got: $value" ;;
    esac
done
[ "$blocks" -ge 1 ] || fail 'at least one block is required'
[ -n "$clip" ] || { usage >&2; fail '--clip is required'; }
[ -f "$clip" ] || fail "clip not found: $clip"
clip=$(CDPATH='' cd -- "$(dirname -- "$clip")" && pwd)/$(basename -- "$clip")

battery=${IRIS_POWER_BATTERY:-/sys/class/power_supply/qcom-battmgr-bat}
[ -d "$battery" ] || fail "battery gauge not found: $battery"
thermal=${IRIS_POWER_THERMAL_ZONE:-/sys/class/thermal/thermal_zone0}
launcher=${IRIS_BROWSER_LAUNCHER:-/usr/local/bin/iris-vaapi-browser}
[ -x "$launcher" ] || fail "browser launcher not executable: $launcher"

[ -n "$out" ] || out=$(iris_artifact_dir "power-browser-$(date -u +%Y%m%dT%H%M%SZ)")
mkdir -p "$out"
out=$(CDPATH='' cd -- "$out" && pwd)

python3 -c 'import sys; sys.exit(0)' >/dev/null 2>&1 || fail 'python3 is required'
sampler=$test_dir/remote/power-sampler.py
cdp=$test_dir/remote/cdp.py
page=$test_dir/power-playback.html
for path in "$sampler" "$cdp" "$page"; do
    [ -f "$path" ] || fail "missing harness file: $path"
done

# ---------------------------------------------------------------- preconditions

supply_online() {
    for supply in "$(dirname -- "$battery")"/*; do
        [ -r "$supply/online" ] || continue
        [ "$(cat "$supply/online")" = 1 ] && return 0
    done
    return 1
}

if supply_online; then
    fail 'an external supply reports online=1; unplug before measuring battery draw'
fi
case "$(cat "$battery/status" 2>/dev/null || true)" in
    Discharging) ;;
    *) fail "battery is not discharging: $(cat "$battery/status" 2>/dev/null || echo unknown)" ;;
esac

node=$("$launcher" --print-node) || fail 'could not resolve the Iris decoder node'
[ -c "$node" ] || fail "resolved Iris node is not a character device: $node"

backlight_dir=''
for candidate in /sys/class/backlight/*; do
    [ -r "$candidate/brightness" ] || continue
    backlight_dir=$candidate
    break
done
[ -n "$backlight_dir" ] || note 'WARN no backlight device found; brightness will not be pinned'

if [ -n "$brightness" ] && [ -n "$backlight_dir" ]; then
    if command -v brightnessctl >/dev/null 2>&1; then
        device=$(basename -- "$backlight_dir")
        brightnessctl -d "$device" set "$brightness" >/dev/null 2>&1 \
            || sudo -n brightnessctl -d "$device" set "$brightness" >/dev/null 2>&1 \
            || note 'WARN could not set the backlight; continuing with the current value'
    else
        note 'WARN brightnessctl is missing; continuing with the current value'
    fi
fi
brightness_start=''
[ -n "$backlight_dir" ] && brightness_start=$(cat "$backlight_dir/brightness")
# A pinned value is a convenience; a stable value is the requirement. Every run
# records the backlight at both edges and the analyser drops a run that moved.
[ -n "$brightness_start" ] && note "backlight $backlight_dir=$brightness_start"

# ------------------------------------------------------------ environment lock

saved_settings=$out/gsettings-restore.sh
: >"$saved_settings"
lock_gsetting() {
    schema=$1
    key=$2
    want=$3
    command -v gsettings >/dev/null 2>&1 || return 0
    current=$(gsettings get "$schema" "$key" 2>/dev/null) || return 0
    printf "gsettings set %s %s %s\n" "$schema" "$key" "'$current'" >>"$saved_settings"
    gsettings set "$schema" "$key" "$want" 2>/dev/null || true
}

lock_gsetting org.gnome.desktop.session idle-delay 0
lock_gsetting org.gnome.desktop.screensaver idle-activation-enabled false
lock_gsetting org.gnome.desktop.screensaver lock-enabled false
lock_gsetting org.gnome.settings-daemon.plugins.power sleep-inactive-battery-type nothing
lock_gsetting org.gnome.settings-daemon.plugins.power sleep-inactive-ac-type nothing
lock_gsetting org.gnome.settings-daemon.plugins.power power-saver-profile-on-low-battery false
lock_gsetting org.gnome.settings-daemon.plugins.power ambient-enabled false
# idle-dim is the one that actually cost a collection. Nobody touches a tablet
# during an unattended power run, so GNOME dimmed the panel partway through the
# first block and every later run was measured against a backlight the harness
# thought it had pinned. Blanking and suspend were locked; dimming was not.
lock_gsetting org.gnome.settings-daemon.plugins.power idle-dim false

power_profile=''
command -v powerprofilesctl >/dev/null 2>&1 && power_profile=$(powerprofilesctl get 2>/dev/null || true)

# Kill the browser without killing the sampler.
#
# The sampler is told which processes to account for, so its own argv contains
# the profile path too and every pattern that finds Chrome also finds it. Two
# rounds of pattern anchoring did not fix that; both times the sampler died
# with the browser and the post-run baseline window came back empty. Match on
# the pattern, then exclude the sampler by pid.
kill_browser() {
    waited=0
    while :; do
        victims=''
        for pid in $(pgrep -f -- "--user-data-dir=$out/profiles" 2>/dev/null || true); do
            [ "$pid" = "${sampler_pid:-}" ] && continue
            [ "$pid" = "$$" ] && continue
            victims="$victims $pid"
        done
        [ -n "$victims" ] || return 0
        if [ "$waited" -gt 30 ]; then
            # shellcheck disable=SC2086
            kill -9 $victims 2>/dev/null || true
            return 0
        fi
        # shellcheck disable=SC2086
        [ "$waited" -eq 0 ] && { kill $victims 2>/dev/null || true; }
        waited=$((waited + 1))
        sleep 1
    done
}

cleanup() {
    kill_browser
    [ -n "${sampler_pid:-}" ] && kill "$sampler_pid" 2>/dev/null || true
    [ -s "$saved_settings" ] && sh "$saved_settings" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

# ------------------------------------------------------------------ provenance

driver_path=$(pkg-config --variable=driverdir libva 2>/dev/null || echo /usr/lib64/dri)/v4l2_drv_video.so
driver_sha256=$(sha256sum "$driver_path" 2>/dev/null | awk '{print $1}')
manifest=/usr/local/share/iris-vaapi/install-manifest.txt
source_commit=$(sed -n 's/^source_commit=//p' "$manifest" 2>/dev/null | head -1)
source_dirty=$(sed -n 's/^source_dirty=//p' "$manifest" 2>/dev/null | head -1)
boot_id=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo unknown)
chrome_version=$(google-chrome --version 2>/dev/null || echo unknown)

clip_probe=$out/clip-probe.txt
if command -v ffprobe >/dev/null 2>&1; then
    ffprobe -v error -select_streams v:0 \
        -show_entries stream=codec_name,profile,width,height,r_frame_rate,has_b_frames,nb_frames \
        -show_entries format=duration,bit_rate -of default=nw=1 "$clip" >"$clip_probe" 2>&1 || true
fi

python3 - "$out/manifest.json" <<PY
import json, sys
json.dump({
    "schema_version": 1,
    "harness": "iris-power-compare.sh",
    "started_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
    "kernel": "$(uname -r)",
    "boot_id": "$boot_id",
    "chrome_version": "$chrome_version",
    "iris_node": "$node",
    "driver_path": "$driver_path",
    "driver_sha256": "$driver_sha256",
    "source_commit": "$source_commit",
    "source_dirty": "$source_dirty",
    "clip": "$clip",
    "blocks": $blocks,
    "measure_seconds": $measure_seconds,
    "warmup_seconds": $warmup_seconds,
    "baseline_seconds": $baseline_seconds,
    "cooldown_seconds": $cooldown_seconds,
    "backlight_device": "$backlight_dir",
    "brightness_start": "$brightness_start",
    "power_profile": "$power_profile",
    "battery": "$battery",
    "thermal_zone": "$thermal",
}, open(sys.argv[1], "w"), indent=2, sort_keys=True)
PY

note "artifacts $out"
note "node=$node driver=$driver_sha256 commit=$source_commit dirty=$source_dirty"

# ------------------------------------------------------------------- one run

run_once() {
    arm=$1
    block=$2
    label=block$block-$arm
    run_dir=$out/$label
    mkdir -p "$run_dir"

    kill_browser

    # The match string starts with "--", so it has to be passed in the
    # --opt=value form: as a separate argument argparse reads it as the next
    # option and the sampler exits before writing a single row.
    run_brightness_start=''
    [ -n "$backlight_dir" ] && run_brightness_start=$(cat "$backlight_dir/brightness")

    python3 "$sampler" \
        --battery "$battery" \
        --thermal-zone "$thermal" \
        --backlight "$backlight_dir" \
        --proc-match="--user-data-dir=$out/profiles" \
        --interval 1 \
        --out "$run_dir/samples.csv" &
    sampler_pid=$!

    # A sampler that dies at startup produced a full run and a PASS line with
    # no gauge data behind it. Require evidence that it is actually recording
    # before spending five minutes of battery on the arm.
    waited=0
    while [ ! -s "$run_dir/samples.csv" ] || [ "$(wc -l <"$run_dir/samples.csv")" -lt 2 ]; do
        waited=$((waited + 1))
        if [ "$waited" -gt 10 ]; then
            kill "$sampler_pid" 2>/dev/null || true
            fail "sampler produced no rows for $label; see $run_dir/samples.csv"
        fi
        kill -0 "$sampler_pid" 2>/dev/null \
            || fail "sampler exited immediately for $label"
        sleep 1
    done

    baseline_pre_start=$(date +%s.%N)
    sleep "$baseline_seconds"
    baseline_pre_end=$(date +%s.%N)

    chrome_args="--remote-debugging-port=$port --remote-allow-origins=* --kiosk
        --autoplay-policy=no-user-gesture-required --no-first-run
        --no-default-browser-check --disable-background-networking
        --disable-component-update --no-pings --disable-sync"
    [ "$arm" = sw ] && chrome_args="$chrome_args --disable-accelerated-video-decode"

    # chrome_args is a deliberate word list, not one argument.
    # shellcheck disable=SC2086
    IRIS_BROWSER_PROFILE_ROOT=$out/profiles/$arm \
        "$launcher" --browser=chrome -- $chrome_args about:blank \
        >"$run_dir/browser.log" 2>&1 &
    browser_pid=$!

    url="file://$page?src=file://$clip"
    rc=0
    python3 "$cdp" --port "$port" run \
        --url "$url" \
        --warmup-seconds "$warmup_seconds" \
        --measure-seconds "$measure_seconds" \
        --out "$run_dir/cdp.json" >"$run_dir/cdp.log" 2>&1 || rc=$?

    # Independent of the browser's self-report: does a Chrome process hold the
    # Iris decoder node right now? Software decode never opens it. Compare the
    # full symlink target rather than a basename suffix, so /dev/video1 cannot
    # be mistaken for /dev/video11.
    : >"$run_dir/iris-node-holders.txt"
    for pid in $(pgrep -f -- "--user-data-dir=$out/profiles" 2>/dev/null || true); do
        [ "$pid" = "${sampler_pid:-}" ] && continue
        for fd in "/proc/$pid/fd"/*; do
            [ -e "$fd" ] || continue
            [ "$(readlink "$fd" 2>/dev/null || true)" = "$node" ] || continue
            printf '%s %s\n' "$pid" \
                "$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null | grep -o -- '--type=[a-z-]*' | head -1)" \
                >>"$run_dir/iris-node-holders.txt"
            break
        done
    done

    kill_browser
    wait "$browser_pid" 2>/dev/null || true

    baseline_post_start=$(date +%s.%N)
    sleep "$baseline_seconds"
    baseline_post_end=$(date +%s.%N)

    kill "$sampler_pid" 2>/dev/null || true
    wait "$sampler_pid" 2>/dev/null || true
    sampler_pid=''

    brightness_end=''
    [ -n "$backlight_dir" ] && brightness_end=$(cat "$backlight_dir/brightness")
    power_profile_end=''
    command -v powerprofilesctl >/dev/null 2>&1 && power_profile_end=$(powerprofilesctl get 2>/dev/null || true)

    python3 - "$run_dir/run.json" <<PY
import json, sys
json.dump({
    "schema_version": 1,
    "label": "$label",
    "arm": "$arm",
    "block": $block,
    "cdp_exit": $rc,
    "baseline_pre": [$baseline_pre_start, $baseline_pre_end],
    "baseline_post": [$baseline_post_start, $baseline_post_end],
    "brightness_start": "$run_brightness_start",
    "brightness_end": "$brightness_end",
    "brightness_session_start": "$brightness_start",
    "power_profile_start": "$power_profile",
    "power_profile_end": "$power_profile_end",
}, open(sys.argv[1], "w"), indent=2, sort_keys=True)
PY

    if [ "$rc" -eq 0 ]; then
        note "PASS $label"
    else
        note "WARN $label cdp exit=$rc (kept as an invalid run)"
    fi
    sleep "$cooldown_seconds"
}

block=1
while [ "$block" -le "$blocks" ]; do
    if [ $((block % 2)) -eq 1 ]; then
        order='hw sw'
    else
        order='sw hw'
    fi
    note "block $block/$blocks order=$order"
    for arm in $order; do
        run_once "$arm" "$block"
    done
    block=$((block + 1))
done

note "PASS collection complete: $out"
printf 'PASS iris-power-compare runs=%d out=%s\n' "$((blocks * 2))" "$out"
