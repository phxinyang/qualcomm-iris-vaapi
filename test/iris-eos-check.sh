#!/bin/sh

# Validate a V4L2 stateful trace against the generic EOS contract (also used by
# Chromium's video decoder tests): every submitted access unit must complete,
# followed by one terminal CAPTURE LAST marker per VA context after decoder
# STOP. FFmpeg may recreate its VA context at coded-size changes while keeping
# one decoder process alive.

set -eu

strict_sync=1
if [ "$#" -eq 3 ] && [ "$3" = "--allow-bounded-sync" ]; then
    strict_sync=0
elif [ "$#" -ne 2 ]; then
    echo "usage: $0 TRACE_LOG FRAME_COUNT [--allow-bounded-sync]" >&2
    exit 2
fi

log=$1
expected=$2

case "$expected" in
    ''|*[!0-9]*)
        echo "FRAME_COUNT must be a non-negative integer" >&2
        exit 2
        ;;
esac

if [ ! -r "$log" ]; then
    echo "trace log is not readable: $log" >&2
    exit 2
fi

count() {
    # Trace records may share a physical line because the driver writes a
    # literal "\\n" separator. Count matches, not physical lines.
    grep -o "$1" "$log" | wc -l | tr -d ' ' || true
}

flush=$(count 'stateful flush batch=')
contexts=$(count 'va create_context done')
# Older hand-captured traces may start after vaCreateContext. Preserve their
# single-context interpretation while requiring explicit counts when present.
[ "$contexts" -gt 0 ] || contexts=1
# The stable browser path aggregates several access units in one OUTPUT
# buffer. Diagnostic one-AU traces omit `surfaces=`; count each such batch as
# one submitted access unit for backward compatibility.
submitted=$(awk '
    {
        records = split($0, parts, /\\n/)
        for (i = 1; i <= records; i++) {
            if (parts[i] !~ /stateful flush batch=/)
                continue
            if (match(parts[i], /surfaces=[0-9]+/)) {
                value = substr(parts[i], RSTART + 9, RLENGTH - 9) + 0
                total += value
            } else {
                total++
            }
        }
    }
    END { print total + 0 }
' "$log")
capture=$(count 'v4l2 dq type=9 index=')
output=$(count 'v4l2 dq type=10 index=')
last=$(count 'v4l2 dq type=9 .*last=1')
stop=$(count 'v4l2 decoder STOP')
drain=$(count 'stateful drain complete last=1')
capture_errors=$(count 'v4l2 dq ERROR type=9')
timeouts=$(count 'Timed out waiting')
misses=$(count 'stateful timestamp miss')

# A stateful decoder returns CAPTURE buffers in sequence order even when their
# timestamps are reordered by the codec. Keep this check independent of
# timestamp order, which is intentionally non-monotonic for B-frames.
sequence_errors=$(awk '
    {
        records = split($0, parts, /\\n/)
        for (i = 1; i <= records; i++) {
            if (parts[i] !~ /v4l2 dq type=9 index=/ || !match(parts[i], /seq=[0-9]+/))
                continue
            seq = substr(parts[i], RSTART + 4, RLENGTH - 4) + 0
            if (seen && seq != previous + 1)
                errors++
            previous = seq
            seen = 1
            if (parts[i] ~ /last=1/)
                seen = 0
        }
    }
    END { print errors + 0 }
' "$log")

terminal_last=$(awk '
    {
        records = split($0, parts, /\\n/)
        for (i = 1; i <= records; i++) {
            if (parts[i] ~ /v4l2 dq type=9 index=/)
                last = parts[i]
        }
    }
    END { print (last ~ /last=1/) ? 0 : 1 }
' "$log")

expected_capture=$((expected + contexts))

fail=0
check() {
    name=$1
    actual=$2
    wanted=$3
    if [ "$actual" -ne "$wanted" ]; then
        echo "FAIL $name=$actual (expected $wanted)" >&2
        fail=1
    fi
}

if [ "$flush" -le 0 ]; then
    echo "FAIL flush_batches=$flush (expected at least 1)" >&2
    fail=1
fi
check submitted_frames "$submitted" "$expected"
check capture "$capture" "$expected_capture"
check output "$output" "$flush"
check stop "$stop" "$contexts"
check drain "$drain" "$contexts"
check last "$last" "$contexts"
check capture_errors "$capture_errors" 0
check capture_sequence_errors "$sequence_errors" 0
check terminal_last "$terminal_last" 0
if [ "$strict_sync" -eq 1 ]; then
    check timeouts "$timeouts" 0
    check timestamp_misses "$misses" 0
elif [ "$timeouts" -ne 0 ] || [ "$misses" -ne 0 ]; then
    echo "WARN bounded_sync timeouts=$timeouts timestamp_misses=$misses" >&2
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "PASS frames=$expected contexts=$contexts batches=$flush submitted=$submitted capture=$capture output=$output stop=$stop drain=$drain last=$last timeouts=$timeouts timestamp_misses=$misses"
