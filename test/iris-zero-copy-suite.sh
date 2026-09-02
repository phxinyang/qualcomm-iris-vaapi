#!/bin/sh

# Hardware qualification for the bounded Iris zero-copy contract.
#
# Run this through scripts/iris-experiment-guard.sh. Positive cases prove that
# H.264 no-B frames land directly in exported DMA-BUFs; boundary cases prove
# that B-frame H.264 and HEVC stay on the stable-copy fallback when the caller
# has not explicitly established the ownership contract.

set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/lib/iris-env.sh"

fail() {
    echo "FAIL zero-copy suite: $*" >&2
    exit 1
}

[ -n "${IRIS_EXPERIMENT_DIR:-}" ] \
    || fail 'run through scripts/iris-experiment-guard.sh so boot and temperature are recorded'

device=$(iris_resolve_device)
driver_path=${LIBVA_DRIVERS_PATH:-$(pwd)/build/src}
root=${IRIS_ZERO_COPY_DIR:-$HOME/Lab/Bridge/tmp/trash/iris-zero-copy-suite-$(date -u +%Y%m%dT%H%M%SZ)}
frames=${IRIS_ZERO_COPY_FRAMES:-24}
timeout_seconds=${IRIS_ZERO_COPY_TIMEOUT_SECONDS:-90}
max_temperature=${IRIS_ZERO_COPY_MAX_TEMP_MILLIC:-85000}

case "$frames:$timeout_seconds:$max_temperature" in
    *[!0-9:]*|0:*|*:0:*|*:0) fail 'numeric settings must be positive integers' ;;
esac
for tool in ffmpeg ffprobe v4l2-ctl timeout sha256sum awk cmp; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool is missing: $tool"
done
[ -c "$device" ] || fail "resolved decoder is not a character device: $device"
[ -f "$driver_path/v4l2_drv_video.so" ] || fail "driver not found: $driver_path/v4l2_drv_video.so"

mkdir -p "$root"
initial_boot=$(tr -d '\n' </proc/sys/kernel/random/boot_id)

hottest_temperature() {
    hottest=-999999
    found=0
    for zone in /sys/class/thermal/thermal_zone*; do
        [ -r "$zone/temp" ] || continue
        value=$(tr -d '\n' <"$zone/temp")
        case "$value" in *[!0-9-]*|'') continue ;; esac
        [ "$value" -ge 1000 ] || value=$((value * 1000))
        [ "$value" -le "$hottest" ] || hottest=$value
        found=1
    done
    [ "$found" -eq 1 ] || fail 'no readable thermal zones'
    printf '%s\n' "$hottest"
}

check_environment() {
    current_boot=$(tr -d '\n' </proc/sys/kernel/random/boot_id)
    [ "$current_boot" = "$initial_boot" ] \
        || fail "boot id changed: $initial_boot -> $current_boot"
    hottest=$(hottest_temperature)
    [ "$hottest" -le "$max_temperature" ] \
        || fail "temperature exceeded limit: $hottest > $max_temperature"
}

frame_count() {
    awk -F, '/^[0-9]+,/ { count++ } END { print count + 0 }' "$1"
}

write_manifest() {
    {
        printf 'schema=qualcomm-iris-vaapi/zero-copy-suite-v1\n'
        printf 'started_at_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'boot_id=%s\n' "$initial_boot"
        printf 'kernel=%s\n' "$(uname -r)"
        printf 'device=%s\n' "$device"
        printf 'driver_path=%s\n' "$driver_path/v4l2_drv_video.so"
        printf 'driver_sha256=%s\n' "$(sha256sum "$driver_path/v4l2_drv_video.so" | awk '{print $1}')"
        printf 'frames=%s\n' "$frames"
        printf 'zero_copy_contract=h264-no-b-v1\n'
    } >"$root/manifest.txt"
}

encode_h264() {
    name=$1
    gop=$2
    bframes=$3
    preset=$4
    timeout "$timeout_seconds" ffmpeg -y -hide_banner -loglevel error \
        -f lavfi -i "testsrc2=size=640x360:rate=24" -frames:v "$frames" \
        -c:v libx264 -preset "$preset" -g "$gop" -bf "$bframes" \
        -x264-params "scenecut=0:keyint=$gop:min-keyint=$gop" -an "$root/$name.mp4"
}

