# Qualcomm Iris VA-API Test Results

Primary latest documented target: Fedora 44 ARM64 Snapdragon SM8550 tablet,
kernel `7.2.0-sm8550`. Device numbers are historical and must be resolved
through `iris_resolve_device`; target labels below distinguish separate boots
and replacement endpoints without exposing private network addresses.

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
- After the August 30, 2026 system installation, Google Chrome 151.0.7922.173
  was launched through `/usr/local/bin/iris-vaapi-browser` with no
  `LIBVA_DRIVERS_PATH` override. A 1920x1080 H.264 run reached
  `kVideoDecoderName=VaapiVideoDecoder` and
  `kIsPlatformVideoDecoder=true` in `chrome://media-internals`; the 30-second
  file completed with `readyState=4`, `ended=true`, 0 dropped frames, and 30
  seconds of media duration. The same environment intentionally selected
  `FFmpegVideoDecoder`/`kIsPlatformVideoDecoder=false` for a 320x180 sample,
  confirming Chromium's size/performance threshold rather than an installation
  failure.
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
- The stable DMA-BUF path adds one NV12 memcpy per decoded frame. The opt-in
  zero-copy experiment now removes that copy only for the validated one-AU,
  no-B H.264/VP9 contract; multi-slot ownership, B-frame reorder and broad
  dynamic-resolution coverage remain open.
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

The latest target deployment rebuilt commit `400a46aed159f8a951bcb0a3d5dc6dd797a960bb`
and installed its driver, launcher and four desktop entries on August 30, 2026.
The system driver is `/usr/lib64/dri/v4l2_drv_video.so` with SHA-256
`f879e5ea417c770ac7042d520625e9cadcec1ffd00b43ca6658cfb811d5763ef`; the
install manifest is `/usr/local/share/iris-vaapi/install-manifest.txt` and
records the previous-driver backup. A system-path `vainfo` smoke test resolved
`/dev/video17` and enumerated H.264 and HEVC VA profiles. Browser playback and
Electron decoder claims still require their separate runtime gates.

## Secondary target

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
the secondary-target decoder. Before the fix, Chrome's context reset destroyed 14
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

## Follow-up structure tests (secondary target)

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

Target: Fedora ARM64 Snapdragon SM8550 tablet, Iris decoder resolved at runtime;
driver built in an external lab workspace.

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
the Fedora 44 ARM64 SM8550 tablet. The repeatable entry
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

Fluster corpus results were completed with the GStreamer V4L2 decoders. The
JSON reports are retained in the tablet scratch directory
`~/Lab/Bridge/tmp/trash/iris-hw-suite-20260830`:

| Suite / decoder | Total | Passed | Error/Fail/Timeout | Interpretation |
| --- | ---: | ---: | ---: | --- |
| JVT-AVC_V1 / H.264 | 135 | 40 | 95 errors | Main-profile vectors pass; failures are concentrated in field/MBAFF/PAFF, extended and other conformance profiles rejected by this node. |
| JVT-FR-EXT / H.264 | 69 | 0 | 69 errors | Fidelity Range Extension/extended interlaced capability is not exposed by the firmware. |
| JCT-VC-HEVC_V1 / HEVC | 147 | 112 | 34 errors, 1 fail | Main/Main10 and ordinary coding vectors pass where advertised; advanced WPP/tiles/10-bit vectors are rejected. |
| VP9-TEST-VECTORS / VP9 | 305 | 235 | 68 errors, 1 fail, 1 timeout | Failures are 8x8 through 66x66 extreme dimensions, 4:2:2/4:4:4, dynamic resize and SVC cases. The timeout is the 16x16 all-intra stress vector. |
| VP9-TEST-VECTORS-HIGH / VP9 | 6 | 0 | 6 errors | 10/12-bit 4:2:0/4:2:2/4:4:4 is outside the advertised 8-bit 4:2:0 capability. |
| AV1-TEST-VECTORS / AV1 | 242 | 200 | 42 errors | Non-passes are limited to tiny dimensions, monochrome, dynamic resize and SVC-layer vectors. |
| CHROMIUM-8bit-AV1-TEST-VECTORS / AV1 | 13 | 13 | 0 | Chromium's 8-bit AV1 coverage passes in full. |

