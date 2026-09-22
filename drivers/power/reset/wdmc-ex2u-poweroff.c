// SPDX-License-Identifier: GPL-2.0-only
/*
 * Power-off for the WD My Cloud Expert Series EX2 Ultra (Marvell Armada
 * 385).
 *
 * Neither mainline nor the vendor 6.6.129 GPL source implements this in
 * kernel: the vendor's own DTS carried a "uart-poweroff" node, but that
 * compatible string matches no driver anywhere, mainline or vendor -- it
 * was dropped rather than ported (see git history, "drop uart-poweroff/
 * restart nodes"). Without this driver, `halt`/`poweroff` on this
 * hardware just parks the CPU with the board still fully powered: SATA
 * bay LEDs lit, USB3 VBUS live, fan and front power LED untouched, no
 * real power cut.
 *
 * There IS a real power-off, reachable only through the board's uart1,
 * connected to a Welltrend 6703F-OG240WT MCU (see that DTS node's own
 * comment) that also controls the fan and the front power LED. The
 * board's userspace has an `mcu_ctl` binary that talks to it; this
 * driver reimplements its wire protocol directly, in-kernel, for the one
 * command that matters here (`sys_shutdown`) -- reverse-engineered by
 * disassembling mcu_ctl (stripped ARM PIE ELF, no source available) and
 * confirmed against real hardware via `strace -e trace=write -x` capture
 * of every command mcu_ctl supports except sys_shutdown/sys_reboot
 * themselves (too destructive to trace directly -- captured those last,
 * relying on the confirmed ~100ms inter-byte pacing giving enough time
 * for the strace output to already be visible over the SSH session
 * before the MCU physically cuts power). Confirmed protocol: 19200 8N1,
 * no parity, no flow control (cfsetispeed(14) == B19200; the same
 * baud/framing set up by mcu_ctl's own set_interface_attribs()), 7-byte
 * frames `FA <cat> <subcmd> <param> 00 00 FB`. sys_shutdown itself is
 * `FA 03 03 01 00 00 FB` -- same cat/subcmd as sys_silent (`FA 03 03 00
 * 00 00 FB`), differing only in the param byte, so this is presumably
 * one MCU-side "system mode" command with silent=0/shutdown=1, not two
 * unrelated opcodes.
 *
 * uart1 is Marvell's own ns16550a-compatible UART (mainline serial8250,
 * already bound and normally used as /dev/ttyS1) -- like the GPIO banks
 * below, this driver piggybacks a second, non-exclusive raw MMIO mapping
 * of the same physical registers rather than going through the tty
 * layer, since by the time pm_power_off() runs, userspace (and any
 * process that might have /dev/ttyS1 open) is already gone. Safe only
 * because this is unconditionally the last thing this handler does,
 * right before halting for good -- nothing else touches this UART
 * afterward. If the MCU actually cuts power on receiving this, the board
 * dies mid-sequence here, which is the intended, confirmed-on-hardware
 * outcome; the GPIO/LED work above and the halt loop below exist as a
 * fallback in case it doesn't (wrong divisor, protocol drift, MCU
 * firmware quirk, etc).
 *
 * The two explicit reg_sata0/reg_sata1 12V drive-power regulators are
 * deliberately left alone by the GPIO fallback below: cutting drive
 * power directly (as opposed to via the MCU's own real shutdown) is a
 * real, consequential action, not a cosmetic one, and hasn't been asked
 * for. That fallback only quiets the two things under this SoC's own
 * direct GPIO control that otherwise stay conspicuously live:
 *
 *  - The four SATA bay LEDs (sata1/sata2 red+blue -- gpio1 bits
 *    11/20/21/22 in the board DTS): already exclusively owned by
 *    leds-gpio, so turning them off through the LED class from here
 *    isn't possible without a second, conflicting gpiod_get. Poked
 *    directly instead, through a second, non-exclusive raw MMIO mapping
 *    of the same gpio1 bank's DATA_OUT register -- safe for the same
 *    "last thing before halting" reason as the UART above. Confirmed on
 *    real hardware that the red and blue channels have opposite polarity
 *    (red active-high, blue active-low), so "off" means clearing bits
 *    11/20 but *setting* bits 21/22 -- see wd,gpioN-clear-bits/
 *    wd,gpioN-set-bits below.
 *
 *  - USB3 VBUS (both ports -- gpio0 bits 26/27): same story, already
 *    exclusively owned by the two regulator-fixed nodes (both also
 *    regulator-always-on, which would make regulator_disable() a no-op
 *    even if they weren't already double-claimed) -- raw MMIO on
 *    gpio0's DATA_OUT register for the same reason as the LEDs above.
 *
 * Direction is left untouched on both GPIO banks: both banks' relevant
 * pins are already configured as outputs by their existing owners
 * (leds-gpio / regulator-fixed) well before this handler ever runs.
 *
 * Real disk spin-down is handled separately, through the SCSI layer's
 * own existing sd_shutdown() mechanism (manage_start_stop_unit), which
 * always runs before pm_power_off() -- unchanged here.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>

#define MVEBU_GPIO_OUT_OFF	0x00
#define WDMC_EX2U_MAX_BITS	8

/* ns16550a registers, reg-shift=2 / reg-io-width=1 per the uart1 DTS node */
#define MCU_UART_REG_SHIFT	2
#define MCU_UART_THR		(0 << MCU_UART_REG_SHIFT)
#define MCU_UART_DLL		(0 << MCU_UART_REG_SHIFT)	/* LCR.DLAB=1 */
#define MCU_UART_IER		(1 << MCU_UART_REG_SHIFT)
#define MCU_UART_DLM		(1 << MCU_UART_REG_SHIFT)	/* LCR.DLAB=1 */
#define MCU_UART_FCR		(2 << MCU_UART_REG_SHIFT)
#define MCU_UART_LCR		(3 << MCU_UART_REG_SHIFT)
#define MCU_UART_LSR		(5 << MCU_UART_REG_SHIFT)

