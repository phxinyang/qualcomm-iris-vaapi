# Qualcomm Iris VA-API Test Results

Target: Fedora 44 ARM64 tablet `192.168.3.129`, Snapdragon SM8550, Iris decoder
`/dev/video17`, kernel `7.2.0-sm8550`.

## Passed

- The driver builds with the tablet's Meson/Ninja toolchain.
- `vainfo` loads the test driver from the project build directory.
- H.264 Main, High and Constrained Baseline profiles enumerate through VA-API.
- Chrome on the real GNOME Wayland session enters `VaapiVideoDecoder` and
  exports the full 25-surface VA pool without `invalid VASurfaceID` errors.
- Official Chrome 151 plays the 30-second H.264 test stream continuously in a
  clean single-browser session with the bounded 100 ms stateful sync wait. A
  25-second sample stayed at `readyState=4`, `paused=false`, and reported zero
  dropped frames while crossing one 30-second loop boundary.
- In a non-looping 30-second run, the hardware page reached
  `currentTime=30`, `ended=true`, `paused=true`, `readyState=4`, with all 720
  video frames accounted for and zero dropped frames.
- A clean-process event probe recorded the final `timeupdate` at 29.789s,
  followed directly by the normal `pause(ended=true)` and `ended` events at
  30s. Reloading an already-ended page can make Chrome seek to a restored
  position (for example 26.464s); that pause/seek/play sequence is browser
  media-session restoration, not a decoder stall.
- The default one-AU batches preserve exact timestamp association and completed
  a 30-second run without queue errors. `V4L2_VA_BATCH_SIZE=2..8` remains
  available for explicit throughput experiments; `1` is the production value.
- The stateful Iris path associates OUTPUT and CAPTURE buffers by timestamp and
  repeats SPS/PPS for every H.264 access unit. CAPTURE timestamps with no live
  OUTPUT entry are deliberately dropped; nearest-timestamp fallback was
  removed because a timed-out frame can otherwise be bound to a future surface
  and cause persistent frame corruption.
- Chrome 151 dual-video playback on the SM8550 tablet uses the stable
  per-surface DMA-BUF path and remained free of the exact `[0,136,0]` green
  frame in repeated 10-30 second canvas samples; both videos stayed on
  `VaapiVideoDecoder`.
- The test page defaults to a single pass. A `?loop=1` stress sample showed
  only the expected `30.0 -> 0.0` browser seek transition, with no anomalous
  green pixels. A software Chrome run selected `FFmpegVideoDecoder`, while
  the hardware run selected `VaapiVideoDecoder` for the same file.
- `chrome://gpu` reports `Video Decode: Hardware accelerated` with the rebuilt
  driver. The two-session context isolation probe passes with distinct V4L2
  session file descriptors.
- The stateful VP9 path decodes 20 frames from the Iris VP9 sample through
  FFmpeg VA-API with no hardware decode errors.
- The repeatable 320x180/24fps Iris matrix now validates decoded content, not
  only queue completion: H.264 and VP9 each decoded 48/48 frames and their
  per-frame `framemd5` output matched the software reference exactly. The
  capture readback path accounts for Iris' padded stride/height, and context
  teardown is serialized with concurrent VA surface downloads.
