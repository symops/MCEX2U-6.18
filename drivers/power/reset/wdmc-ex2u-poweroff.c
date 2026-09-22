// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cosmetic power-off for the WD My Cloud Expert Series EX2 Ultra
 * (Marvell Armada 385).
 *
 * Neither mainline nor the vendor 6.6.129 GPL source implements a real
 * hardware power-off reachable from a plain GPIO or SoC register: the
 * vendor's own DTS carried a "uart-poweroff" node, but that compatible
 * string matches no driver anywhere, mainline or vendor -- it was
 * dropped rather than ported (see git history, "drop uart-poweroff/
 * restart nodes"). No board-level 12V power-hold GPIO was found either.
 * `halt`/`poweroff` on this hardware therefore just park the CPU with
 * the board still fully powered: SATA bay LEDs lit, USB3 VBUS live.
 *
 * A real power-off likely *does* exist, just not through this driver: the
 * board's userspace has an `mcu_ctl` binary that talks to the Welltrend
 * 6703F-OG240WT MCU on uart1 (see that node's own comment) and exposes a
 * `sys_shutdown` command, plus `fan_set_0`/`led_set_off` for the fan and
 * the separate front power LED this driver has no access to at all. That
 * MCU protocol isn't implemented in-kernel anywhere (it's presumably what
 * the dropped "uart-poweroff" node was for) -- the real fix belongs in a
 * userspace shutdown hook (e.g. a systemd unit running `mcu_ctl
 * sys_shutdown` before `shutdown.target`), not here, since `mcu_ctl` is a
 * userspace binary this kernel-side handler cannot invoke once userspace
 * is already torn down.
 *
 * This driver does not attempt to cut real board power itself -- there is
 * no known way to do that from a plain GPIO/register poke on this
 * hardware (as opposed to the MCU protocol above). The two explicit
 * reg_sata0/reg_sata1 12V drive-power regulators are deliberately left
 * alone: cutting drive power is a real, consequential action, not a
 * cosmetic one, and hasn't been asked for. This driver only quiets the
 * two things under this SoC's own direct GPIO control that otherwise
 * stay conspicuously live:
 *
 *  - The four SATA bay LEDs (sata1/sata2 red+blue -- gpio1 bits
 *    11/20/21/22 in the board DTS): already exclusively owned by
 *    leds-gpio, so turning them off through the LED class from here
 *    isn't possible without a second, conflicting gpiod_get. Poked
 *    directly instead, through a second, non-exclusive raw MMIO mapping
 *    of the same gpio1 bank's DATA_OUT register -- safe because this
 *    only ever runs as the very last thing before the CPU parks, with
 *    nothing left to race against. Confirmed on real hardware that the
 *    red and blue channels have opposite polarity (red active-high,
 *    blue active-low), so "off" means clearing bits 11/20 but *setting*
 *    bits 21/22 -- see wd,gpioN-clear-bits/wd,gpioN-set-bits below.
 *
 *  - USB3 VBUS (both ports -- gpio0 bits 26/27): same story, already
 *    exclusively owned by the two regulator-fixed nodes (both also
 *    regulator-always-on, which would make regulator_disable() a no-op
 *    even if they weren't already double-claimed) -- raw MMIO on
 *    gpio0's DATA_OUT register for the same reason as the LEDs above.
 *
 * Both GPIO banks are Marvell's own gpio-mvebu.c controller (mainline,
 * full gpiolib support, unlike the Realtek misc-gpio case this driver
 * is modelled after on the Monarch/Duo ports) -- this driver piggybacks
 * a second, ordinary ioremap of the same physical registers rather than
 * a gpiod_get, purely to avoid the double-claim that gpiolib's
 * exclusive-request model would otherwise reject. Direction is left
 * untouched: both banks' relevant pins are already configured as
 * outputs by their existing owners (leds-gpio / regulator-fixed) well
 * before this handler ever runs.
 *
 * Real disk spin-down is handled separately, through the SCSI layer's
 * own existing sd_shutdown() mechanism (manage_start_stop_unit), which
 * always runs before pm_power_off() -- unchanged here.
 */

#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>

#define MVEBU_GPIO_OUT_OFF	0x00
#define WDMC_EX2U_MAX_BITS	8

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

	pr_emerg("wdmc-ex2u-poweroff: handler done, halting\n");

	/*
	 * This board has no real power-cut mechanism, so machine_power_off()
	 * (arch/arm/kernel/reboot.c) falls straight through after this
	 * handler returns instead of looping forever the way machine_halt()
	 * does -- confirmed on real hardware: the system kept running for
	 * 10+ more seconds (CPU0 alive, CPU1 parked by the earlier
	 * smp_send_stop()), tripping an RCU stall warning, and something
	 * still running during that window (a disk-activity LED trigger,
	 * most likely) re-lit the SATA bay LEDs we'd just turned off above.
	 * Halt for real here so the off state we just set actually sticks.
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

	if (pm_power_off)
		return dev_err_probe(dev, -EBUSY, "pm_power_off already claimed\n");

	wdmc_ex2u_poweroff = data;
	pm_power_off = wdmc_ex2u_poweroff_handler;
	platform_set_drvdata(pdev, data);

	dev_info(dev, "registered as pm_power_off (gpio0: %u clear; gpio1: %u clear, %u set)\n",
		 data->gpio0.n_clear_bits, data->gpio1.n_clear_bits, data->gpio1.n_set_bits);

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

MODULE_DESCRIPTION("Cosmetic power-off (SATA bay LEDs/USB3 VBUS) for WD My Cloud Expert EX2 Ultra");
MODULE_LICENSE("GPL");