#define MCU_UART_LCR_DLAB	0x80
#define MCU_UART_LCR_8N1	0x03
#define MCU_UART_FCR_RESET	0x07	/* FIFO enable + RX/TX clear */
#define MCU_UART_LSR_THRE	0x20
#define MCU_UART_LSR_TEMT	0x40

/* Confirmed on real hardware: uart1 is clocked from the SoC's 200MHz
 * TCLK (coreclk 0 in the DTS; "TClock @ 200 [MHz]" in this board's own
 * boot log), and mcu_ctl configures 19200 baud (cfsetispeed(14) ==
 * B19200). Standard ns16550 divisor -- not exact (651 gives ~19200.9
 * baud), well within normal UART tolerance.
 */
#define MCU_UART_CLOCK_HZ	200000000UL
#define MCU_UART_BAUD		19200
#define MCU_UART_DIVISOR	(MCU_UART_CLOCK_HZ / (16 * MCU_UART_BAUD))

#define MCU_UART_TIMEOUT_LOOPS	1000000

static const u8 wdmc_ex2u_mcu_sys_shutdown[] = {
	0xfa, 0x03, 0x03, 0x01, 0x00, 0x00, 0xfb,
};

struct wdmc_ex2u_gpio_bank {
	void __iomem *base;
	unsigned int clear_bits[WDMC_EX2U_MAX_BITS];
	unsigned int n_clear_bits;
	unsigned int set_bits[WDMC_EX2U_MAX_BITS];
	unsigned int n_set_bits;
};

struct wdmc_ex2u_poweroff_data {
	struct wdmc_ex2u_gpio_bank gpio0;
	struct wdmc_ex2u_gpio_bank gpio1;
	void __iomem *uart1_base;
};

static struct wdmc_ex2u_poweroff_data *wdmc_ex2u_poweroff;

