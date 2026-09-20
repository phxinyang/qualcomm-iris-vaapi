# Contributing

Contributions should preserve the project's explicit hardware-decoder contract:
real media playback, selected decoder evidence, frame/drop counters, and safe
fallbacks matter more than a video that merely plays.

## Development loop

Configure and build with Meson, then run the same gates used by CI:

```sh
meson setup build-local
ninja -C build-local
sh test/run-static-checks.sh
meson test -C build-local --print-errorlogs
git diff --check
```

Hardware suites need a target with Qualcomm Iris. Set `IRIS_REMOTE_HOST`
explicitly before using `test/remote/deploy.sh` or `test/remote/setup.sh`;
remote scripts intentionally have no private-host default. Keep downloaded
corpora, traces, screenshots, and media outside the repository.

When changing codec support, queue ownership, or browser launch behavior, add a
regression check and update the relevant capability or compatibility document.
Never hardcode a boot-specific `/dev/videoN` node.

## Commits and pull requests

Use a short imperative commit subject and describe the validation performed.
Pull requests should state the target/runtime, decoder name, frame/drop counts,
and any unqualified or hardware-dependent results. Do not attach private logs,
device addresses, credentials, or generated binaries.
