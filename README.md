# V4L2 libVA Backend
This libVA backend is designed to work with the [Video for Linux Memory-To-Memory API](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-mem2mem.html) that is used by a number of video codecs drivers, in particular SoCs found on SBCs.
After an initial implementation by Bootlin, development on this backend ceased before the Stateless V4L2-M2M API it uses became stable.
Development has since been picked up again, see [#Status] for details.

## Building
The project is built using meson:
```
meson setup build
meson compile -Cbuild
```

## Usage
Applications using the backend can be launched by adding the build directory to the libVA driver path, and, optionally, setting the driver name to load:
```
LIBVA_DRIVERS_PATH=<project dir>/build/src LIBVA_DRIVER_NAME=v4l2 vainfo
```

Alternatively, the library can be installed to the default driver path:
```
meson install -Cbuild
```

The driver probes the system for appropriate V4L2 devices, advertising all of their capabilities.
This can be overriden by explicitly specifying a device pair to use:
```
export LIBVA_V4L2_VIDEO_PATH=/dev/videoX LIBVA_V4L2_MEDIA_PATH=/dev/mediaY
```

For Qualcomm Iris stateful codecs, each OUTPUT buffer contains one access unit
by default. This preserves exact timestamp and display-order association when
Iris returns several CAPTURE frames for a reordered stream. Set
`V4L2_VA_BATCH_SIZE=2..8` only for throughput experiments; `1` is the safe
diagnostic and production setting.
Keep `V4L2_VA_TRACE` disabled during normal playback because it is intentionally
verbose.

The backend allocates a stable system DMA-BUF for each exported VA surface and
copies completed Iris CAPTURE frames into a private CPU snapshot, publishes
that snapshot before returning the rotating V4L2 CAPTURE slot, and retains a
`vaSyncSurface()` retry path if publication fails. This keeps an imported
buffer immutable while it is eligible for composition and supports clients
whose VA surface pools outlive the rotating CAPTURE indices. The stable
snapshot path is the default; set `V4L2_VA_COPY_SURFACES=0` only for clients
that explicitly manage rotating V4L2 CAPTURE buffer lifetime themselves.
The complete CAPTURE pool is queued by default so firmware reorder and EOS
drain cannot run out of free slots.

Stateful Iris may hold a reordered B/P frame until a later AU is submitted.
`vaSyncSurface()` therefore uses a bounded 1000 ms wait by default, allowing
an asynchronous producer to continue after a stalled reorder point instead of
deadlocking a looping or seeked stream. Override it with
`V4L2_VA_SYNC_TIMEOUT_MS=100..60000` when diagnosing another application.
CAPTURE frames are matched to the exact monotonic timestamp copied into their
OUTPUT AU. A frame that arrives after its surface was dropped is discarded and
the CAPTURE slot is requeued; binding it to the nearest live timestamp causes
old frames to flash in a newer surface and shifts the rest of the stream.

Some VA clients do not issue a separate end-of-stream call after their final
access unit. On Iris, enable the opt-in idle drain with
`V4L2_VA_STATEFUL_EOS_DRAIN=1` (the legacy `V4L2_VA_EOS_DRAIN=1` name is also
accepted). The default 400 ms threshold is based on measured client batch
gaps and can be tuned with `V4L2_VA_EOS_IDLE_MS=10..5000`. The watchdog only
arms after eight submitted AUs and one completed frame, so codec startup gaps
cannot be mistaken for EOS. All stateful clients still perform synchronous
STOP draining during context teardown.

Note that some applications need further configuration to load the library.
In particular, gstreamer based applications have a whitelist for supported drivers, that can be disabled manually (`GST_VAAPI_ALL_DRIVERS=1`).

## Status
The project currently supports these codecs: MPEG2, H264, VP8, Qualcomm Iris
stateful VP9, and stateful HEVC on nodes that advertise the corresponding V4L2
formats. Set `V4L2_VA_DISABLE_HEVC_STATEFUL=1` to disable the generated
parameter-set HEVC path on firmware with a non-standard contract.
VP9 support depends on a part of gstreamer that is not likely to be present in the version shipped by your distribution.
The implementation has been tested using Intel's [vaapi-fits](https://github.com/intel/vaapi-fits) on an RK3399, which is supported by the `hantro` and `rockchip` drivers.
Feedback on results for other platforms are very welcome, do not expect the library to simply work smoothly though.
This fork experimentally detects Qualcomm Iris stateful H.264 when the device advertises
`V4L2_PIX_FMT_H264` rather than `V4L2_PIX_FMT_H264_SLICE`. Queue setup is lazy
so a client can create a VA context before allocating render targets. The
stateful VP9 path is enabled when the Iris device advertises `V4L2_PIX_FMT_VP9`.
AV1 is not enabled by default: VA-API supplies AV1 tile payloads while Iris'
stateful node requires complete OBU temporal units. An incomplete experimental
translator can be selected with `V4L2_VA_ENABLE_AV1_STATEFUL=1`, but it is not
part of the supported matrix; use the native V4L2 or codec2 stack for AV1. The HEVC
implementation synthesizes parameter sets and short-term reference-picture
sets from VA metadata and passes the target Iris 48-frame content/EOS matrix.
Clients without Linux HEVC codec support cannot select this profile.
`V4L2_VA_RESET_ON_IDR=1` enables an experimental queue
reset on detected new IDRs for debugging seek/replay behavior; it is not
recommended as the default on kernels with different flush semantics.
