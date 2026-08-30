#!/bin/sh

# Install the built VA driver into libva's configured system driver directory,
# then install the browser wrapper and desktop entries under /usr/local by
# default. Run as root, or set DESTDIR for an unprivileged packaging test.

set -eu

usage() {
    cat <<'EOF'
Usage: install-system.sh [BUILD_DIR]

Environment:
  IRIS_INSTALL_PREFIX  Launcher/data prefix (default: /usr/local)
  IRIS_VA_DRIVER_DIR   Explicit libva driver directory override when
                       libva.pc is unavailable (for packaging/staging)
  DESTDIR              Optional package staging root

The driver directory is read from libva.pc. This script never configures a
global LIBVA_DRIVERS_PATH and never writes a fixed /dev/video number.
EOF
}

fail() {
    echo "install-system.sh: $*" >&2
    exit 1
}

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
esac
[ "$#" -le 1 ] || { usage >&2; exit 2; }

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${1:-$root/build}
case "$build_dir" in
    /*) ;;
    *) build_dir=$PWD/$build_dir ;;
esac
prefix=${IRIS_INSTALL_PREFIX:-/usr/local}
destdir=${DESTDIR:-}

case "$prefix" in /*) ;; *) fail 'IRIS_INSTALL_PREFIX must be absolute' ;; esac
case "$destdir" in ''|/*) ;; *) fail 'DESTDIR must be empty or absolute' ;; esac
case "$destdir" in *'/../'*|*'/..'|*'/./'*|*'/.'|*'//') fail 'DESTDIR must be normalized' ;; esac

if [ -z "$destdir" ] && [ "$(id -u)" -ne 0 ]; then
    fail 'run as root for a live system install, or set DESTDIR for staging'
fi

for command in pkg-config install ldd; do
    command -v "$command" >/dev/null 2>&1 || fail "$command is required"
done

driver_dir=${IRIS_VA_DRIVER_DIR:-}
if [ -z "$driver_dir" ]; then
    driver_dir=$(pkg-config --variable=driverdir libva 2>/dev/null || true)
    if [ -z "$driver_dir" ]; then
        libdir=$(pkg-config --variable=libdir libva 2>/dev/null || true)
        [ -n "$libdir" ] || fail 'libva.pc does not provide driverdir or libdir; set IRIS_VA_DRIVER_DIR for staging'
        driver_dir=$libdir/dri
    fi
fi
case "$driver_dir" in /*) ;; *) fail "libva driverdir is not absolute: $driver_dir" ;; esac

artifact=$build_dir/src/v4l2_drv_video.so
[ -f "$artifact" ] || fail "driver artifact not found: $artifact"
undefined=$(ldd -r "$artifact" 2>&1 | grep -i 'undefined symbol' || true)
[ -z "$undefined" ] || fail "driver has unresolved symbols:\n$undefined"

install -D -m 0755 "$artifact" "$destdir$driver_dir/v4l2_drv_video.so"
install -D -m 0755 "$root/scripts/iris-vaapi-browser" \
    "$destdir$prefix/bin/iris-vaapi-browser"
for desktop in "$root"/data/*.desktop; do
    install -D -m 0644 "$desktop" \
        "$destdir$prefix/share/applications/${desktop##*/}"
done

if [ -z "$destdir" ] && command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$prefix/share/applications"
fi

printf 'PASS installed VA driver: %s\n' "$destdir$driver_dir/v4l2_drv_video.so"
printf 'PASS installed browser launcher: %s\n' "$destdir$prefix/bin/iris-vaapi-browser"
printf 'PASS installed desktop entries: %s\n' "$destdir$prefix/share/applications"
