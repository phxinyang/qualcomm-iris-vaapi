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
drain_block=$(sed -n '/bool Context::drain_stateful_decoder()/,/^void Context::mark_source_buffer_dequeued()/p' "$context")
reset_block=$(sed -n '/bool Context::reset_stateful_decoder()/,/^bool Context::drain_stateful_decoder()/p' "$context")
resume_block=$(sed -n '/void Context::resume_after_drain()/,/^namespace {/p' "$context")
watchdog_block=$(sed -n '/void Context::stateful_watchdog_loop()/,/^bool Context::bind_surface/p' "$context")
input_block=$(sed -n '/bool Context::stateful_input_consumed()/,/^void Context::note_stateful_submission/p' "$context")
reset_block=$(sed -n '/bool Context::reset_stateful_decoder()/,/^bool Context::drain_stateful_decoder()/p' "$context")

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
if ! printf '%s\n' "$drain_block" | grep -q 'const auto output_deadline'; then
    echo "FAIL stateful drain does not wait for trailing OUTPUT DQBUF" >&2
    exit 1
fi
if ! printf '%s\n' "$drain_block" | grep -q 'mark_source_buffer_dequeued'; then
    echo "FAIL stateful drain does not retire trailing OUTPUT buffers" >&2
    exit 1
fi
if ! printf '%s\n' "$drain_block" | grep -q 'if (!saw_last)'; then
    echo "FAIL stateful drain can resume without terminal LAST" >&2
    exit 1
fi
if ! printf '%s\n' "$reset_block" | grep -q 'if (!saw_last)'; then
    echo "FAIL timeout reset can restart without terminal LAST" >&2
    exit 1
fi
if ! printf '%s\n' "$resume_block" | grep -q '!stateful_last_marker_seen'; then
    echo "FAIL resume path has no terminal LAST guard" >&2
    exit 1
fi
if ! printf '%s\n' "$watchdog_block" | grep -q 'if (drained)'; then
    echo "FAIL watchdog resumes without checking drain result" >&2
    exit 1
fi
if ! printf '%s\n' "$input_block" | grep -q 'stateful_submitted_count < 8' \
    || ! printf '%s\n' "$input_block" | grep -q 'stateful_completed_count == 0'; then
    echo "FAIL idle drain has no cold-start history guard" >&2
    exit 1
fi
if ! grep -q 'V4L2_VA_STATEFUL_EOS_DRAIN' "$context" \
    || ! grep -q 'V4L2_VA_EOS_DRAIN' "$context"; then
    echo "FAIL generic EOS drain environment compatibility is missing" >&2
    exit 1
fi
if ! printf '%s\n' "$drain_block" | grep -q 'if (!saw_last)'; then
    echo "FAIL stateful drain can resume without terminal LAST" >&2
    exit 1
fi
if ! printf '%s\n' "$reset_block" | grep -q 'if (!saw_last)'; then
    echo "FAIL timeout reset can restart without terminal LAST" >&2
    exit 1
fi

if printf '%s\n' "$timeout_block" | grep -q 'discard_stateful_surface(surface_id)'; then
    echo "FAIL timeout path deletes the late CAPTURE timestamp mapping" >&2
    exit 1
fi

echo "PASS stateful OUTPUT order and late timestamp mapping invariants"
