# Testing
Set up [vaapi-fits](https://github.com/intel/vaapi-fits), e.g. in the local directory.
Setup venv with the local `requirements.txt` as well as those of `vaapi-fits`.
In addition to environment variables described on project level, set `VAAPI_FITS_CAPS=.` to use the custom V4L2 capabilities.
Run tests:
```
./vaapi-fits/vaapi-fits run --platform V4L2 test/gst-vaapi/decode test/ffmpeg-vaapi/decode
```

For a repeatable browser playback check, download the public 30-second Big Buck
Bunny sample used by `test/hevc.html` into the project root:

```
curl -fL -o Big_Buck_Bunny_1080p_30sec.mp4 \
  https://raw.githubusercontent.com/chthomos/video-media-samples/master/big-buck-bunny-1080p-30sec.mp4
```

The sample is 1920x1080 H.264 High/AAC, 24 fps, and 30 seconds long.
Open `test/hevc.html` in the test Chrome session for a single pass. Use
`test/hevc.html?loop=1` only for a looping stress run; the loop seek at the
30-second boundary is an intentional browser seek and is not part of the
decode-quality check.

For a frame-counting run, keep the video tab visible and active and close any
other Chrome instance using the V4L2 device. Background-tab scheduling can
increment Chrome's `droppedVideoFrames` counter even when the decoder trace is
clean.

To force a software baseline in Chrome, unset the VA-API variables and launch
with `--disable-features=VaapiVideoDecoder --disable-accelerated-video-decode`.
`chrome://media-internals` should then show `FFmpegVideoDecoder` for the video;
the same page with the rebuilt driver must show `VaapiVideoDecoder`. Record
`kIsPlatformVideoDecoder=true` when that property is exposed by the tested
Chrome build; playback alone is not evidence that hardware decode was used.

## Qualcomm Iris codec matrix

On the tablet, the node selected by `iris_resolve_device` advertises H.264,
HEVC, VP9 and AV1 OUTPUT formats. Run the native AV1 baseline with the
GStreamer stateful client:

```
gst-launch-1.0 -e filesrc location=$HOME/Lab/Bridge/tmp/trash/iris-av1-matrix/test.webm ! \
  matroskademux ! av1parse ! v4l2av1dec capture-io-mode=2 output-io-mode=2 ! fakesink sync=false
```

The complete repeatable matrix (material generation, VAAPI decode, EOS
oracle, and per-frame MD5 comparison) is available as:

```
./test/iris-matrix.sh
```

On the SM8550 tablet, resolve the Iris node by driver name before the matrix:

```
. ./test/lib/iris-env.sh
device=$(iris_resolve_device)
LIBVA_V4L2_VIDEO_PATH="$device" \
IRIS_MATRIX_DIR=$HOME/Lab/Bridge/tmp/trash/iris-va-matrix-139 \
./test/iris-matrix.sh
```

Set `IRIS_MATRIX_DIR` to keep artifacts in another directory under
`$HOME/Lab/Bridge/tmp/trash`; set `IRIS_MATRIX_FRAMES` to change the short
test length.

To isolate H.264 reference-structure effects, run the shorter dedicated
matrix. It generates all-I, IP-only, B=2 and B=4 streams and requires exact
software/VA per-frame MD5 equality:

```
. ./test/lib/iris-env.sh
device=$(iris_resolve_device)
LIBVA_V4L2_VIDEO_PATH="$device" \
IRIS_STRUCTURE_DIR=$HOME/Lab/Bridge/tmp/trash/iris-va-structure-matrix \
./test/iris-structure-matrix.sh
```

The script uses a 180-second per-sample limit because a B-frame reorder queue
can take tens of seconds to drain on this firmware. Keep GOP-middle `-c copy`
clips outside this positive matrix: a cut that starts on a P/B picture without
its reference chain is intentionally a negative random-access test.

The VA-API matrix is run with the same `LIBVA_*` variables and FFmpeg's VAAPI
decoder. A codec counts as passed only when FFmpeg exits successfully, emits
the expected frame count, and the trace has no capture errors or timestamp
mismatches. Fixed-resolution VP9 still runs through the native V4L2 baseline,
but its VA lane is an intentional `SKIP`: repeated session/source-change tests
reboot the target, so the driver no longer advertises VP9 Profile 0. MPEG-2 and
VP8 are marked `SKIP` when the target node does not
enumerate their OUTPUT formats. AV1 is currently `BASELINE-PASS / VA-SKIP`:
the VA AV1 API provides tile payloads, whereas Iris stateful AV1 requires a
complete OBU temporal unit; enabling the experimental translator is expected
to fail until an OBU-preserving VA caller is available. HEVC is enabled
automatically when its V4L2 OUTPUT format is advertised; set
`V4L2_VA_DISABLE_HEVC_STATEFUL=1` to opt out.
The HEVC VA pass runs before its native baseline because some Iris firmware
revisions leave a reorder queue warm when a GStreamer session closes; the
ordering keeps the VA cold-start and EOS checks deterministic.

The production dynamic-resolution gate is `iris-dynamic-resolution.sh`. It
generates an H.264 stream that alternates 640x360 and 1280x720 exactly 100
times and requires byte-identical native V4L2/software output, exact frame
MD5 from one FFmpeg VA process across all changes, and exact output from 101
fresh VA decoder processes. Both native and VA VP9 dynamic probes rebooted the
qualified Iris kernel/firmware, so the script rejects VP9 before decoding and
the driver withdraws its VA profile. Unsupported H.264 formats or missing
required elements fail the run; they are not recorded as passes or skips. Use
`IRIS_DYNAMIC_SWITCHES` only to shorten a development run.

For dual-H.264 and mixed H.264+HEVC long-run qualification, use:

```
./test/iris-concurrency-soak.sh dual-h264
./test/iris-concurrency-soak.sh mixed
```

Both scenarios default to two wall-clock hours and retain FFmpeg progress,
stateful traces, temperature samples, uptime and boot IDs. The script rejects
an interrupted/rebooted predecessor through a durable guard file, enforces a
thermal ceiling, and runs an exact software/VA MD5 preflight before starting.
H.264 workers keep one VA context for the full duration. The mixed HEVC worker
also uses one long-lived context, but its generated input spans the complete
requested duration with a continuous POC/reference timeline. Repeating a
48-frame HEVC file with FFmpeg `-stream_loop` would omit EOS between independent
sequences and create an artificial reorder carry-over that eventually exhausts
bounded timeout recovery. The real-time aggregate frame floor still applies to
both concurrent workers.
`IRIS_SOAK_SECONDS` is available for development, but shortened output is
labelled explicitly and is not release evidence.

For the stateful multi-context regression probe on the tablet:

```
. ./test/lib/iris-env.sh
device=$(iris_resolve_device)
LIBVA_DRIVER_NAME=v4l2 \
LIBVA_DRIVERS_PATH=$PWD/build/src \
LIBVA_V4L2_VIDEO_PATH="$device" \
./build/test/v4l2-context-isolation
```

For an end-of-stream teardown check, run a client to completion with
`V4L2_VA_TRACE=1`, then validate the driver trace. The HTML probe is useful for
browser playback, but the same check applies to FFmpeg or another VA client:

```
test/iris-ending-check.sh /path/to/client-trace.log
```

The check rejects bounded sync timeouts and timestamp misses during stateful
surface recycling, and requires at least one immediate stateful surface
teardown record.

For a browser context-recycling run (for example, repeated seeks or loop
teardown), validate that every destroyed VA context is reclaimed after its
surfaces disappear:

```
test/iris-context-retirement-check.sh /path/to/client-trace.log
```

The trace must be collected after the final surface teardown; the check fails
if any destroyed context remains deferred until `vaTerminate()`.

For a visual hardware/software comparison, serve the project root and open
`test/compare.html?mode=hardware` and `test/compare.html?mode=software` in
separate browser profiles. The page labels the active decoder and uses the
same 30-second H.264 sample in both windows.

### Electron real-video acceptance

Use `test/electron-video-acceptance.html` inside an isolated Electron profile
or a small Electron test shell. It runs one bounded foreground `<video>` pass
(10 seconds by default; set `?duration_ms=5000` for a shorter development run),
counts decoded/dropped frames with `getVideoPlaybackQuality()` and
`requestVideoFrameCallback()`, and exports a JSON report. The page can be
prefilled with query parameters such as `app=Obsidian&display=wayland`; fields
that are not visible to page JavaScript must be entered from the launch command
and `chrome://media-internals`.

The decoder field is intentionally manual: an Electron renderer cannot inspect
`chrome://media-internals` cross-origin. A hardware pass requires the exact
`VaapiVideoDecoder` name, `kIsPlatformVideoDecoder=true`, and a positive decoded
frame count. GPU-process startup, `libva.so` loading, or a video that merely
plays is unqualified. Keep the test window foregrounded and close competing
video clients so frame counters are not distorted by background throttling.

Validate the exported report before attaching it to a result or compatibility
matrix:

```sh
test/electron-video-report-check.sh . path/to/iris-electron-video-report.json
IRIS_ELECTRON_EXPECT_HARDWARE=1 \
  test/electron-video-report-check.sh . path/to/iris-electron-video-report.json
```

The report must also record the Electron/Chrome versions, user agent,
Wayland/X11 and ozone path, sandbox flags, VA environment, dynamically resolved
Iris node, boot ID, source commit, and installed driver SHA-256. The example
shape is `test/electron-video-report.example.json`. A report is acceptance
evidence for the selected app/runtime only; it does not generalize to another
Electron version or prove zero-copy, power, or long-duration stability.

For a stateful EOS/drain regression check, run FFmpeg with `V4L2_VA_TRACE=1`
and validate the resulting trace. The check expects one CAPTURE completion for
each submitted access unit plus the terminal `V4L2_BUF_FLAG_LAST` marker. It
also rejects capture errors, broken sequence numbers, missing drain completion,
and a non-terminal LAST marker. The two-argument form is strict and rejects
bounded sync timeouts; for a burst-style FFmpeg producer, pass
`--allow-bounded-sync` to keep the EOS/queue assertions strict while reporting
expected bounded-wait diagnostics:

```
test/iris-eos-check.sh /path/to/ffmpeg-trace.log 720
test/iris-eos-check.sh /path/to/ffmpeg-burst-trace.log 720 --allow-bounded-sync
```

This follows the Linux stateful decoder contract: `VIDIOC_DECODER_CMD(STOP)`
initiates a drain, both queues remain active until CAPTURE
`V4L2_BUF_FLAG_LAST`, and only then may a stateful decoder be restarted.
`STREAMOFF`/`close()` implicitly stops and discards buffered data.

## Full target hardware suite

`iris-hardware-suite.sh` combines the repository matrices with a V4L2
capability inventory, `v4l2-compliance`, native GStreamer EOS checks, the
Qualcomm `v4l-video-test-app`, and optional Fluster conformance runs. It is
device-agnostic, probes for `iris_driver`, and fails rather than guessing a
node; set `V4L2_DEVICE` (or `LIBVA_V4L2_VIDEO_PATH`) for an inspected override.
All logs and generated media stay
under `$HOME/Lab/Bridge/tmp/trash`:

```
IRIS_HARDWARE_SUITE_DIR=$HOME/Lab/Bridge/tmp/trash/iris-hardware-suite \
./test/iris-hardware-suite.sh
```

The repository matrices are enabled by default. Set `IRIS_RUN_REPO_MATRICES=0`
when only external tools are desired. To add the Qualcomm JSON client, point
`IRIS_V4L_TEST_BINARY` at `iris_v4l2_test` and provide a whitespace-separated
`IRIS_V4L_TEST_CONFIGS` list. Set `IRIS_V4L_TEST_CWD` to the Qualcomm
checkout root when a JSON file uses relative `InputPath`/`Outputpath` values.
To add Fluster, set `FLUSTER_DIR`,
`IRIS_FLUSTER_SUITES` and `IRIS_FLUSTER_DECODERS`, for example:

```
FLUSTER_DIR=$HOME/Lab/iris-vaapi-lab/tools/fluster \
IRIS_FLUSTER_SUITES='JVT-AVC_V1 JCT-VC-HEVC_V1 VP9-TEST-VECTORS AV1-TEST-VECTORS' \
IRIS_FLUSTER_DECODERS='GStreamer-H.264-V4L2' \
./test/iris-hardware-suite.sh
```

`GStreamer-*-V4L2` talks directly to the Iris kernel node and therefore
characterises firmware capabilities without loading this VA-API driver. To
exercise this repository's code, select the matching `FFmpeg-*-VAAPI` decoder:

```
FLUSTER_DIR=$HOME/Lab/iris-vaapi-lab/tools/fluster \
IRIS_FLUSTER_SUITES='JVT-AVC_V1' \
IRIS_FLUSTER_DECODERS='FFmpeg-H.264-VAAPI' \
V4L2_VA_SYNC_TIMEOUT_MS=100 \
./test/iris-hardware-suite.sh
```

Fluster's stock FFmpeg VA-API wrapper uses a software pixel-format filter and
does not request a VAAPI output pool explicitly. Its results are useful for
finding surface-lifetime and timeout boundaries, but the repository's
`iris-matrix.sh` is the authoritative positive VA-API pixel oracle. Record the
decoder name with every Fluster result so firmware-only numbers are not
mistaken for driver coverage. Fluster's non-zero result is retained as a
capability warning because its corpus intentionally includes profiles, bit
depths, dimensions and dynamic-resolution modes that a particular Iris
firmware may not implement. The raw JSON and log are the review artifacts.
`v4l2-compliance` is strict except for the known stateful mem2mem control
classification; set
`IRIS_ALLOW_KNOWN_COMPLIANCE_LIMIT=0` to make that result fail the suite.

For the opt-in DMA-BUF CAPTURE contract, run the qualification through the
experiment guard:

```sh
IRIS_EXPERIMENT_DIR=$HOME/Lab/Bridge/tmp/trash/zero-copy-guard \
IRIS_ZERO_COPY_DIR=$HOME/Lab/Bridge/tmp/trash/zero-copy-suite \
IRIS_EXPERIMENT_TIMEOUT_SECONDS=600 \
./scripts/iris-experiment-guard.sh -- ./test/iris-zero-copy-suite.sh
```

The suite requires exact framemd5 and strict EOS for direct no-B H.264, proves
that no `copy_surface_frame` ran, and verifies that B-frame H.264 and HEVC use
the stable-copy fallback when the explicit ownership contract is absent.

The browser power harness has three separate, non-poolable modes:

```sh
sh ./test/iris-power-compare.sh --clip /path/to/h264.mp4 \
  --browser chrome --hardware-mode copy
sh ./test/iris-power-compare.sh --clip /path/to/h264.mp4 \
  --browser chrome --hardware-mode zero-copy
sh ./test/iris-power-compare.sh --clip /path/to/h264.mp4 \
  --browser chromium --hardware-mode native
```

The analyser checks the expected decoder name and whether the resolved Iris
node is actually held by the hardware arm. Chrome VA-API, Chrome zero-copy,
and Chromium native V4L2 answer different questions and require separate
collections.

## Open-source test corpora

There is no single corpus that covers both codec conformance and browser
surface-lifetime behavior. Use the following projects together:

- [Fluster](https://github.com/fluendo/fluster) is the primary codec
  conformance corpus. It includes H.264/AVC, HEVC, VP8/VP9 and AV1 suites with
  reference checksums, and can run an external decoder command. The current
  suites include 204 H.264 vectors (`JVT-AVC_V1` + `JVT-FR-EXT`), 147 HEVC
  vectors, 311 VP9 vectors and 242 AV1 vectors. A focused download is:

  ```
  git clone https://github.com/fluendo/fluster.git
  cd fluster
  ./fluster.py download JVT-AVC_V1 JVT-FR-EXT JCT-VC-HEVC_V1 VP9-TEST-VECTORS AV1-TEST-VECTORS
  ./fluster.py list
  ```

  Start with one vector and the software decoder, then add the VA-API decoder
  wrapper. Compare decoded frame count and pixels, not only process exit status.
- [Chromium media/test/data](https://chromium.googlesource.com/chromium/src/+/lkgr/media/test/data/)
  is useful for container and playback regressions. In particular,
  `front-discard.mp4`, `mid-file-start-time.mp4`, `bear-1280x720.mp4`,
  `avc-bitstream-format-0.h264` and `avc-bitstream-format-1.h264` cover short
  files, non-zero start timestamps, discard-at-start and H.264 framing. The
  directory also contains VP8/VP9/HEVC/AV1 samples and documents how each file
  was generated.
- [GStreamer GstValidate](https://gstreamer.freedesktop.org/documentation/gst-devtools/gst-validate.html)
  is a scenario runner rather than a corpus. It is useful for replaying a
  downloaded clip through play, seek, flush and EOS scenarios, for example:

  ```
  gst-validate-1.0 playbin uri=file:///path/to/sample.mp4
  ```

- [Qualcomm v4l-video-test-app](https://github.com/quic/v4l-video-test-app)
  provides a V4L2 decoder test client and JSON-configured codec cases. It is a
  good native baseline for Iris, while [v4l2-compliance](https://github.com/gjasny/v4l-utils/blob/master/utils/v4l2-compliance/v4l2-test-codecs.cpp)
  checks ioctl/queue semantics rather than decoded picture correctness.

For the current Iris investigation, the smallest useful local set is the
generated 1-3 second matrix (all-I, IP-only, B-frame, long-GOP and a GOP-middle
`-c copy` clip) plus Chromium's `mid-file-start-time.mp4`. Record codec/profile,
GOP/B-frame structure, start PTS, decoded frame count, per-frame MD5 or pixel
error, CAPTURE/OUTPUT/`LAST` counts, timestamp misses and bounded sync timeouts.
The positive 640x360 matrix currently passes for IP/all-I/B=2/B=4. The
long-GOP middle-cut sample remains a negative random-access/contract test.

References:

- [Linux `VIDIOC_DECODER_CMD` documentation](https://docs.kernel.org/userspace-api/media/v4l/vidioc-decoder-cmd.html)
- [Linux stateful decoder interface](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-decoder.html)
- [Chromium video decoder test usage](https://chromium.googlesource.com/chromium/src/+/b1a274d92dbc807081d60a4dd110294205bdbd87/docs/media/gpu/video_decoder_test_usage.md)
- [Chromium EOS flush tests](https://chromium.googlesource.com/chromium/src/+/c6a258bb5d64045152f89681e84a982d7ae68f46/media/gpu/video_decode_accelerator_tests.cc)
- [ChromiumOS stateful decoder reference client](https://chromium.googlesource.com/chromiumos/platform/drm-tests/+/8db6cd9d8e4a7927f43e499066352354b8297b86/v4l2_stateful_decoder.c)
- [v4l-utils codec ioctl checks](https://github.com/gjasny/v4l-utils/blob/master/utils/v4l2-compliance/v4l2-test-codecs.cpp)
