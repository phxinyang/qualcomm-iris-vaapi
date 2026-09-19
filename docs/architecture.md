# Architecture

This document is the contract for the rebuild started on 2026-09-20 (tag
`pre-rebuild-20260920`, branch `iris/rebuild`). Every later phase, every
review and every task handed to a worker agent is scoped against it. When
code and this document disagree, one of them is wrong and the disagreement
is a bug to file, not a detail to paper over.

## 1. What this driver is for

Stock Google Chrome on arm64 Linux is built with `use_vaapi=true` and
`use_v4l2_codec=false`: the only hardware video decode entry point compiled
into the binary is libva. The same is true of every prebuilt Electron
application and of Firefox's FFmpeg hwaccel. Fedora's own Chromium is built
with `use_v4l2_codec=true`, talks to the Iris node directly through
Chromium's `V4L2StatefulVideoDecoder`, and never loads this driver.

So the product is: **a libva backend that turns VA-API decode calls into
Qualcomm Iris V4L2 stateful M2M sessions, good enough that Google Chrome
selects `VaapiVideoDecoder` and plays 1080p H.264/HEVC with zero dropped
frames and no CPU-side copy on the hot path.** Everything else (FFmpeg,
GStreamer, mpv, Firefox, Electron) follows from the same mechanism and is
welcome, but Chrome is the acceptance client.

