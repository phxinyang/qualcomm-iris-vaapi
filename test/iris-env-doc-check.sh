#!/bin/sh

# Every runtime switch the driver reads must be documented, and every switch
# the documentation promises must exist.
#
# The switches accumulated across several debugging rounds and four of them
# were readable but undocumented, which is how a diagnostic-only flag ends up
# being treated as a supported knob. This check makes the README table and the
# source agree in both directions.

set -eu

root=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
readme="$root/README.md"

# Names read by the implementation, through either std::getenv or the
# getenv_opt wrapper in utils.h.
in_source=$(grep -rhoE '(std::)?getenv(_opt)?\("[A-Z0-9_]*"\)' "$root/src" \
    | sed -E 's/.*\("//; s/"\)//' \
    | grep -E '^(V4L2_VA_|LIBVA_V4L2_)' \
    | sort -u)

# Names listed in the README's environment table. Table rows start with a
# backticked identifier in the first column.
in_readme=$(sed -n 's/^| *`\([A-Z0-9_]*\)` *|.*/\1/p' "$readme" \
    | grep -E '^(V4L2_VA_|LIBVA_V4L2_)' \
    | sort -u)

undocumented=$(printf '%s\n' "$in_source" | grep -vxF "$in_readme" || true)
stale=$(printf '%s\n' "$in_readme" | grep -vxF "$in_source" || true)

status=0
if [ -n "$undocumented" ]; then
    echo "FAIL environment variables read by src/ but absent from the README table:" >&2
    printf '  %s\n' $undocumented >&2
    status=1
fi
if [ -n "$stale" ]; then
    echo "FAIL environment variables documented but no longer read by src/:" >&2
    printf '  %s\n' $stale >&2
    status=1
fi
[ "$status" -eq 0 ] || exit 1

# The sync timeout is a behavioural contract, not just a variable name. Keep
# the documented default/range/cold-start allowance tied to the implementation
# so changing either side alone fails CI.
require_literal() {
    file=$1
    literal=$2
    description=$3
    if ! grep -Fq "$literal" "$file"; then
        echo "FAIL $description: expected '$literal' in ${file#"$root"/}" >&2
        status=1
    fi
}

surface="$root/src/surface.cc"
require_literal "$surface" 'constexpr int default_timeout_ms = 2000;' 'sync timeout implementation default'
require_literal "$surface" 'std::clamp(parsed, 50L, 60000L)' 'sync timeout implementation range'
require_literal "$surface" 'if (cold_start && !stateful_sync_timeout_overridden())' 'cold-start override guard'
require_literal "$surface" 'sync_timeout = std::max(sync_timeout, 30000);' 'cold-start timeout allowance'
require_literal "$readme" '`vaSyncSurface()` therefore uses a bounded 2000 ms wait by default' 'sync timeout prose default'
require_literal "$readme" '`V4L2_VA_SYNC_TIMEOUT_MS=50..60000`' 'sync timeout prose range'
require_literal "$readme" 'first frame of a cold stateful sequence gets a 30000 ms' 'cold-start prose allowance'
require_literal "$readme" '| `V4L2_VA_SYNC_TIMEOUT_MS` | `2000` | Bounded `vaSyncSurface()` wait, `50..60000`; without an override, the first cold-start frame may wait up to `30000` ms. |' 'sync timeout environment table'

[ "$status" -eq 0 ] || exit 1

printf 'PASS environment documentation covers %d switches\n' "$(printf '%s\n' "$in_source" | wc -l)"
