# Qualcomm Iris V4L2/libVA backend

A libva driver for the stateful V4L2 decoder that Qualcomm's Iris VPU exposes
on SM8550 and related SoCs. It exists for the clients that have no other
hardware path: Google Chrome, Electron and Firefox on ARM64 Linux, where
VA-API is the only entry point to the video decoder. FFmpeg, GStreamer and
mpv use the same driver but have native V4L2 paths of their own.

This is a fork of [mxsrc/libva-v4l2](https://github.com/mxsrc/libva-v4l2)
(MIT / LGPL-2.1, see [License and credits](#license-and-credits)). Everything
below the fork point was rebuilt around one ownership model; the stateful
Iris path, the browser acceptance gates and the Fedora/Arch packaging are
this fork's work. Development targets a Snapdragon SM8550 tablet (Xiaomi Pad
6S Pro, device codename `sheng`) on Fedora 44 with a self-built kernel.

Qualification status lives in [TEST-RESULTS.md](TEST-RESULTS.md), with the
boot ID, kernel, module hash and driver hash of every recorded run. In short:
H.264, HEVC Main, HEVC Main10 and VP9 Profile 0/2 decode in hardware with
exact bitstream-level verification; Chrome reaches the hardware decoder for
all of them; AV1 is opt-in because the firmware returns no CAPTURE buffer for
hidden frames.

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

The driver owns one model with three publish policies chosen per surface
automatically. It owns the CAPTURE pool.

- A surface that is exported or mapped only after its decode (FFmpeg,
  GStreamer, mpv) takes the completed CAPTURE slot itself: **Direct**, true
  zero-copy.
- A surface the client exported *before* its first decode (Chrome
  pre-exports its whole surface pool) is either decoded into directly
  (**Import**) or filled by a GPU blit (**Copy**). Import queues the client's
  buffer into the CAPTURE queue for exactly that picture, which needs
  decode-order output and gives Chrome a path with no copy at all; it is
  selected per codec from the measured evidence (H.264, HEVC, Main10) and
  falls back to Copy for VP9 and on stock kernels. Copy blits on the Adreno
  GPU (EGL) into a stable buffer whose file descriptor never changes; CPU
  memcpy is the fallback engine.

Import is only exact when the session keeps one client buffer in flight and
never skips a submitted picture; both are enforced in the session, and a
completion that lands in a neighbour's buffer fails both surfaces instead of
publishing. The measurements behind the policy are in TEST-RESULTS.md.

The driver wants two Iris kernel patches from strongtz/libva-v4l2 applied to the self-built kernel module: decode-order output (via the display-delay control) and 64-buffer CAPTURE max. `packaging/kernel/` carries them rebased onto the `sheng-7.2.6` tree, with build notes for rebuilding only the `qcom-iris` module. The driver probes for the control and falls back to display-order behaviour (bounded sync wait plus one STOP/LAST/START drain on timeout) on a stock kernel.

## License and credits

The driver is distributed under the same terms as upstream: MIT, with the
LGPL-2.1-covered parts, see `COPYING`, `COPYING.MIT` and `COPYING.LGPL`.

- The stateful Iris implementation in `src/` is this fork's work; the VA and
  V4L2 scaffolding descends from [mxsrc/libva-v4l2](https://github.com/mxsrc/libva-v4l2).
- `include/linux/` vendors the kernel UAPI headers, each carrying its own
  SPDX tag (`GPL-2.0+ WITH Linux-syscall-note OR BSD-3-Clause`).
- `packaging/kernel/` carries the two Iris kernel patches from
  [strongtz/libva-v4l2](https://github.com/strongtz/libva-v4l2) (Radxa)
  rebased onto the current kernel tree. That patch touches GPL-2.0 kernel
  code and carries that license, not the driver's.

## Environment variables

Every switch the driver reads is listed here and read in exactly one place, `src/util/options.cc`; `test/iris-env-doc-check.sh` fails the build if this table and that file disagree in either direction.

| Variable | Default | Purpose |
| --- | --- | --- |
| `LIBVA_V4L2_VIDEO_PATH` | probe | Use this decoder node instead of scanning `/dev/video*` for the `iris_driver` decoder. |
| `V4L2_VA_TRACE` | off | Per-frame trace on stderr. Any value enables it. The record vocabulary is documented in `docs/architecture.md`. |
| `V4L2_VA_SYNC_TIMEOUT_MS` | `2000` | Bounded `vaSyncSurface()` wait, `50..60000`. Without an override the first frame of a cold session may wait up to `30000` ms for firmware bring-up. |
| `V4L2_VA_COPY` | `gpu` | Engine used to publish a frame into a surface the client exported before its first decode: `gpu` (EGL blit on Adreno) or `cpu`. |
| `V4L2_VA_PUBLISH` | `auto` | Force `copy`, `direct` or `import` publication for every surface. Diagnostics only; `auto` picks per surface from the client's export behaviour (Direct for post-decode exporters, Import for pre-exporters on a decode-order kernel, Copy otherwise). `import` degrades to `copy` where the session cannot honour it. |
| `V4L2_VA_DUMP` | off | Directory to write every submitted access unit into. |
| `V4L2_VA_EXPERIMENTAL_PROFILES` | off | Also advertise AV1. Qualification only; the launcher never sets it. |

## Status

| Codec | Qualification Level |
| --- | --- |
| H.264 | Qualified (exact framemd5 matrix, structure matrix, 100-switch dynamic resolution, 120 s dual-context soak, Chrome B-frame zero dropped, Direct-path zero-copy under FFmpeg) |
| HEVC Main | Qualified |
| VP9 Profile 0 | Qualified (additionally 100 context recreations and Chrome with alt-ref) |
| HEVC Main10 | Qualified (exact framemd5 at 10-bit, Chrome 1080p24 Main10 B-frame stream with the GPU copy engine on P010) |
| VP9 Profile 2 | Qualified (exact framemd5 at 10-bit through FFmpeg) |
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
