#!/bin/sh

# Cheap source-level guards for the browser-facing Iris invariants. These run
# on hosts without V4L2 hardware and complement the runtime trace checks.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
context="$root/src/context.cc"
surface="$root/src/surface.cc"
env_helper="$root/test/lib/iris-env.sh"
matrix="$root/test/iris-matrix.sh"
structure_matrix="$root/test/iris-structure-matrix.sh"

grep -q 'return !value || std::strcmp(value, "0") != 0;' "$surface"
grep -q 'size_t limit = 1;' "$context"
grep -q 'VA_DRIVER_INIT_FUNC' "$root/src/driver.cc"

if grep -Eq 'printf .* /dev/video0|echo .* /dev/video0' "$env_helper" \
    || ! grep -q 'no /dev/video\* node reports iris_driver' "$env_helper"; then
    echo "FAIL Iris test device resolver can silently fall back to a camera node" >&2
    exit 1
fi

if grep -q 'memset(surface.export_buffer_mapping' "$surface"; then
    echo "FAIL stable export is cleared on sync timeout" >&2
    exit 1
fi

# The positive hardware matrix is an exact pixel oracle. A decoder that exits
# successfully but emits different frames must fail the run rather than leave
# a warning in otherwise-green output.
if grep -q 'WARN .*content=frame-md5-mismatch' "$matrix" \
    || ! grep -q 'FAIL \$codec content=frame-md5-mismatch' "$matrix"; then
    echo "FAIL Iris matrix does not fail closed on pixel mismatches" >&2
    exit 1
fi

# Normal CAPTURE completion traces include error=0. Do not let a broad
# "capture.*error" matcher reject every successfully decoded frame.
if grep -q "capture\.\*error\\\\|V4L2_BUF_FLAG_ERROR" "$structure_matrix" \
    || ! grep -q 'capture\.\*error=\[1-9\]' "$structure_matrix"; then
    echo "FAIL Iris structure matrix treats successful CAPTURE logs as errors" >&2
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
completion_block=$(sed -n '/bool Context::handle_capture_completion/,/^}/p' "$context")
if ! printf '%s\n' "$completion_block" | grep -q 'surface_for_capture_flags(device.last_dequeued_flags())'; then
    echo "FAIL unified CAPTURE completion has no flag fallback" >&2
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
export_block=$(sed -n '/VAStatus exportSurfaceHandle(/,/^}/p' "$surface")
if ! printf '%s\n' "$export_block" | grep -q '!capture_bound && !copy_surfaces_enabled()'; then
    echo "FAIL unbound no-copy export can return a stale stable buffer" >&2
    exit 1
fi

service_block=$(sed -n '/void Context::service_stateful_queues()/,/^}/p' "$context")
if ! printf '%s\n' "$service_block" | grep -q 'handle_capture_completion' \
    || ! printf '%s\n' "$completion_block" | grep -q 'copy_surface_frame(surface'; then
    echo "FAIL queue service does not copy stable frame before CAPTURE reuse" >&2
    exit 1
fi

echo "PASS source invariants"
