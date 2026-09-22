# Project execution contract

This repository targets Qualcomm Iris V4L2/libVA decoding on ARM64 Fedora
devices. The original product goal is verifiable browser hardware decode with
safe fallback, not merely successful playback.

## Non-obvious constraints

- Resolve Iris decoder nodes by the `iris_driver` name at runtime. Never encode
  `/dev/video0`, `/dev/video17`, or any other boot-specific number.
- Production VA exposure is limited to the profiles proven by the target matrix.
  VP9 is qualified. AV1 is opt-in because the firmware returns no CAPTURE buffer
  for hidden frames. Keep software/native fallback explicit in docs and tests.
- The default path is Wayland + VA-API.
- Zero-copy is automatic where the evidence allows it: the Direct path for
  post-decode exporters, the Import path for pre-exporters on a decode-order
  kernel. No environment variable turns it on; `V4L2_VA_PUBLISH` is a
  diagnostic override only. Import keeps one client buffer in flight and
  never skips a submitted picture; those two rules are load-bearing
  (TEST-RESULTS.md, "Client-owned CAPTURE buffers").
- A browser counts as hardware-decoder compatible only when a real media surface
  reports the selected decoder and frame/drop evidence. GPU-process startup or
  libva mapping alone is insufficient.
- Dynamic-resolution experiments can reset firmware. Use a bounded remote command,
  record boot ID and temperature, and stop after a reset; do not immediately chain
  another experiment.

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