Two corpus downloads reported checksum mismatches (`brcm_freh5` in
`JVT-FR-EXT` and `SLPPLP_A_VIDYO_2` in `JCT-VC-HEVC_V1`). Fluster keeps those
metadata entries as errors; they are not treated as decoded hardware failures.

The one HEVC checksum failure, `RAP_A_docomo_6`, was rerun three times. The
software reference emits 86 416x240 I420 frames (`8a536a80...`); Iris emits a
stable 85-frame sequence (`c074ee17...`) beginning at the software second
frame. The three hardware byte streams were identical. This is a deterministic
RAP/RASL output capability difference in the native stateful path, not a
non-repeatable pixel corruption; it is kept as a negative capability result
until a client/firmware contract for leading RASL pictures is established.

The VP9 timeout is likewise an isolated 16x16 all-intra stress case. The
remaining 235 vectors complete in 64.5 seconds, and no timeout appears in the
standard VA matrix or Qualcomm mixed-codec client. These external corpus
results therefore do not justify another production-driver change.

This run adds the generic orchestration script and documents the distinction
between codec correctness, V4L2 contract checks, and firmware capability
boundaries. It does not change the production driver based on external-tool
diagnostics.

## Codex follow-up (2026-08-30, `codex/claude-followup`, secondary target)

The latest Claude route was continued on a durable secondary lab. Local
`bash test/run-static-checks.sh` and `ninja -C build-local` passed. The remote
deployment performed the content sync, clean rebuild and `ldd -r` gate with no
undefined symbols; the resulting driver SHA-256 was
`f8a8e684f6ddb072bc19b31f9bc3c33e3145b2bcc85ecede45ff560919a64ee3`.

The positive 48-frame VA matrix was then repeated from the deployed checkout:
H.264, VP9 and HEVC each produced 48/48 frames with exact software framemd5,
strict EOS (`48 OUTPUT`, `49 CAPTURE`, one terminal `LAST`) and zero timeout or
timestamp-miss diagnostics. Native H.264/VP9/HEVC/AV1 GStreamer baselines all
reached EOS; AV1 VA remains intentionally disabled by its tile-payload versus
OBU contract.

After commit `6074609` reset the barren cold-start budget on every STOP/START,
the driver was deployed again (`68cc99da41d81f162944fec487d9371f15b1beefa7c05020a70a96af936cdb83`)
and the same H.264/VP9/HEVC 48-frame matrix passed unchanged. With
`LIBVA_V4L2_VIDEO_PATH` unset, `vainfo` also auto-discovered `/dev/video0` by
the `iris_driver` name and enumerated the expected H.264, HEVC and VP9
profiles.

The driver-path Fluster probes used `FFmpeg-*-VAAPI`, unlike the firmware-only
`GStreamer-*-V4L2` table above. Results were deliberately kept as diagnostics:

| Probe | Result |
| --- | --- |
| H.264 `BA1_FT_C`, `BA1_Sony_D` | `1/2` passed; `BA1_Sony_D` remains a reproducible content mismatch. |
| HEVC `DBLK_G_VIXS_2`, `RAP_B_Bossen_2`, `SAO_G_Canon_3` | `0/3` passed; all returned content mismatches. |
| VP9 two basic vectors plus one resize vector | `2/3` passed; resize returned an accelerator error. |
| AV1 Chromium 8-bit, three vectors, opt-in translator | `0/3` passed; the experimental VA path returned decoder errors. |

The stock Fluster FFmpeg wrapper uses `-hwaccel vaapi` followed by a software
format filter and does not request an explicit VAAPI output pool. It is useful
for exposing timeout and surface-lifetime boundaries, but `iris-matrix.sh` is
the authoritative VA pixel oracle. A full 135-vector H.264 driver-path run was
started with a 5-second per-vector bound and safely stopped after repeated
cases still took roughly 30 seconds to terminate; its partial output is not
claimed as a suite result.

Two bounded stress checks explain the remaining H.264 signal. For
`BA1_Sony_D`, one-, two- and three-frame keyframe-aligned streams were
bit-exact, while the complete 17-frame stream produced 17 frames with
non-deterministic duplicate/out-of-order hashes. Increasing FFmpeg's surface
pool and toggling `V4L2_VA_COPY_SURFACES` did not remove it. `AUD_MW_E` returned
100 frames in 9 seconds with 63 bounded sync timeouts and 10 startup
backpressure records, but its frame hashes still differed from software. These
are open surface/firmware interaction diagnostics, not grounds for another
unverified production patch.

