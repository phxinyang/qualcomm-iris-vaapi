#!/bin/sh

# Exercise setup.sh's remote lab-root validation without contacting a host.

set -eu

root=${1:-$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)}
setup="$root/test/remote/setup.sh"
scratch_base=${IRIS_TEST_SCRATCH_ROOT:-$HOME/Lab/Bridge/tmp/trash}
mkdir -p "$scratch_base"
scratch=$(mktemp -d "$scratch_base/iris-setup-path-check.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

calls="$scratch/calls"
mkdir -p "$scratch/bin"
: >"$calls"
sed "s|@CALLS@|$calls|g" >"$scratch/bin/ssh" <<'SHIM'
#!/bin/sh
printf '%s\n' ssh >>'@CALLS@'
exit 73
SHIM
chmod +x "$scratch/bin/ssh"

assert_rejected() {
    label=$1
    value=$2
    : >"$calls"
    if PATH="$scratch/bin:$PATH" IRIS_REMOTE_HOST=shim IRIS_LAB_ROOT="$value" \
        sh "$setup" >"$scratch/out" 2>"$scratch/err"; then
        echo "FAIL unsafe lab root accepted: $label" >&2
        exit 1
    fi
    if [ -s "$calls" ]; then
        echo "FAIL unsafe lab root reached ssh: $label" >&2
        exit 1
    fi
    grep -Fq 'FAIL unsafe IRIS_LAB_ROOT' "$scratch/err" \
        || { echo "FAIL unsafe lab root had the wrong error: $label" >&2; exit 1; }
}

assert_rejected absolute '/home/user/Lab/iris'
assert_rejected parent '../Lab/iris'
assert_rejected traversal 'Lab/../iris'
assert_rejected whitespace 'Lab/iris bad'
assert_rejected shell 'Lab/iris;touch-pwned'
assert_rejected outside 'Projects/iris'

: >"$calls"
if PATH="$scratch/bin:$PATH" IRIS_REMOTE_HOST=shim \
    IRIS_LAB_ROOT='Lab/iris-vaapi-lab' sh "$setup" \
    >"$scratch/out" 2>"$scratch/err"; then
    echo 'FAIL command shim unexpectedly let setup complete' >&2
    exit 1
fi
grep -Fxq ssh "$calls" || {
    echo 'FAIL safe lab root did not reach ssh after validation' >&2
    exit 1
}

echo 'PASS setup lab-root validation rejects unsafe paths before ssh'
