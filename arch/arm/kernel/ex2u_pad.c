// SPDX-License-Identifier: GPL-2.0
/*
 * Deliberate kernel image padding for the WD My Cloud EX2 Ultra
 * (armada-385-wdmc-Ex2-Ultra) board port.
 *
 * This board's legacy U-Boot (2013.01) always loads uRamdisk at a
 * fixed physical address baked into the saved "bootcmd" environment
 * variable (0x00f00000, size up to 0x00333000, i.e. covering roughly
 * 15-18.2 MB of RAM) and there is no per-boot way to relocate it from
 * the kernel/DTS side.
 *
 * With CONFIG_ARM_APPENDED_DTB + CONFIG_ARM_ATAG_DTB_COMPAT, the
 * zImage decompressor (arch/arm/boot/compressed/head.S) relocates the
 * merged ATAG+DTB blob to just past the end of the decompressed
 * kernel image whenever it would otherwise overwrite the still-being-
 * read compressed payload. For this kernel's particular size, that
 * relocation happened to land the DTB at ~0x01008a80 - squarely
 * inside the fixed ramdisk window above - which made the kernel
 * disable the initrd ("INITRD: ... overlaps in-use memory region")
 * and panic with "VFS: Unable to mount root fs".
 *
 * Rather than edit the device's saved U-Boot environment, pad the
 * kernel's own uncompressed size so the relocated DTB lands safely
 * past 0x01233000 instead.
 *
 * A first attempt padded via .bss (4 MiB, zero-cost to flash size).
 * That only shifted the relocated DTB by ~0.68 MiB out of the 4 MiB
 * added: head.S's relocation-destination math (r9, via
 * get_inflated_image_size) reflects the *inflated/initialized*
 * image span, not _end; .bss only feeds into it through a separate,
 * partial "don't let .bss swallow the DTB" correction
 * (_kernel_bss_size handling in head.S), not a straight 1:1 term.
 * So this array is placed in .data instead (forced via an explicit
 * section, regardless of its content) so it counts fully and
 * directly toward that inflated size. It is still cheap on flash:
 * a big same-valued fill compresses to almost nothing under the
 * XZKERN piggy compression.
 *
 * If this board's real fix (bumping the saved U-Boot bootcmd's
 * ramdisk load address) is ever applied instead, this file can be
 * dropped -- verify with `memblock=debug` on the kernel command line
 * that the DTB reservation no longer falls inside the ramdisk's load
 * window before removing it.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/types.h>

#define EX2U_BOOT_PAD_SIZE (8 * 1024 * 1024)

static u8 ex2u_boot_pad[EX2U_BOOT_PAD_SIZE] __used __section(".data.ex2u_pad");

static int __init ex2u_boot_pad_touch(void)
{
	/* __used (and the explicit .data section) keep this from being elided. */
	pr_info("ex2u_pad: %u bytes reserved at %p\n",
		EX2U_BOOT_PAD_SIZE, ex2u_boot_pad);
	return 0;
}
early_initcall(ex2u_boot_pad_touch);