## Codex dynamic-resolution follow-up (2026-08-30, `codex/claude-followup`, primary target)

The next Claude investigation item was completed against the current Iris node
`/dev/video4` on the SM8550 tablet. `VIDIOC_ENUM_FRAMESIZES` reports the NV12
capture range `96x96 - 8192x8192`; the VA runtime probe returned the same
`MinWidth=96`, `MaxWidth=8192`, `MinHeight=96`, and `MaxHeight=8192` values.
The old hard-coded `32/2048` surface attributes are gone. The range is scoped
to the profile's eligible decoder devices and uses their common intersection.

The deployed driver passed the 48-frame VA matrix for H.264, VP9, and HEVC:
each codec matched the software framemd5 exactly, with strict EOS accounting
(`48 OUTPUT`, `49 CAPTURE`, one `LAST`, zero timeout or timestamp-miss
diagnostics). The two-context isolation probe also passed.

The Fluster VP9 dynamic vector
`vp90-2-14-resize-fp-tiles-1-8.webm` completed all 18 frames through VA. The
driver and native `v4l2vp9dec` outputs were byte-identical for every 432x240 and
3840x2160 segment. One 3840x2160 segment from both hardware paths differs from
the software decoder at the same byte offset, so that isolated difference is
recorded as deterministic firmware behavior rather than a VA-driver mismatch.
The backend now also drains and rebuilds stateful V4L2 queues when a VA client
reuses one context for a surface with a new geometry.

## Codex zero-copy experiment (2026-08-30, `codex/claude-followup`, primary target)

The current Claude route was extended with an opt-in `V4L2_VA_ZERO_COPY=1`
path. Stateful single-plane NV12 CAPTURE buffers can now import the per-surface
DMA-BUF directly; the V4L2 `Buffer` owns a duplicated fd and no longer maps a
kernel MMAP slot. Surface reuse requeues the imported buffer, while setup or
plane validation failures fall back to the existing MMAP plus stable-copy path.
The default remains unchanged and Chrome is not modified.

The Iris firmware assigns a decoded frame to any queued CAPTURE slot rather
than honoring a VA-surface index. The experiment therefore keeps one imported
slot in flight for H.264/VP9 and uses an extra scratch slot for STOP/LAST drain;
otherwise a frame can land in the wrong exported fd. HEVC/AV1 are rejected from
this experimental contract and automatically use the stable-copy path because
their reorder depth needs multiple simultaneously queued CAPTURE slots.

| Run | Result |
| --- | --- |
| Primary target, zero-copy H.264 matrix, 48 frames | `48/48`, exact software framemd5, `49 CAPTURE`, one `LAST`, zero timeout/timestamp misses. Trace confirms no `copy_surface_frame` for decoded CAPTURE. |
| Primary target, zero-copy VP9 matrix, 48 frames | `48/48`, exact software framemd5, strict EOS and zero timeout/timestamp misses. |
| Primary target, zero-copy HEVC matrix, 48 frames | Stable-copy fallback (`codec_reorder_contract`); `48/48` exact software framemd5 and strict EOS. |
| Primary target, zero-copy H.264 structure, 24 frames | all-I and IP/GOP12 pass; B=2/B=4 fail because the one-slot contract cannot satisfy Iris reorder depth. This is retained as a known opt-in limit, not a default-path regression. |
| Primary target, default MMAP structure, 24 frames | all-I, IP/GOP12, B=2 and B=4 all pass with exact software framemd5. |

The remote clean rebuild and `ldd -r` gate passed for every iteration. The
experiment removes the CAPTURE-to-stable-buffer memcpy only for the validated
no-B H.264/VP9 path; it is not yet a universal replacement for native
multi-slot zero-copy behavior.

After the first dynamic VP9 probe, the tablet powered off before the bounded
command returned and the SSH endpoint changed. A follow-up guard now detects a
dynamic-resolution request while DMA-BUF capture is active, releases the
experimental queue without a long drain, and rebuilds the context on MMAP plus
stable copy. This guard passed local static checks and remote compilation on
the earlier primary/secondary targets; hardware confirmation on a replacement
endpoint is still
pending because that address was not reachable during this run.

