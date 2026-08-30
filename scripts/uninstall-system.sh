#!/bin/sh

# Remove an installation created by install-system.sh and restore the driver
# that was present before it. Refuses to remove a driver whose hash changed.

set -eu

prefix=${IRIS_INSTALL_PREFIX:-/usr/local}
destdir=${DESTDIR:-}
manifest=${IRIS_INSTALL_MANIFEST:-$destdir$prefix/share/iris-vaapi/install-manifest.txt}
fail() { echo "uninstall-system.sh: $*" >&2; exit 1; }
[ -f "$manifest" ] || fail "install manifest not found: $manifest"
get() { awk -F= -v key="$1" '$1 == key { sub(/^[^=]*=/, ""); print; exit }' "$manifest"; }
schema=$(get schema)
[ "$schema" = qualcomm-iris-vaapi/system-install-v1 ] || fail "unsupported manifest schema: $schema"
driver=$(get driver_path)
launcher=$(get launcher_path)
desktop_dir=$(get desktop_dir)
expected=$(get artifact_sha256)
backup=$(get previous_driver_backup)
[ -n "$driver" ] || fail 'manifest has no driver path'
[ -f "$driver" ] || fail "installed driver is missing: $driver"
actual=$(sha256sum "$driver" | awk '{print $1}')
[ "$actual" = "$expected" ] || fail "refusing to remove modified driver (expected $expected, got $actual)"
rm -f "$driver" "$launcher"
for desktop in "$desktop_dir"/*.desktop; do
    [ -f "$desktop" ] || continue
    case "$desktop" in
        *chromium-iris-v4l2.desktop|*google-chrome-iris-v4l2.desktop|*chromium-iris-vulkan-webgpu.desktop|*google-chrome-iris-vulkan-webgpu.desktop) rm -f "$desktop" ;;
    esac
done
if [ -n "$backup" ] && [ -f "$backup" ]; then
    install -D -m 0755 "$backup" "$driver"
    rm -f "$backup"
fi
rm -f "$manifest"
rmdir "$desktop_dir" 2>/dev/null || true
rmdir "$(dirname "$manifest")/backup" "$(dirname "$manifest")" 2>/dev/null || true
printf 'PASS system installation removed; previous driver restored when recorded\n'
