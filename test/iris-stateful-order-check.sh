#!/bin/sh

# Guard the stateful Iris invariants that are not observable on hosts without
# the Qualcomm V4L2 device: bounded AU aggregation for smooth playback and
# retention of a timed-out surface's timestamp mapping until a late CAPTURE is
# reaped.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
context="$root/src/context.cc"
surface="$root/src/surface.cc"

if [ ! -r "$context" ] || [ ! -r "$surface" ]; then
    echo "FAIL source files are not readable" >&2
    exit 2
fi

flush_block=$(sed -n '/VAStatus Context::flush_stateful_batch()/,/^void Context::service_stateful_queues()/p' "$context")
timeout_block=$(sed -n '/if (surface.status == VASurfaceRendering)/,/^        }/p' "$surface")

if ! printf '%s\n' "$flush_block" | grep -q 'stateful_batch_limit_from_env'; then
    echo "FAIL stateful flush has no bounded batch limit" >&2
    exit 1
fi
if ! printf '%s\n' "$flush_block" | grep -q 'batch_surfaces'; then
    echo "FAIL stateful flush does not retain the surfaces in an aggregate batch" >&2
    exit 1
fi
if ! printf '%s\n' "$flush_block" | grep -q 'aggregate_size'; then
    echo "FAIL stateful flush does not aggregate AU payloads" >&2
    exit 1
fi

# A terminal CAPTURE LAST flag is overwritten by the next DQBUF call. Save it
# immediately after CAPTURE dequeue so draining cannot continue into EPIPE.
if ! grep -q 'const bool capture_was_last = device.last_dequeued_last();' "$context"; then
    echo "FAIL stateful drain does not preserve CAPTURE LAST before OUTPUT DQBUF" >&2
    exit 1
fi

if printf '%s\n' "$timeout_block" | grep -q 'discard_stateful_surface(surface_id)'; then
    echo "FAIL timeout path deletes the late CAPTURE timestamp mapping" >&2
    exit 1
fi

echo "PASS stateful OUTPUT order and late timestamp mapping invariants"
