# Building the TB132FU Pure Pixel 17 R5 kernel

## Exact source and toolchain

- release-source branch: `tb132fu-purepixel17-r5-k52`
- kernel release: `4.19.325-cip135-st19+`
- toolchain: Android Clang `r383902` (clang 11.0.1, Android build 6443078)
- architecture: arm64
- defconfig: `tb132fu_android17_defconfig`
- TB132FU build number: 52

Kernel #52 preserves the physically qualified K51 HDMI audio and DisplayPort
implementation. Its source correction restores CIP's imported `-st19` staging
suffix and keeps the independent TB132FU counter in `KBUILD_BUILD_VERSION`.
The version string and build counter therefore have separate meanings:

```text
4.19.325-cip135-st19+ #52
```

## Build

Obtain Android's `clang-r383902` Linux host prebuilt, then set `CLANG_DIR` to
its root directory. The directory must contain `bin/clang`.

```sh
export CLANG_DIR=/path/to/clang-r383902
export OUT_DIR=/path/to/an/empty/output-directory
./build-tb132fu.sh
```

`OUT_DIR` defaults to `out` below the source tree. `JOBS` defaults to the host
CPU count and `KBUILD_BUILD_VERSION` defaults to `52`. The helper produces the
kernel Image, compressed Image, DTBs, and in-tree modules. It verifies the
expanded configuration before compiling.

The qualified expanded `.config` has SHA-256:

```text
c1730b517e0c36dc9641925ee3eee7bc9dceb2a2143263c6301c6a59abce5fe7
```

Pure Pixel release engineering additionally rebuilds the matching external
connectivity modules, creates the recovery-as-boot image with the qualified
ramdisk and DTB, and verifies module CRCs and vermagic against the resulting
kernel release. Changing any `localversion*` file requires that complete module
step; replacing only the kernel Image is not a supported release procedure.

## Historical reproducibility

The R1 source checkpoint remains available at
`tb132fu-purepixel17-r1-k13`. It used the same Android Clang toolchain and the
earlier accepted configuration and module-ABI receipts documented on that
branch. R2 Kernel #14 and R4 Kernel #17 likewise remain available on their
named branches and tags. Do not change those historical records when building
R5.

## Boot-image boundary

The generated `arch/arm64/boot/Image.gz` is not a flashable Android boot image.
A working TB132FU boot image also requires the matching ramdisk, DTB, external
connectivity modules, boot header v2 geometry, command line, 64 MiB partition
sizing, and AVB metadata. Those inputs belong to the matching device/ROM
release and are intentionally not embedded in this kernel source repository.
Never flash a standalone Image or an image assembled for another device.
