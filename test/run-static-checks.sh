#!/bin/sh

# Everything that can be verified without a V4L2 device or a running browser.
# This is the entry point used by CI and the one to run before every commit.
#
# Hardware-dependent suites (iris-matrix.sh, iris-structure-matrix.sh,
# iris-hardware-suite.sh) and the runtime log analysers (iris-eos-check.sh,
# iris-ending-check.sh, iris-context-retirement-check.sh) are deliberately not
# invoked here: the first group needs the decoder node, the second needs a
# trace captured from a real playback.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
test_dir="$root/test"
failures=0

run() {
    name=$1
    shift
    if "$@"; then
        return 0
    fi
    echo "FAIL $name" >&2
    failures=$((failures + 1))
}

# A shell script that does not parse is a check that silently never ran. One
# of the trace checkers shipped broken for a while because a typo made it grep
# a path instead of a file, so parse every script we own on every run.
echo "== shell syntax"
for script in "$test_dir"/*.sh "$test_dir"/remote/*.sh "$root"/scripts/*.sh; do
    [ -f "$script" ] || continue
    run "syntax $(basename "$script")" sh -n "$script"
done

echo "== browser launcher invariants"
launcher="$root/scripts/iris-vaapi-browser"
run browser-launcher-syntax sh -n "$launcher"
run browser-launcher-unsets-build-path grep -Fq 'unset LIBVA_DRIVERS_PATH' "$launcher"
run browser-launcher-dynamic-node grep -Fq 'iris_driver' "$launcher"
run browser-launcher-no-fixed-node sh -c '! grep -Eq "/dev/video[0-9]+" "$1"' sh "$launcher"

# Keep the VA-only Chrome path free of video-decoder feature overrides.
#
# The previous guard only read the block between `common_args=` and the
# Vulkan branch, which never contained the exec it was meant to protect. The
# VA-only exec below that branch carried
# --disable-features=AcceleratedVideoDecodeLinuxZeroCopyGL, and on Chrome
# 151.0.7922.173 that switch makes the pipeline build VaapiVideoDecoder, hit
# the ImageProcessor output path, tear the decoder down and fall back to
# FFmpegVideoDecoder. Every installed desktop entry shipped software decode
# while the check reported a pass, so both sections are inspected now.
# The guard reads code, not prose: the launcher documents the feature names it
# must never pass, so comment lines are stripped before matching. The default
# exec is everything after the LAST top-level `fi`, which is the only way to
# skip both the --print-node early exit and the Vulkan/WebGPU branch; anchoring
# on the first `fi` pulled the experiment's flags in and made the guard
# unfalsifiable.
common_section=$(sed -n '/common_args=/,/if \[ "\$graphics_mode" = vulkan-webgpu \]/p' "$launcher" \
    | grep -v '^[[:space:]]*#')
default_exec=$(awk '/^fi$/ { buf = ""; next } { buf = buf $0 "\n" } END { printf "%s", buf }' \
    "$launcher" | grep -v '^[[:space:]]*#')
run browser-launcher-common-no-video-feature-override sh -c \
    '! printf "%s\n" "$1" | grep -Fq AcceleratedVideoDecode' sh "$common_section"
run browser-launcher-default-no-video-feature-override sh -c \
    '! printf "%s\n" "$1" | grep -Fq AcceleratedVideoDecode' sh "$default_exec"
run browser-launcher-default-execs-browser sh -c \
    'printf "%s\n" "$1" | grep -Fq "exec \"\$browser\""' sh "$default_exec"

echo "== source invariants"
run iris-source-invariants sh "$test_dir/iris-source-invariants.sh" "$root"
run iris-stateful-order-check sh "$test_dir/iris-stateful-order-check.sh" "$root"
run iris-env-doc-check sh "$test_dir/iris-env-doc-check.sh" "$root"
run iris-surface-limits-check sh "$test_dir/iris-surface-limits-check.sh" "$root"
run iris-codec-matrix-check sh "$test_dir/iris-codec-matrix-check.sh" "$root"
run iris-experiment-guard-self-test sh "$test_dir/iris-experiment-guard-self-test.sh" "$root"
run iris-eos-check-self-test sh "$test_dir/iris-eos-check-self-test.sh" "$root"
run iris-setup-path-check sh "$test_dir/iris-setup-path-check.sh" "$root"
run provenance-python-syntax python3 -c \
    'import ast, pathlib, sys; ast.parse(pathlib.Path(sys.argv[1]).read_text())' \
    "$test_dir/remote/provenance.py"
run provenance-self-test sh "$test_dir/provenance-self-test.sh" "$root"
run electron-video-report-check sh "$test_dir/electron-video-report-check.sh" "$root"

if [ "$failures" -ne 0 ]; then
    printf 'FAIL static checks: %d failing\n' "$failures" >&2
    exit 1
fi
echo "PASS static checks"
