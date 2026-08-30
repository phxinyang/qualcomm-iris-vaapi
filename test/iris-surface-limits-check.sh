#!/bin/sh

# Surface dimensions come from the selected V4L2 decoder, not from a
# codec-specific constant that can reject larger hardware capabilities.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
v4l2="$root/src/v4l2.cc"
v4l2_header="$root/src/v4l2.h"
surface="$root/src/surface.cc"
picture="$root/src/picture.cc"
context="$root/src/context.cc"

if [ ! -r "$v4l2" ] || [ ! -r "$v4l2_header" ] || [ ! -r "$surface" ] \
    || [ ! -r "$picture" ] || [ ! -r "$context" ]; then
    echo "FAIL surface limit sources are not readable" >&2
    exit 2
fi

if ! grep -q 'VIDIOC_ENUM_FRAMESIZES' "$v4l2" \
    || ! grep -q 'frame_size_limits' "$v4l2_header"; then
    echo "FAIL V4L2 frame-size enumeration is missing" >&2
    exit 1
fi

query_block=$(sed -n '/VAStatus querySurfaceAttributes(/,/^}/p' "$surface")
if ! printf '%s\n' "$query_block" | grep -q 'surface_size_limits'; then
    echo "FAIL VA surface attributes do not use device limits" >&2
    exit 1
fi
if printf '%s\n' "$query_block" | grep -Eq 'value\.value\.i = (32|2048);'; then
    echo "FAIL VA surface dimensions contain a hard-coded limit" >&2
    exit 1
fi

if ! grep -q 'Context::supported_profiles(device)' "$surface"; then
    echo "FAIL surface limits are not scoped to the config profile" >&2
    exit 1
fi
begin_block=$(sed -n '/VAStatus beginPicture(/,/^}/p' "$picture")
bind_block=$(sed -n '/bool Context::bind_surface(/,/^}/p' "$context")
export_block=$(sed -n '/VAStatus exportSurfaceHandle(/,/^}/p' "$surface")
if ! printf '%s\n' "$begin_block" | grep -q 'reconfigure_stateful_dimensions'; then
    echo "FAIL beginPicture does not own stateful dynamic-resolution reconfiguration" >&2
    exit 1
fi
if printf '%s\n' "$bind_block" | grep -q 'reconfigure_stateful_dimensions'; then
    echo "FAIL bind_surface still hides a decoder reconfiguration" >&2
    exit 1
fi
if printf '%s\n' "$export_block" | grep -Eq 'bind_surface|reconfigure_stateful_dimensions'; then
    echo "FAIL exportSurfaceHandle mutates decoder binding or geometry" >&2
    exit 1
fi

echo "PASS surface size limits"