## Post-reset diagnosis and stable-path regression (2026-08-30, replacement target)

The replacement target was inspected read-only before any
decode run. The previous boot ended abruptly at `17:24:56` and the next boot
started at `17:25:41`; there is no `reboot.target`, `shutdown`, `OOM`, panic,
Oops, or watchdog record in the previous boot. The new boot also reported that
the old system and user journals were corrupted or uncleanly shut down. The
Qualcomm command line carried `bootinfo.pureason=0x80001` and
`bootinfo.pdreason=0x2`: the public Xiaomi bootinfo layout maps this to the
`HWRST` power-up bit plus the `OTHER` reset-reason bit, not a normal software
reboot, kernel panic, or watchdog code. `/sys/fs/pstore` was empty and no
`/proc/last_kmsg` was available, so the lower-level trigger remains unknown.

Immediately before the reset, `uperf-linux` recorded heavy load and a maximum
temperature of `92.7 C`, then returned to `51 C`; this makes a thermal/power
transient plausible but does not prove it. After reboot the battery reported
88% and `Discharging`, so an empty battery is not supported by the snapshot.

The remote `.so` left by the interrupted run was zero-filled (`invalid ELF
header`), which explains why the first post-reset VA probe stopped before
decoding. A single-thread clean rebuild produced a valid AArch64 shared object
and passed `ldd -r` with no undefined symbols. With `V4L2_VA_ZERO_COPY` and all
optional drain/trace switches explicitly unset, the stable MMAP/copy path then
passed:

| Check | Result |
| --- | --- |
| H.264 VA, 12 frames | 12/12 exact framemd5; 12 OUTPUT, 13 CAPTURE, one LAST; zero timeout/miss |
| VP9 VA, 12 frames | 12/12 exact framemd5; 12 OUTPUT, 13 CAPTURE, one LAST; zero timeout/miss |
| HEVC VA, 12 frames | 12/12 exact framemd5; 12 OUTPUT, 13 CAPTURE, one LAST; zero timeout/miss |
| Native AV1 V4L2 | GStreamer EOS |
| VA context isolation | two contexts created successfully |
| `v4l2-compliance` | 47/48; only the known stateful `V4L2_CID_MIN_BUFFERS_FOR_CAPTURE` control classification failed |

The device remained on the same boot throughout this regression. The opt-in
DMA-BUF zero-copy path was not run after the reset diagnosis.

## Zero-copy follow-up (2026-08-30, replacement target)

After the clean rebuild, the opt-in DMA-BUF path was exercised on the same
device with the default EOS and watchdog switches unset. Static no-B H.264 and
VP9 both passed 12-frame and 48-frame runs with exact software framemd5. The
48-frame traces each contain 48 OUTPUT, 49 CAPTURE (including one terminal
`LAST`), zero timeout/timestamp diagnostics, and zero `copy_surface_frame`
records; the trace shows the expected DMA-BUF import and single-slot CAPTURE
queue. HEVC with the switch enabled passed 12/12 as well, but correctly logged
`codec_reorder_contract` and used the stable copy path (`copy_surface_frame`
records present).

Dynamic-resolution probes were kept separate from the positive static oracle:

- `320x180 <-> 160x90` was rejected at the second geometry because `160x90`
  is below the node's declared minimum height of 96; only 10/30 frames were
  valid, so this is a capability-boundary result.
- The `432x240 <-> 848x480` VP9 sample produced the same CAPTURE errors and
  sequence failures with both zero-copy and MMAP VA paths. It is not evidence
  of a zero-copy-only regression and remains outside the positive oracle.
- A smaller `352x288 <-> 282x173` resize sample likewise failed in the MMAP
  VA path after its context handoff, with native V4L2 still reaching EOS.

These FFmpeg resize flows create a new VA context for each geometry, so the
context-local `dynamic_resolution` zero-copy fallback is not reached before
the firmware/input failure. Keep zero-copy disabled for dynamic-resolution
clients until a caller that reuses one VA context across the change is tested.
No Chrome or driver source change was made in this follow-up.

## Zero-copy batch-contract guard (2026-08-30, replacement target)

