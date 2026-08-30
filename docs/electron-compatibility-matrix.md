# Electron Compatibility Matrix

This matrix records what has actually been verified for Electron-based
applications on the remote ARM64 Fedora target. An application starting and
loading the VA driver is not, by itself, proof that its video elements use the
Iris decoder.

| Application | Version / runtime | Display path | Sandbox used in test | GPU/VA evidence | Video decoder evidence | Result |
| --- | --- | --- | --- | --- | --- | --- |
| Visual Studio Code | 1.125.1 (Electron runtime reported by the package) | Wayland (`--ozone-platform=wayland`) | `--disable-gpu-sandbox` in isolated test profile | GPU process loaded project `v4l2_drv_video.so` and `libva.so` | N/A: no video playback surface was opened | Electron shell/GPU pass; video decode unqualified |
| Obsidian | 1.13.7, Electron 43.3.0, Chrome 150.0.7871.212 | Wayland (`--ozone-platform=wayland`) | `--no-sandbox --disable-gpu-sandbox` in isolated test profile | GPU process loaded `/usr/lib64/dri/v4l2_drv_video.so` and `libva.so`; opened `/dev/dri/renderD128` | N/A: starter vault contained no video playback surface | Electron shell/GPU pass; video decode unqualified |

## What these results mean

- Both applications can launch on the target, render through Wayland, and
  initialize the GPU process with the installed VA-API driver.
- Neither result demonstrates hardware video decode. A valid decode result must
  exercise a real HTML/media playback surface and report the decoder selected by
  the application. For Chromium-family validation, the release gate is
  `VaapiVideoDecoder` together with `kIsPlatformVideoDecoder=true`, plus frame
  and drop counters and a trace showing Iris OUTPUT/CAPTURE activity.
- The browser result must not be copied to Electron applications. Electron
  versions, command-line feature defaults, sandboxing, and media plumbing vary
  independently.

## Reproduction records

The tests used independent profiles under
`~/Lab/Bridge/tmp/trash/` and CDP ports 9240 (VS Code) and 9241 (Obsidian), so
existing user sessions were not modified. The Obsidian renderer user agent was:

```text
Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36
(KHTML, like Gecko) obsidian/1.13.7 Chrome/150.0.7871.212
Electron/43.3.0 Safari/537.36
```

The Iris decoder node was resolved dynamically as `/dev/video17` during this
boot. Do not encode that number in an application launcher; use the project's
`iris_resolve_device` logic or `LIBVA_V4L2_VIDEO_PATH` for diagnostics.

## Next Electron validation gate

When disk headroom is restored, validate one application at a time with a
small local H.264 sample and an isolated profile:

1. Launch with the project's VA-only environment and Wayland flags.
2. Open a real `<video>` playback surface in the application (without touching
   the user's vault/workspace).
3. Capture the application's media-internals/devtools evidence, selected
   decoder name, decoded/dropped frames, and Iris trace activity.
4. Repeat once with VA disabled to establish the software-decoder baseline.
5. Record whether the application requires `--no-sandbox`, and treat that as a
   security limitation rather than a hardware-decoder success criterion.

Do not install additional large Electron products while the root filesystem is
at or near 100% usage. Reclaim or provision storage first; then prefer a small,
official ARM64 build and record its exact version before testing.
