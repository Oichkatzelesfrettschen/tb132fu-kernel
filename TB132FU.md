# Lenovo TB132FU kernel

This repository contains the source history for the Lenovo TB132FU kernel
bring-up and Android compatibility work. The Pure Pixel 17 R5/R6 release
source is Linux 4.19.325-cip135-st19 with the TB132FU vendor hardware support
retained. The separate TB132FU build counter is Kernel #52.

## Pure Pixel 17 R5/R6 checkpoint

Kernel #52 is the release-identity successor to the physically qualified K51
implementation. It preserves K51's kernel behavior and corrects only its
version provenance: `-cip135` and `-st19` are the imported CIP release and
staging-tree suffixes, while `#52` is supplied by `KBUILD_BUILD_VERSION` as the
TB132FU build counter. The resulting release identity is
`4.19.325-cip135-st19+ #52`.

R5 adds the MediaTek USB-C DisplayPort transmitter path and restores Lenovo's
TB132FU DPTX audio programming. The accepted policy rejects the physically
unstable uncompressed 4K30 mode, allows 4K60 only when the trained link reports
both DSC and FEC, and otherwise selects the physically qualified 1080p60
fallback. The matching R5 userspace stack produces stable 48 kHz HDMI stereo
at normal pitch with independent left and right channels.

Kernel #52 also retains the R4 BQ27541 battery-health reporting, negotiated
charger reporting, native 80 percent charge limit across both charging paths,
and GT9768 rear-camera autofocus. It keeps the working PRC GC08A3 camera path
unchanged and restores Lenovo's alternate S5K4H7 sensor implementation for
compiled ROW coverage. The alternate S5K4H7 hardware path still requires
physical confirmation on an affected tablet.

The historical K20 through K51 local versions identify development boots.
They do not represent newer CIP `-st` releases. K49 remains the accepted HDMI
audio rollback point, K51 is the physically qualified final display-policy
implementation, and K52 is the corrected public R5 release identity built from
that implementation. Pure Pixel 17 R6 reuses K52 byte-for-byte; R6 does not
claim a new kernel implementation or build number.

## Pure Pixel 17 R2 checkpoint

Pure Pixel 17 R2 uses kernel build 14 from commit `f766d4120417b`. It is the
accepted R1 kernel #13 plus a focused power-supply change that reports battery
health and charge cycles from the tablet's BQ27541 fuel gauge. The existing
MediaTek gauge remains the fallback. Charging policy, capacity, suspend
behavior, configuration, module ABI, and the `4.19.325-cip135-st19+` release
baseline are unchanged.

The R2 candidate physically reported a valid battery condition and nonzero
cycle count while retaining the accepted boot, charging, suspend/wake, and
hardware behavior.

## Pure Pixel 17 R1 checkpoint

The Android 17 R1 release uses kernel build 13 from immutable implementation
commit `5b0f70f39eff047774cf24d61301f92506330113`. Release-source tag
`tb132fu-purepixel17-r1-k13` adds only the reproducible defconfig and public
documentation. Its kernel release string is `4.19.325-cip135-st19+`.

The exact configuration is published as
`arch/arm64/configs/tb132fu_android17_defconfig`. With Android Clang r383902,
it expands to the accepted `.config` SHA-256
`6c6920de38ca9d55e1a7b4cbc74fc0cdbb9c5ab4b402c5fbec35201dfa7c5f43`.
The accepted module ABI contains 12,233 `Module.symvers` entries and has
SHA-256 `75ba5cd713a4a4d0ec2472bdfd67e5639ee3eb72a814394c2099d427c829269a`.

Pure Pixel 17 R1 physically passed boot, repeated suspend/wake, full battery
depletion and unattended charger recovery, display and touch, Goodix pen,
pogo keyboard/trackpad/lid, Wi-Fi, Bluetooth, front/rear cameras, flash/torch,
four-speaker audio, microphone, haptics, 60/120 Hz operation, adaptive
brightness, AOD/DT2W, and hardware media decode qualification on TB132FU_PRC.
The kernel is shared by the PRC and ROW product contract, but a native ROW
tablet has not completed the same physical qualification matrix.

