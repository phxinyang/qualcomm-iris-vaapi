#!/bin/sh

# Hardware-independent guards for capability advertisement.  The runtime
# profile set must be the intersection of compressed formats, V4L2 profile
# menus and surface formats the VA backend can actually publish.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
h264="$root/src/h264.cc"
vp9="$root/src/vp9_stateful.cc"
vp9_request="$root/src/vp9.cc"
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

printf '%s\n' "$vp9_body" | grep -Fq '(void)device;' \
    || fail 'stateful VP9 capability withdrawal is not explicit'
if printf '%s\n' "$vp9_body" | grep -Eq 'VAProfileVP9Profile[0-3]'; then
    fail 'stateful VP9 must remain withdrawn until session/source-change reboot is fixed'
fi

if [ -f "$vp9_request" ]; then
    vp9_request_format=$(sed -n '/fourcc vp9_output_format/,/^}/p' "$vp9_request")
    vp9_request_profiles=$(sed -n '/VP9Context::supported_profiles/,/^}/p' "$vp9_request")
    if printf '%s\n%s\n' "$vp9_request_format" "$vp9_request_profiles" \
        | grep -Eq 'V4L2_PIX_FMT_VP9([^_A-Z0-9]|$)'; then
        fail 'request-api VP9 path still admits the unsafe stateful VP90 format'
    fi
    printf '%s\n' "$vp9_request_profiles" | grep -Fq 'V4L2_PIX_FMT_VP9_FRAME' \
        || fail 'request-api VP9 advertisement no longer requires VP9_FRAME'
fi

echo 'PASS codec profiles are bounded and unsafe stateful VP9 is withdrawn'
