# MCEX2U-6.18

Linux 6.18.x port for the **WD My Cloud Expert Series EX2 Ultra** (BVBZ/Ranger
Peak) — Marvell Armada 385, ARMv7. Board DTS:
[`arch/arm/boot/dts/marvell/armada-385-wdmc-Ex2-Ultra.dts`](arch/arm/boot/dts/marvell/armada-385-wdmc-Ex2-Ultra.dts).

This is otherwise a pristine `linux.git` tree, kept in sync with upstream
stable point releases; all board-specific work is isolated to the commits
listed below.

## Board-specific fixes

- **`CONFIG_THUMB2_KERNEL=y` is required.** Without it, the decompressed
  kernel is large enough that the appended/merged DTB's relocated address
  collides with U-Boot's fixed uRamdisk load window (`0xf00000`–
  `0x1234fff`), panicking at `INITRD: ... overlaps in-use memory region`.
  This is the real, hardware-confirmed fix — not a workaround. ARMv7-only;
  see [`drivers/power/reset/wdmc-ex2u-poweroff.c`](drivers/power/reset/wdmc-ex2u-poweroff.c)'s
  own git history for the workarounds that were tried and rejected first.

- **`uImage` must stay ≤ 5,242,880 bytes.** Two sequential U-Boot `fatload`
  calls stage at overlapping addresses (`0xa00000`→`0xf00000`); going over
  this size corrupts the load. Currently ~3.9 MB (~74% of the ceiling).

- **Fixed rescue-userspace module list.** The board's rescue environment
  `insmod`s exactly `ahci_mvebu.ko ext4.ko jbd2.ko libahci_platform.ko
  mbcache.ko raid1.ko uas.ko usb-storage.ko spi-nor.ko sata_mv.ko
  raid456.ko`, and each must load without needing any module outside this
  list. Several prompt-less Kconfig library symbols (`libahci`, `xor`,
  `async_tx`/`async_memcpy`/`async_xor`/`async_pq`/`async_raid6_recov`,
  `raid6_pq`) are forced built-in via targeted `Makefile` patches rather
  than Kconfig, to satisfy this without pulling the real driver modules
  themselves out of the fixed list. See commits `e8cba7ac5` and
  `8fc6a4648`.

- **Reset button sends `KEY_PROG1`, not `KEY_RESTART`.** `KEY_RESTART` is
  treated as a genuine reboot request by `systemd-logind` — confirmed on
  real hardware, a single press rebooted the board. See commit
  `9f8f06ac2`. Reacting to the button in userspace (this board runs full
  Debian, not a busybox rescue image) is done via `triggerhappy`, not a
  custom daemon.

- **Real power-off via the Welltrend 6703F-OG240WT MCU.** The vendor's
  `uart-poweroff` DTS node matched no driver anywhere and was dropped
  (commit `cc3937011`); [`drivers/power/reset/wdmc-ex2u-poweroff.c`](drivers/power/reset/wdmc-ex2u-poweroff.c)
  reimplements the real protocol instead, natively in-kernel, no
  userspace helper needed. The MCU's wire protocol (19200 8N1, `FA <cat>
  <subcmd> <param> 00 00 FB` frames) was reverse-engineered from the
  vendor's stripped `mcu_ctl` userspace binary and confirmed against real
  hardware via `strace`. Confirmed on real hardware: this is a genuine
  power cut (everything but the network card turns off — the network
  card is left powered on purpose, for Wake-on-LAN). SATA-bay-LED/USB3-VBUS
  GPIO pokes and a guaranteed CPU halt are kept as a fallback in case the
  MCU command doesn't fire for some reason.

## Notable `.config` additions over the vendor baseline

- USB Wi-Fi (modern drivers only where a legacy duplicate exists),
  non-USB wireless (PCIe etc.) disabled.
- `NTFS3_FS=m` (modern NTFS driver), `ISCSI_TARGET=m`/`TARGET_CORE=m` (LIO
  iSCSI target stack).
- `WATCHDOG=y` (orion_wdt); PCI removed (unused on this board).
- Legacy `iptables`/`ip6tables` NAT + MASQUERADE, in addition to the
  nftables equivalent (both work for IPv4 and IPv6).
- UBIFS enabled (also happens to be what makes `CRC16=y` without forcing
  `EXT4_FS=y` — see git history if this looks like an odd combination).

## Known, deliberate non-goals

- **Network card stays powered after shutdown** — needed for Wake-on-LAN,
  confirmed wanted by the board's owner. Do not "fix" this without
  checking first.
- **Fan and front (3rd, blue-only) power LED are not controlled by this
  kernel.** Both are `mcu_ctl`-only (`fan_set_*`, `led_set_on/off`), not
  wired into the poweroff driver — only `sys_shutdown` was ported, since
  that's the one action that must survive userspace already being torn
  down. If you need fan/LED control at a point where userspace is still
  alive, call `mcu_ctl` directly (e.g. a `systemd-shutdown` hook) rather
  than extending this kernel driver.

## Building

```sh
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- LOCALVERSION= zImage modules dtbs
```

`LOCALVERSION=` is required on every invocation — without it,
`scripts/setlocalversion` appends a stray `+` to the version string
whenever the tree isn't perfectly clean.

Package `uImage` (plain `make uImage` does **not** append the DTB
correctly for this board's U-Boot):

```sh
cat arch/arm/boot/zImage arch/arm/boot/dts/marvell/armada-385-wdmc-Ex2-Ultra.dtb > /tmp/zImage+dtb
mkimage -A arm -O linux -T kernel -C none -a 0x00008000 -e 0x00008000 \
        -n "Linux $(make kernelversion) EX2U" -d /tmp/zImage+dtb uImage
```
