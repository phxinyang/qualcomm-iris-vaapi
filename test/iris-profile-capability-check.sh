#!/bin/sh

# Hardware-independent guards for capability advertisement.  The runtime
# profile set must be the intersection of compressed formats, V4L2 profile
# menus and surface formats the VA backend can actually publish.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
h264="$root/src/h264.cc"
vp9="$root/src/vp9_stateful.cc"
v4l2="$root/src/v4l2.cc"

fail() {
    echo "FAIL $*" >&2
    exit 1
}

h264_body=$(sed -n '/H264Context::supported_profiles/,/^};/p' "$h264")
vp9_body=$(sed -n '/VP9StatefulContext::supported_profiles/,/^}/p' "$vp9")
menu_body=$(sed -n '/V4L2M2MDevice::menu_control_values/,/^}/p' "$v4l2")

printf '%s\n' "$menu_body" | grep -Fq 'VIDIOC_QUERYCTRL' \
    || fail 'V4L2 menu capability probe no longer queries the control'
printf '%s\n' "$menu_body" | grep -Fq 'VIDIOC_QUERYMENU' \
    || fail 'V4L2 menu capability probe no longer enumerates valid entries'

printf '%s\n' "$h264_body" | grep -Fq 'V4L2_CID_MPEG_VIDEO_H264_PROFILE' \
    || fail 'H.264 advertisement no longer intersects the profile menu'
printf '%s\n' "$h264_body" | grep -Fq 'V4L2_PIX_FMT_NV12' \
    || fail 'H.264 advertisement no longer requires an implemented capture surface'
printf '%s\n' "$h264_body" | grep -Fq 'if (stateless && !stateful)' \
    || fail 'H.264 stateless-only menu fallback was broadened'
if printf '%s\n' "$h264_body" | grep -Eq 'VAProfileH264(MultiviewHigh|StereoHigh)'; then
    fail 'H.264 MVC profiles must not be advertised by the implemented 2D path'
fi

printf '%s\n' "$vp9_body" | grep -Fq 'V4L2_CID_MPEG_VIDEO_VP9_PROFILE' \
    || fail 'stateful VP9 advertisement no longer intersects the profile menu'
printf '%s\n' "$vp9_body" | grep -Fq 'V4L2_PIX_FMT_NV12' \
    || fail 'stateful VP9 advertisement no longer requires an implemented capture surface'
printf '%s\n' "$vp9_body" | grep -Fq 'VAProfileVP9Profile0' \
    || fail 'stateful VP9 Profile 0 is no longer mapped'
if printf '%s\n' "$vp9_body" | grep -Eq 'VAProfileVP9Profile[123]'; then
    fail 'stateful VP9 high-chroma/high-bit-depth profiles cannot use NV12 surfaces'
fi

echo 'PASS codec profiles are bounded by V4L2 menus and VA surface formats'
