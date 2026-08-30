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
for script in "$test_dir"/*.sh "$test_dir"/remote/*.sh; do
    [ -f "$script" ] || continue
    run "syntax $(basename "$script")" sh -n "$script"
done

echo "== source invariants"
run iris-source-invariants sh "$test_dir/iris-source-invariants.sh" "$root"
run iris-stateful-order-check sh "$test_dir/iris-stateful-order-check.sh" "$root"

if [ "$failures" -ne 0 ]; then
    printf 'FAIL static checks: %d failing\n' "$failures" >&2
    exit 1
fi
echo "PASS static checks"