run_case() {
    name=$1
    input=$2
    expected_path=$3
    expected_fallback=${4:-}
    software=$root/$name.software.md5
    hardware=$root/$name.hardware.md5
    trace=$root/$name.trace

    timeout "$timeout_seconds" ffmpeg -y -hide_banner -loglevel error \
        -i "$input" -pix_fmt yuv420p -f framemd5 "$software"

    set +e
    if [ "$expected_path" = direct ]; then
        timeout "$timeout_seconds" env \
            LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" \
            LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_TRACE=1 \
            V4L2_VA_ZERO_COPY=1 \
            V4L2_VA_ZERO_COPY_CONTRACT=h264-no-b-v1 \
            V4L2_VA_BATCH_SIZE=1 \
            ffmpeg -y -hide_banner -loglevel error \
            -vaapi_device /dev/dri/renderD128 \
            -hwaccel vaapi -hwaccel_output_format vaapi -i "$input" \
            -vf 'hwdownload,format=nv12,format=yuv420p' -f framemd5 "$hardware" \
            2>"$trace"
    else
        timeout "$timeout_seconds" env -u V4L2_VA_ZERO_COPY_CONTRACT \
            LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$driver_path" \
            LIBVA_V4L2_VIDEO_PATH="$device" V4L2_VA_TRACE=1 \
            V4L2_VA_ZERO_COPY=1 V4L2_VA_BATCH_SIZE=1 \
            ffmpeg -y -hide_banner -loglevel error \
            -vaapi_device /dev/dri/renderD128 \
            -hwaccel vaapi -hwaccel_output_format vaapi -i "$input" \
            -vf 'hwdownload,format=nv12,format=yuv420p' -f framemd5 "$hardware" \
            2>"$trace"
    fi
    rc=$?
    set -e

    [ "$rc" -eq 0 ] || fail "$name decoder exit=$rc"
    actual=$(frame_count "$hardware")
    [ "$actual" -eq "$frames" ] || fail "$name frame count $actual/$frames"
    cmp -s "$software" "$hardware" || fail "$name frame MD5 differs from software"
    test/iris-eos-check.sh "$trace" "$frames" >/dev/null \
        || fail "$name strict EOS check"
    if grep -Eqi 'stateful timestamp miss|Timed out waiting for surface|v4l2 dq ERROR|capture complete.*error=[1-9]|undefined symbol' "$trace"; then
        fail "$name trace contains a decode error"
    fi

    if [ "$expected_path" = direct ]; then
        grep -q 'stateful zero-copy enabled' "$trace" \
            || fail "$name did not enter zero-copy"
        grep -q 'stateful zero-copy queue' "$trace" \
            || fail "$name queued no imported surface"
        if grep -q 'copy_surface_frame copied' "$trace"; then
            fail "$name copied a decoded frame"
        fi
    else
        grep -q "stateful zero-copy fallback reason=$expected_fallback" "$trace" \
            || fail "$name did not record fallback=$expected_fallback"
        grep -q 'copy_surface_frame copied' "$trace" \
            || fail "$name fallback did not publish stable copies"
        if grep -q 'stateful zero-copy enabled' "$trace"; then
            fail "$name entered zero-copy outside the ownership contract"
        fi
    fi
    check_environment
    printf 'PASS %s path=%s frames=%s\n' "$name" "$expected_path" "$actual"
}

write_manifest
check_environment
encode_h264 h264-all-i 1 0 ultrafast
encode_h264 h264-ip 12 0 ultrafast
encode_h264 h264-b2 "$frames" 2 medium
timeout "$timeout_seconds" ffmpeg -y -hide_banner -loglevel error \
    -f lavfi -i "testsrc2=size=640x360:rate=24" -frames:v "$frames" \
    -c:v libx265 -preset ultrafast \
    -x265-params "keyint=$frames:min-keyint=$frames:scenecut=0" -an "$root/hevc.mkv"

run_case h264-all-i "$root/h264-all-i.mp4" direct
run_case h264-ip "$root/h264-ip.mp4" direct
run_case h264-b2-safe-fallback "$root/h264-b2.mp4" fallback ownership_contract
run_case hevc-safe-fallback "$root/hevc.mkv" fallback codec_reorder_contract

printf 'completed_at_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$root/manifest.txt"
printf 'PASS zero-copy suite boot_id=%s root=%s\n' "$initial_boot" "$root"
