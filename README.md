# V4L2 libVA Backend
This libVA backend is designed to work with the [Video for Linux Memory-To-Memory API](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-mem2mem.html) that is used by a number of video codecs drivers, in particular SoCs found on SBCs.

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
```sh
LIBVA_DRIVERS_PATH=<project dir>/build/src LIBVA_DRIVER_NAME=v4l2 vainfo
```

Alternatively, a packaging build configured with the system prefix installs the driver:
```sh
meson setup build --prefix=/usr
meson compile -C build
sudo meson install -C build
```

For direct deployment, prefer the helper below. It reads libva's canonical `driverdir` from `libva.pc`:
```sh
sudo ./scripts/install-system.sh build
```

The project is packaged for Fedora and Arch Linux (`packaging/fedora/libva-v4l2-iris.spec`, `packaging/arch/PKGBUILD`). Both install through the same helper.

## How it decodes

The driver owns one model with two publish policies chosen per surface automatically. It owns the CAPTURE pool. A surface either takes the completed CAPTURE slot itself (Direct path for FFmpeg, GStreamer, mpv, offering true zero-copy), or, if the client exported it before its first decode (like Chrome), the frame is blitted on the GPU (EGL, Adreno) into a stable buffer whose file descriptor never changes (Copy path). CPU memcpy is the fallback engine.

The driver requires two Iris kernel patches from strongtz/libva-v4l2 applied to the self-built kernel module: decode-order output (via the display-delay control) and 64-buffer CAPTURE max. The driver probes for the control and falls back to display-order behaviour (bounded sync wait plus one STOP/LAST/START drain on timeout) on a stock kernel.

## Environment variables

Every switch the driver reads is listed here and read in exactly one place, `src/util/options.cc`; `test/iris-env-doc-check.sh` fails the build if this table and that file disagree in either direction.

| Variable | Default | Purpose |
| --- | --- | --- |
| `LIBVA_V4L2_VIDEO_PATH` | probe | Use this decoder node instead of scanning `/dev/video*` for the `iris_driver` decoder. |
| `V4L2_VA_TRACE` | off | Per-frame trace on stderr. Any value enables it. The record vocabulary is documented in `docs/architecture.md`. |
| `V4L2_VA_SYNC_TIMEOUT_MS` | `2000` | Bounded `vaSyncSurface()` wait, `50..60000`. Without an override the first frame of a cold session may wait up to `30000` ms for firmware bring-up. |
| `V4L2_VA_COPY` | `gpu` | Engine used to publish a frame into a surface the client exported before its first decode: `gpu` (EGL blit on Adreno) or `cpu`. |
| `V4L2_VA_PUBLISH` | `auto` | Force `copy` or `direct` publication for every surface. Diagnostics only; `auto` picks per surface from the client's export behaviour. |
| `V4L2_VA_DUMP` | off | Directory to write every submitted access unit into. |
| `V4L2_VA_EXPERIMENTAL_PROFILES` | off | Also advertise AV1. Qualification only; the launcher never sets it. |

## Status

| Codec | Qualification Level |
| --- | --- |
| H.264 | Qualified (exact framemd5 matrix, structure matrix, 100-switch dynamic resolution, 120 s dual-context soak, Chrome B-frame zero dropped, Direct-path zero-copy under FFmpeg) |
| HEVC Main | Qualified |
| VP9 Profile 0 | Qualified (additionally 100 context recreations and Chrome with alt-ref) |
| HEVC Main10 | Advertised when the node offers P010 but not separately qualified |
| VP9 Profile 2 | Advertised when the node offers P010 but not separately qualified |
| AV1 | Opt-in. The OBU rebuild is bit-exact (software oracle) but the firmware returns no CAPTURE buffer for hidden (show_frame=0) pictures and VA has no show_existing_frame call, so VA clients time out on streams with alt-ref frames. Not a driver bug. |

## Verifying

Before claiming a change is complete, run the relevant local gates:
- `sh test/run-static-checks.sh`
- `meson test -C build-local`
- `test/iris-browser-acceptance.sh --clip … [--driver …]` on the target
- `test/iris-matrix.sh`
- `test/iris-structure-matrix.sh`
- `test/iris-zero-copy-suite.sh`
- `test/iris-dynamic-resolution.sh`
- `test/iris-concurrency-soak.sh`
- `test/av1-obu-roundtrip.sh`
- experiments through `scripts/iris-experiment-guard.sh`

For complete architecture details and contract definitions, see [`docs/architecture.md`](docs/architecture.md).
For instructions on the clean target rebuild, symbol gate, and durable tablet lab workflow, see [`docs/build-and-deploy.md`](docs/build-and-deploy.md).
