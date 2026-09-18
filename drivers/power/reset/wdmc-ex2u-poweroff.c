// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cosmetic power-off for the WD My Cloud Expert Series EX2 Ultra
 * (Marvell Armada 385).
 *
 * Neither mainline nor the vendor 6.6.129 GPL source implements a real
 * hardware power-off for this board: the vendor's own DTS carried a
 * "uart-poweroff" node, but that compatible string matches no driver
 * anywhere, mainline or vendor -- it was dropped rather than ported
 * (see git history, "drop uart-poweroff/restart nodes"). No board-level
 * 12V power-hold GPIO was found either. `halt`/`poweroff` on this
 * hardware therefore just park the CPU with the board still fully
 * powered: SATA bay LEDs lit, USB3 VBUS live.
 *
 * This driver does not attempt to cut real board power -- there is
 * currently no known way to do that from software on this hardware.
 * The two explicit reg_sata0/reg_sata1 12V drive-power regulators are
 * deliberately left alone: cutting drive power is a real, consequential
 * action, not a cosmetic one, and hasn't been asked for. This driver
 * only quiets the two things under this SoC's own direct GPIO control
 * that otherwise stay conspicuously live:
 *
 *  - The four SATA bay LEDs (sata1/sata2 red+blue -- gpio1 bits
 *    11/20/21/22 in the board DTS): already exclusively owned by
 *    leds-gpio, so turning them off through the LED class from here
 *    isn't possible without a second, conflicting gpiod_get. Poked
 *    directly instead, through a second, non-exclusive raw MMIO mapping
 *    of the same gpio1 bank's DATA_OUT register -- safe because this
 *    only ever runs as the very last thing before the CPU parks, with
 *    nothing left to race against.
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

struct wdmc_ex2u_poweroff_data {
	void __iomem *gpio0_base;
	unsigned int gpio0_off_bits[WDMC_EX2U_MAX_BITS];
	unsigned int n_gpio0_off_bits;

	void __iomem *gpio1_base;
	unsigned int gpio1_off_bits[WDMC_EX2U_MAX_BITS];
	unsigned int n_gpio1_off_bits;
};

static struct wdmc_ex2u_poweroff_data *wdmc_ex2u_poweroff;

static void wdmc_ex2u_clear_bits(void __iomem *base, const unsigned int *bits, unsigned int n)
{
	u32 val;
	unsigned int i;

	val = readl(base + MVEBU_GPIO_OUT_OFF);
	for (i = 0; i < n; i++)
		val &= ~BIT(bits[i]);
	writel(val, base + MVEBU_GPIO_OUT_OFF);
}

static void wdmc_ex2u_poweroff_handler(void)
{
	pr_emerg("wdmc-ex2u-poweroff: handler entered (%s)\n",
		 wdmc_ex2u_poweroff ? "data present" : "data NULL, bailing out");

	if (!wdmc_ex2u_poweroff)
		return;

	if (wdmc_ex2u_poweroff->gpio1_base) {
		wdmc_ex2u_clear_bits(wdmc_ex2u_poweroff->gpio1_base,
				      wdmc_ex2u_poweroff->gpio1_off_bits,
				      wdmc_ex2u_poweroff->n_gpio1_off_bits);
		pr_emerg("wdmc-ex2u-poweroff: sata bay leds off\n");
	}

	if (wdmc_ex2u_poweroff->gpio0_base) {
		wdmc_ex2u_clear_bits(wdmc_ex2u_poweroff->gpio0_base,
				      wdmc_ex2u_poweroff->gpio0_off_bits,
				      wdmc_ex2u_poweroff->n_gpio0_off_bits);
		pr_emerg("wdmc-ex2u-poweroff: usb3 vbus off\n");
	}

	pr_emerg("wdmc-ex2u-poweroff: handler done\n");
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

static int wdmc_ex2u_poweroff_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wdmc_ex2u_poweroff_data *data;
	struct resource *res;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "gpio0");
	if (res) {
		data->gpio0_base = devm_ioremap(dev, res->start, resource_size(res));
		if (!data->gpio0_base)
			return -ENOMEM;

		ret = wdmc_ex2u_read_bits(dev, "wd,gpio0-off-bits",
					   data->gpio0_off_bits,
					   WDMC_EX2U_MAX_BITS,
					   &data->n_gpio0_off_bits);
		if (ret)
			return dev_err_probe(dev, ret, "bad wd,gpio0-off-bits\n");
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "gpio1");
	if (res) {
		data->gpio1_base = devm_ioremap(dev, res->start, resource_size(res));
		if (!data->gpio1_base)
			return -ENOMEM;

		ret = wdmc_ex2u_read_bits(dev, "wd,gpio1-off-bits",
					   data->gpio1_off_bits,
					   WDMC_EX2U_MAX_BITS,
					   &data->n_gpio1_off_bits);
		if (ret)
			return dev_err_probe(dev, ret, "bad wd,gpio1-off-bits\n");
	}

	if (pm_power_off)
		return dev_err_probe(dev, -EBUSY, "pm_power_off already claimed\n");

	wdmc_ex2u_poweroff = data;
	pm_power_off = wdmc_ex2u_poweroff_handler;
	platform_set_drvdata(pdev, data);

	dev_info(dev, "registered as pm_power_off (%u gpio0 bit(s), %u gpio1 bit(s))\n",
		 data->n_gpio0_off_bits, data->n_gpio1_off_bits);

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