- The same matrix's native AV1 baseline (`matroskademux ! av1parse !
  v4l2av1dec`) reached GStreamer EOS successfully on `/dev/video17`.
- The matrix records MPEG-2 and VP8 as `SKIP` on this node because Iris does
  not enumerate their OUTPUT formats; this is a hardware capability result,
  not a driver failure.
- The EOS regression trace passes `test/iris-eos-check.sh`: all 720 H.264
  access units produce 720 OUTPUT DQBUF and 720 CAPTURE DQBUF completions,
  followed by one CAPTURE `LAST` marker after `decoder STOP`; CAPTURE sequence
  numbers are contiguous and no capture errors, timeout, or timestamp-miss
  records remain in the strict Chrome run. The pre-drain trace is rejected by
  the same check. Burst-style FFmpeg runs use the explicit
  `--allow-bounded-sync` mode, which still enforces all EOS/queue invariants
  while reporting bounded-wait diagnostics.
- The strict Qualcomm Iris matrix now passes H.264, VP9 and opt-in HEVC on
  five repeated cold starts: each codec produced 48/48 frames whose
  per-frame MD5 matches the software baseline, with 48 OUTPUT, 49 CAPTURE,
  one terminal LAST, no capture errors, and no timeout/timestamp diagnostics.
- A forced `V4L2_VA_SYNC_TIMEOUT_MS=100` HEVC run exercised the trailing
  reorder recovery (one STOP/START pair) and still passed the same 48-frame
  MD5 and strict EOS oracle.
- The browser tail-frame regression was reproduced with a 60 Hz Canvas probe:
  before the follow-up fix, hardware sampling produced hashes that did not
  belong to any complete `requestVideoFrameCallback` frame and occasionally
  skipped `presentedFrames`; software did not. The queue-service path now stages
  CAPTURE data in a per-surface CPU snapshot and `syncSurface()` publishes it
  only while the target is still `Rendering`. Two rebuilt hardware single-pass
  runs and one loop-boundary run had contiguous frame numbers, no foreign pixel
  hashes, and a stable final frame. `test/iris-source-invariants.sh` rejects
  the old direct-copy path (red on the previous source, green on the fix).

## Pending

- Long-running arbitrary sites such as Douyin still need a user-session check;
  network-selected codecs and MSE seek patterns can exercise different paths.
- A new VA context can fail its initial export while another Iris session is
  holding the firmware queue; Chrome then reports `VaapiVideoDecoder` followed
  by an intentional `FFmpegVideoDecoder` fallback. Keep one hardware session
  per decoder node during diagnostics and treat this resource-contention case
  separately from decoded-pixel correctness.
- The stable DMA-BUF path adds one NV12 memcpy per decoded frame. A future
  zero-copy implementation would need a V4L2 DMABUF capture queue shared with
  the per-surface allocations.
- HEVC is advertised automatically when the node exposes stateful HEVC. The VA
  long format does not carry the original VPS/SPS/PPS, so the driver generates
  parameter sets from VA metadata; this path passes the strict 48-frame
  content/EOS matrix and repeated cold-start runs on both SM8550 tablets. Set
  `V4L2_VA_DISABLE_HEVC_STATEFUL=1` for firmware with a non-standard contract.
- AV1 VA is deliberately disabled by default. FFmpeg's VA-API path supplies
  tile payloads (the first byte is not an OBU header), while Iris stateful AV1
  requires a complete temporal-unit OBU stream. The experimental translator
  can be enabled with `V4L2_VA_ENABLE_AV1_STATEFUL=1`, but native V4L2 AV1 is
  the only passing AV1 path until an OBU-preserving VA caller is available.

## Deployment note

The verified driver is in the project build tree and a tablet scratch checkout;
it has not been installed into `/usr/lib64/dri` or made the system default.

## Tablet 192.168.3.139

The second SM8550 tablet exposes the Iris decoder as `/dev/video0` (its
`/dev/video17` is a camera node). The full matrix was repeated three times on
`/dev/video0`, with the context-isolation probe before the first run:

- H.264, VP9 and opt-in HEVC each produced 48/48 frames whose per-frame MD5
  matched the software reference exactly.
- Each VA run passed strict EOS: 48 OUTPUT DQBUF, 49 CAPTURE DQBUF, one
  terminal `LAST`, contiguous capture sequence numbers, and zero timeout or
  timestamp-miss diagnostics.
- Native `v4l2h264dec`, `v4l2vp9dec`, `v4l2h265dec` and `v4l2av1dec` all reached
  GStreamer EOS. AV1 remains VA-skipped because the VA caller supplies tile
  payloads rather than the complete OBU temporal unit required by Iris.
- The two-context isolation probe passed with distinct V4L2 session file
  descriptors.

The browser-ending regression was reproduced with Chrome 151 on Wayland using
the `.139` decoder. Before the fix, Chrome's context reset destroyed 14
stateful `Rendering` surfaces and each synchronous `destroySurfaces()` call
waited for the bounded timeout, producing 14 `Timed out waiting for surface`
records. The driver now removes the stateful batch mapping immediately when
the caller destroys such a target; late CAPTURE buffers are requeued by the
normal queue service. The old trace fails the regression checker with
`teardown_timeouts=14`; the rebuilt driver passes:

```
PASS teardown_timeouts=0 timestamp_misses=0 stateful_surface_teardown_skip=14
```

A clean 10-second single-pass browser probe then reached `ended=true`,
`readyState=4`, 301 presented frames, zero dropped frames, and identical canvas
pixel samples after `ended`. The final frame remained stable for repeated
samples, with no additional decode submissions after EOS.

## Follow-up structure tests (192.168.3.134, `/dev/video0`)

The browser probe was repeated with full RGBA SHA-256 snapshots at 28.5-30.0s.
The final canvas remained stable. The earlier 32-bit canvas hash result that
looked like a repeated old frame was a hash collision; it is not used as
evidence anymore.

Short, generated H.264 samples were then compared against FFmpeg software
`framemd5` output through the VA-API path:

| Sample | Structure | Result |
| --- | --- | --- |
| `ip-g12-1s.mp4` | 24 frames, no B, GOP 12 | 24/24 MD5 match |
| `ip-g48-2s.mp4` | 48 frames, no B, GOP 48 | 48/48 MD5 match |
| `intra-2s.mp4` | 48 all-I frames | 48/48 MD5 match |
| `b8-g120-2s.mp4` | 48 frames, B-frame stream | 48/48 MD5 match |
| `b4-g48-2s.mp4` | 48 frames, B-frame stream | 48/48 MD5 match after a 90s allowance |

The B=4 sample was previously reported as 12-14 frames only because the
15-25s harness timeout expired while the VA caller was synchronously waiting
for the reorder queue. A longer run completed normally and matched software;
that short-timeout result must not be called a decode corruption.

Two GOP-middle `-c copy` clips are different tests. `copy-mid-nots.mp4` starts
with P pictures before the next IDR and has an incomplete reference chain. It
produced 145/145 frames but one timestamp miss and one pixel mismatch. This is
consistent with the Linux stateful decoder contract: a client must seek to a
random-access point with the required SPS/PPS/reference state, and output from
an arbitrary P-frame cut is not a valid picture oracle. By contrast, Chromium's
official `mid-file-start-time.mp4` has a valid stream with a non-zero start PTS
and passed 250/250 VA frames with an exact software MD5 match; non-zero PTS by
itself is not the trigger.

These results narrow the remaining browser report to EOS/compositor timing or
an input-specific random-access pattern. They do not justify another driver
patch yet. Keep the long-GOP middle-cut sample as a negative/contract test, but
use complete files or keyframe-aligned clips for decoded-pixel pass criteria.

The reusable `test/iris-structure-matrix.sh` was run on the same `/dev/video0`
with 640x360/48-frame inputs. Its all-I, IP-only, B=2 and B=4 cases all passed
48/48 frames with exact software MD5, zero timestamp misses and zero bounded
sync timeouts.

## Latest follow-up (2026-08-30, `test/iris-stateful-followup`)

Target: Fedora ARM64 tablet `192.168.3.133`, Snapdragon SM8550, Iris decoder
`/dev/video0`; driver built in
`/home/xinyang/Lab/Bridge/tmp/trash/iris-restore-20260830/build/src`.

- Stateful EOS drain is opt-in with `V4L2_VA_STATEFUL_EOS_DRAIN=1` (the legacy
  `V4L2_VA_EOS_DRAIN=1` alias remains supported). The default idle threshold is
  400ms, based on the measured 325ms maximum normal batch gap and the 458ms
  stale-tail onset. A 30s H.264 browser run reached `ended=true` with 72/72
  tail frames and zero repeated hashes; the trace had one STOP/LAST/START,
  zero timeouts and zero timestamp misses.
- The drain implementation now waits for trailing OUTPUT DQBUF after the
  terminal CAPTURE LAST marker before issuing START, as required by the Linux
  stateful V4L2 contract. The post-change `matrix-general-followup` repeated
  the H.264, VP9 and HEVC checks with no regressions.
- A 115s `loop=true` Chrome probe crossed three 30s loop seeks with 2741 frame
  callbacks, zero gaps over 200ms, zero dropped frames and no trace timeout or
  timestamp miss.
- The standard 48-frame VA matrix passed H.264, VP9 and HEVC with exact software
  framemd5 equality. Each had 48 OUTPUT, 49 CAPTURE, one terminal LAST, and no
  capture errors, timeouts or timestamp misses. Native AV1 reached GStreamer
  EOS; VA AV1 remains skipped because the VA tile payload lacks Iris' required
  complete OBU temporal unit.
- The H.264 structure matrix (all-I, IP/GOP12, B=2, B=4) passed 48/48 with exact
  software MD5. HEVC with `V4L2_VA_SYNC_TIMEOUT_MS=100` also passed 48/48 and
  strict EOS with one recovery STOP/START pair.
- The EOS watchdog remains opt-in because VA-API has no portable end-of-input
  callback. Its startup guard now requires eight submitted AUs and one
  completed frame, so normal HEVC startup gaps are not treated as EOS.
- The generic watchdog now requires eight submitted AUs and one completed
  frame, and both timeout recovery and idle drain refuse to issue START until
  CAPTURE `V4L2_BUF_FLAG_LAST` is observed. Default and watchdog-enabled
  H.264/VP9/HEVC matrices on `/dev/video0` passed 48/48 with exact software
  MD5; the forced 100ms timeout HEVC matrix also passed strict EOS and content
  checks. MPEG-2/VP8 remain correctly reported as unavailable on this node,
  while native AV1 reaches EOS and VA AV1 remains skipped for its tile/OBU
  payload contract.

The state-machine changes follow the Linux stateful decoder contract and the
queue handling used by reference clients:

- Linux V4L2 stateful decoder drain: <https://www.kernel.org/doc/html/v6.4/userspace-api/media/v4l/dev-decoder.html>
- Chromium stateful V4L2 client: <https://chromium.googlesource.com/chromium/src/+/a576d2ae67f9a89b4ac118460c7e4e4763f8c22a/media/gpu/v4l2/v4l2_stateful_video_decoder.cc>
- GStreamer V4L2 decoder: <https://github.com/GStreamer/gst-plugins-good/blob/master/sys/v4l2/gstv4l2videodec.c>

This follow-up is recorded on branch `test/iris-stateful-followup` in commits
`95de0d4` (state-machine safety), `e753088` (watchdog/source guards),
`0288082` and `c2f1211` (generic client documentation), and `cafa6b9`
(generic trace-checker arguments).

## Dedicated hardware suite (2026-08-30, `/dev/video0`)

The complete hardware plan from the earlier Claude investigation was run on
the Fedora 44 ARM64 SM8550 tablet at `192.168.3.133`. The repeatable entry
point is `test/iris-hardware-suite.sh`; its logs and generated media are kept
under `~/Lab/Bridge/tmp/trash` and are not repository inputs.

Qualcomm's upstream `v4l-video-test-app` (`b1f1a04`) was built natively with
shared Fedora FFmpeg/JsonCpp libraries (the upstream CMake file requests static
libraries that are not shipped by Fedora). Its JSON decoder client passed all
four advertised codecs:

- H.264: 32/32 decoded frames, 44,236,800-byte NV12 dump;
- VP9, HEVC and AV1: 48/48 frames each, 4,147,200-byte NV12 dumps.

Every dump matched an FFmpeg software `nv12` decode byte-for-byte. A second
Qualcomm run with two concurrent H.264 contexts passed both clients and both
outputs matched the software reference. A mixed four-context run (H.264,
VP9, HEVC, AV1) also returned four `SUCCESS` results and all four dumps matched
their software references.

The V4L2 contract checks reported 47/48 tests succeeded. The sole reported
failure is the `v4l2-compliance` stateful decoder control-class check, while
the same device exposes `min_number_of_capture_buffers` (`0x00980927`) through
`v4l2-ctl`; the verbose report and raw control inventory are retained for
follow-up with v4l-utils. No ioctl, format, queue, buffer or decoder-command
failure was reported.

Native GStreamer pipelines (`v4l2h264dec`, `v4l2vp9dec`, `v4l2h265dec`,
`v4l2av1dec`) all reached EOS. GstValidate reached EOS as well, but on this
GStreamer 1.28 build it reports a `gst_structure_remove_field: IS_MUTABLE`
critical for VP9/HEVC/AV1 and an EOS-seqnum diagnostic for H.264; the plain
`gst-launch-1.0` pipelines are clean, so these are recorded as GStreamer
validator diagnostics rather than Iris pixel failures.

Fluster standard AV1 vectors completed 200/242 with the GStreamer V4L2 AV1
decoder. The 42 non-passes are isolated to the suite's deliberately extreme
cases: 16x16/odd tiny dimensions, monochrome, dynamic resolution and SVC
layers. One downloaded H.264 subset (50 vectors) produced 29 successes; the
remaining vectors exercised extended/interlaced profiles or dimensions that
the firmware rejected. The standard H.264, HEVC and VP9 suites were downloaded
in the same trash checkout for the next run; their full results are not claimed
until the remaining corpus download completes.

This run adds the generic orchestration script and documents the distinction
between codec correctness, V4L2 contract checks, and firmware capability
boundaries. It does not change the production driver based on external-tool
diagnostics.