## Earlier tested checkpoints

| Tag | Kernel | Status |
| --- | --- | --- |
| `tb132fu-purepixel17-r6-k52` | 4.19.325-cip135-st19 | Pure Pixel 17 R6 documentation branch; kernel implementation unchanged from R5 K52 |
| `tb132fu-purepixel17-r5-k52` | 4.19.325-cip135-st19 | Pure Pixel 17 R5 Kernel #52 release source |
| `tb132fu-purepixel17-r5-k51` | 4.19.325-cip135-st51 | Physically qualified R5 development predecessor; suffix retained as history |
| `tb132fu-purepixel17-r4-k17` | 4.19.325-cip135-st19 | Pure Pixel 17 R4 Kernel #17 |
| R2 branch `purepixel17-tb132fu-r2-kernel` | 4.19.325-cip135-st19 | Android 17 R2 kernel build 14 |
| `tb132fu-purepixel17-r1-k13` | 4.19.325-cip135-st19 | Android 17 R1 kernel build 13 |
| `tb132fu-v84-4.19.325-cip135` | 4.19.325-cip135-st19 | Earlier Android 16 checkpoint |
| `tb132fu-v83-4.19.325` | 4.19.325 | Known-good rollback checkpoint |
| `tb132fu-v82-4.19.300` | 4.19.300 | Known-good rollback checkpoint |
| `tb132fu-v77-4.19.200` | 4.19.200 | Earlier known-good checkpoint |

The current R6 release branch is `tb132fu-purepixel17-r6-k52`; the exact R5
kernel implementation remains at `tb132fu-purepixel17-r5-k52`. The Android 17
R1 development branch is `tb132fu-android17-cip135-k13-audio-slot-controls`.
Earlier matching source branches are `tb132fu-4.19.325-cip135`,
`tb132fu-4.19.325`, and `tb132fu-4.19.300`; their branches and tags are retained
for history.

## Earlier Android 16 validation on 4.19.325-cip135-st19

- Android 16 boots successfully.
- The 1536x2560 panel operates at 60 Hz and 120 Hz.
- Touch, Goodix pen, and screen off/on resume work across the full panel.
- Wi-Fi connects, Bluetooth reaches ON state, and the Lenovo pen reconnects.
- Both cameras and the rear flash enumerate through Android's camera service.
- GPS and FM modules load; speaker, haptic, double-tap gesture, and pogo-keyboard
  vendor paths are retained from the preceding functional checkpoint.
- Seven external connectivity modules pass an exact kernel symbol/CRC audit.
- Automated severe-event scans and hands-on use found no kernel panic, oops,
  warning, lockup, module ABI failure, or hardware regression.

Double-tap-to-wake is validated when Android performs a normal screen-off
transition. A generic GSI Always-On Display overlay can instead keep Android in
doze without issuing the framebuffer power-down transition used to arm the
Goodix gesture path. That ROM configuration issue is not a kernel regression.

Pure Pixel 17 uses a device-specific AOD/DT2W integration and does not rely on
that generic GSI behavior.

## Source composition

The tree combines the Motorola/MediaTek 4.19 vendor implementation used as the
newer vendor base, Lenovo TB132FU-specific hardware behavior recovered from the
available Lenovo 4.14 source and running device, Linux stable updates through
4.19.325, and the CIP v4.19.325-cip135 maintenance checkpoint based on the
v4.19-st19 staging tree. Hardware-sensitive conflicts are resolved
conservatively and audited against the working tablet.

This is a downstream vendor kernel, not a byte-identical vanilla stable tree.
The version number does not guarantee that every known or unknown vulnerability
is fixed, especially in out-of-tree MediaTek, Motorola, or Lenovo drivers.

## Important warning

This source is specific to the Lenovo TB132FU. A boot image also depends on the
correct device tree, ramdisk, connectivity modules, boot-header layout, and
partition size. Do not flash a generic Image or use these artifacts on another
device. Keep a known-good boot image and a working fastboot connection before
testing changes.

Boot images, ramdisks, proprietary firmware, signing keys, device logs, and
private build artifacts are not distributed in this source tree. See
`COPYING` and individual file headers for source licensing.