static void wdmc_ex2u_apply_bank(const struct wdmc_ex2u_gpio_bank *bank)
{
	u32 val;
	unsigned int i;

	if (!bank->base)
		return;

	val = readl(bank->base + MVEBU_GPIO_OUT_OFF);
	for (i = 0; i < bank->n_clear_bits; i++)
		val &= ~BIT(bank->clear_bits[i]);
	for (i = 0; i < bank->n_set_bits; i++)
		val |= BIT(bank->set_bits[i]);
	writel(val, bank->base + MVEBU_GPIO_OUT_OFF);
}

static void wdmc_ex2u_uart_putc(void __iomem *base, u8 c)
{
	unsigned long timeout = MCU_UART_TIMEOUT_LOOPS;

	while (!(readb(base + MCU_UART_LSR) & MCU_UART_LSR_THRE) && --timeout)
		cpu_relax();

	writeb(c, base + MCU_UART_THR);
}

static void wdmc_ex2u_mcu_shutdown(void __iomem *base)
{
	unsigned long timeout;
	unsigned int i;

	if (!base)
		return;

	pr_emerg("wdmc-ex2u-poweroff: sending sys_shutdown to MCU via uart1\n");

	writeb(0x00, base + MCU_UART_IER);
	writeb(MCU_UART_LCR_DLAB | MCU_UART_LCR_8N1, base + MCU_UART_LCR);
	writeb(MCU_UART_DIVISOR & 0xff, base + MCU_UART_DLL);
	writeb((MCU_UART_DIVISOR >> 8) & 0xff, base + MCU_UART_DLM);
	writeb(MCU_UART_LCR_8N1, base + MCU_UART_LCR);
	writeb(MCU_UART_FCR_RESET, base + MCU_UART_FCR);

	for (i = 0; i < sizeof(wdmc_ex2u_mcu_sys_shutdown); i++) {
		wdmc_ex2u_uart_putc(base, wdmc_ex2u_mcu_sys_shutdown[i]);
		/* mirror mcu_ctl's own inter-byte pacing (strace-confirmed) */
		mdelay(100);
	}

	timeout = MCU_UART_TIMEOUT_LOOPS;
	while (!(readb(base + MCU_UART_LSR) & MCU_UART_LSR_TEMT) && --timeout)
		cpu_relax();

	pr_emerg("wdmc-ex2u-poweroff: sys_shutdown sent, MCU should cut power now\n");
}

static void wdmc_ex2u_poweroff_handler(void)
{
	pr_emerg("wdmc-ex2u-poweroff: handler entered (%s)\n",
		 wdmc_ex2u_poweroff ? "data present" : "data NULL, bailing out");

	if (!wdmc_ex2u_poweroff)
		return;

	wdmc_ex2u_apply_bank(&wdmc_ex2u_poweroff->gpio1);
	pr_emerg("wdmc-ex2u-poweroff: sata bay leds off\n");

	wdmc_ex2u_apply_bank(&wdmc_ex2u_poweroff->gpio0);
	pr_emerg("wdmc-ex2u-poweroff: usb3 vbus off\n");

	/*
	 * If the MCU actually cuts power on receiving this, the board dies
	 * right here -- the confirmed, intended outcome. Everything below
	 * is only reached if it doesn't (see this file's top comment).
	 */
	wdmc_ex2u_mcu_shutdown(wdmc_ex2u_poweroff->uart1_base);

	pr_emerg("wdmc-ex2u-poweroff: handler done, halting\n");

	/*
	 * This board's machine_power_off() (arch/arm/kernel/reboot.c) falls
	 * straight through once this handler returns instead of looping
	 * forever the way machine_halt() does -- confirmed on real hardware
	 * (before the MCU shutdown above existed): the system kept running
	 * for 10+ more seconds (CPU0 alive, CPU1 parked by the earlier
	 * smp_send_stop()), tripping an RCU stall warning, and something
	 * still running during that window (a disk-activity LED trigger,
	 * most likely) re-lit the SATA bay LEDs we'd just turned off above.
	 * Halt for real here so the off state we just set actually sticks,
	 * in case the MCU shutdown above didn't actually cut power.
	 */
	local_irq_disable();
	while (1)
		cpu_relax();
}

