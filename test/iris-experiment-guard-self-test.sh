#!/bin/sh

set -eu
root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
scratch_base=${IRIS_TEST_SCRATCH_ROOT:-${TMPDIR:-/tmp}}
mkdir -p "$scratch_base"
work=$(mktemp -d "$scratch_base/iris-experiment-guard.XXXXXX")
trap 'rm -rf "$work"' 0 1 2 15
mkdir -p "$work/thermal/thermal_zone0"
printf 'test\n' >"$work/thermal/thermal_zone0/type"
printf '42000\n' >"$work/thermal/thermal_zone0/temp"

IRIS_EXPERIMENT_DIR="$work/artifacts" \
IRIS_EXPERIMENT_THERMAL_ROOT="$work/thermal" \
IRIS_EXPERIMENT_TIMEOUT_SECONDS=10 \
IRIS_EXPERIMENT_TEMPERATURE_INTERVAL_SECONDS=1 \
sh "$root/scripts/iris-experiment-guard.sh" -- sh -c 'exit 0' >/dev/null
grep -Fxq 'status=complete' "$work/artifacts/guard.state"
grep -q '^boot_id=' "$work/artifacts/guard.state"
test -s "$work/artifacts/temperature-initial.csv"
echo 'PASS experiment guard completion and evidence recording'
