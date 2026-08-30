# Project execution contract

This repository targets Qualcomm Iris V4L2/libVA decoding on ARM64 Fedora
devices. The original product goal is verifiable browser hardware decode with
safe fallback, not merely successful playback.

## Non-obvious constraints

- Resolve Iris decoder nodes by the `iris_driver` name at runtime. Never encode
  `/dev/video0`, `/dev/video17`, or any other boot-specific number.
- Production VA exposure is limited to the profiles proven by the target
  matrix. VP9 VA is withdrawn on the qualified firmware; AV1 VA remains
  disabled because of the tile/OBU contract. Keep software/native fallback
  explicit in docs and tests.
- The default path is Wayland + VA-API. Vulkan, ANGLE Vulkan and unsafe WebGPU
  belong only to the separately named experiment entry.
- `V4L2_VA_ZERO_COPY=1` is experimental and currently limited to the validated
  one-AU, no-B H.264/VP9 ownership contract. Do not make it the default without
  multi-slot lifetime and dynamic-resolution evidence.
- A browser or Electron app counts as hardware-decoder compatible only when a
  real media surface reports the selected decoder and frame/drop evidence.
  GPU-process startup or libva mapping alone is insufficient.
- Dynamic-resolution, VP9 and zero-copy experiments can reset firmware. Use a
  bounded remote command, record boot ID and temperature, and stop after a
  reset; do not immediately chain another experiment.

## Verification contract

Before claiming a change is complete, run the relevant local gates:

```sh
sh test/run-static-checks.sh
meson test -C build-local --print-errorlogs
git diff --check
```

For a target claim, also record the exact source commit, driver hash, resolved
node, boot ID, decoder name, decoded/dropped frames, and whether the runtime
uses the system-installed driver or a build-tree override. Do not claim a
system install from a `DESTDIR` staging run.

## Worktree and release safety

Preserve user-owned untracked artifacts. Do not run `git clean`, destructive
reset, or push unless the user explicitly asks for it. Keep one-off traces and
large generated media outside the repository when possible. A release claim
requires source, installed runtime, package contents, and rollback state to be
checked separately.
