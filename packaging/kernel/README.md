# Iris kernel module patch

`0001-media-iris-decode-order-output-and-64-capture-buffers.patch` is the
two strongtz/libva-v4l2 Iris patches (decode-order output through
`V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY{,_ENABLE}`, CAPTURE pool up to 64)
rebased onto `ianchb/sm8550-mainline` branch `sheng-7.2.6`
(commit `42f3b40c702a`). The driver works without it (display-order
fallback) but Chrome zero-copy and the VP9 dynamic-resolution qualification
need it.

Rebuild only the module, on the target, against the running kernel:

```sh
git clone --depth 1 -b sheng-7.2.6 https://github.com/ianchb/sm8550-mainline linux
cd linux && git am ../0001-*.patch
cp /boot/config-$(uname -r) .config
export LOCALVERSION=""                      # keep the "+" off the release string
make ARCH=arm64 LLVM=1 olddefconfig modules_prepare
# Module.symvers is not shipped; derive the dependency symbols from the
# running kernel so modpost records "depends:" correctly.
awk '$4 ~ /^\[/ && $2 ~ /[TDR]/ {gsub(/[\[\]]/,"",$4); print "0x00000000\t"$3"\t"$4"\tEXPORT_SYMBOL\t"}' /proc/kallsyms \
  | grep -E 'videobuf2|videodev|v4l2_mem2mem' > Module.symvers
make ARCH=arm64 LLVM=1 KBUILD_MODPOST_WARN=1 \
     KCFLAGS=-mbranch-protection=pac-ret+bti \
     M=drivers/media/platform/qcom/iris modules
llvm-readelf -n drivers/media/platform/qcom/iris/qcom-iris.ko | grep -q 'BTI, PAC'
```

`KCFLAGS=-mbranch-protection=pac-ret+bti` is required: the kernel is built
with `CONFIG_ARM64_BTI_KERNEL`, but Kconfig drops that option under
clang ≥ 21, and a module without BTI landing pads oopses on load.

Install by replacing the file and rebooting. Do not `modprobe -r qcom_iris`
on a booted system: unloading hangs in `iris_vpu_power_off` while the VPU
is powered.

```sh
zstd -19 -f qcom-iris.ko -o qcom-iris.ko.zst
sudo cp qcom-iris.ko.zst /lib/modules/$(uname -r)/kernel/drivers/media/platform/qcom/iris/
sudo depmod -a && sudo systemctl reboot
v4l2-ctl -d /dev/videoN --list-ctrls | grep display_delay   # present = decode-order
```

Keep the stock `qcom-iris.ko.zst` beside it for rollback.
