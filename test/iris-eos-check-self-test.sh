#!/bin/sh

# A single FFmpeg process may replace its VA context at a coded-size change.
# Each context must still have an independent strict STOP/LAST drain contract.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
scratch_base=${IRIS_TEST_SCRATCH_ROOT:-$HOME/Lab/Bridge/tmp/trash}
mkdir -p "$scratch_base"
scratch=$(mktemp -d "$scratch_base/iris-eos-check-self-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
log="$scratch/multi-context.trace"

cat >"$log" <<'EOF'
va create_context done id=0 ptr=0x1
stateful flush batch=0 surfaces=2
v4l2 dq type=10 index=0 flags=0x0 last=0 ts=1.0 seq=0
v4l2 dq type=9 index=0 flags=0x0 last=0 ts=1.0 seq=0
v4l2 dq type=9 index=1 flags=0x0 last=0 ts=2.0 seq=1
v4l2 decoder STOP
v4l2 dq type=9 index=0 flags=0x0 last=1 ts=0.0 seq=2
stateful drain complete last=1 output=0 batches=0
va create_context done id=0 ptr=0x2
stateful flush batch=0 surfaces=2
v4l2 dq type=10 index=0 flags=0x0 last=0 ts=3.0 seq=0
v4l2 dq type=9 index=0 flags=0x0 last=0 ts=3.0 seq=0
v4l2 dq type=9 index=1 flags=0x0 last=0 ts=4.0 seq=1
v4l2 decoder STOP
v4l2 dq type=9 index=0 flags=0x0 last=1 ts=0.0 seq=2
stateful drain complete last=1 output=0 batches=0
EOF

sh "$root/test/iris-eos-check.sh" "$log" 4
echo 'PASS multi-context EOS checker'
