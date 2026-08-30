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

# Keep the known-good Chrome path free of the Linux GL decode feature. That
# feature selects ImageProcessor output negotiation on Chrome 151 and causes
# an immediate VA context teardown before the first picture. It remains valid
# as a separately documented experiment only when explicitly requested.
default_launcher=$(sed -n '/common_args=/,/if \[ "\$graphics_mode" = vulkan-webgpu \]/p' "$launcher")
run browser-launcher-default-no-linux-gl-feature sh -c '! printf "%s\\n" "$1" | grep -Fq AcceleratedVideoDecodeLinuxGL' sh "$default_launcher"

echo "== source invariants"
run iris-source-invariants sh "$test_dir/iris-source-invariants.sh" "$root"
run iris-stateful-order-check sh "$test_dir/iris-stateful-order-check.sh" "$root"
run iris-env-doc-check sh "$test_dir/iris-env-doc-check.sh" "$root"
run iris-surface-limits-check sh "$test_dir/iris-surface-limits-check.sh" "$root"
run iris-eos-check-self-test sh "$test_dir/iris-eos-check-self-test.sh" "$root"
run provenance-python-syntax python3 -c \
    'import ast, pathlib, sys; ast.parse(pathlib.Path(sys.argv[1]).read_text())' \
    "$test_dir/remote/provenance.py"
run provenance-self-test sh "$test_dir/provenance-self-test.sh" "$root"

if [ "$failures" -ne 0 ]; then
    printf 'FAIL static checks: %d failing\n' "$failures" >&2
    exit 1
fi
echo "PASS static checks"
