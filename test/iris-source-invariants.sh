#!/bin/sh

# Cheap source-level guards for the browser-facing Iris invariants. These run
# on hosts without V4L2 hardware and complement the runtime trace checks.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
context="$root/src/context.cc"
surface="$root/src/surface.cc"

grep -q 'return !value || std::strcmp(value, "0") != 0;' "$surface"
grep -q 'size_t limit = 8;' "$context"
grep -q 'VA_DRIVER_INIT_FUNC' "$root/src/driver.cc"

if grep -q 'memset(surface.export_buffer_mapping' "$surface"; then
    echo "FAIL stable export is cleared on sync timeout" >&2
    exit 1
fi

if ! grep -q 'device.buffer(device.capture_buf_type, i).queue()' "$context"; then
    echo "FAIL stateful start_capture does not queue the CAPTURE pool" >&2
    exit 1
fi

# A CAPTURE completion may be serviced before Chrome consumes its VA surface.
# Publish the complete per-surface snapshot before returning the rotating
# CAPTURE slot; otherwise Chrome presents the previous contents until a later
# surface reuse happens to call vaSyncSurface().
if ! grep -q 'stage_surface_frame(surface' "$context"; then
    echo "FAIL stateful queue service does not stage completed frames" >&2
    exit 1
fi
if ! grep -q 'publish_surface_frame(surface)' "$context"; then
    echo "FAIL stateful queue service does not publish completed frames" >&2
    exit 1
fi
if ! grep -q 'surface_for_capture_flags(device.last_dequeued_flags())' "$context" \
    || ! grep -q 'surface_for_capture_flags(device.last_dequeued_flags())' "$surface"; then
    echo "FAIL stateful CAPTURE flag fallback is disabled" >&2
    exit 1
fi
if ! grep -q 'surface.pending_frame_ready' "$surface" || ! grep -q 'publish_surface_frame(surface)' "$surface"; then
    echo "FAIL staged surface snapshots have no syncSurface fallback" >&2
    exit 1
fi

# Reuse the per-surface snapshot allocation. Clearing a full 1080p NV12
# buffer before every CAPTURE copy adds a third multi-megabyte write while the
# decoder synchronization lock is held and causes visible playback jitter.
if grep -q 'pending_frame.assign(total, 0)' "$surface"; then
    echo "FAIL pending surface snapshot is zero-filled on every frame" >&2
    exit 1
fi
if ! grep -q 'pending_frame.resize(total)' "$surface"; then
    echo "FAIL pending surface snapshot does not reuse its allocation" >&2
    exit 1
fi

service_block=$(sed -n '/void Context::service_stateful_queues()/,/^}/p' "$context")
if ! printf '%s\n' "$service_block" | grep -q 'publish_surface_frame(surface)'; then
    echo "FAIL queue service defers stable-frame publication" >&2
    exit 1
fi

echo "PASS source invariants"