static int wdmc_ex2u_read_bits(struct device *dev, const char *prop,
				unsigned int *bits, unsigned int max,
				unsigned int *n_out)
{
	int ret, i;

	ret = of_property_count_u32_elems(dev->of_node, prop);
	if (ret < 0)
		return ret;
	if (ret > max)
		return -EINVAL;

	for (i = 0; i < ret; i++) {
		u32 bit;

		of_property_read_u32_index(dev->of_node, prop, i, &bit);
		if (bit >= 32)
			return -EINVAL;
		bits[i] = bit;
	}

	*n_out = ret;
	return 0;
}

static int wdmc_ex2u_init_bank(struct device *dev, struct platform_device *pdev,
				 const char *res_name, struct wdmc_ex2u_gpio_bank *bank,
				 const char *clear_prop, const char *set_prop)
{
	struct resource *res;
	int ret;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, res_name);
	if (!res)
		return 0;

	bank->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!bank->base)
		return -ENOMEM;

	if (clear_prop) {
		ret = wdmc_ex2u_read_bits(dev, clear_prop, bank->clear_bits,
					   WDMC_EX2U_MAX_BITS, &bank->n_clear_bits);
		if (ret)
			return dev_err_probe(dev, ret, "bad %s\n", clear_prop);
	}

	if (set_prop) {
		ret = wdmc_ex2u_read_bits(dev, set_prop, bank->set_bits,
					   WDMC_EX2U_MAX_BITS, &bank->n_set_bits);
		if (ret)
			return dev_err_probe(dev, ret, "bad %s\n", set_prop);
	}

	return 0;
}

static int wdmc_ex2u_poweroff_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wdmc_ex2u_poweroff_data *data;
	struct resource *res;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	ret = wdmc_ex2u_init_bank(dev, pdev, "gpio0", &data->gpio0,
				   "wd,gpio0-clear-bits", NULL);
	if (ret)
		return ret;

	ret = wdmc_ex2u_init_bank(dev, pdev, "gpio1", &data->gpio1,
				   "wd,gpio1-clear-bits", "wd,gpio1-set-bits");
	if (ret)
		return ret;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "uart1");
	if (res) {
		data->uart1_base = devm_ioremap(dev, res->start, resource_size(res));
		if (!data->uart1_base)
			return -ENOMEM;
	}

	if (pm_power_off)
		return dev_err_probe(dev, -EBUSY, "pm_power_off already claimed\n");

	wdmc_ex2u_poweroff = data;
	pm_power_off = wdmc_ex2u_poweroff_handler;
	platform_set_drvdata(pdev, data);

	dev_info(dev, "registered as pm_power_off (gpio0: %u clear; gpio1: %u clear, %u set; uart1 mcu shutdown: %s)\n",
		 data->gpio0.n_clear_bits, data->gpio1.n_clear_bits, data->gpio1.n_set_bits,
		 data->uart1_base ? "yes" : "no");

	return 0;
}

static void wdmc_ex2u_poweroff_remove(struct platform_device *pdev)
{
	if (pm_power_off == wdmc_ex2u_poweroff_handler) {
		pm_power_off = NULL;
		wdmc_ex2u_poweroff = NULL;
	}
}

static const struct of_device_id wdmc_ex2u_poweroff_of_match[] = {
	{ .compatible = "wd,ex2u-poweroff" },
	{ }
};
MODULE_DEVICE_TABLE(of, wdmc_ex2u_poweroff_of_match);

static struct platform_driver wdmc_ex2u_poweroff_driver = {
	.probe = wdmc_ex2u_poweroff_probe,
	.remove = wdmc_ex2u_poweroff_remove,
	.driver = {
		.name = "wdmc-ex2u-poweroff",
		.of_match_table = wdmc_ex2u_poweroff_of_match,
	},
};
module_platform_driver(wdmc_ex2u_poweroff_driver);

MODULE_DESCRIPTION("Power-off (MCU sys_shutdown via uart1, SATA bay LEDs/USB3 VBUS fallback) for WD My Cloud Expert EX2 Ultra");
MODULE_LICENSE("GPL");
