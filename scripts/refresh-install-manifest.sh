#!/bin/sh

# Refresh the payload identity in a staged install manifest.
#
# Distribution builders may strip an ELF after install-system.sh has copied it
# into DESTDIR. The manifest must describe the bytes that will actually be
# installed, not the unstripped build-tree artifact.

set -eu

usage() {
    echo "usage: $0 MANIFEST ARTIFACT" >&2
    exit 2
}

[ "$#" -eq 2 ] || usage
manifest=$1
artifact=$2
[ -f "$manifest" ] || { echo "manifest not found: $manifest" >&2; exit 1; }
[ -f "$artifact" ] || { echo "artifact not found: $artifact" >&2; exit 1; }

command -v sha256sum >/dev/null 2>&1 || { echo 'sha256sum is required' >&2; exit 1; }
command -v awk >/dev/null 2>&1 || { echo 'awk is required' >&2; exit 1; }

schema=$(awk -F= '$1 == "schema" { print $2; exit }' "$manifest")
[ "$schema" = 'qualcomm-iris-vaapi/system-install-v1' ] \
    || { echo "unsupported manifest schema: ${schema:-missing}" >&2; exit 1; }

artifact_sha256=$(sha256sum "$artifact" | awk '{print $1}')
artifact_size=$(wc -c <"$artifact" | tr -d '[:space:]')
case "$artifact_sha256:$artifact_size" in
    *[!0-9a-fA-F:]*|:*|*:|*:0) echo 'invalid artifact identity' >&2; exit 1 ;;
esac

manifest_new=$manifest.new.$$
trap 'rm -f "$manifest_new"' 0 1 2 15
awk -F= -v hash="$artifact_sha256" -v size="$artifact_size" '
BEGIN { hash_seen = 0; size_seen = 0 }
$1 == "artifact_sha256" { print "artifact_sha256=" hash; hash_seen = 1; next }
$1 == "artifact_size" { print "artifact_size=" size; size_seen = 1; next }
{ print }
END {
    if (!hash_seen || !size_seen)
        exit 1
}
' "$manifest" >"$manifest_new" \
    || { echo "manifest lacks artifact identity fields: $manifest" >&2; exit 1; }
mv -f "$manifest_new" "$manifest"
trap - 0 1 2 15
printf 'PASS refreshed install manifest artifact_sha256=%s artifact_size=%s\n' \
    "$artifact_sha256" "$artifact_size"
