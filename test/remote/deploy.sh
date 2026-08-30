#!/bin/sh

# Sync this checkout to the V4L2 target, rebuild the driver from scratch, and
# refuse to hand back a library that cannot resolve its own symbols.
#
# This exists because an earlier debugging session lost most of a day to a
# stale incremental build: rsync preserved the original mtimes, Ninja decided
# the changed translation units were already up to date, and the resulting
# .so linked with undefined symbols. Every runtime symptom observed after that
# point was an artifact of the broken library, not of the driver logic. The
# clean rebuild plus the `ldd -r` gate below make that failure mode impossible
# to reach silently.
#
# Environment:
#   IRIS_REMOTE_HOST   ssh destination                (default xinyang@192.168.3.133)
#   IRIS_REMOTE_ROOT   remote checkout directory      (default ~/Lab/iris-vaapi-lab/src)
#   IRIS_DEPLOY_INCREMENTAL=1  skip the clean step (still refreshes mtimes)

set -eu

host=${IRIS_REMOTE_HOST:-xinyang@192.168.3.133}
remote_root=${IRIS_REMOTE_ROOT:-Lab/iris-vaapi-lab/src}
local_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if ! command -v rsync >/dev/null 2>&1; then
    echo "FAIL rsync is required to deploy" >&2
    exit 1
fi

echo "== sync $local_root -> $host:$remote_root"
ssh "$host" "mkdir -p '$remote_root'"

# --checksum instead of the default size/mtime heuristic: the whole point of
# this script is that mtimes lie after a sync.
rsync -rlv --checksum --delete \
    --exclude '.git' \
    --exclude 'build*' \
    --exclude 'output' \
    --exclude 'chromium' \
    --exclude '.playwright-cli' \
    --exclude '*.mp4' \
    --exclude '*.ivf' \
    --exclude '.DS_Store' \
    "$local_root/" "$host:$remote_root/" | tail -20

echo "== rebuild"
ssh "$host" "sh -s" <<REMOTE
set -eu
cd "\$HOME/$remote_root"

# Defeat any mtime that survived the sync. Ninja compares mtimes, and a
# restored-from-backup or checksum-synced tree can carry timestamps older than
# the objects built from a previous revision.
find src include -type f \( -name '*.cc' -o -name '*.h' \) -exec touch {} +

if [ ! -f build/build.ninja ]; then
    meson setup build
else
    meson setup --reconfigure build >/dev/null
fi

if [ -z "\${IRIS_DEPLOY_INCREMENTAL:-}" ]; then
    ninja -C build -t clean >/dev/null
fi
ninja -C build

so=build/src/v4l2_drv_video.so
if [ ! -f "\$so" ]; then
    echo "FAIL driver was not produced at \$so" >&2
    exit 1
fi

# ldd -r resolves data and function relocations. Anything reported here means
# the link succeeded but the library would fail at dlopen time inside libva,
# which surfaces as a silent fallback to software decoding.
undefined=\$(ldd -r "\$so" 2>&1 | grep -i 'undefined symbol' || true)
if [ -n "\$undefined" ]; then
    echo "FAIL driver has undefined symbols:" >&2
    printf '%s\n' "\$undefined" >&2
    exit 1
fi

printf 'PASS driver built  sha256=%s  path=%s/%s\n' \
    "\$(sha256sum "\$so" | cut -d' ' -f1)" "\$PWD" "\$so"
REMOTE