Non-goals: being a generic backend for stateless request-API decoders (that
is upstream `mxsrc/libva-v4l2`'s job and stays on `master`), encoding,
video post-processing, and shipping a custom Chromium.

## 2. Target and kernel contract

The qualified target is a Snapdragon SM8550 tablet (`sheng`) on Fedora 44
with a self-built kernel from `ianchb/sm8550-mainline` (Iris is the
`qcom-iris` module, HFI gen2). Because the kernel is self-built, kernel-side
fixes are in scope.

Two Iris kernel changes from `strongtz/libva-v4l2` (Radxa, Xilin Wu) apply
to `sheng-7.2.2` with fuzz and are adopted as part of the target:

| Patch | What it changes | Why the driver needs it |
| --- | --- | --- |
| decode-order output (`V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE`, delay 0) | Firmware returns pictures in decode order instead of display order | A VA client may `vaSyncSurface` a picture before submitting the ones it is reordered behind. Display-order firmware withholds it, and the only userspace escape is a timeout plus STOP/START drain. In decode order every submitted picture completes independently. |
| CAPTURE pool up to 64 buffers | Raises the vb2 `max_num_buffers` for the gen2 decoder CAPTURE queue | Threaded VA clients keep more than 32 surfaces; a driver-owned pool needs headroom above the client's pool. |

The driver **must not require** the patches. It probes for the display-delay
control at session open and runs in one of two session modes:

- **decode-order** (control present): sync waits are bounded by the normal
  timeout and a timeout is a hard error.
- **display-order** (control absent, stock Fedora kernel): sync waits are
  bounded; on timeout the session issues STOP, waits for LAST, and START,
  which flushes the reorder window. This is the old driver's recovery path
  and the only reason it survives. No idle-time watchdog thread: a timer that
  guesses end-of-stream from silence is how the previous tree got a thread
  waking every 25 ms in every context.

Kernel version, Iris module hash and firmware identity are part of every
hardware result (`test/remote/provenance.py` already records `uname`; the
module hash is added in Phase 1).

## 3. The one ownership model

The previous tree carried five CAPTURE ownership modes selected by
environment variables. The rebuild has one model with two publish policies
chosen per surface from observed client behaviour.

### 3.1 Objects

```
Session (one per VA context, one fd)
  owns  OUTPUT pool   : 4 MMAP slots, one access unit each
  owns  CAPTURE pool  : N MMAP slots, N = clamp(surfaces + 4, 32, kernel max)
                        every slot EXPBUF'd once at allocation -> Frame
  owns  pending map   : token -> Surface (token = strictly increasing
                        CLOCK_MONOTONIC-shaped timestamp copied by the kernel
                        from OUTPUT to CAPTURE; the only association key)

Frame  (shared_ptr)   : one CAPTURE slot's DMA-BUF fd, layout, index, queued flag
Surface               : width/height/fourcc
                        memory : shared_ptr<Frame> | StableBuffer | none
                        persistent_export : bool
                        token, pending, status
```

### 3.2 Slot lifetime

A CAPTURE slot is in exactly one state, and the state is the `shared_ptr`
use count, not a flag:

| use count | state | meaning |
| --- | --- | --- |
| 1 (pool only), `queued` | Queued | firmware owns it |
| 1 (pool only), not `queued` | Free | will be QBUF'd on the next service pass |
| > 1 | Held | a Surface (or a GPU-copy view) is using its pixels |

`Session::requeue()` QBUFs every Free slot. A Surface that adopts a Frame
holds it until the surface is reused (`vaBeginPicture` on it), destroyed,
or the client releases its exported handle. There is no "requeue predicate":
releasing the reference is the requeue.

### 3.3 Publish policies

When a CAPTURE completion arrives with a token that has a pending Surface:

- **Direct**: the surface takes the `shared_ptr<Frame>`. Zero copies. This
  is the policy for clients that export or map a surface only after it has
  been decoded (FFmpeg, GStreamer, mpv, `vaGetImage` users). Every export
  after this point hands out the slot's own DMA-BUF.
- **Copy**: the surface already owns a `StableBuffer` because the client
  exported it *before* its first decode (Chrome pre-exports its whole
  NativePixmap pool and caches the fd for the surface's lifetime). The
  completion is copied into that buffer and the slot is released
  immediately. The copy engine is **GPU blit** (EGL/GLES on Adreno, both
  sides imported as DMA-BUF, no CPU touch) by default, **CPU memcpy** as
  fallback and for diagnostics. Before writing, wait on the destination
  DMA-BUF's implicit fence (`poll(POLLOUT)`): the compositor may still be
  sampling the previous frame from that buffer.

`persistent_export` is set by `vaExportSurfaceHandle`, `vaAcquireBufferHandle`
or a `DRM_PRIME_2` import when the surface has no token yet. It is never set
by an environment variable. `V4L2_VA_PUBLISH=copy|direct` exists only to
force a policy in diagnostics and is not read on the hot path.

### 3.4 Stable buffers

Persistent-export surfaces need memory that the GPU can sample and Iris can
be copied from. Allocation order: MSM GEM cached-coherent, MSM GEM
write-combined, system DMA heap. NV12 (or P010 for 10-bit), 64-byte-aligned
pitch, one object, two layers, linear modifier. The fd, pitch and offsets
never change for the surface's lifetime.

### 3.5 What is gone

Pre-binding surface *i* to CAPTURE ordinal *i*; the DMA-BUF import path
that queued client buffers into CAPTURE; the scheduled-slot mode; the
no-copy mode; batch sizes above one AU; reset-on-IDR; STREAMOFF-pair reset;
the EOS watchdog thread; `retired_contexts` reaping (a surface holds a
`shared_ptr<Frame>`, so nothing dangles when its context dies);
per-thread current-surface maps; `reference_wrapper` into device vectors.

Client-owned CAPTURE buffers (true zero-copy for Chrome) is a research
track, not a policy: it requires evidence that Iris fills CAPTURE buffers in
QBUF order under decode-order output and that references live only in the
firmware's internal DPB. It is listed in §9 with its acceptance test and
is not scheduled.

## 4. Session state machine

```
Closed --open()--> Configured(OUTPUT S_FMT, DISPLAY_DELAY probe, OUTPUT REQBUFS 4, OUTPUT STREAMON)
Configured --first submit + SOURCE_CHANGE--> Streaming(CAPTURE G_FMT/S_FMT, G_SELECTION, MIN_BUFFERS, REQBUFS N, EXPBUF, QBUF all, STREAMON)
Streaming --SOURCE_CHANGE (DRC)--> Draining(STOP) --LAST--> Reconfiguring(STREAMOFF CAPTURE, REQBUFS 0, drop pool, re-enter Streaming with new geometry)
Streaming --sync timeout in display-order mode--> Draining(STOP) --LAST--> Streaming(START)
Streaming --destroyContext--> Finishing(wait pending, STOP, LAST) --> Closed
any --EIO/EPIPE on DQBUF, firmware error--> Failed (every pending surface -> DECODING_ERROR; no retries on this fd)
```

Ordering facts learned on this firmware and kept verbatim:

- OUTPUT `S_FMT` first, CAPTURE format is only meaningful after
  `SOURCE_CHANGE`.
- Every CAPTURE slot is queued before CAPTURE `STREAMON` and stays queued
  except while Held.
- One access unit per OUTPUT buffer with a strictly increasing timestamp;
  SPS/PPS repeated on every H.264 AU; HEVC VPS/SPS/PPS synthesised from VA
  parameters.
- `EBUSY` on `DECODER_CMD_START` with an OUTPUT buffer still queued is a
  recoverable ordering condition (retry after OUTPUT drain), not a dead
  instance. `EIO` and `EPIPE` are.
- A CAPTURE buffer with `bytesused <= data_offset` or the ERROR flag and a
  zero timestamp is a firmware error marker: count it, requeue it, never
  publish it.
- VP9 in-context resolution change and VP9 context recreation rebooted the
  device on display-order firmware; VP9 bring-up (Phase 5) runs under
  `scripts/iris-experiment-guard.sh` only.

## 5. Module layout

```
src/
  va/                 VA entry points; thin, no ioctl, no policy
    driver.cc         vtable, object tables, init/terminate, one recursive mutex for tables only
    config.cc         profiles/entrypoints from the capability table
    surface.cc        create/destroy/sync/query/export/derive/getImage
    picture.cc        begin/render/end -> codec translator -> Session::submit
    image.cc          images and subpictures (minimal)
  iris/               the only code that touches /dev/videoN
    device.{h,cc}     Device interface (ioctl/poll/mmap virtuals) + real implementation;
                      discovery by `iris_driver` QUERYCAP name, decoder node only
    session.{h,cc}    Session: the only ioctl sequencer (state machine of §4)
    slots.{h,cc}      Frame + pool helpers (allocate, export, requeue)
    publish.{h,cc}    PublishPolicy: Direct | Copy(engine)
    gpu_copy.{h,cc}   EGL/GLES blit, weak view cache keyed by Frame*
    alloc.{h,cc}      StableBuffer allocation (MSM GEM -> dma-heap)
  codec/              VA parameters -> bytestream the firmware accepts; pure functions
    bitwriter.h
    h264.cc           SPS/PPS synthesis, AU assembly
    hevc.cc           VPS/SPS/PPS/RPS synthesis (fix PTL from profile, LTR, non-uniform tiles)
    vp9.cc            raw frame passthrough, superframe for invisible frames
    av1.cc av1_obu.cc VA tiles -> OBU temporal unit
  util/
    options.{h,cc}    every environment variable, read once at init
    trace.{h,cc}      stable trace vocabulary (§8)
    error.h
test/
  unit/               host-only: slot lifetime, publish policy with FakeDevice, session ordering
                      with scripted DQBUF/event sequences, codec golden bitstreams, options
  hardware/           tablet: matrix (framemd5), structure matrix, DRC, soak, browser acceptance,
                      zero-copy trace audit
  remote/             deploy, provenance, cdp
```

`va/` may include `iris/` and `codec/`; `iris/` and `codec/` never include
`va/` or each other. `context.cc` and the stateless request-API classes do
not survive; MPEG2/VP8 translators leave with them (no Iris node advertises
those formats).

## 6. Environment surface

Read once in `VA_DRIVER_INIT_FUNC`, stored in `Options`, passed down.

| Variable | Default | Purpose |
| --- | --- | --- |
| `LIBVA_V4L2_VIDEO_PATH` | probe | Decoder node override (kept for launcher/packaging compatibility). |
| `V4L2_VA_TRACE` | off | Trace to stderr. |
| `V4L2_VA_SYNC_TIMEOUT_MS` | `2000` | Bounded `vaSyncSurface` wait; first frame of a cold session gets `30000`. |
| `V4L2_VA_COPY` | `gpu` | Copy engine for persistent exports: `gpu` or `cpu`. |
| `V4L2_VA_PUBLISH` | `auto` | Force `copy` or `direct` for diagnostics. |
| `V4L2_VA_DUMP` | off | Directory for submitted access units. |

Six switches, down from eighteen. `test/iris-env-doc-check.sh` keeps README
and `options.cc` in sync by name, not by sentence.

## 7. Threading and locking

- `Driver::mutex` guards the object tables and nothing else. It is never
  held across a wait, a drain or an ioctl that can block.
- `Session::mutex` serialises submit/service/sync/finish for one context.
  `sync` releases it while polling the fd.
- No background threads in the driver. `querySurfaceStatus` and
  `syncSurface` drive the service pass (`pump`); a client that never calls
  either never needs completions.
- VA entry points are reentrant per context from different threads only in
  the way libva already allows; `beginPicture..endPicture` state lives in the
  Context, not in thread-local storage.

## 8. Trace vocabulary

Hardware checkers (`iris-eos-check.sh`, `iris-ending-check.sh`,
`iris-context-retirement-check.sh`, `iris-zero-copy-suite.sh`) parse trace
lines. These records are an API; renaming one requires updating its
consumer in the same commit:

```
session open node=<path> mode=decode-order|display-order caps=<N>
submit token=<n> bytes=<n> output=<i>
capture token=<n> index=<i> flags=<hex> publish=direct|copy-gpu|copy-cpu|drop
capture error token=<n> index=<i>                (never published)
capture last
drain begin reason=eos|timeout|drc|finish
drain complete last=1
sync timeout token=<n> pending=<n> queued=<n> held=<n>
session reconfigure <w>x<h> -> <w>x<h>
session failed errno=<n> op=<ioctl>
va create_context id=<n>
va destroy_context id=<n> pending=<n>
va export_surface id=<n> persistent=0|1
```

## 9. Phases and gates

Each phase is independently mergeable and leaves a shippable driver. Each
ends with a tag. Gates are commands, not opinions.

| Phase | Deliverable | Gate | Rollback |
| --- | --- | --- | --- |
| 0 | Branch layout, baseline, acceptance script, this document, tests unpinned from source text | `sh test/run-static-checks.sh`; driver bytes identical; `TEST-RESULTS.md` baseline recorded (done) | tag `pre-rebuild-20260920` |
| 1 | Kernel: both Iris patches applied to `sheng-7.2.2`, `qcom-iris.ko` rebuilt and installed with the previous module kept | `v4l2-ctl --list-ctrls` shows `display_delay*`; CAPTURE max 64; acceptance H.264+HEVC pass on the unchanged driver; boot ID unchanged | restore saved `.ko.zst`, `depmod`, reboot |
| 2 | `src/iris/` core (Device, Session, slots, publish, alloc) with FakeDevice unit tests; not yet wired | `meson test` unit suite green on host; scripted sequences: decode-order, display-order timeout drain, DRC, LAST, ERROR marker, EBUSY retry, EIO fail | none needed (unwired) |
| 3 | VA glue rewired to the new core for H.264 + HEVC; old `context.cc`/`surface.cc` stateful paths, watchdog, retired contexts, env knobs deleted | host units; `iris-matrix.sh` 48/48 exact framemd5 both codecs; structure matrix; `iris-dynamic-resolution.sh`; `iris-concurrency-soak.sh`; browser acceptance both codecs on patched **and** unpatched module; FFmpeg trace shows `publish=direct` and zero copies | tag `phase-2` |
| 4 | GPU blit copy engine for persistent exports | acceptance; GPU-process CPU time per frame lower than CPU copy; `V4L2_VA_COPY=cpu` still passes | `V4L2_VA_COPY=cpu` default flip, tag `phase-3` |
| 5 | VP9 (decode-order, superframe for invisible frames) and AV1 (OBU translator) on the new core, advertised only when the matrix passes | framemd5 vs software for both; 100 context recreations and 100 DRC switches under the experiment guard with boot ID unchanged; capability JSON updated from evidence | keep profiles unadvertised, tag `phase-4` |
| 6 | Delete stateless/request-API inheritance, MPEG2/VP8, gstreamer dependency, media-controller walk; harness trim; packaging and docs | static checks; package build; acceptance from the installed package | tag `phase-5` |

Research track (unscheduled): client-owned CAPTURE buffers. Acceptance test
before any code: a standalone V4L2 program that, under decode-order output,
queues N distinct DMA-BUFs, submits N AUs, and proves by content hash that
frame *k* landed in the *k*-th queued buffer for 1000 frames including
B-frame streams. Without that proof the policy does not exist.

## 10. Invariants (the ones tests exist for)

1. A CAPTURE completion is published to at most one surface, found only by
   exact token match. Unknown tokens are requeued and counted.
2. A slot is QBUF'd only when its use count is one.
3. A persistent export's fd/pitch/offsets never change after the first
   export; pixels are written only after the buffer's implicit fence
   signals.
4. `vaSyncSurface` returns `SUCCESS` only after the surface's token has been
   published; `DECODING_ERROR` if the firmware flagged it; `TIMEDOUT` after
   the bound (decode-order) or after drain still leaves it pending
   (display-order). Never returns `SUCCESS` for pixels that are not there.
5. No VA entry point sleeps while holding `Driver::mutex`.
6. The node is found by `iris_driver` name and decoder capability; a fixed
   `/dev/videoN` appears nowhere in code, launcher or packaging.
7. Every environment variable is read exactly once, in one file.
8. A firmware error marker (ERROR flag, zero timestamp, or empty payload)
   is never published.

## 11. How work is divided

Claude owns this document, the `iris/` core, the session state machine and
every gate decision. Worker agents (`agy`) receive bounded tasks with a file
allowlist, a do/don't list and the exact verification command; they work in
a worktree, never commit, never deploy to the target, and every diff is
reviewed against §3 to §10 before it lands. Factual claims from a worker
(kernel behaviour, Chromium behaviour, upstream status) are re-verified
before being relied on.
