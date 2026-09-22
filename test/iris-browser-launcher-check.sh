#!/bin/sh

# Exercise the launcher with fake browsers so compatibility flags are tested
# without requiring a desktop session or a real /dev/video node.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
launcher="$root/scripts/iris-vaapi-browser"
scratch_base=${IRIS_TEST_SCRATCH_ROOT:-${TMPDIR:-/tmp}}
mkdir -p "$scratch_base"
scratch=$(mktemp -d "$scratch_base/iris-browser-launcher-check.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

make_browser() {
    path=$1
    version=$2
    cat >"$path" <<EOF
#!/bin/sh
if [ "\${1:-}" = --version ]; then
    printf '%s\\n' '$version'
    exit 0
fi
printf '%s\\n' "\$@" > "\${BROWSER_ARGS_FILE:?}"
EOF
    chmod +x "$path"
}

run_case() {
    name=$1
    family=$2
    browser=$3
    args_file="$scratch/$name.args"
    : >"$args_file"
    env \
        IRIS_BROWSER_BIN="$browser" \
        IRIS_BROWSER_PROFILE_ROOT="$scratch/$name-profile" \
        BROWSER_ARGS_FILE="$args_file" \
        LIBVA_V4L2_VIDEO_PATH=/dev/null \
        "$launcher" --browser="$family" -- --test-browser-arg 2>"$scratch/$name.stderr"
}

chromium="$scratch/chromium"
chrome="$scratch/chrome"
make_browser "$chromium" 'Chromium 152.0.7977.75 Built from source for Fedora release 44 (Forty Four)'
make_browser "$chrome" 'Google Chrome 152.0.7977.82'

run_case chromium-152 chromium "$chromium"
grep -Fqx -- '--enable-features=AcceleratedVideoDecoder' "$scratch/chromium-152.args"
grep -Fqx -- '--disable-features=AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL' \
    "$scratch/chromium-152.args"
grep -Fq -- 'using software fallback' "$scratch/chromium-152.stderr"

run_case chrome-152 chrome "$chrome"
if grep -Fq -- 'AcceleratedVideoDecodeLinux' "$scratch/chrome-152.args"; then
    echo 'FAIL Chrome VA-API launcher inherited Chromium compatibility flags' >&2
    exit 1
fi

# The Fedora protection must not turn the separately built Chromium 154 VA
# path into FFmpegVideoDecoder. Pair this absence assertion with the positive
# Fedora case above, so the guard cannot silently become a no-op.
make_browser "$chromium" 'Chromium 154.0.8015.0'
run_case chromium-154 chromium "$chromium"
if grep -Fq -- 'AcceleratedVideoDecode' "$scratch/chromium-154.args"; then
    echo 'FAIL dedicated Chromium 154 inherited Fedora compatibility flags' >&2
    exit 1
fi
grep -Fqx -- '--ozone-platform=wayland' "$scratch/chromium-154.args"
grep -Fqx -- '--test-browser-arg' "$scratch/chromium-154.args"

make_browser "$chromium" 'Chromium 151.0.7922.173 Built from source for Fedora release 44 (Forty Four)'
run_case chromium-151 chromium "$chromium"
if grep -Fq -- 'AcceleratedVideoDecode' "$scratch/chromium-151.args"; then
    echo 'FAIL previously qualified Chromium 151 inherited newer compatibility flags' >&2
    exit 1
fi

echo 'PASS Fedora Chromium protection and Chrome/dedicated Chromium VA defaults'
