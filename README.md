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

For the clean target rebuild, symbol gate, and durable tablet lab workflow,
see [`docs/build-and-deploy.md`](docs/build-and-deploy.md).

## Usage
Applications using the backend can be launched by adding the build directory to the libVA driver path, and, optionally, setting the driver name to load:
```
LIBVA_DRIVERS_PATH=<project dir>/build/src LIBVA_DRIVER_NAME=v4l2 vainfo
```

Alternatively, a packaging build configured with the system prefix installs
the driver, browser launcher, and desktop entries together:
```
meson setup build --prefix=/usr
meson compile -C build
sudo meson install -C build
```

For direct deployment, prefer the helper below. It reads libva's canonical
`driverdir` from `libva.pc`, so the driver does not accidentally land under
`/usr/local/lib*/dri` while the system libva only scans `/usr/lib*/dri`:

```
sudo ./scripts/install-system.sh build
```

The install helper records the artifact hash and source identity under
`/usr/local/share/iris-vaapi/install-manifest.txt`; remove that exact install
with `sudo ./scripts/uninstall-system.sh`, which restores a previously backed
up VA driver and refuses to remove a modified artifact.

For a system that should decode in hardware on first boot, build a package
rather than running the helper by hand. `packaging/fedora/libva-v4l2-iris.spec`
and `packaging/arch/PKGBUILD` both install through the same helper, so the
packaged layout cannot drift from the lab one, and both require `v4l-utils`
because the launcher resolves the decoder node by driver name. See
[`docs/build-and-deploy.md`](docs/build-and-deploy.md).

The helper also installs `iris-vaapi-browser` and four desktop entries for
Chromium and Google Chrome. The normal entries are VA-only: they use Wayland,
leave the packaged browser's video-decoder feature selection at its tested
default, dynamically select the `iris_driver` decoder node, and intentionally
do not enable Vulkan, ANGLE Vulkan, or unsafe WebGPU. The separately named
Vulkan/WebGPU entries are an isolated experiment and may fall back to software
video decode. Neither path sets a global `LIBVA_DRIVERS_PATH` or stores a
`/dev/videoN` number.

See [`docs/build-and-deploy.md`](docs/build-and-deploy.md) for installation,
desktop migration, and browser decoder acceptance checks.

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

For a power/throughput experiment, `V4L2_VA_ZERO_COPY=1` together with
`V4L2_VA_ZERO_COPY_CONTRACT=h264-no-b-v1` keeps the driver-allocated
MMAP CAPTURE pool deep and hands each completed slot to its timestamp owner
for direct display: no per-surface DMA-BUF import. A completed slot stays
held until VA reuses the surface (beginPicture requeues it), and
`vaExportSurfaceHandle` exports the held slot itself. Surfaces exported
before their first decode (Chromium pre-exports its pool) keep displaying
that original handle, so the driver also refreshes the pre-exported stable
snapshot from the adopted slot; surfaces with no stable export stay fully
zero-copy with no CAPTURE-to-stable memcpy. The experiment is restricted to single-plane NV12 no-B H.264
(one AU per OUTPUT) and automatically falls back to MMAP plus the stable
copy outside that contract. It is disabled by default and is not required
for Chrome or other clients.
H.264 streams with B-frame reordering must leave this switch off, while
HEVC and AV1 stay on the stable-copy path. The trace records
`stateful zero-copy direct`, per-frame `zero-copy direct hold`, and direct
`va export_surface` exports; any timestamp miss or error recycles the slot
instead of showing another surface's pixels.
If a stateful stream requests a dynamic-resolution reconfiguration while the
experiment is active, the context drops that DMA-BUF queue and rebuilds on
MMAP/copy for the new geometry.
The experiment also requires one AU per OUTPUT buffer: when
`V4L2_VA_BATCH_SIZE` is greater than one, the backend keeps the stable
MMAP/copy path instead of risking an ambiguous multi-frame CAPTURE ownership
mapping.
If the ownership contract is absent or does not match exactly, the request
falls back to the stable path. This prevents an arbitrary VA client from
silently enabling zero-copy for a B-frame or dynamic stream.
The qualification entry point is `test/iris-zero-copy-suite.sh`; run it through
`scripts/iris-experiment-guard.sh` and keep its direct path opt-in until the
multi-slot ownership contract is independently proven.

Surface size attributes are read from the selected V4L2 capture format with
`VIDIOC_ENUM_FRAMESIZES`; the VA limits therefore follow each decoder's real
minimum and maximum instead of assuming a 2048-pixel ceiling. Stateful clients
may also reuse one VA context across an H.264 resolution change: the backend
drains the old queue and rebuilds its V4L2 capture/output pools for the new
surface geometry before submitting the next access unit. VP9 VA capability is
withdrawn on the qualified Iris target because both source changes and repeated
VA context recreation reboot the device.

Stateful Iris may hold a reordered B/P frame until a later AU is submitted.
`vaSyncSurface()` therefore uses a bounded 2000 ms wait by default, allowing
an asynchronous producer to continue after a stalled reorder point instead of
deadlocking a looping or seeked stream. Override it with
`V4L2_VA_SYNC_TIMEOUT_MS=50..60000` when diagnosing another application. When
no override is set, the first frame of a cold stateful sequence gets a 30000 ms
startup allowance for firmware bring-up; later frames use the normal bound.
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

