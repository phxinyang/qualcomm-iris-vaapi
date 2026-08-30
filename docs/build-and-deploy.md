# Build And Deploy

The target-independent build is reproducible on the development host:

```sh
CXX=/usr/bin/g++ CC=/usr/bin/gcc PKG_CONFIG=/usr/bin/pkg-config \
PKG_CONFIG_PATH=/usr/lib64/pkgconfig meson setup build-local
ninja -C build-local
ldd -r build-local/src/v4l2_drv_video.so
```

The last command must print no `undefined symbol` lines. A successful link is
not enough for a VA-API driver: an unresolved relocation otherwise appears to
clients as a silent software-decoder fallback.

For a tablet or other V4L2 target, set `IRIS_REMOTE_HOST` and run:

```sh
IRIS_REMOTE_HOST=xinyang@192.168.3.139 ./test/remote/deploy.sh
```

The deploy script synchronizes by content, excludes generated build and test
artifacts, refreshes source timestamps, performs a clean Ninja rebuild, and
gates the resulting shared object with `ldd -r`. The remote lab directory is
durable and defaults to `~/Lab/iris-vaapi-lab`; downloaded Fluster resources
and Qualcomm test tools stay there rather than under a disposable scratch
directory.

The decoder node is resolved by the `iris_driver` name in
`test/remote/setup.sh`. Do not hardcode `/dev/video17`: V4L2 numbering changes
across boots and that node may belong to the camera subsystem. Override the
resolved node for a run with `LIBVA_V4L2_VIDEO_PATH`.

After deployment, run the short positive oracle before external corpora:

```sh
ssh "$IRIS_REMOTE_HOST" 'cd ~/Lab/iris-vaapi-lab/src && \
  LIBVA_DRIVER_NAME=v4l2 \
  LIBVA_DRIVERS_PATH=$PWD/build/src \
  LIBVA_V4L2_VIDEO_PATH=/dev/video0 \
  ./test/iris-matrix.sh'
```

The matrix requires exact software/VA frame MD5 equality and strict stateful
EOS accounting. Fluster and native V4L2 suites are additional capability and
firmware diagnostics; always record which decoder path was used.
