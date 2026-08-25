# Qualcomm Iris VA-API Test Results

Target: Fedora 44 ARM64 tablet `192.168.3.123`, Snapdragon SM8550, Iris decoder
`/dev/video17`, kernel `7.2.0-sm8550`.

## Passed

- The driver builds with the tablet's Meson/Ninja toolchain.
- `vainfo` loads the test driver from `/tmp/qualcomm-iris-vaapi/build/src`.
- H.264 Main, High and Constrained Baseline profiles enumerate through VA-API.
- Chrome on the real GNOME Wayland session enters `VaapiVideoDecoder` and can
  export the VA surface without the previous `invalid VASurfaceID` setup error.
- The stateful Iris path now batches VA H.264 pictures into continuous
  bytestream OUTPUT buffers. Native V4L2 tracing shows real CAPTURE DQBUF
  results (including reordered H.264 output) instead of a poll timeout.
- `ffmpeg` with `-vf hwdownload,format=nv12` produces non-empty NV12 output
  for the first drained batch (3 frames, 1,036,800 bytes) when the explicit
  `V4L2_VA_DRAIN_BATCH=1` diagnostic mode is enabled.

## Not yet passed

- The Iris stateful H.264 frame submission still fails to produce a VA CAPTURE
  frame for the multi-frame test. OUTPUT buffers are accepted and dequeued,
  but CAPTURE polling times out.
- FFmpeg reproduces the same issue with `Timed out waiting for surface` and
  `hardware accelerator failed to decode picture`.
- The one-frame `ffmpeg -f null` command can exit successfully, but an actual
  `hwdownload` sync fails, so this is not evidence of a decoded hardware frame.
- Chrome therefore has not passed the required `VaapiVideoDecoder` /
  `kIsPlatformVideoDecoder=true` and no-fallback acceptance gate.

## Current blocker

- With the default streaming path, Chrome still records
  `video decoder fallback after initial decode error` and ends on
  `FFmpegVideoDecoder` (`kIsPlatformVideoDecoder=false`).
- The explicit drain experiment can download the first batch, but a later
  batch may return a CAPTURE buffer marked `V4L2_BUF_FLAG_ERROR` on this
  kernel/firmware combination. The diagnostic mode is therefore not enabled
  by default and is not a completion claim.

The driver is therefore left in the project tree and tablet `/tmp` only. It has
not been installed into `/usr/lib64/dri` or made the system default. No Chrome
hardware-decoding completion claim is made until media-internals remains on
`VaapiVideoDecoder` for the full playback with no fallback.
