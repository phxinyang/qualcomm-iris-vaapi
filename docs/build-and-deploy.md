# Build And Deploy

The target-independent build is reproducible on the development host:

```sh
CXX=/usr/bin/g++ CC=/usr/bin/gcc PKG_CONFIG=/usr/bin/pkg-config \
PKG_CONFIG_PATH=/usr/lib64/pkgconfig meson setup build-local
ninja -C build-local
ldd -r build-local/src/v4l2_drv_video.so
```

The last command must print no `undefined symbol` lines. A successful link is
not enough for a VA-API driver: an unresolved relocation otherwise appears to
clients as a silent software-decoder fallback.

For a tablet or other V4L2 target, set `IRIS_REMOTE_HOST` and run:

```sh
IRIS_REMOTE_HOST=xinyang@192.168.3.142 ./test/remote/deploy.sh
```

The deploy script synchronizes by content, excludes generated build and test
artifacts, refreshes source timestamps, performs a clean Ninja rebuild, and
gates the resulting shared object with `ldd -r`. It rejects absolute,
dot-relative, parent-relative, whitespace-containing and shell-active
`IRIS_REMOTE_ROOT` values before the `rsync --delete` command can contact the
target. The remote lab directory is durable and defaults to
`~/Lab/iris-vaapi-lab`; downloaded Fluster resources and Qualcomm test tools
stay there rather than under a disposable scratch directory.

Every successful deployment also writes
`build/iris-vaapi-provenance.json`. Because `.git` is deliberately not copied
to the target, `deploy.sh` resolves the full source commit and a SHA-256 over
the current contents of all tracked paths locally, then explicitly injects
both values into the clean remote build. The manifest additionally records
whether the source checkout was dirty, Meson's compiler inventory, the libva
and Meson versions, the target kernel, and the exact driver path, size and
SHA-256. Deployment immediately verifies the manifest against the generated
shared object; a copied, rebuilt or modified `.so` cannot retain a passing
record accidentally.

To re-check a deployed artifact later:

```sh
cd ~/Lab/iris-vaapi-lab/src
python3 test/remote/provenance.py verify \
  --manifest build/iris-vaapi-provenance.json \
  --artifact build/src/v4l2_drv_video.so
```

For a source export that has no local `.git`, deployment requires the source
identity to be explicit:

```sh
IRIS_SOURCE_COMMIT=<full-object-id> \
IRIS_TRACKED_SOURCE_SHA256=<sha256-of-tracked-paths> \
./test/remote/deploy.sh
```

The decoder node is resolved by the `iris_driver` name in
`test/remote/setup.sh`. Do not hardcode `/dev/video17`: V4L2 numbering changes
across boots and that node may belong to the camera subsystem. Override the
resolved node for a run with `LIBVA_V4L2_VIDEO_PATH`.

After deployment, run the short positive oracle before external corpora:

```sh
ssh "$IRIS_REMOTE_HOST" 'cd ~/Lab/iris-vaapi-lab/src && \
  . ./test/lib/iris-env.sh && device=$(iris_resolve_device) && \
  LIBVA_DRIVER_NAME=v4l2 \
  LIBVA_DRIVERS_PATH=$PWD/build/src \
  LIBVA_V4L2_VIDEO_PATH="$device" \
  ./test/iris-matrix.sh'
```

The matrix requires exact software/VA frame MD5 equality and strict stateful
EOS accounting. Fluster and native V4L2 suites are additional capability and
firmware diagnostics; always record which decoder path was used.

## Dynamic resolution qualification

Run the dedicated H.264 and VP9 reconfiguration gate on the target:

```sh
. ./test/lib/iris-env.sh
device=$(iris_resolve_device)
LIBVA_DRIVER_NAME=v4l2 \
LIBVA_DRIVERS_PATH=$PWD/build/src \
LIBVA_V4L2_VIDEO_PATH="$device" \
./test/iris-dynamic-resolution.sh
```

The production default assembles deterministic 640x360 and 1280x720 keyframe
segments into streams with exactly 100 geometry changes. A codec passes only
if the native GStreamer V4L2 decoder produces byte-identical I420 output, one
FFmpeg VA process survives every change with exact software frame MD5 and EOS
accounting, and 101 fresh VA processes decode the alternating geometries with
the same checks. FFmpeg is allowed to recreate VA contexts on a coded-size
change; the single-process lane therefore qualifies context retirement and
session cleanup rather than asserting a client behavior FFmpeg does not have.
A missing device format, native element or VA capability is a failure, not a
skip. Development runs may shorten the matrix explicitly with
`IRIS_DYNAMIC_SWITCHES` and `IRIS_DYNAMIC_FRAMES_PER_SEGMENT`; qualification
results must retain the defaults.

## Concurrent soak qualification

The concurrent runner has separate dual-H.264 and mixed
H.264+VP9+HEVC scenarios:

```sh
./test/iris-concurrency-soak.sh dual-h264
./test/iris-concurrency-soak.sh mixed
# Run both sequentially:
./test/iris-concurrency-soak.sh all
```

Each selected scenario defaults to 7200 seconds of wall-clock playback, using
real-time throttling rather than decoding two hours of timestamps as quickly
as possible. Before the soak starts, every selected codec must pass an exact
software/VA frame-MD5 oracle. During the run the script records progress,
driver traces, boot ID, uptime and every readable thermal zone. It terminates
workers and fails on a boot-ID change, an unreadable thermal inventory, a
temperature above 85000 millidegrees Celsius, a driver timeout/timestamp
error, incomplete stateful teardown, too few decoded frames, or any worker
failure.

`IRIS_SOAK_SECONDS` may shorten a development run, but the report is labelled
`SHORTENED` unless it is exactly 7200 seconds. `IRIS_SOAK_MAX_TEMP_MILLIC` and
`IRIS_SOAK_TEMPERATURE_INTERVAL_SECONDS` tune the thermal guard. A durable
guard file records a running test; a later invocation refuses to hide an
interrupted or rebooted soak until the operator inspects it and deliberately
sets `IRIS_SOAK_ALLOW_STALE_GUARD=1`.

## Host-side release gate

The CI and local release sequence is:

```sh
meson setup build
ninja -C build
sh test/run-static-checks.sh
meson test -C build --list
meson test -C build --no-rebuild --print-errorlogs
ldd -r build/src/v4l2_drv_video.so
```

CI asserts that the Meson inventory is non-empty and names every required
hardware-independent test, including `stateful-session` and the provenance
tamper test, before executing it. This prevents a renamed, unregistered or
unbuilt behavioural test from yielding an empty green result.