An opt-in probe with `V4L2_VA_ZERO_COPY=1` and `V4L2_VA_BATCH_SIZE=8` showed
that Iris can hold the aggregated OUTPUT until STOP when only one imported
CAPTURE slot is queued; the first `vaSyncSurface()` then failed with an empty
framemd5 output. This is the expected limit of the current one-slot ownership
model, not a default-path regression. The backend now requires the explicit
one-AU batch contract for DMA-BUF capture and logs
`stateful zero-copy fallback reason=batch_contract`, leaving the stable
MMAP/copy path active for larger throughput batches.

Repeating the same `BATCH_SIZE=8` command after the guard logged the expected
fallback, but the stable-copy batch run still failed before its first frame on
this Iris firmware/input combination. Larger batching therefore remains an
unsupported throughput experiment here; the guard's purpose is to prevent it
from being mistaken for a working zero-copy mode.

After deploying the guard, the explicit one-AU run (`V4L2_VA_ZERO_COPY=1`,
`V4L2_VA_BATCH_SIZE=1`) passed the complete 48-frame matrix on the replacement
target:
H.264, VP9, and HEVC each produced exact software framemd5 with strict
`49 CAPTURE / 48 OUTPUT / 1 LAST` accounting and zero timeout/timestamp-miss
diagnostics. H.264 and VP9 had zero `copy_surface_frame` records; HEVC had 48
stable-copy records plus the expected `codec_reorder_contract` fallback.

## Battery power comparison (2026-08-30, replacement target)

