#!/bin/sh

# Cheap source-level guards for the browser-facing Iris invariants. These run
# on hosts without V4L2 hardware and complement the runtime trace checks.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
context="$root/src/context.cc"
surface="$root/src/surface.cc"

grep -q 'return !value || std::strcmp(value, "0") != 0;' "$surface"
grep -q 'size_t limit = 1;' "$context"
grep -q 'VA_DRIVER_INIT_FUNC' "$root/src/driver.cc"

if grep -q 'memset(surface.export_buffer_mapping' "$surface"; then
    echo "FAIL stable export is cleared on sync timeout" >&2
    exit 1
fi

if ! grep -q 'device.buffer(device.capture_buf_type, i).queue()' "$context"; then
    echo "FAIL stateful start_capture does not queue the CAPTURE pool" >&2
    exit 1
fi

# A CAPTURE completion may be serviced before a VA client consumes its surface.
# Copy the complete frame to the stable export before returning the rotating
# CAPTURE slot; otherwise a client may present the previous contents until a
# later surface reuse happens to call vaSyncSurface().
if ! grep -q 'copy_surface_frame(surface' "$context"; then
    echo "FAIL stateful queue service does not copy completed frames" >&2
    exit 1
fi
if ! grep -q 'copy_surfaces_enabled()' "$context" || ! grep -q 'copy_surface_frame(Surface' "$surface"; then
    echo "FAIL stable surface snapshot path is missing" >&2
    exit 1
fi
if ! grep -q 'surface_for_capture_flags(device.last_dequeued_flags())' "$context" \
    || ! grep -q 'surface_for_capture_flags(device.last_dequeued_flags())' "$surface"; then
    echo "FAIL stateful CAPTURE flag fallback is disabled" >&2
    exit 1
fi
if ! grep -q 'stateful_batch_limit() == 1' "$surface" \
    || ! grep -q 'fallback reason=batch_contract' "$surface"; then
    echo "FAIL zero-copy batch ownership guard is missing" >&2
    exit 1
fi
if ! grep -q 'export_buffer_mapping' "$surface" || ! grep -q 'copy_surface_frame' "$surface"; then
    echo "FAIL stable surface snapshots have no copy path" >&2
    exit 1
fi

service_block=$(sed -n '/void Context::service_stateful_queues()/,/^}/p' "$context")
if ! printf '%s\n' "$service_block" | grep -q 'copy_surface_frame(surface'; then
    echo "FAIL queue service does not copy stable frame before CAPTURE reuse" >&2
    exit 1
fi

echo "PASS source invariants"
