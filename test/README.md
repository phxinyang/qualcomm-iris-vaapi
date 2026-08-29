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
the same page with the rebuilt driver shows `VaapiVideoDecoder`.

## Qualcomm Iris codec matrix

On the tablet, `/dev/video17` advertises H.264, HEVC, VP9 and AV1 OUTPUT
formats. Run the native AV1 baseline with the GStreamer stateful client:

```
gst-launch-1.0 -e filesrc location=$HOME/Lab/Bridge/tmp/trash/iris-av1-matrix/test.webm ! \
  matroskademux ! av1parse ! v4l2av1dec capture-io-mode=2 output-io-mode=2 ! fakesink sync=false
```

The complete repeatable matrix (material generation, VAAPI decode, EOS
oracle, and per-frame MD5 comparison) is available as:

```
./test/iris-matrix.sh
```

On the SM8550 tablet at `192.168.3.139`, select its Iris decoder explicitly:

```
LIBVA_V4L2_VIDEO_PATH=/dev/video0 \
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
LIBVA_V4L2_VIDEO_PATH=/dev/video0 \
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
mismatches. MPEG-2 and VP8 are marked `SKIP` when the target node does not
enumerate their OUTPUT formats. AV1 is currently `BASELINE-PASS / VA-SKIP`:
the VA AV1 API provides tile payloads, whereas Iris stateful AV1 requires a
complete OBU temporal unit; enabling the experimental translator is expected
to fail until an OBU-preserving VA caller is available. HEVC is enabled
automatically when its V4L2 OUTPUT format is advertised; set
`V4L2_VA_DISABLE_HEVC_STATEFUL=1` to opt out.
The HEVC VA pass runs before its native baseline because some Iris firmware
revisions leave a reorder queue warm when a GStreamer session closes; the
ordering keeps the VA cold-start and EOS checks deterministic.

For the stateful multi-context regression probe on the tablet:

```
LIBVA_DRIVER_NAME=v4l2 \
LIBVA_DRIVERS_PATH=$PWD/build/src \
LIBVA_V4L2_VIDEO_PATH=/dev/video17 \
./build/test/v4l2-context-isolation
```

For a browser-facing end-of-stream teardown check, launch the single-pass
`test/iris-ending-probe.html` page with `V4L2_VA_TRACE=1`, wait for its
`ended` event, then validate the driver trace:

```
test/iris-ending-check.sh /path/to/chrome-trace.log
```

The check rejects bounded sync timeouts and timestamp misses during Chrome's
stateful surface recycling, and requires at least one immediate stateful
surface teardown record.

For a browser context-recycling run (for example, repeated seeks or loop
teardown), validate that every destroyed VA context is reclaimed after its
surfaces disappear:

```
test/iris-context-retirement-check.sh /path/to/chrome-trace.log
```

The trace must be collected after the final surface teardown; the check fails
if any destroyed context remains deferred until `vaTerminate()`.

For a visual hardware/software comparison, serve the project root and open
`test/compare.html?mode=hardware` and `test/compare.html?mode=software` in
separate Chrome profiles. The page labels the active decoder and uses the same
30-second H.264 sample in both windows.

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
initiates a drain, while `STREAMOFF`/`close()` implicitly stops and discards
buffered data. Chromium's reference video decoder tests apply the same EOS
oracle by waiting for flush completion and comparing decoded-frame count and
per-frame MD5 metadata.

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