## Environment variables

Every switch the driver reads is listed here; `test/iris-env-doc-check.sh`
fails the build if this table and the source disagree in either direction.
Only the first group is intended for production use.

| Variable | Default | Purpose |
| --- | --- | --- |
| `LIBVA_V4L2_VIDEO_PATH` | probe | Use a specific decoder node instead of probing. |
| `LIBVA_V4L2_MEDIA_PATH` | probe | Media node paired with the above; required when the video path is overridden on a stateless driver. |
| `V4L2_VA_BATCH_SIZE` | `1` | Access units per OUTPUT buffer. Values above `1` are throughput experiments only; see the note above on display-order association. |
| `V4L2_VA_COPY_SURFACES` | `1` | Publish each frame through a stable per-surface DMA-BUF. Set `0` only for clients that manage rotating CAPTURE lifetime themselves. |
| `V4L2_VA_SYNC_TIMEOUT_MS` | `2000` | Bounded `vaSyncSurface()` wait, `50..60000`; without an override, the first cold-start frame may wait up to `30000` ms. |
| `V4L2_VA_STATEFUL_EOS_DRAIN` | off | Opt-in idle drain for clients that never signal end of stream. |
| `V4L2_VA_EOS_DRAIN` | off | Legacy spelling of the above, still honoured. |
| `V4L2_VA_EOS_IDLE_MS` | `400` | Idle threshold for that watchdog, `10..5000`. |
| `V4L2_VA_DISABLE_HEVC_STATEFUL` | off | Withdraw the generated parameter-set HEVC path. |
| `V4L2_VA_ENABLE_AV1_STATEFUL` | off | Expose the experimental AV1 translator. Not part of the supported matrix. |

Diagnostics. These exist to investigate firmware behaviour and are not
supported configurations:

| Variable | Default | Purpose |
| --- | --- | --- |
| `V4L2_VA_TRACE` | off | Verbose per-buffer trace on stderr. Any value enables it. Leave off during normal playback. |
| `V4L2_VA_DUMP` | off | Directory to write decoded frames into. |
| `V4L2_VA_DUMP_BATCH` | off | Directory to write each submitted access unit into. |
| `V4L2_VA_CAPTURE_SCHEDULED` | off | Let the queue service own CAPTURE slot rotation instead of binding a slot to the surface. |
| `V4L2_VA_RESET_OUTPUT_STREAM` | off | Use a STREAMOFF/STREAMON pair on both queues when restarting a stateful sequence, instead of requeueing in place. |
| `V4L2_VA_RESET_ON_IDR` | off | Reset the queues on every detected IDR. Measured to fire on ordinary mid-stream scene-change IDRs and cascade into timeouts; do not enable by default. |
| `V4L2_VA_ZERO_COPY` | off | Experimental no-B H.264 single-plane NV12 DMA-BUF CAPTURE path with one-AU batching. Keeps a surface's buffer pinned until reuse; setup or batch-contract failures fall back to MMAP. |
| `V4L2_VA_ZERO_COPY_CONTRACT` | off | Required exact value `h264-no-b-v1` for the validated zero-copy ownership contract; any other value selects stable MMAP/copy. |

## Status
The project currently supports these codecs: MPEG2, H264, VP8, and Qualcomm
Iris stateful HEVC on nodes that advertise the corresponding V4L2 formats.
Set `V4L2_VA_DISABLE_HEVC_STATEFUL=1` to disable the generated
parameter-set HEVC path on firmware with a non-standard contract.
The target node can decode fixed-resolution VP9 natively, but VA Profile 0 is
withdrawn: repeated VP9 source changes and VA context recreation reproducibly
reboot the qualified kernel/firmware. Applications therefore fall back to
software VP9 instead of exposing a device-wide crash path.
The implementation has been tested using Intel's [vaapi-fits](https://github.com/intel/vaapi-fits) on an RK3399, which is supported by the `hantro` and `rockchip` drivers.
Feedback on results for other platforms are very welcome, do not expect the library to simply work smoothly though.
This fork experimentally detects Qualcomm Iris stateful H.264 when the device advertises
`V4L2_PIX_FMT_H264` rather than `V4L2_PIX_FMT_H264_SLICE`. Queue setup is lazy
so a client can create a VA context before allocating render targets. The
dormant stateful VP9 translator remains in the tree for firmware diagnosis,
but it is not advertised through VA-API.
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

### Machine-readable capability contract

The release boundary is recorded in
[`data/iris-codec-capabilities.json`](data/iris-codec-capabilities.json). It
separates production VA support from software/native fallback and rejected
inputs: H.264 and HEVC are the qualified Iris VA paths, while VA VP9 remains
withdrawn and VA AV1 remains rejected because the caller's tile payload does
not preserve Iris' complete OBU contract. Consumers and packaging checks
should read this file instead of inferring support from a device node alone.

Dynamic-resolution, VP9 and zero-copy investigations should be run through
`scripts/iris-experiment-guard.sh -- command args...`. The guard records the
boot ID and thermal samples, applies a timeout, stops on reboot or an 85 C
thermal limit, and leaves an `interrupted` marker that must be reviewed before
another run. It stores one-off artifacts under
`~/Lab/Bridge/tmp/trash/iris-experiment` by default.