This section measures paced GStreamer/FFmpeg pipelines into fakesink, not a
browser, and ends by requiring an actual Chrome playback run before any
battery-life claim. That run is now recorded below under
[Browser battery comparison](#browser-battery-comparison-2026-09-02-chrome-151).

The comparison used the battery gauge exposed at
`/sys/class/power_supply/qcom-battmgr-bat`: `voltage_now` multiplied by the
absolute value of `current_now`, sampled once per second. Each run used the
same 1280x720 or 1920x1080, 30 fps, 60-second H.264 file with no B frames and
ran a 30-second real-time decode. The native path was GStreamer
`v4l2h264dec` with `capture-io-mode=4` (DMABUF capture). The VA paths used the
deployed driver with either stable copy (`V4L2_VA_ZERO_COPY=0`) or the
one-AU DMA-BUF experiment (`V4L2_VA_ZERO_COPY=1`); software used FFmpeg's
ordinary CPU decoder. Every run collected 10 seconds of pre- and post-run
baseline, 30 seconds of decode samples, temperature, load, and capacity.

The primary value below is the battery-side decode increment: run power minus
the midpoint of the pre/post baselines. It is not decoder-package power; the
screen and the rest of the tablet remain included. Values are mean +/- a
two-sided 95% t interval over independent runs.

Important interpretation: this is a paced `-re`/fakesink baseline, not a
browser playback result. It measures the work needed to sustain 30 fps while
allowing the decoder and compositor to sleep between frames, so it can hide
the cost of a busy software pipeline. Do not use the small real-time delta or
the runtime table below as a claim that software video playback costs only a
few tens of milliwatts.

| Workload | Native DMABUF | VA stable copy | VA zero-copy | Software |
| --- | ---: | ---: | ---: | ---: |
| 1280x720, n=5 | +42 +/- 16 mW | +50 +/- 21 mW | +64 +/- 41 mW | +92 +/- 55 mW |
| 1920x1080, filtered n=4/3/4/3 | +68 +/- 21 mW | +100 +/- 5 mW | +50 +/- 27 mW | +187 +/- 133 mW |

Absolute battery power during the 720p runs was 2.554 W (native), 2.554 W
(VA copy), 2.559 W (VA zero-copy), and 2.615 W (software). The 1080p filtered
means were 2.595 W, 2.601 W, 2.556 W, and 2.699 W respectively. Temperature
stayed between 30.5 and 31.0 C; capacity moved from 83% to 79% over the whole
experiment. All 20 runs at 720p and all 16 runs at 1080p returned zero;
the native and zero-copy traces were separately checked for the expected
capture mode and no VA decode errors.

Two 1080p samples were excluded before aggregation because their pre/post
baseline gap was about 7.4 W, a power-management transition unrelated to the
decoder. The raw CSV files and logs remain in
`~/Lab/iris-vaapi-lab/artifacts/power-compare-20260830/` on the tablet (and
the copied analysis inputs are under `~/Lab/Bridge/tmp/trash/power-compare-20260830/`).

This measurement does not demonstrate a statistically significant battery
advantage for the experimental zero-copy path over the native or VA-copy
paths at 720p; the memcpy removed by zero-copy is small compared with gauge
noise and whole-device background power. At 1080p the zero-copy result trends
lower than VA copy, but the filtered sample is still too small for a firm
claim. Software is consistently the highest path, while all hardware paths
remain in the same roughly 2.55-2.60 W whole-device band.

### CPU and unrestricted-load cross-check

FFmpeg's verbose stream mapping for the software command was
`h264 (native) -> wrapped_avframe (native)`, with no hardware input options.
During an additional real-time 720p run, the decoder process consumed about
1.7% of one CPU core for native V4L2, 4.4% for VA stable copy, 3.3% for VA
zero-copy, and 21.1% for the software decoder. This confirms that the small
real-time battery delta is not a software-decoder bypass.

To expose the cost hidden by real-time pacing, each path was also run in a
20-second unrestricted loop at 720p. Absolute battery power during that load
was 2.98 W (native V4L2), 3.04 W (VA zero-copy), and 7.23 W (software). The
loop was deliberately stopped by the harness (the FFmpeg paths returned their
expected signal-stop code), so these figures are a stress direction check,
not a playback estimate. They show that CPU software decoding can add several
watts when the content, frame rate, or browser pipeline keeps the decoder busy.

An independent software-only recheck on the same tablet used the same input
and sampling method. It measured 6.764 W during the 20-second unrestricted
decode, with 3.251 W and 3.639 W pre/post baselines; against their 3.445 W
midpoint, the software decode increment was about +3.32 W. The different
absolute baseline shows why one short run cannot provide a fixed watt number,
but the multi-watt software increment reproduces the direction and order of
magnitude of the original stress result.

### Runtime translation

At the end of the run the tablet reported `charge_full=8884000` uAh,
`state_of_health=94`, and 78% capacity. Using a conservative 3.85 V average
battery voltage gives about 25.0 Wh from the observed 78% down to a 5% reserve,
or 32.5 Wh from 100% down to that reserve. Dividing those energies by the
measured whole-device decode power gives the following fakesink baseline:

| Mode | 78% -> 5% | 100% -> 5% |
| --- | ---: | ---: |
| Native DMABUF, 720p | 9.8 h | 12.7 h |
| VA copy, 720p | 9.8 h | 12.7 h |
| VA zero-copy, 720p | 9.8 h | 12.7 h |
| Software, 720p | 9.5 h | 12.4 h |

These are not browser numbers: they include the measured screen/background
load but not Chrome composition, page activity, or network traffic. They are a
controlled low-load baseline, not a promise that software playback only costs
50-100 mW. The unrestricted-load cross-check demonstrates that a busy software
decoder can consume several additional watts. For a real browser estimate,
the decisive variables are codec, resolution, frame complexity, refresh rate,
panel brightness, and Wi-Fi; an actual Chrome playback run is required before
turning this table into a battery-life guarantee.

## Browser battery comparison (2026-09-02, Chrome 151)

The run the section above asks for. Google Chrome 151.0.7922.173 played one
1920x1080, 30 fps, High profile, level 4.0 H.264 file with no B frames, looped
from `file://` in kiosk mode on the tablet's own panel, with hardware VA-API
decode measured against Chrome's software decoder.

`test/iris-power-compare.sh` produced the collection and
`test/remote/power-analyze.py` aggregated it. Five interleaved ABBA blocks, ten
runs; each run is a 300-second measurement window after 20 seconds of warmup,
bracketed by 60-second idle baselines with a 30-second gap between runs. The
arms differ by one switch: the software arm adds
`--disable-accelerated-video-decode`. Everything else, including the launcher,
the LIBVA environment, the profile layout and the page, is identical.

### What the runs show

| Arm | Playback | Baseline | Increment | CPU | Decoder |
| --- | ---: | ---: | ---: | ---: | --- |
| Hardware, n=5 | 2.745 +/- 0.040 W | 2.458 W | +0.287 +/- 0.058 W | 0.205 cores | `VaapiVideoDecoder`, `kIsPlatformVideoDecoder=true` |
| Software, n=5 | 2.844 +/- 0.013 W | 2.443 W | +0.401 +/- 0.039 W | 0.490 cores | `FFmpegVideoDecoder`, `kIsPlatformVideoDecoder=false` |

The statistic is the per-block paired difference, not two pooled means:

```text
software - hardware, playback power:  +0.0988 +/- 0.0448 W   (n=5 blocks)
software - hardware, increment:       +0.1145 +/- 0.0861 W
```

Both intervals are two-sided 95% t intervals and both exclude zero. Hardware
decode costs about 99 mW less whole-device power, roughly 3.5% of the 2.8 W the
tablet draws while playing, and it halves the browser's CPU.

All ten runs were valid and none was dropped. The two arms' idle baselines
landed 15 mW apart (2.458 W against 2.443 W), which is the control that makes
the 99 mW separation readable at all.

### Provenance per run

Playback alone proves nothing here, because both arms play the video. Each run
required the Media domain to name the decoder and the frame counters to advance
inside the window. Independently of Chrome's self-report, every hardware run
had exactly one process holding the resolved Iris node and it was the GPU
process; every software run had none.

Frame accounting was 8991-8997 decoded per 300-second window against a 9000
frame ideal, with one dropped frame across all ten runs. The backlight was
pinned at 1024/2047 and recorded per sample: every run reported a single level
for its whole duration. Thermals stayed between 36.9 C and 37.7 C mean, 39.4 C
peak. Capacity fell from 63% to 52% across the collection; no external supply
came online in any sample.

Driver `f879e5ea417c770a`, source commit `0ced73ee3f3b`, clean tree, kernel
`7.2.2-sm8550-gad75da3`, decoder node `/dev/video0`. The artifacts are under
`~/Lab/iris-vaapi-lab/artifacts/power-browser-20260902b/` on the tablet, with
`report.json` holding the full per-run record.

### Runtime translation

`charge_full` reads 8931000 uAh. At a conservative 3.85 V average that is about
34.4 Wh, or 32.7 Wh from full down to a 5% reserve.

| Mode | Playback time from 100% to 5% |
| --- | ---: |
| Hardware VA-API | 11.9 h |
| Software | 11.5 h |

About 25 extra minutes of 1080p30 playback per charge. That is the honest size
of this effect for this clip, at this brightness, with no network traffic and a
page that does nothing but hold a video element.

### What this does not say

The earlier fakesink section measured several watts of difference under an
unrestricted decode loop. Both results are real and they answer different
questions: a browser playing 30 fps is a paced workload that lets the CPU idle
between frames, so the software decoder never runs flat out. Content at a
higher resolution or frame rate, a heavier page, or a device with a dimmer
panel would all widen the gap; this run does not measure any of them.

The comparison is also whole-device. The panel at 1024/2047 accounts for most
of the 2.4 W baseline, which is why a 99 mW decode difference is 3.5% of the
total rather than 3.5% of the decoder.

Only H.264 was measured. HEVC is a qualified VA path but was not part of this
collection, and VA VP9 remains withdrawn on this target.

Only Google Chrome was measured, and that is not an arbitrary choice. Fedora's
Chromium 151.0.7922.169, launched through the same `iris-vaapi-browser` with
the same environment, reports `V4L2VideoDecoder` with
`kIsPlatformVideoDecoder=true` and its GPU process holds the Iris node
directly. It decodes in hardware through Chromium's own native V4L2 stack and
never loads this driver, so a Chromium arm would measure that stack rather than
this project. A Chromium hardware-versus-software comparison is still worth
running; it just answers a different question and needs its own section.

### First collection, discarded

An earlier collection the same night lost all ten runs. GNOME's `idle-dim` was
not among the locked settings, so the panel dimmed partway through the first
block, and `brightness_start` was captured once before the first block rather
than per run, which then reported every later run as broken against a value it
never had. The runs were internally consistent, with hardware increments in
0.349-0.403 W and software in 0.468-0.658 W, but by the harness's own rules
they were not comparable and they are not used here.

`idle-dim` is now locked, and the backlight is a sampled column rather than two
edge readings, so a panel that dims and recovers inside a run cannot pass.
`test/iris-power-report-check.sh` holds the analyser to that rule.
