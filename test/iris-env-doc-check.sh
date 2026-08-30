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

printf 'PASS environment documentation covers %d switches\n' "$(printf '%s\n' "$in_source" | wc -l)"
